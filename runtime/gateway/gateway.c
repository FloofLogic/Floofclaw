#include "gateway.h"

#include "../runtime.h"
#include "../runtime_kernel.h"
#include "../bus/bus.h"
#include "../support/color.h"
#include "../support/fsutil.h"
#include "../support/heap_guard.h"
#include "../support/cli_output.h"
#include "../support/json.h"
#include "../support/logo.h"
#include "bus_intake_module.h"
#include "jobrunner_module.h"
#include "reactor.h"
#include "affair_watcher_module.h"
#include "janitor_module.h"
#include "runtime_module.h"
#include "status_module.h"
#include "wake_module.h"
#include "adapter.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

/* Set when run_service() has completed the daemonize fork + stdout
 * redirect to GW_LOG_PATH. Read by the janitor's gateway.log
 * rotation task — only in daemon mode does fd 1 point at
 * gateway.log, so only in daemon mode is it safe to reopen fd 1
 * to the freshly-created path after rotation. */
static int g_daemonized_to_gw_log = 0;
int gateway_is_daemonized_stdout(void) { return g_daemonized_to_gw_log; }

static void on_signal(int sig) { (void)sig; g_stop = 1; }

void gateway_install_signal_policy(void) {
  /* Socket peers disappear routinely. A closed peer must surface as EPIPE
   * at the adapter write boundary, never terminate the gateway process. */
  (void)signal(SIGPIPE, SIG_IGN);
}

static int gateway_owner_lock_try(void) {
  int fd;
  if (fs_mkdir_p(".fclaw/run") != 0) return -1;
  fd = open(GW_RUNTIME_OWNER_LOCK_PATH, O_CREAT | O_RDWR, 0644);
  if (fd < 0) return -1;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return -1;
  }
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

static void gateway_owner_unlock(int fd) {
  if (fd < 0) return;
  (void)flock(fd, LOCK_UN);
  close(fd);
}

static int channels_disabled(void) {
  const char *v = getenv("FCLAW_GATEWAY_DISABLE_CHANNELS");
  return v && *v && strcmp(v, "0") != 0;
}

/* An adapter the gateway actually built, paired with the descriptor that
 * built it so teardown never has to guess which destroy to call. */
typedef struct {
  const FcAdapter *descriptor;
  FcReactorModule *module;
} LiveAdapter;

static void destroy_adapters(LiveAdapter *live, int count) {
  int i;
  for (i = 0; i < count; ++i)
    if (live[i].module) live[i].descriptor->destroy(live[i].module);
}

/* What this deployment actually discovered, printed unconditionally at
 * startup. A capability the operator cannot see is a capability they cannot
 * reason about, and "did my adapter load?" should never require --verbose or
 * a second command. Adapters that returned NULL are listed as off, so the
 * absent ones are visible too rather than merely missing. */
static void print_discovered_adapters(const LiveAdapter *live, int count,
                                      int human) {
  size_t i;
  int j;
  if (human) printf("gateway: adapters");
  else fputs("{\"type\":\"gateway.adapters\",\"adapters\":[", stdout);
  if (fc_adapter_count == 0) {
    if (human) printf(" (none built — adapters/ is empty)\n");
    else puts("]}");
    return;
  }
  for (i = 0; i < fc_adapter_count; ++i) {
    int on = 0;
    for (j = 0; j < count; ++j)
      if (live[j].descriptor == fc_adapters[i]) on = 1;
    if (human)
      printf(" %s=%s", fc_adapters[i]->name, on ? "on" : "off");
    else {
      if (i) putchar(',');
      fputs("{\"id\":", stdout);
      fc_cli_print_json_string(stdout, fc_adapters[i]->name);
      printf(",\"enabled\":%s}", on ? "true" : "false");
    }
  }
  if (human) printf("\n"); else puts("]}");
  fflush(stdout);
}

