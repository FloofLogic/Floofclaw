#include "status_module.h"

#include "../bus/bus.h"
#include "gateway.h"
#include "../support/fsutil.h"
#include "../support/json.h"
#include "../version.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
  RtScheduler *scheduler;
  const FcReactor *reactor;
  int listen_fd;
} StatusState;

static int count_waiting_action_slot(const RtScheduler *s) {
  int n = 0;
  if (!s) return 0;
  for (size_t i = 0; i < s->run_count; ++i) {
    if (s->runs[i] && s->runs[i]->state == RT_RUN_WAITING_ACTION_SLOT) n++;
  }
  return n;
}

static int appendf(char **cursor, size_t *remaining, const char *fmt, ...) {
  va_list ap;
  int n;
  if (!cursor || !*cursor || !remaining || *remaining == 0) return -1;
  va_start(ap, fmt);
  n = vsnprintf(*cursor, *remaining, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= *remaining) return -1;
  *cursor += (size_t)n;
  *remaining -= (size_t)n;
  return 0;
}

int fc_status_build_json_with_reactor(const RtScheduler *scheduler,
                                      const FcReactor *reactor,
                                      char *out, size_t out_len) {
  int inbox = bus_inbox_count();
  int active_jobs = scheduler ? rt_jobrunner_count_active_all(&scheduler->runner) : 0;
  int active_runs = scheduler ? (int)scheduler->run_count : 0;
  int waiting_action = count_waiting_action_slot(scheduler);
  unsigned long long completed = scheduler ? (unsigned long long)scheduler->runs_succeeded : 0;
  unsigned long long failed = scheduler ? (unsigned long long)scheduler->runs_failed : 0;
  char *cursor = out;
  size_t remaining = out_len;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (appendf(&cursor, &remaining,
              "{\n"
              "  \"gateway\": {\n"
              "    \"running\": true,\n"
              "    \"pid\": %d\n"
              "  },\n"
              "  \"runs\": {\n"
              "    \"active\": %d,\n"
              "    \"waiting_action_slot\": %d,\n"
              "    \"completed\": %llu,\n"
              "    \"failed\": %llu\n"
              "  },\n"
              "  \"jobs\": {\n"
              "    \"active\": %d,\n"
              "    \"queued\": 0\n"
              "  },\n"
              "  \"bus\": {\n"
              "    \"inbox_count\": %d\n"
              "  },\n"
              "  \"reactor\": {\n"
              "    \"module_errors\": %llu,\n"
              "    \"slow_ticks\": %llu,\n"
              "    \"poll_overflows\": %llu,\n"
              "    \"modules\": [",
              getpid(), active_runs, waiting_action, completed, failed,
              active_jobs, inbox,
              (unsigned long long)fc_reactor_total_module_errors(reactor),
              (unsigned long long)fc_reactor_total_slow_ticks(reactor),
              (unsigned long long)fc_reactor_poll_overflows(reactor)) != 0)
    return -1;

  if (reactor) {
    for (size_t i = 0; i < reactor->module_count; ++i) {
      const FcReactorModule *module = reactor->modules[i];
      const FcReactorModuleStats *stats = &reactor->module_stats[i];
      char name[128];
      char module_id[128];
      if (json_escape(module && module->name ? module->name : "",
                      name, sizeof(name)) != 0 ||
          json_escape(module && module->module_id ? module->module_id : "",
                      module_id, sizeof(module_id)) != 0)
        return -1;
      if (appendf(&cursor, &remaining,
                  "%s\n      {\"name\":\"%s\",\"module_id\":\"%s\","
                  "\"errors\":%llu,\"collect_fds_errors\":%llu,"
                  "\"on_fd_errors\":%llu,\"tick_errors\":%llu,"
                  "\"slow_ticks\":%llu,\"last_tick_ms\":%llu,"
                  "\"max_tick_ms\":%llu}",
                  i == 0 ? "" : ",", name, module_id,
                  (unsigned long long)stats->callback_errors,
                  (unsigned long long)stats->collect_fds_errors,
                  (unsigned long long)stats->on_fd_errors,
                  (unsigned long long)stats->tick_errors,
                  (unsigned long long)stats->slow_ticks,
                  (unsigned long long)stats->last_tick_ms,
                  (unsigned long long)stats->max_tick_ms) != 0)
        return -1;
    }
  }
  return appendf(&cursor, &remaining,
                 "%s    ]\n"
                 "  }\n"
                 "}\n",
                 reactor && reactor->module_count > 0 ? "\n" : "") == 0
             ? 0 : -1;
}

int fc_status_build_json(const RtScheduler *scheduler, char *out,
                         size_t out_len) {
  return fc_status_build_json_with_reactor(scheduler, NULL, out, out_len);
}

