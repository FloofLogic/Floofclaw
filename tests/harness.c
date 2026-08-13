#include "harness.h"
#include "test_support.h"

#include "../runtime/runtime.h"
#include "../runtime/runtime_kernel.h"
#include "../runtime/bus/bus.h"
#include "../runtime/channel/channel.h"
#include "../runtime/gateway/reactor.h"
#include "../runtime/gateway/status_module.h"
#include "../runtime/support/fsutil.h"
#include "../runtime/support/heap_guard.h"
#include "../runtime/support/json.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern long long bus_diag_strdups(void);
extern long long bus_diag_frees(void);

/* runtime/main.c provides rt_exe_path() in production. The test binary
 * doesn't link main.c (it has its own main() in test_runner.c), so we
 * provide it here. Points at the production fclaw binary because the
 * runtime spawns `fclaw internal agent-llm` children for LLM agents. */
const char *rt_exe_path(void) { return "./bin/fclaw"; }

/* ===== high-level: synchronous run ============================== */

int harness_run(const char *channel,
                const char *loop,
                const char *user_text,
                char *response_out, size_t response_len,
                char *run_id_out, size_t run_id_len) {
  if (!channel) channel = "tests";
  return channel_synchronous(channel, loop, user_text,
                             response_out, response_len,
                             run_id_out, run_id_len);
}

/* ===== mid-level: gateway emulation ============================= */

struct HarnessGateway {
  RtScheduler scheduler;
  FcHeapStats heap_init;
  uint64_t total_ticks;
};

HarnessGateway *harness_gateway_init(const char *loop_name) {
  char err[256];
  HarnessGateway *g = calloc(1, sizeof(*g));
  if (!g) return NULL;
  if (rt_scheduler_init(&g->scheduler, loop_name, err, sizeof(err)) != 0) {
    fprintf(stderr, "harness_gateway_init: %s\n", err);
    free(g);
    return NULL;
  }
  fc_heap_snapshot(&g->heap_init);
  return g;
}

int harness_gateway_publish(HarnessGateway *g,
                            const char *channel, const char *text) {
  char escaped[RT_LARGE];
  char payload[RT_LARGE + 64];
  if (!channel || !text) return -1;
  if (json_escape(text, escaped, sizeof(escaped)) != 0) return -1;
  snprintf(payload, sizeof(payload), "{\"text\":\"%s\"}", escaped);
  return harness_gateway_publish_payload(g, channel, payload);
}

int harness_gateway_publish_payload(HarnessGateway *g,
                                    const char *channel,
                                    const char *payload_json) {
  char id[64];
  (void)g;
  if (!channel || !payload_json) return -1;
  if (bus_publish(channel, "user_message", payload_json,
                  id, sizeof(id)) != 0)
    return -1;
  return 0;
}

void harness_gateway_drive_one_tick(HarnessGateway *g) {
  if (!g) return;
  rt_scheduler_tick(&g->scheduler, fc_now_ms());
  g->total_ticks++;
}

int harness_gateway_drive_until_completed(HarnessGateway *g,
                                          int expected_completed,
                                          uint64_t timeout_ms) {
  if (!g) return -1;
  uint64_t deadline = fc_now_ms() + timeout_ms;
  while (fc_now_ms() < deadline) {
    rt_scheduler_tick(&g->scheduler, fc_now_ms());
    g->total_ticks++;
    if ((int)g->scheduler.runs_completed >= expected_completed) return 0;
    usleep(100);
  }
  return -1;
}

