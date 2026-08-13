#include "../../runtime/bus/bus.h"
#include "../../runtime/runtime.h"
#include "../../runtime/runtime_kernel.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#endif

typedef struct {
  uint64_t peak_rss_kb;
  uint64_t peak_footprint_kb;
  uint64_t samples;
} MemoryStats;

static pid_t g_gateway_pid;
static volatile sig_atomic_t g_interrupted;

/* bus.c narrates only from the retry API. This driver uses bus_publish(),
 * but the link still needs the runtime symbol. */
int rt_narrate(const char *fmt, ...) {
  (void)fmt;
  return 0;
}

static void failf(const char *fmt, ...) {
  va_list ap;
  fputs("thrash: FAIL ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static long env_long(const char *name, long fallback, long min, long max) {
  const char *text = getenv(name);
  char *end = NULL;
  long value;
  if (!text || !*text) return fallback;
  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < min || value > max) {
    failf("invalid %s=%s (expected %ld..%ld)", name, text, min, max);
    exit(2);
  }
  return value;
}

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(uint64_t duration_ns) {
  struct timespec ts;
  ts.tv_sec = (time_t)(duration_ns / 1000000000ULL);
  ts.tv_nsec = (long)(duration_ns % 1000000000ULL);
  while (!g_interrupted && nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static void sleep_until(uint64_t target_ns) {
  uint64_t now = monotonic_ns();
  if (now < target_ns) sleep_ns(target_ns - now);
}

static int gateway_alive(void) {
  int status;
  pid_t waited;
  if (g_gateway_pid <= 1) return 0;
  waited = waitpid(g_gateway_pid, &status, WNOHANG);
  if (waited == 0) return kill(g_gateway_pid, 0) == 0;
  if (waited == g_gateway_pid) {
    failf("gateway exited early status=%d", status);
    g_gateway_pid = 0;
  }
  return 0;
}

static void signal_cleanup(int sig) {
  g_interrupted = 1;
  if (g_gateway_pid > 1) (void)kill(g_gateway_pid, SIGTERM);
  (void)sig;
}

static int read_pid_file(const char *path, pid_t expected) {
  FILE *file = fopen(path, "r");
  long pid = 0;
  int ok = file && fscanf(file, "%ld", &pid) == 1 && pid == (long)expected;
  if (file) fclose(file);
  return ok;
}

static int start_gateway(const char *binary, const char *log_path,
                         const char *floop) {
  int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (log_fd < 0) {
    failf("cannot open gateway log %s: %s", log_path, strerror(errno));
    return -1;
  }
  g_gateway_pid = fork();
  if (g_gateway_pid < 0) {
    failf("gateway fork failed: %s", strerror(errno));
    close(log_fd);
    return -1;
  }
  if (g_gateway_pid == 0) {
    if (dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0)
      _exit(126);
    if (log_fd != STDOUT_FILENO && log_fd != STDERR_FILENO) close(log_fd);
    (void)setenv("FCLAW_GATEWAY_DISABLE_CHANNELS", "1", 1);
    (void)setenv("FCLAW_GATEWAY_POLL_MS", "1", 1);
    execl(binary, binary, "gateway", "run", "--floop", floop, (char *)NULL);
    _exit(127);
  }
  close(log_fd);
  for (int i = 0; i < 1000; ++i) {
    if (read_pid_file(".fclaw/run/gateway.pid", g_gateway_pid) && gateway_alive())
      return 0;
    if (!gateway_alive()) return -1;
    sleep_ns(10000000ULL);
  }
  failf("gateway readiness timeout pid=%ld", (long)g_gateway_pid);
  return -1;
}

static int stop_gateway(void) {
  int status = 0;
  if (g_gateway_pid <= 1) return -1;
  if (kill(g_gateway_pid, SIGTERM) != 0 && errno != ESRCH) {
    failf("gateway SIGTERM failed: %s", strerror(errno));
    return -1;
  }
  for (int i = 0; i < 3000; ++i) {
    pid_t waited = waitpid(g_gateway_pid, &status, WNOHANG);
    if (waited == g_gateway_pid) {
      g_gateway_pid = 0;
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failf("gateway shutdown status=%d", status);
        return -1;
      }
      return 0;
    }
    if (waited < 0 && errno != EINTR) break;
    sleep_ns(10000000ULL);
  }
  failf("gateway shutdown timeout");
  (void)kill(g_gateway_pid, SIGKILL);
  (void)waitpid(g_gateway_pid, &status, 0);
  g_gateway_pid = 0;
  return -1;
}

static int sample_memory(pid_t pid, MemoryStats *stats) {
  uint64_t rss_kb = 0;
  uint64_t footprint_kb = 0;
#ifdef __APPLE__
  struct proc_taskinfo task;
  struct rusage_info_v0 usage;
  memset(&task, 0, sizeof(task));
  memset(&usage, 0, sizeof(usage));
  if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) !=
      (int)sizeof(task))
    return -1;
  rss_kb = task.pti_resident_size / 1024ULL;
  if (proc_pid_rusage(pid, RUSAGE_INFO_V0, (rusage_info_t *)&usage) != 0)
    return -1;
  footprint_kb = usage.ri_phys_footprint / 1024ULL;
