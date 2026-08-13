#include "bus_intake_module.h"

#include "../bus/bus.h"
#include "../ports.h"

#include <stdlib.h>

static int bi_init(FcReactorModule *m) { (void)m; return 0; }

static int bi_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  (void)m; (void)ps;
  return 0;
}

static int bi_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  (void)m; (void)fd; (void)events; (void)tag;
  return 0;
}

static int bi_tick(FcReactorModule *m, uint64_t now_ms) {
  RtScheduler *s = (RtScheduler *)m->state;
  (void)now_ms;
  if (!s) return 0;
  rt_ports_drain_sources(s, RT_INTAKE_PER_TICK);
  return 0;
}

static void bi_shutdown(FcReactorModule *m) { (void)m; }

static uint64_t bi_next_deadline(FcReactorModule *m, uint64_t now_ms) {
  RtScheduler *s = (RtScheduler *)m->state;
  if (!s) return FC_NO_DEADLINE;
  if (s->run_count >= RT_MAX_ACTIVE_RUNS) return FC_NO_DEADLINE;
  return bus_inbox_has_pending() ? now_ms : FC_NO_DEADLINE;
}

FcReactorModule *fc_bus_intake_module_create(RtScheduler *scheduler) {
  FcReactorModule *m;
  if (!scheduler) return NULL;
  m = (FcReactorModule *)calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->name = "bus_intake";
  m->module_id = "bus_intake-main";
  m->state = scheduler;
  m->init = bi_init;
  m->collect_fds = bi_collect_fds;
  m->on_fd = bi_on_fd;
  m->tick = bi_tick;
  m->shutdown = bi_shutdown;
  m->next_deadline_ms = bi_next_deadline;
  return m;
}

void fc_bus_intake_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m);
}