int fc_local_health_build_json(const RtScheduler *scheduler,
                               char *out, size_t out_len) {
  char floop[RT_MED] = "";
  int active_runs = scheduler ? (int)scheduler->run_count : 0;
  int active_jobs = scheduler
      ? rt_jobrunner_count_active_all(&scheduler->runner) : 0;
#ifndef FCLAW_BUILD_REVISION
#define FCLAW_BUILD_REVISION "unknown"
#endif
  if (!out || out_len == 0) return -1;
  if (scheduler &&
      json_escape(scheduler->loop_name, floop, sizeof(floop)) != 0)
    return -1;
  return snprintf(
             out, out_len,
             "{\"schema_version\":1,\"ready\":true,\"running\":true,"
             "\"pid\":%d,\"floop\":\"%s\",\"api_version\":1,"
             "\"chat_protocol\":\"FloofClawWS/1.0\","
             "\"runs\":{\"active\":%d},\"jobs\":{\"active\":%d},"
             "\"build\":{\"version\":\"%s\",\"revision\":\"%s\"},"
             "\"capabilities\":[\"chat_stream\",\"resume\",\"cancel\","
             "\"pulse\",\"usage\"]}\n",
             getpid(), floop, active_runs, active_jobs,
             FCLAW_VERSION, FCLAW_BUILD_REVISION) >= (int)out_len
             ? -1 : 0;
}

static int sm_init(FcReactorModule *m) {
  StatusState *s = m ? (StatusState *)m->state : NULL;
  struct sockaddr_un addr;
  int fd;
  if (!s) return -1;
  if (fs_mkdir_p(".fclaw/run") != 0) return -1;
  (void)unlink(GW_STATUS_SOCK_PATH);
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (set_nonblocking(fd) != 0) { close(fd); return -1; }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", GW_STATUS_SOCK_PATH);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
  if (listen(fd, 16) != 0) { close(fd); return -1; }
  s->listen_fd = fd;
  return 0;
}

static int sm_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  StatusState *s = m ? (StatusState *)m->state : NULL;
  if (!s || s->listen_fd < 0) return 0;
  return fc_pollset_add(ps, s->listen_fd, POLLIN, m, NULL);
}

static int sm_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  StatusState *s = m ? (StatusState *)m->state : NULL;
  (void)tag;
  if (!s || fd != s->listen_fd || !(events & POLLIN)) return 0;
  for (;;) {
    int cfd = accept(s->listen_fd, NULL, NULL);
    char json[FC_STATUS_JSON_CAP];
    if (cfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return 0;
    }
    if (fc_status_build_json_with_reactor(s->scheduler, s->reactor,
                                          json, sizeof(json)) == 0) {
      (void)write(cfd, json, strlen(json));
    }
    close(cfd);
  }
  return 0;
}

static int sm_tick(FcReactorModule *m, uint64_t now_ms) { (void)m; (void)now_ms; return 0; }

static void sm_shutdown(FcReactorModule *m) {
  StatusState *s = m ? (StatusState *)m->state : NULL;
  if (!s) return;
  if (s->listen_fd >= 0) close(s->listen_fd);
  s->listen_fd = -1;
  (void)unlink(GW_STATUS_SOCK_PATH);
}

FcReactorModule *fc_status_module_create(RtScheduler *scheduler,
                                         const FcReactor *reactor) {
  FcReactorModule *m;
  StatusState *s;
  if (!scheduler) return NULL;
  m = (FcReactorModule *)calloc(1, sizeof(*m));
  s = (StatusState *)calloc(1, sizeof(*s));
  if (!m || !s) { free(m); free(s); return NULL; }
  s->scheduler = scheduler;
  s->reactor = reactor;
  s->listen_fd = -1;
  m->name = "status";
  m->module_id = "status-main";
  m->state = s;
  m->init = sm_init;
  m->collect_fds = sm_collect_fds;
  m->on_fd = sm_on_fd;
  m->tick = sm_tick;
  m->shutdown = sm_shutdown;
  return m;
}

void fc_status_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m->state);
  free(m);
}

int fc_status_query_socket(char *out, size_t out_len) {
  int fd;
  struct sockaddr_un addr;
  size_t used = 0;
  int truncated = 0;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", GW_STATUS_SOCK_PATH);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
  for (;;) {
    char extra;
    char *target = used + 1 < out_len ? out + used : &extra;
    size_t capacity = used + 1 < out_len ? out_len - used - 1 : 1;
    ssize_t n = read(fd, target, capacity);
    if (n > 0) {
      if (target == &extra) {
        truncated = 1;
        break;
      }
      used += (size_t)n;
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    close(fd);
    return -1;
  }
  out[used] = '\0';
  close(fd);
  if (truncated) errno = ENOSPC;
  return used > 0 && !truncated ? 0 : -1;
}