#else
  char path[64];
  FILE *file;
  unsigned long pages = 0;
  snprintf(path, sizeof(path), "/proc/%ld/statm", (long)pid);
  file = fopen(path, "r");
  if (!file || fscanf(file, "%*lu %lu", &pages) != 1) {
    if (file) fclose(file);
    return -1;
  }
  fclose(file);
  rss_kb = (uint64_t)pages * (uint64_t)sysconf(_SC_PAGESIZE) / 1024ULL;
  footprint_kb = rss_kb;
#endif
  if (rss_kb > stats->peak_rss_kb) stats->peak_rss_kb = rss_kb;
  if (footprint_kb > stats->peak_footprint_kb)
    stats->peak_footprint_kb = footprint_kb;
  stats->samples++;
  return 0;
}

static int publish_one(uint64_t sequence, int contexts, int one_context) {
  char payload[512];
  char id[BUS_ID_MAX];
  int context = one_context ? 0 : (int)((sequence - 1ULL) % (uint64_t)contexts);
  snprintf(payload, sizeof(payload),
           "{\"text\":\"thrash sequence=%llu\","
           "\"adapter_id\":\"thrash_ctx_%03d\"}",
           (unsigned long long)sequence, context);
  if (bus_publish("thrash", "user_message", payload, id, sizeof(id)) != 0) {
    failf("publish failed sequence=%llu: %s",
          (unsigned long long)sequence, strerror(errno));
    return -1;
  }
  return 0;
}

static long delivery_count(FILE **stream, char **line, size_t *line_cap,
                           long current) {
  ssize_t n;
  if (!*stream) {
    *stream = fopen("workspace/logs/deliveries.jsonl", "r");
    if (!*stream) return errno == ENOENT ? current : -1;
  }
  clearerr(*stream);
  while ((n = getline(line, line_cap, *stream)) >= 0) {
    (void)n;
    current++;
  }
  return ferror(*stream) ? -1 : current;
}

static int wait_for_drain(long expected, int timeout_seconds,
                          MemoryStats *memory, double *drain_seconds) {
  FILE *deliveries = NULL;
  char *line = NULL;
  size_t line_cap = 0;
  long count = 0;
  uint64_t start = monotonic_ns();
  uint64_t deadline = start + (uint64_t)timeout_seconds * 1000000000ULL;
  uint64_t next_report = start;
  while (monotonic_ns() < deadline) {
    if (g_interrupted) return -1;
    count = delivery_count(&deliveries, &line, &line_cap, count);
    if (count < 0) {
      failf("cannot read delivery ledger");
      break;
    }
    if (count > expected) {
      failf("delivery count exceeded expected: %ld > %ld", count, expected);
      break;
    }
    if (count == expected && bus_inbox_count() == 0) {
      *drain_seconds = (double)(monotonic_ns() - start) / 1000000000.0;
      if (deliveries) fclose(deliveries);
      free(line);
      return 0;
    }
    if (!gateway_alive()) break;
    (void)sample_memory(g_gateway_pid, memory);
    if (monotonic_ns() >= next_report) {
      printf("thrash: drain deliveries=%ld/%ld inbox=%d\n",
             count, expected, bus_inbox_count());
      fflush(stdout);
      next_report += 30000000000ULL;
    }
    sleep_ns(50000000ULL);
  }
  if (deliveries) fclose(deliveries);
  free(line);
  failf("drain timeout/crash deliveries=%ld/%ld inbox=%d",
        count, expected, bus_inbox_count());
  return -1;
}

static int count_regular_files(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  int count = 0;
  if (!dir) return errno == ENOENT ? 0 : -1;
  while ((entry = readdir(dir)) != NULL) {
    char child[PATH_MAX];
    struct stat st;
    if (entry->d_name[0] == '.') continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
        (int)sizeof(child)) continue;
    if (stat(child, &st) == 0 && S_ISREG(st.st_mode)) count++;
  }
  closedir(dir);
  return count;
}