static int tty_rows(FILE *out) {
  struct winsize ws;
  if (!out) return 24;
  if (ioctl(fileno(out), TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return (int)ws.ws_row;
  return 24;
}

static void gateway_screen_enter(FILE *out) {
  int rows;
  if (!out) return;
  rows = tty_rows(out);
  if (rows <= FCLAW_HEADER_ROWS) rows = FCLAW_HEADER_ROWS + 1;
  fputs("\033[?1049h", out);
  fputs("\033[?25l", out);
  fputs("\033[0m", out);
  fputs("\033[2J\033[H", out);
  fprintf(out, "\033[%d;%dr", FCLAW_HEADER_ROWS + 1, rows);
  fprintf(out, "\033[%d;1H", FCLAW_HEADER_ROWS + 1);
  fflush(out);
}

static void gateway_screen_leave(FILE *out) {
  if (!out) return;
  fputs("\033[r", out);
  fputs("\033[0m", out);
  fputs("\033[?25h", out);
  fputs("\033[?1049l", out);
  fflush(out);
}

typedef struct {
  int tick;
  int log_row;
  uint64_t last_frame_ms;
} LogoAnimState;

/* Minimal in-process logo animator adapter. Foreground gateway only; daemon
 * writes nothing here. Animation cadence is intentionally independent from
 * reactor polling so terminal rendering does not spam SSH/PTYs. */
#define LOGO_ANIM_FRAME_MS 100

static int logo_anim_collect_fds(FcReactorModule *a, FcPollSet *ps) { (void)a; (void)ps; return 0; }
static int logo_anim_on_fd(FcReactorModule *a, int fd, int rev, void *t) { (void)a; (void)fd; (void)rev; (void)t; return 0; }
static int logo_anim_init(FcReactorModule *a) { (void)a; return 0; }
static int logo_anim_tick(FcReactorModule *a, uint64_t now_ms) {
  LogoAnimState *s = (LogoAnimState *)a->state;
  if (!s) return 0;
  if (s->last_frame_ms && now_ms < s->last_frame_ms + LOGO_ANIM_FRAME_MS) return 0;
  s->last_frame_ms = now_ms;
  fclaw_logo_print_frame_at(stdout, 1, 1, s->tick++);
  fprintf(stdout, "\033[%d;1H", s->log_row);
  fflush(stdout);
  return 0;
}
static uint64_t logo_anim_next_deadline(FcReactorModule *a, uint64_t now_ms) {
  LogoAnimState *s = (LogoAnimState *)a->state;
  (void)now_ms;
  if (!s) return FC_NO_DEADLINE;
  /* Without a deadline, the reactor would only run the animator when
   * something else woke it; the 100ms throttle wouldn't keep 10fps,
   * it would just cap whatever cadence happens. Returning an absolute
   * timestamp wakes the reactor in time for the next frame. */
  if (!s->last_frame_ms) return 0; /* draw first frame immediately */
  return s->last_frame_ms + LOGO_ANIM_FRAME_MS;
}
static void logo_anim_shutdown(FcReactorModule *a) { (void)a; }

static FcReactorModule *make_logo_animator(LogoAnimState *state) {
  FcReactorModule *a = (FcReactorModule *)calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->name = "logo_anim";
  a->module_id = "logo-foreground";
  a->state = state; /* state lives on the caller's stack */
  a->init = logo_anim_init;
  a->collect_fds = logo_anim_collect_fds;
  a->on_fd = logo_anim_on_fd;
  a->tick = logo_anim_tick;
  a->shutdown = logo_anim_shutdown;
  a->next_deadline_ms = logo_anim_next_deadline;
  return a;
}
static void destroy_logo_animator(FcReactorModule *a) { free(a); }

static int read_pid(void) {
  char *text = NULL;
  int pid = 0;
  if (fs_read_text(GW_PID_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  pid = atoi(text);
  free(text);
  return pid;
}

static int read_loop(char *out, size_t out_len) {
  char *text = NULL;
  if (fs_read_text(GW_LOOP_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  fs_trim_newline(text);
  snprintf(out, out_len, "%s", text);
  free(text);
  return 0;
}

static int pid_alive(int pid) {
  if (pid <= 0) return 0;
  if (kill(pid, 0) == 0) return 1;
  return errno == EPERM;
}

static int gateway_running(void) {
  int pid = read_pid();
  if (pid <= 0) return 0;
  if (!pid_alive(pid)) {
    (void)unlink(GW_PID_PATH);
    (void)unlink(GW_LOOP_PATH);
    return 0;
  }
  return pid;
}

static int write_gateway_owner_files(const char *loop_name) {
  char pid_buf[32];
  if (fs_mkdir_p(".fclaw/run") != 0) return -1;
  snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
  if (fs_write_text(GW_PID_PATH, pid_buf) != 0) return -1;
  if (fs_write_text(GW_LOOP_PATH, loop_name ? loop_name : rt_default_loop()) != 0) return -1;
  return 0;
}

static void clear_gateway_owner_files(int pid) {
  int current = read_pid();
  if (current > 0 && current != pid) return;
  (void)unlink(GW_PID_PATH);
  (void)unlink(GW_LOOP_PATH);
}

int gateway_running_pid(void) {
  int pid = gateway_running();
  if (pid > 0) return pid;
  {
    char status_json[FC_STATUS_JSON_CAP];
    JsonRef root, gateway;
    long long socket_pid = 0;
    if (fc_status_query_socket(status_json, sizeof(status_json)) == 0 &&
        json_ref_first_object(status_json, &root) == 0 &&
        json_ref_object_get_object(&root, "gateway", &gateway) == 0 &&
        json_ref_object_get_long(&gateway, "pid", &socket_pid) == 0 &&
        socket_pid > 0) {
      return (int)socket_pid;
    }
  }
  return 0;
}

int gateway_running_floop(char *out, size_t out_len) {
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (gateway_running_pid() <= 0) return 1;
  if (read_loop(out, out_len) != 0 || !out[0]) return -1;
  return 0;
}

static int latest_run_id(char *out, size_t out_len) {
  DIR *d = opendir("workspace/runs");
  struct dirent *e;
  int max_n = 0;
  if (!d) return -1;
  while ((e = readdir(d)) != NULL) {
    int n = 0;
    if (sscanf(e->d_name, "run_%d", &n) == 1 && n > max_n) max_n = n;
  }
  closedir(d);
  if (max_n == 0) return -1;
  snprintf(out, out_len, "run_%03d", max_n);
  return 0;
}

static void print_gateway_stats(const FcReactor *reactor, const RtScheduler *scheduler,
                                const FcHeapStats *heap_init, int human) {
  extern long long bus_diag_strdups(void);
  extern long long bus_diag_frees(void);
  FcHeapStats heap_end;
  fc_heap_snapshot(&heap_end);
  if (human) {
    printf("gateway: stats polls=%llu ticks=%llu runs_completed=%llu max_runs=%zu max_action_jobs=%zu inbox_strdups=%lld inbox_frees=%lld\n",
           (unsigned long long)reactor->total_polls,
           (unsigned long long)reactor->total_ticks,
           (unsigned long long)scheduler->runs_completed,
           scheduler->run_count_peak, scheduler->action_count_peak,
           bus_diag_strdups(), bus_diag_frees());
    printf("gateway: heap mallocs=%llu callocs=%llu reallocs=%llu strdups=%llu frees=%llu\n",
           (unsigned long long)(heap_end.mallocs  - heap_init->mallocs),
           (unsigned long long)(heap_end.callocs  - heap_init->callocs),
           (unsigned long long)(heap_end.reallocs - heap_init->reallocs),
           (unsigned long long)(heap_end.strdups  - heap_init->strdups),
           (unsigned long long)(heap_end.frees    - heap_init->frees));
  } else {
    printf("{\"type\":\"gateway.stats\",\"polls\":%llu,\"ticks\":%llu,"
           "\"runs_completed\":%llu,\"max_runs\":%zu,"
           "\"max_action_jobs\":%zu,\"inbox_strdups\":%lld,"
           "\"inbox_frees\":%lld,\"heap\":{\"mallocs\":%llu,"
           "\"callocs\":%llu,\"reallocs\":%llu,\"strdups\":%llu,"
           "\"frees\":%llu}}\n",
           (unsigned long long)reactor->total_polls,
           (unsigned long long)reactor->total_ticks,
           (unsigned long long)scheduler->runs_completed,
           scheduler->run_count_peak, scheduler->action_count_peak,
           bus_diag_strdups(), bus_diag_frees(),
           (unsigned long long)(heap_end.mallocs  - heap_init->mallocs),
           (unsigned long long)(heap_end.callocs  - heap_init->callocs),
           (unsigned long long)(heap_end.reallocs - heap_init->reallocs),
           (unsigned long long)(heap_end.strdups  - heap_init->strdups),
           (unsigned long long)(heap_end.frees    - heap_init->frees));
  }
  fflush(stdout);
}

static int run_gateway_reactor(const char *loop_name, int poll_ms,
                               LogoAnimState *logo_state, int verbose,
                               int human) {
  FcReactor reactor;
  RtScheduler *scheduler = (RtScheduler *)fc_xmalloc(sizeof(*scheduler));
  FcReactorModule *wake = NULL, *jr = NULL, *intake = NULL, *runtime = NULL;
  FcReactorModule *status = NULL, *logo_anim = NULL, *affair_watcher = NULL, *janitor = NULL;
  LiveAdapter adapters[FC_REACTOR_MAX_MODULES];
  int adapter_count = 0;
  FcHeapStats heap_init;
  if (!scheduler) return 1;
  {
    char init_err[RT_LARGE] = "";
    if (rt_scheduler_init(scheduler, loop_name, init_err, sizeof(init_err)) != 0) {
      fprintf(stderr, "gateway: failed to load loop %s: %s\n",
              loop_name, init_err[0] ? init_err : "(unknown)");
      fc_xfree(scheduler);
      return 1;
    }
  }
  if (verbose) {
    if (human)
      printf("gateway: rtrun_size=%zu max_active=%d pool_bytes=%zu\n",
             sizeof(RtRun), RT_MAX_ACTIVE_RUNS,
             sizeof(RtRun) * (size_t)RT_MAX_ACTIVE_RUNS);
    else
      printf("{\"type\":\"gateway.capacity\",\"rtrun_size\":%zu,"
             "\"max_active\":%d,\"pool_bytes\":%zu}\n",
             sizeof(RtRun), RT_MAX_ACTIVE_RUNS,
             sizeof(RtRun) * (size_t)RT_MAX_ACTIVE_RUNS);
    fflush(stdout);
  }
#ifndef FCLAW_HAVE_LIBCURL
  fprintf(stderr,
          "llm: HTTP transport disabled — libcurl was not linked. "
          "Every non-mock provider, including loopback OpenAI-compatible "
          "endpoints, will fail at the agent layer with "
          "\"libcurl not compiled in\". Rebuild after installing "
          "libcurl-devel / libcurl4-openssl-dev / brew install curl.\n");
#endif
  fc_reactor_init(&reactor);
  wake    = fc_bus_wake_module_create();
  jr      = fc_jobrunner_module_create(&scheduler->runner);
  intake  = fc_bus_intake_module_create(scheduler);
  runtime = fc_runtime_module_create(scheduler);
  status  = fc_status_module_create(scheduler, &reactor);
  affair_watcher = fc_affair_watcher_module_create(scheduler);
  janitor = fc_janitor_module_create();
  if (wake)    (void)fc_reactor_register(&reactor, wake);
  if (jr)      (void)fc_reactor_register(&reactor, jr);
  if (intake)  (void)fc_reactor_register(&reactor, intake);
  if (runtime) (void)fc_reactor_register(&reactor, runtime);
  if (status)  (void)fc_reactor_register(&reactor, status);
  if (affair_watcher) (void)fc_reactor_register(&reactor, affair_watcher);
  if (janitor) (void)fc_reactor_register(&reactor, janitor);
  if (logo_state) {
    logo_anim = make_logo_animator(logo_state);
    if (logo_anim) (void)fc_reactor_register(&reactor, logo_anim);
  }
  /* Adapters register after every internal module: reactor.h documents that
   * registration order is semantic where modules share state. Order among
   * adapters is not — each owns its own socket and its own delivery cursor —
   * so the generated registry's sorted order is free to differ from the old
   * hand-written one. */
  if (!channels_disabled()) {
    size_t i;
    int startup_error = 0;
    for (i = 0; i < fc_adapter_count && !startup_error; ++i) {
      const FcAdapter *a = fc_adapters[i];
      int err = 0;
      /* NULL is "disabled or not configured" and is not an error; the
       * adapter sets err only when it was enabled and could not start. */
      adapters[adapter_count].module = a->create(scheduler, &err);
      if (err) {
        startup_error = 1;
        if (adapters[adapter_count].module) {
          adapters[adapter_count].descriptor = a;
          adapter_count++;
        }
        break;
      }
      if (!adapters[adapter_count].module) continue;
      adapters[adapter_count].descriptor = a;
      adapter_count++;
    }
    if (startup_error) {
      destroy_adapters(adapters, adapter_count);
      if (runtime) fc_runtime_module_destroy(runtime);
      if (intake) fc_bus_intake_module_destroy(intake);
      if (jr) fc_jobrunner_module_destroy(jr);
      if (wake) fc_bus_wake_module_destroy(wake);
      if (status) fc_status_module_destroy(status);
      if (affair_watcher) fc_affair_watcher_module_destroy(affair_watcher);
      if (janitor) fc_janitor_module_destroy(janitor);
      if (logo_anim) destroy_logo_animator(logo_anim);
      rt_scheduler_destroy(scheduler);
      fc_xfree(scheduler);
      return 1;
    }
    /* Registration can only fail by running out of reactor slots. Before
     * discovery that was unreachable; now it is reachable by copying a
     * directory into adapters/, so a full reactor must name itself rather
     * than silently drop a configured adapter. */
    print_discovered_adapters(adapters, adapter_count, human);
    for (i = 0; i < (size_t)adapter_count; ++i) {
      if (fc_reactor_register(&reactor, adapters[i].module) != 0) {
        fprintf(stderr,
                "gateway: no reactor slot for the %s adapter — %d modules is "
                "the limit and the internal modules are already registered; "
                "fix: disable a channel, remove a directory from adapters/, "
                "or raise FC_REACTOR_MAX_MODULES\n",
                adapters[i].descriptor->name, FC_REACTOR_MAX_MODULES);
        fc_reactor_shutdown(&reactor);
        destroy_adapters(adapters, adapter_count);
        if (runtime) fc_runtime_module_destroy(runtime);
        if (intake) fc_bus_intake_module_destroy(intake);
        if (jr) fc_jobrunner_module_destroy(jr);
        if (wake) fc_bus_wake_module_destroy(wake);
        if (status) fc_status_module_destroy(status);
        if (affair_watcher) fc_affair_watcher_module_destroy(affair_watcher);
        if (janitor) fc_janitor_module_destroy(janitor);
        if (logo_anim) destroy_logo_animator(logo_anim);
        rt_scheduler_destroy(scheduler);
        fc_xfree(scheduler);
        return 1;
      }
      if (verbose) {
        if (human)
          printf("gateway: %s adapter registered\n", adapters[i].descriptor->name);
        else {
          fputs("{\"type\":\"gateway.adapter_registered\",\"id\":", stdout);
          fc_cli_print_json_string(stdout, adapters[i].descriptor->name);
          puts("}");
        }
      }
    }
    if (verbose) fflush(stdout);
  }
  fc_heap_snapshot(&heap_init);
  fc_reactor_run(&reactor, &g_stop, poll_ms);
  fc_reactor_shutdown(&reactor);
  destroy_adapters(adapters, adapter_count);
  if (runtime) fc_runtime_module_destroy(runtime);
  if (intake)  fc_bus_intake_module_destroy(intake);
  if (jr)      fc_jobrunner_module_destroy(jr);
  if (wake)    fc_bus_wake_module_destroy(wake);
  if (status)  fc_status_module_destroy(status);
  if (affair_watcher) fc_affair_watcher_module_destroy(affair_watcher);
  if (janitor) fc_janitor_module_destroy(janitor);
  if (logo_anim) destroy_logo_animator(logo_anim);
  print_gateway_stats(&reactor, scheduler, &heap_init, human);
  rt_scheduler_destroy(scheduler);
  fc_xfree(scheduler);
  return 0;
}

static int run_service(const char *loop_name, int poll_ms) {
  int rc;
  gateway_install_signal_policy();
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGHUP, on_signal); /* reload also stops the daemon for now */
  g_stop = 0;
  printf("gateway: started loop=%s pid=%d poll=%dms\n", loop_name, getpid(), poll_ms);
  fflush(stdout);
  rc = run_gateway_reactor(loop_name, poll_ms, NULL, 1, 1);
  printf("gateway: stopping pid=%d\n", getpid());
  fflush(stdout);
  return rc;
}

int gateway_run_foreground(const char *loop_name, int human) {
  int poll_ms = GW_DEFAULT_POLL_MS;
  const char *poll_env = getenv("FCLAW_GATEWAY_POLL_MS");
  LogoAnimState logo = {0, FCLAW_HEADER_ROWS + 1, 0};
  int colored;
  int existing;
  int owner_fd = -1;
  if (poll_env && *poll_env) poll_ms = atoi(poll_env);
  if (poll_ms <= 0) poll_ms = GW_DEFAULT_POLL_MS;
  gateway_install_signal_policy();
  existing = gateway_running_pid();
  if (existing) {
    fprintf(stderr, "gateway: already running (pid=%d). stop it first.\n", existing);
    return 1;
  }
  if (!loop_name || !*loop_name) loop_name = rt_default_loop();
  if (fs_mkdir_p("workspace/logs") != 0) {
    fprintf(stderr, "gateway: failed to create workspace/logs\n");
    return 1;
  }
  owner_fd = gateway_owner_lock_try();
  if (owner_fd < 0) {
    fprintf(stderr, "gateway: another runtime owner is active\n");
    return 1;
  }
  if (write_gateway_owner_files(loop_name) != 0) {
    fprintf(stderr, "gateway: failed to write gateway owner files\n");
    gateway_owner_unlock(owner_fd);
    return 1;
  }
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  g_stop = 0; /* arm: not stopping yet */
  colored = human && isatty(fileno(stdout));
  if (colored) {
    gateway_screen_enter(stdout);
    fclaw_logo_print_frame_at(stdout, 1, 1, logo.tick++);
    fprintf(stdout,
            "\033[%d;1H\033[2Kgateway running foreground "
            "(loop=%s, pid=%d, poll=%dms, ctrl-C to stop)",
            FCLAW_LOGO_ROWS + 2, loop_name, getpid(), poll_ms);
    fprintf(stdout, "\033[%d;1H", logo.log_row);
  } else if (human) {
    fclaw_logo_print(stdout);
    printf("\ngateway running foreground (loop=%s, pid=%d, poll=%dms, ctrl-C to stop)\n\n",
           loop_name, getpid(), poll_ms);
  } else {
    fputs("{\"type\":\"gateway.started\",\"mode\":\"foreground\",\"floop\":", stdout);
    fc_cli_print_json_string(stdout, loop_name);
    printf(",\"pid\":%d,\"poll_ms\":%d}\n", getpid(), poll_ms);
  }
  fflush(stdout);

  int rc = run_gateway_reactor(loop_name, poll_ms < 60 ? poll_ms : 60,
                               colored ? &logo : NULL, 0, human);
  if (colored) {
    gateway_screen_leave(stdout);
  }
  clear_gateway_owner_files((int)getpid());
  gateway_owner_unlock(owner_fd);
  if (human) printf("gateway: stopped\n");
  else printf("{\"type\":\"gateway.stopped\",\"pid\":%d}\n", getpid());
  fflush(stdout);
  return rc;
}

int gateway_start(const char *loop_name, int human) {
  pid_t pid;
  int poll_ms = GW_DEFAULT_POLL_MS;
  const char *poll_env = getenv("FCLAW_GATEWAY_POLL_MS");
  char loop_buf[RT_SMALL];
  int existing;
  int owner_fd = -1;
  if (poll_env && *poll_env) poll_ms = atoi(poll_env);
  if (poll_ms <= 0) poll_ms = GW_DEFAULT_POLL_MS;
  existing = gateway_running_pid();
  if (existing) {
    fprintf(stderr, "gateway: already running (pid=%d)\n", existing);
    return 1;
  }
  if (!loop_name || !*loop_name) loop_name = rt_default_loop();
  snprintf(loop_buf, sizeof(loop_buf), "%s", loop_name);
  if (fs_mkdir_p(".fclaw/run") != 0) {
    fprintf(stderr, "gateway: failed to create .fclaw/run\n");
    return 1;
  }
  if (fs_mkdir_p("workspace/logs") != 0) {
    fprintf(stderr, "gateway: failed to create workspace/logs\n");
    return 1;
  }
  owner_fd = gateway_owner_lock_try();
  if (owner_fd < 0) {
    fprintf(stderr, "gateway: another runtime owner is active\n");
    return 1;
  }
  pid = fork();
  if (pid < 0) {
    fprintf(stderr, "gateway: fork failed\n");
    gateway_owner_unlock(owner_fd);
    return 1;
  }
  if (pid > 0) {
    /* parent — wait briefly for child to write its pid file */
    for (int i = 0; i < 30; ++i) {
      if (gateway_running()) {
        if (human) {
          fclaw_logo_print(stdout);
          printf(COLOR_GREEN_BRIGHT "\ngateway started" COLOR_RESET
                 " (loop=%s, pid=%d, poll=%dms)\n", loop_buf, read_pid(), poll_ms);
        } else {
          fputs("{\"type\":\"gateway.started\",\"mode\":\"daemon\",\"floop\":", stdout);
          fc_cli_print_json_string(stdout, loop_buf);
          printf(",\"pid\":%d,\"poll_ms\":%d}\n", read_pid(), poll_ms);
        }
        close(owner_fd); /* child inherited and keeps the lock held */
        return 0;
      }
      struct timespec ts = {0, 50 * 1000000L};
      nanosleep(&ts, NULL);
    }
    fprintf(stderr, "gateway: child did not become ready\n");
    close(owner_fd);
    return 1;
  }
  /* child */
  setsid();
  {
    int log_fd;
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
    log_fd = open(GW_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
      dup2(log_fd, 1);
      dup2(log_fd, 2);
      close(log_fd);
      g_daemonized_to_gw_log = 1;
    }
  }
  if (write_gateway_owner_files(loop_buf) != 0) _exit(1);
  run_service(loop_buf, poll_ms);
  clear_gateway_owner_files((int)getpid());
  gateway_owner_unlock(owner_fd);
  _exit(0);
}

int gateway_status(int human) {
  int pid = gateway_running_pid();
  char loop[RT_SMALL] = "";
  char run_id[RT_SMALL] = "";
  char status_json[FC_STATUS_JSON_CAP];
  if (!human && fc_status_query_socket(status_json, sizeof(status_json)) == 0) {
    char compact[FC_STATUS_JSON_CAP];
    if (json_compact(status_json, compact, sizeof(compact)) != 0) {
      fc_cli_print_json_error("gateway.status", "invalid_status",
                              "gateway returned invalid status JSON");
      return 1;
    }
    printf("%s\n", compact);
    return 0;
  }
  if (!pid) {
    if (!human) {
      printf("{\"gateway\":{\"running\":false,\"pid\":0},"
             "\"runs\":{\"active\":0,\"waiting_action_slot\":0,"
             "\"completed\":0,\"failed\":0},"
             "\"jobs\":{\"active\":0,\"queued\":0},"
             "\"bus\":{\"inbox_count\":%d}}\n", bus_inbox_count());
      return 1;
    }
    printf(COLOR_RED_BRIGHT "gateway:" COLOR_RESET " not running\n");
    return 1;
  }
  if (!human) {
    fprintf(stderr, "gateway: status socket unavailable\n");
    return 1;
  }
  (void)read_loop(loop, sizeof(loop));
  if (latest_run_id(run_id, sizeof(run_id)) != 0) snprintf(run_id, sizeof(run_id), "(none)");
  printf(COLOR_GREEN_BRIGHT "gateway:" COLOR_RESET " running\n");
  printf(COLOR_CYAN "  pid:" COLOR_RESET "      %d\n", pid);
  printf(COLOR_CYAN "  floop:" COLOR_RESET "    %s\n", loop[0] ? loop : "(unknown)");
  printf(COLOR_CYAN "  last_run:" COLOR_RESET " %s\n", run_id);
  return 0;
}

int gateway_stop(int human) {
  int pid = gateway_running_pid();
  if (!pid) {
    fprintf(stderr, "gateway: not running\n");
    return 1;
  }
  if (kill(pid, SIGTERM) != 0) {
    fprintf(stderr, "gateway: failed to signal pid %d\n", pid);
    return 1;
  }
  for (int i = 0; i < 60; ++i) {
    struct timespec ts = {0, 50 * 1000000L};
    nanosleep(&ts, NULL);
    if (!pid_alive(pid)) {
      clear_gateway_owner_files(pid);
      if (human) {
        fclaw_logo_print(stdout);
        printf(COLOR_GREEN_BRIGHT "\ngateway stopped" COLOR_RESET
               " (pid=%d)\n", pid);
      } else {
        printf("{\"type\":\"gateway.stopped\",\"pid\":%d}\n", pid);
      }
      return 0;
    }
  }
  fprintf(stderr, "gateway: pid %d did not exit in 3s\n", pid);
  return 1;
}

int gateway_reload(const char *loop_name, int human) {
  if (gateway_stop(human) != 0) {
    fprintf(stderr, "gateway: stop failed during reload\n");
    return 1;
  }
  return gateway_start(loop_name, human);
}
