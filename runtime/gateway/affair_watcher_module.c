#include "affair_watcher_module.h"

#include "../bus/bus.h"
#include "../runtime.h"
#include "../runtime_kernel.h"
#include "../support/json.h"
#include "../support/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tick every ~5s. Coarse on purpose: affair triggers are minutes-to-days
 * granularity; checking more often just costs more file reads. */
#define AFFAIR_WATCHER_TICK_MS    5000

typedef struct {
  RtScheduler *scheduler;
  uint64_t     next_tick_ms;
} AffairWatcherState;

static int context_has_active_run(RtScheduler *s, const char *context_id) {
  size_t i;
  if (!s || !context_id || !*context_id) return 0;
  for (i = 0; i < s->run_count; ++i) {
    const RtRun *r = s->runs[i];
    if (!r) continue;
    if (r->state == RT_RUN_DONE || r->state == RT_RUN_FAILED) continue;
    if (strcmp(r->ctx.context_id, context_id) == 0) return 1;
  }
  return 0;
}

static void publish_review(const char *affair_id, const char *context_id) {
  char manage[RT_MED], synth[RT_LARGE];
  char escaped_text[RT_LARGE], escaped_affair[RT_MED], escaped_adapter[RT_MED];
  char payload[RT_LARGE + 256], id[BUS_ID_MAX];
  char channel[RT_SMALL] = "", adapter[RT_SMALL] = "";
  const char *channel_use;
  if (rt_affair_get_manage(affair_id, manage, sizeof(manage), NULL, 0) != 0)
    return;
  /* context_id is "chat:<channel>" or "chat:<channel>:<adapter>". The
   * bus envelope carries the channel at the top level and the adapter
   * in the payload, so intake reconstructs the affair's EXACT context —
   * otherwise the review run lands in a different context and every
   * affair mutation it makes is rejected by the reducer. */
  if (context_id && strncmp(context_id, "chat:", 5) == 0) {
    const char *p = context_id + 5;
    size_t n = 0;
    while (p[n] && p[n] != ':' && n + 1 < sizeof(channel)) n++;
    memcpy(channel, p, n);
    channel[n] = '\0';
    if (p[n] == ':' && p[n + 1])
      snprintf(adapter, sizeof(adapter), "%s", p + n + 1);
  }
  channel_use = channel[0] ? channel : "tests";
  snprintf(synth, sizeof(synth),
           "(affair review) Check on this affair and respond: %s", manage);
  if (json_escape(synth, escaped_text, sizeof(escaped_text)) != 0) return;
  if (json_escape(affair_id, escaped_affair, sizeof(escaped_affair)) != 0) return;
  if (json_escape(adapter, escaped_adapter, sizeof(escaped_adapter)) != 0) return;
  snprintf(payload, sizeof(payload),
           "{\"text\":\"%s\",\"affair_id\":\"%s\",\"adapter_id\":\"%s\"}",
           escaped_text, escaped_affair, escaped_adapter);
  /* affair_review is its own envelope type — distinct from user_message.
   * A watcher fire is the system talking to itself; saying it was a
   * user message would lie in the event log and pollute memory. */
  if (bus_publish(channel_use, "affair_review", payload, id, sizeof(id)) == 0)
    (void)rt_narrate("review fired: %s — %.100s", affair_id, manage);
}

static int watcher_init(FcReactorModule *m) {
  AffairWatcherState *s = m ? (AffairWatcherState *)m->state : NULL;
  if (!s) return -1;
  s->next_tick_ms = 0; /* fire on first tick */
  return 0;
}

static int watcher_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  (void)m; (void)ps;
  return 0;
}

static int watcher_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  (void)m; (void)fd; (void)events; (void)tag;
  return 0;
}

static int watcher_tick(FcReactorModule *m, uint64_t now_ms) {
  AffairWatcherState *s = m ? (AffairWatcherState *)m->state : NULL;
  uint64_t stable_wall_ms;
  uint64_t actual_wall_ms;
  char due_ids[16][RT_SMALL];
  size_t due_count = 0, i;
  /* Track which contexts we already published to in this tick so two
   * due affairs in the same context don't double-publish. */
  char fired_contexts[16][RT_MED];
  size_t fired_count = 0;
  if (!s) return -1;
  if (now_ms < s->next_tick_ms) return 0;
  s->next_tick_ms = now_ms + AFFAIR_WATCHER_TICK_MS;
  stable_wall_ms = timing_stable_wall_from_monotonic(now_ms);
  actual_wall_ms = timing_wall_ms();
  if (rt_affair_list_due((long long)stable_wall_ms, due_ids,
                         sizeof(due_ids) / sizeof(due_ids[0]),
                         &due_count) != 0)
    return 0;
  for (i = 0; i < due_count; ++i) {
    char ctx_id[RT_MED];
    int already_fired = 0;
    size_t j;
    if (rt_affair_context_id_of(due_ids[i], ctx_id, sizeof(ctx_id)) != 0) continue;
    if (context_has_active_run(s->scheduler, ctx_id)) continue;
    for (j = 0; j < fired_count; ++j)
      if (strcmp(fired_contexts[j], ctx_id) == 0) { already_fired = 1; break; }
    if (already_fired) continue;
    publish_review(due_ids[i], ctx_id);
    if (fired_count < sizeof(fired_contexts) / sizeof(fired_contexts[0]))
      snprintf(fired_contexts[fired_count++], RT_MED, "%s", ctx_id);
    /* Mark the affair reviewed: clears next_review_at_ms so the watcher
     * won't fire it again until something re-arms it (the speaker via
     * affair_patch action:update, or the CLI via affair schedule). */
    {
      char payload[RT_LARGE], eaff[RT_MED];
      if (json_escape(due_ids[i], eaff, sizeof(eaff)) == 0) {
        snprintf(payload, sizeof(payload),
                 "{\"affair_id\":\"%s\",\"ms\":%llu}",
                 eaff, (unsigned long long)actual_wall_ms);
        (void)rt_affair_apply_event("affair_reviewed", payload);
      }
    }
  }
  return 0;
}

static uint64_t watcher_next_deadline_ms(FcReactorModule *m, uint64_t now_ms) {
  AffairWatcherState *s = m ? (AffairWatcherState *)m->state : NULL;
  (void)now_ms;
  if (!s) return FC_NO_DEADLINE;
  /* The reactor expects an ABSOLUTE ms timestamp (or 0 / <=now_ms for
   * "immediate, don't sleep"). next_tick_ms is already absolute. The
   * old code subtracted now_ms and returned a relative value, which
   * the reactor compared as if it were absolute — that always lost
   * (a relative 15ms is "less than" any absolute timestamp) and
   * collapsed the poll timeout to zero, burning a CPU core. */
  return s->next_tick_ms;
}

static void watcher_shutdown(FcReactorModule *m) {
  (void)m;
}

FcReactorModule *fc_affair_watcher_module_create(RtScheduler *scheduler) {
  FcReactorModule *m = (FcReactorModule *)calloc(1, sizeof(*m));
  AffairWatcherState *s = (AffairWatcherState *)calloc(1, sizeof(*s));
  if (!m || !s) { free(m); free(s); return NULL; }
  s->scheduler = scheduler;
  m->name = "affair_watcher";
  m->module_id = "affair-watcher-main";
  m->state = s;
  m->init = watcher_init;
  m->collect_fds = watcher_collect_fds;
  m->on_fd = watcher_on_fd;
  m->tick = watcher_tick;
  m->next_deadline_ms = watcher_next_deadline_ms;
  m->shutdown = watcher_shutdown;
  return m;
}

void fc_affair_watcher_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m->state);
  free(m);
}