static int count_run_directories(void) {
  DIR *dir = opendir("workspace/runs");
  struct dirent *entry;
  int count = 0;
  if (!dir) return errno == ENOENT ? 0 : -1;
  while ((entry = readdir(dir)) != NULL) {
    char path[PATH_MAX];
    struct stat st;
    int number;
    if (sscanf(entry->d_name, "run_%d", &number) != 1) continue;
    snprintf(path, sizeof(path), "workspace/runs/%s", entry->d_name);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) count++;
  }
  closedir(dir);
  return count;
}

static int verify_deliveries(long expected) {
  FILE *file = fopen("workspace/logs/deliveries.jsonl", "r");
  unsigned char *origins = calloc((size_t)expected + 1U, 1U);
  unsigned char *runs = calloc((size_t)expected + 1U, 1U);
  char *line = NULL;
  size_t cap = 0;
  long count = 0;
  int rc = 0;
  if (!file || !origins || !runs) {
    failf("cannot allocate/open delivery verification state");
    rc = -1;
    goto done;
  }
  while (getline(&line, &cap, file) >= 0) {
    JsonRef root, delivery;
    char origin[RT_SMALL] = "";
    char run_id[RT_SMALL] = "";
    char delivery_id[RT_SMALL] = "";
    char delivery_prefix[RT_MED];
    long origin_number = 0;
    long run_number = 0;
    char extra;
    count++;
    if (json_ref_top_object(line, &root) != 0 ||
        json_ref_object_get_string(&root, "run_id", run_id, sizeof(run_id)) != 0 ||
        json_ref_object_get_object(&root, "delivery", &delivery) != 0 ||
        json_ref_object_get_string(&delivery, "origin_event_id", origin,
                                   sizeof(origin)) != 0 ||
        json_ref_object_get_string(&delivery, "delivery_id", delivery_id,
                                   sizeof(delivery_id)) != 0 ||
        sscanf(origin, "bus_%ld%c", &origin_number, &extra) != 1 ||
        sscanf(run_id, "run_%ld%c", &run_number, &extra) != 1 ||
        origin_number < 1 || origin_number > expected ||
        run_number < 1 || run_number > expected || !delivery_id[0] ||
        snprintf(delivery_prefix, sizeof(delivery_prefix), "deliv_%s_", run_id) >=
            (int)sizeof(delivery_prefix) ||
        strncmp(delivery_id, delivery_prefix, strlen(delivery_prefix)) != 0) {
      failf("malformed delivery line=%ld", count);
      rc = -1;
      break;
    }
    if (origins[origin_number] || runs[run_number]) {
      failf("duplicate delivery origin=%s run_id=%s", origin, run_id);
      rc = -1;
      break;
    }
    origins[origin_number] = 1;
    runs[run_number] = 1;
  }
  if (count != expected) {
    failf("delivery count=%ld expected=%ld", count, expected);
    rc = -1;
  }
done:
  if (file) fclose(file);
  free(line);
  free(origins);
  free(runs);
  return rc;
}

static int verify_runs(long expected, int contexts) {
  unsigned char *seen_context = calloc((size_t)contexts, 1U);
  int seen_count = 0;
  int rc = 0;
  if (!seen_context) return -1;
  for (long i = 1; i <= expected; ++i) {
    char path[PATH_MAX];
    char *text = NULL;
    JsonRef root;
    char status[RT_SMALL] = "";
    char context[RT_MED] = "";
    int context_number = -1;
    char extra;
    snprintf(path, sizeof(path), "workspace/runs/run_%03ld/runstate.json", i);
    if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text ||
        json_ref_top_object(text, &root) != 0 ||
        json_ref_object_get_string(&root, "status", status, sizeof(status)) != 0 ||
        json_ref_object_get_string(&root, "context_id", context,
                                   sizeof(context)) != 0 ||
        strcmp(status, "done") != 0 ||
        sscanf(context, "chat:thrash:thrash_ctx_%d%c", &context_number, &extra) != 1 ||
        context_number < 0 || context_number >= contexts) {
      failf("bad/nonterminal runstate run=%ld status=%s context=%s",
            i, status, context);
      free(text);
      rc = -1;
      break;
    }
    if (!seen_context[context_number]) {
      seen_context[context_number] = 1;
      seen_count++;
    }
    free(text);
  }
  if (seen_count != contexts) {
    failf("runtime contexts=%d expected=%d", seen_count, contexts);
    rc = -1;
  }
  free(seen_context);
  return rc;
}

