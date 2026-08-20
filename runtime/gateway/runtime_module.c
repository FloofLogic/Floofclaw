#include "runtime_module.h"
#include "../runtime.h"
#include "../support/timing.h"

#include <stdlib.h>

static int rm_init(FcReactorModule *m) { (void)m; return 0; }

static int rm_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  (void)m; (void)ps;
  return 0;
}

static int rm_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  (void)m; (void)fd; (void)events; (void)tag;
  return 0;
}

static int rm_tick(FcReactorModule *m, uint64_t now_ms) {
  static uint64_t next_narration_retry_ms;
  RtScheduler *s = (RtScheduler *)m->state;
  if (!s) return 0;
  if (now_ms >= next_narration_retry_ms) {
    (void)rt_narrate_flush_pending();
    next_narration_retry_ms = now_ms + 250U;
  }
  rt_scheduler_advance_and_retire(s, now_ms);
  return 0;
}

static void rm_shutdown(FcReactorModule *m) {
  RtScheduler *s = (RtScheduler *)m->state;
  if (!s) return;
  rt_scheduler_shutdown(s, 3000 /* ms grace */);
}

static uint64_t rm_next_deadline(FcReactorModule *m, uint64_t now_ms) {
  RtScheduler *s = (RtScheduler *)m->state;
  RtJob *j;
  size_t i;
  if (!s) return FC_NO_DEADLINE;
  /* Any ready run -> consume immediately. */
  for (i = 0; i < s->run_count; ++i) {
    if (s->runs[i] && s->runs[i]->state == RT_RUN_READY) return now_ms;
  }
  /* Any finished/failed job sitting in the runner -> pump immediately. */
  for (j = s->runner.jobs; j; j = j->next) {
    if (j->state == JOB_FINISHED || j->state == JOB_FAILED) return now_ms;
  }
  /* Earliest managed-operation completion deadline -> wake when due. */
  {
    long long due = rt_operation_earliest_deadline_ms();
    if (due > 0) {
      uint64_t stable_wall_ms = timing_stable_wall_from_monotonic(now_ms);
      uint64_t delay;
      if ((uint64_t)due <= stable_wall_ms) return now_ms;
      delay = (uint64_t)due - stable_wall_ms;
      return UINT64_MAX - now_ms < delay ? UINT64_MAX : now_ms + delay;
    }
  }
  return FC_NO_DEADLINE;
}

FcReactorModule *fc_runtime_module_create(RtScheduler *scheduler) {
  FcReactorModule *m;
  if (!scheduler) return NULL;
  m = (FcReactorModule *)calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->name = "runtime";
  m->module_id = "runtime-main";
  m->state = scheduler;
  m->init = rm_init;
  m->collect_fds = rm_collect_fds;
  m->on_fd = rm_on_fd;
  m->tick = rm_tick;
  m->shutdown = rm_shutdown;
  m->next_deadline_ms = rm_next_deadline;
  return m;
}

void fc_runtime_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m);
}