int harness_gateway_drive_until_run_done(HarnessGateway *g,
                                         const char *run_id,
                                         uint64_t timeout_ms) {
  if (!g || !run_id) return -1;
  uint64_t deadline = fc_now_ms() + timeout_ms;
  while (fc_now_ms() < deadline) {
    rt_scheduler_tick(&g->scheduler, fc_now_ms());
    g->total_ticks++;
    if (g->scheduler.last_retired_run_id[0] &&
        strcmp(g->scheduler.last_retired_run_id, run_id) == 0) {
      return 0;
    }
    /* The last_retired_run_id only holds the most recent — for runs
     * that completed earlier, scan the active pool: if not in pool
     * and event log shows run_done/run_failed, we count it as done. */
    char log_path[256];
    snprintf(log_path, sizeof(log_path),
             "workspace/runs/%s/event_log.jsonl", run_id);
    char *log = NULL;
    if (test_read_file(log_path, &log) == 0 && log) {
      int done = (strstr(log, "\"type\":\"run_done\"") != NULL ||
                  strstr(log, "\"type\":\"run_failed\"") != NULL);
      free(log);
      if (done) return 0;
    }
    usleep(100);
  }
  return -1;
}

char *harness_gateway_status_json(const HarnessGateway *g) {
  if (!g) return NULL;
  char *buf = malloc(4096);
  if (!buf) return NULL;
  if (fc_status_build_json(&g->scheduler, buf, 4096) != 0) {
    free(buf);
    return NULL;
  }
  return buf;
}

const RtRun *harness_gateway_find_run(const HarnessGateway *g, const char *run_id) {
  if (!g || !run_id) return NULL;
  for (size_t i = 0; i < g->scheduler.run_count; ++i) {
    const RtRun *r = g->scheduler.runs[i];
    if (r && strcmp(r->ctx.run_id, run_id) == 0) return r;
  }
  return NULL;
}

char *harness_gateway_stats_line(const HarnessGateway *g) {
  if (!g) return NULL;
  FcHeapStats end;
  fc_heap_snapshot(&end);
  char *buf = malloc(1024);
  if (!buf) return NULL;
  snprintf(buf, 1024,
           "gateway: stats polls=0 ticks=%llu runs_completed=%llu "
           "max_runs=%zu max_action_jobs=%zu inbox_strdups=%lld inbox_frees=%lld\n"
           "gateway: heap mallocs=%llu callocs=%llu reallocs=%llu strdups=%llu frees=%llu\n",
           (unsigned long long)g->total_ticks,
           (unsigned long long)g->scheduler.runs_completed,
           g->scheduler.run_count_peak, g->scheduler.action_count_peak,
           bus_diag_strdups(), bus_diag_frees(),
           (unsigned long long)(end.mallocs  - g->heap_init.mallocs),
           (unsigned long long)(end.callocs  - g->heap_init.callocs),
           (unsigned long long)(end.reallocs - g->heap_init.reallocs),
           (unsigned long long)(end.strdups  - g->heap_init.strdups),
           (unsigned long long)(end.frees    - g->heap_init.frees));
  return buf;
}

int harness_gateway_fast_loop(int events, char **out_log) {
  HarnessGateway *g = harness_gateway_init("fast");
  if (!g) return -1;
  for (int i = 1; i <= events; ++i) {
    char text[64];
    snprintf(text, sizeof(text), "event_%d", i);
    if (harness_gateway_publish(g, "tests", text) != 0) {
      harness_gateway_close(g);
      return -1;
    }
  }
  /* Drain inbox in a tight loop until everything completes. */
  int rc = (events == 0)
    ? 0  /* No events expected; one tick is enough to flush rejects. */
    : harness_gateway_drive_until_completed(g, events, 6000);
  if (events == 0) harness_gateway_drive_one_tick(g);
  if (out_log) *out_log = harness_gateway_stats_line(g);
  harness_gateway_close(g);
  return rc;
}

void harness_gateway_close(HarnessGateway *g) {
  if (!g) return;
  rt_scheduler_destroy(&g->scheduler);
  free(g);
}

/* ===== CLI subcommands in-process =============================== */

/* Capture stdout AND stderr of `fn(argc, argv)` into a malloc'd
 * string — same `2>&1` shape that test_run_fclaw used to produce. */