static int gateway_stats(const char *path, long *completed, long *max_runs) {
  FILE *file = fopen(path, "r");
  char *line = NULL;
  size_t cap = 0;
  int found = 0;
  if (!file) return -1;
  while (getline(&line, &cap, file) >= 0) {
    char *runs = strstr(line, "runs_completed=");
    char *peak = strstr(line, "max_runs=");
    if (strstr(line, "gateway: stats") && runs && peak) {
      *completed = strtol(runs + strlen("runs_completed="), NULL, 10);
      *max_runs = strtol(peak + strlen("max_runs="), NULL, 10);
      found = 1;
    }
  }
  free(line);
  fclose(file);
  return found ? 0 : -1;
}

static int run_active_cap(const char *gateway_binary, long cap_events) {
  const char *gateway_log = "workspace/logs/thrash_active_cap_gateway.log";
  long completed = 0;
  long max_runs = 0;
  uint64_t deadline;
  int runs = 0;
  int inbox = 0;

  printf("thrash: active-cap prefill=%ld one serialized context\n", cap_events);
  fflush(stdout);
  for (long i = 1; i <= cap_events; ++i)
    if (publish_one((uint64_t)i, 64, 1) != 0) return -1;
  if (start_gateway(gateway_binary, gateway_log, "thrash_cap") != 0) return -1;
  deadline = monotonic_ns() + 30000000000ULL;
  while (monotonic_ns() < deadline) {
    runs = count_run_directories();
    inbox = bus_inbox_count();
    if (runs == RT_MAX_ACTIVE_RUNS && inbox == cap_events - RT_MAX_ACTIVE_RUNS)
      break;
    if (!gateway_alive()) return -1;
    sleep_ns(10000000ULL);
  }
  if (runs != RT_MAX_ACTIVE_RUNS || inbox != cap_events - RT_MAX_ACTIVE_RUNS) {
    failf("active-run cap not reached runs=%d/%d inbox=%d/%ld",
          runs, RT_MAX_ACTIVE_RUNS, inbox,
          cap_events - RT_MAX_ACTIVE_RUNS);
    return -1;
  }
  if (stop_gateway() != 0) return -1;
  if (gateway_stats(gateway_log, &completed, &max_runs) != 0 ||
      max_runs != RT_MAX_ACTIVE_RUNS) {
    failf("active-run stats max_runs=%ld/%d", max_runs, RT_MAX_ACTIVE_RUNS);
    return -1;
  }
  printf("{\"active_run_cap\":%d,\"admitted\":%d,"
         "\"backpressured_inbox\":%d,\"max_active_runs\":%ld}\n",
         RT_MAX_ACTIVE_RUNS, runs, inbox, max_runs);
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv) {
  const char *mode;
  const char *gateway_binary;
  const char *gateway_log = "workspace/logs/thrash_gateway.log";
  const long duration_seconds = env_long("THRASH_DURATION_SECONDS", 600, 10, 3600);
  const long rate = env_long("THRASH_RATE", 60, 50, 1000);
  const long min_rate = env_long("THRASH_MIN_RATE", 50, 1, rate);
  const long contexts = env_long("THRASH_CONTEXTS", 64, 64, 512);
  const long cap_events = env_long("THRASH_PREFILL", 32, RT_MAX_ACTIVE_RUNS + 1, 100000);
  const long drain_timeout = env_long("THRASH_DRAIN_TIMEOUT_SECONDS", 300, 10, 1800);
  const long rss_limit_kb = env_long("THRASH_RSS_KB_MAX", 1048576, 1024, 8388608);
  const long footprint_limit_kb = env_long("THRASH_FOOTPRINT_KB_MAX", 262144, 1024, 4194304);
  const long window_seconds = 10;
  const long window_count = duration_seconds / window_seconds;
  const long paced_events = duration_seconds * rate;
  const long expected = paced_events;
  long *windows = calloc((size_t)window_count, sizeof(*windows));
  MemoryStats memory = {0};
  uint64_t start_ns;
  uint64_t end_ns;
  uint64_t next_sample;
  double minimum_window_rate = (double)rate;
  double drain_seconds = 0.0;
  long completed = 0;
  long max_runs = 0;
  int rc = 1;

  if (argc != 3 || !windows) {
    fprintf(stderr,
            "usage: fclaw_thrash_driver active-cap|load /absolute/path/to/fclaw\n");
    free(windows);
    return 2;
  }
  mode = argv[1];
  gateway_binary = argv[2];
  signal(SIGINT, signal_cleanup);
  signal(SIGTERM, signal_cleanup);

  if (strcmp(mode, "active-cap") == 0) {
    rc = run_active_cap(gateway_binary, cap_events) == 0 ? 0 : 1;
    goto done;
  }
  if (strcmp(mode, "load") != 0) {
    failf("unknown mode=%s", mode);
    rc = 2;
    goto done;
  }

  printf("thrash: load contexts=%ld\n", contexts);
  fflush(stdout);
  if (start_gateway(gateway_binary, gateway_log, "fast") != 0) goto done;
  if (sample_memory(g_gateway_pid, &memory) != 0) {
    failf("initial memory sample failed");
    goto done;
  }

  start_ns = monotonic_ns();
  end_ns = start_ns + (uint64_t)duration_seconds * 1000000000ULL;
  next_sample = start_ns + 1000000000ULL;
  for (long i = 0; i < paced_events; ++i) {
    uint64_t target = start_ns + (uint64_t)i * 1000000000ULL / (uint64_t)rate;
    uint64_t now;
    long window;
    sleep_until(target);
    if (g_interrupted) goto done;
    if (!gateway_alive()) goto done;
    if (publish_one((uint64_t)i + 1ULL, (int)contexts, 0) != 0)
      goto done;
    now = monotonic_ns();
    window = (long)((now - start_ns) / ((uint64_t)window_seconds * 1000000000ULL));
    if (window >= 0 && window < window_count) windows[window]++;
    if (now >= next_sample) {
      if (sample_memory(g_gateway_pid, &memory) != 0) {
        failf("memory sample failed during load");
        goto done;
      }
      next_sample += 1000000000ULL;
    }
    if ((i + 1) % (rate * 30) == 0) {
      printf("thrash: sustained elapsed=%lds published=%ld/%ld\n",
             (i + 1) / rate, i + 1, paced_events);
      fflush(stdout);
    }
  }
  sleep_until(end_ns);
  for (long i = 0; i < window_count; ++i) {
    double window_rate = (double)windows[i] / (double)window_seconds;
    if (window_rate < minimum_window_rate) minimum_window_rate = window_rate;
  }
  if (minimum_window_rate < (double)min_rate) {
    failf("minimum 10s publish rate=%.2f/s required=%ld/s",
          minimum_window_rate, min_rate);
    goto done;
  }
  printf("thrash: sustained clean duration=%lds target=%ld/s min_10s=%.2f/s events=%ld\n",
         duration_seconds, rate, minimum_window_rate, paced_events);
  fflush(stdout);

  if (wait_for_drain(expected, (int)drain_timeout, &memory,
                     &drain_seconds) != 0)
    goto done;
  if (verify_deliveries(expected) != 0 ||
      verify_runs(expected, (int)contexts) != 0)
    goto done;
  if (count_regular_files("workspace/bus/processed") != expected ||
      count_regular_files("workspace/bus/inbox") != 0) {
    failf("bus files do not match expected processed=%ld", expected);
    goto done;
  }
  if (memory.peak_rss_kb > (uint64_t)rss_limit_kb ||
      memory.peak_footprint_kb > (uint64_t)footprint_limit_kb) {
    failf("memory budget exceeded rss=%llu/%ldKB footprint=%llu/%ldKB",
          (unsigned long long)memory.peak_rss_kb, rss_limit_kb,
          (unsigned long long)memory.peak_footprint_kb, footprint_limit_kb);
    goto done;
  }
  if (stop_gateway() != 0) goto done;
  if (gateway_stats(gateway_log, &completed, &max_runs) != 0 ||
      completed != expected || max_runs < 1 || max_runs > RT_MAX_ACTIVE_RUNS) {
    failf("gateway stats completed=%ld/%ld max_runs=%ld (cap=%d)",
          completed, expected, max_runs, RT_MAX_ACTIVE_RUNS);
    goto done;
  }
  printf("{\"duration_seconds\":%ld,\"target_rate\":%ld,"
         "\"minimum_10s_rate\":%.2f,\"contexts\":%ld,"
         "\"events\":%ld,\"deliveries\":%ld,"
         "\"max_active_runs\":%ld,\"peak_rss_kb\":%llu,"
         "\"peak_footprint_kb\":%llu,\"memory_samples\":%llu,"
         "\"drain_seconds\":%.3f}\n",
         duration_seconds, rate, minimum_window_rate, contexts,
         expected, expected, max_runs,
         (unsigned long long)memory.peak_rss_kb,
         (unsigned long long)memory.peak_footprint_kb,
         (unsigned long long)memory.samples, drain_seconds);
  fflush(stdout);
  rc = 0;

done:
  if (g_gateway_pid > 1) (void)stop_gateway();
  free(windows);
  return rc;
}
