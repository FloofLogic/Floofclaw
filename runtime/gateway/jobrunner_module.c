#include "jobrunner_module.h"

#include <stdlib.h>

static int jr_init(FcReactorModule *m) { (void)m; return 0; }

static int jr_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  RtJobRunner *r = (RtJobRunner *)m->state;
  if (!r) return 0;
  return rt_jobrunner_collect_fds(r, ps, m);
}

static int jr_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  RtJobRunner *r = (RtJobRunner *)m->state;
  if (!r) return 0;
  return rt_jobrunner_on_fd(r, fd, events, tag);
}

static int jr_tick(FcReactorModule *m, uint64_t now_ms) {
  RtJobRunner *r = (RtJobRunner *)m->state;
  if (!r) return 0;
  return rt_jobrunner_tick(r, now_ms);
}

static void jr_shutdown(FcReactorModule *m) {
  /* The runner is owned by gateway / scheduler; don't shutdown here. */
  (void)m;
}

static uint64_t jr_next_deadline(FcReactorModule *m, uint64_t now_ms) {
  RtJobRunner *r = (RtJobRunner *)m->state;
  RtJob *j;
  uint64_t earliest = FC_NO_DEADLINE;
  (void)now_ms;
  if (!r) return FC_NO_DEADLINE;
  for (j = r->jobs; j; j = j->next) {
    if (j->state != JOB_RUNNING) continue;
    if (j->deadline_ms > 0 && j->deadline_ms < earliest) earliest = j->deadline_ms;
    if (j->sigterm_at_ms != 0) {
      uint64_t kill_at = j->sigterm_at_ms + r->kill_grace_ms;
      if (kill_at < earliest) earliest = kill_at;
    }
  }
  return earliest;
}

FcReactorModule *fc_jobrunner_module_create(RtJobRunner *runner) {
  FcReactorModule *m;
  if (!runner) return NULL;
  m = (FcReactorModule *)calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->name = "jobrunner";
  m->module_id = "jobrunner-main";
  m->state = runner;
  m->init = jr_init;
  m->collect_fds = jr_collect_fds;
  m->on_fd = jr_on_fd;
  m->tick = jr_tick;
  m->shutdown = jr_shutdown;
  m->next_deadline_ms = jr_next_deadline;
  return m;
}

void fc_jobrunner_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m);
}