static int capture_stdout(int (*fn)(int, char **), int argc, char **argv,
                          char **stdout_out, int *exit_code) {
  char tmp_path[] = "/tmp/fclaw_test_capture_XXXXXX";
  int tmp_fd = mkstemp(tmp_path);
  if (tmp_fd < 0) return -1;
  fflush(stdout); fflush(stderr);
  int saved_out = dup(STDOUT_FILENO);
  int saved_err = dup(STDERR_FILENO);
  if (saved_out < 0 || saved_err < 0) {
    if (saved_out >= 0) close(saved_out);
    if (saved_err >= 0) close(saved_err);
    close(tmp_fd); unlink(tmp_path); return -1;
  }
  if (dup2(tmp_fd, STDOUT_FILENO) < 0 || dup2(tmp_fd, STDERR_FILENO) < 0) {
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out); close(saved_err); close(tmp_fd); unlink(tmp_path);
    return -1;
  }
  int rc = fn(argc, argv);
  fflush(stdout); fflush(stderr);
  dup2(saved_out, STDOUT_FILENO);
  dup2(saved_err, STDERR_FILENO);
  close(saved_out); close(saved_err); close(tmp_fd);
  char *buf = NULL;
  if (fs_read_text(tmp_path, &buf, FS_READ_TEXT_DEFAULT_CAP) != 0) buf = strdup("");
  unlink(tmp_path);
  if (stdout_out) *stdout_out = buf; else free(buf);
  if (exit_code) *exit_code = rc;
  return 0;
}

int harness_cli_view(int argc, char **argv, char **stdout_out, int *exit_code) {
  return capture_stdout(rt_view_main, argc, argv, stdout_out, exit_code);
}

int harness_cli_cacheview(int argc, char **argv, char **stdout_out, int *exit_code) {
  return capture_stdout(rt_cacheview_main, argc, argv, stdout_out, exit_code);
}

int harness_cli_tools(int argc, char **argv, char **stdout_out, int *exit_code) {
  return capture_stdout(rt_tools_main, argc, argv, stdout_out, exit_code);
}

int harness_cli_channel(int argc, char **argv, char **stdout_out, int *exit_code) {
  return capture_stdout(rt_channel_main, argc, argv, stdout_out, exit_code);
}

int harness_cli_usage(int argc, char **argv, char **stdout_out, int *exit_code) {
  return capture_stdout(rt_usage_main, argc, argv, stdout_out, exit_code);
}

int harness_cli_gateway(int argc, char **argv, char **stdout_out, int *exit_code) {
  return capture_stdout(rt_gateway_main, argc, argv, stdout_out, exit_code);
}

int harness_cli_replay(const char *event_log_path, char **state_out) {
  if (!event_log_path || !state_out) return 1;
  char *events = NULL;
  if (fs_read_text(event_log_path, &events, FS_READ_TEXT_DEFAULT_CAP) != 0 || !events) return 1;
  char *state = malloc(RT_XL);
  if (!state) { free(events); return 1; }
  int rc = rt_reduce_event_log_text(events, state, RT_XL);
  free(events);
  if (rc != 0) { free(state); return 1; }
  *state_out = state;
  return 0;
}

int harness_cli_bus_publish(const char *channel, const char *text) {
  char id[64];
  char escaped[RT_LARGE];
  char payload[RT_LARGE + 64];
  if (!channel || !text) return -1;
  if (json_escape(text, escaped, sizeof(escaped)) != 0) return -1;
  snprintf(payload, sizeof(payload), "{\"text\":\"%s\"}", escaped);
  if (bus_publish(channel, "user_message", payload, id, sizeof(id)) != 0) return -1;
  return 0;
}

/* ===== artifacts ================================================ */

char *harness_read_event_log(const char *run_id) {
  char path[256];
  char *buf = NULL;
  snprintf(path, sizeof(path), "workspace/runs/%s/event_log.jsonl", run_id);
  if (test_read_file(path, &buf) != 0) return NULL;
  return buf;
}

char *harness_read_runstate(const char *run_id) {
  char path[256];
  char *buf = NULL;
  snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json", run_id);
  if (test_read_file(path, &buf) != 0) return NULL;
  return buf;
}
