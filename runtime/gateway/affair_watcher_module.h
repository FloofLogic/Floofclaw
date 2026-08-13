#ifndef FC_AFFAIR_WATCHER_MODULE_H
#define FC_AFFAIR_WATCHER_MODULE_H

#include "reactor.h"

/* Tier-2 affair watcher. On each tick, walks the affair store and fires
 * a synthetic user_message (with affair_id stamped) for any active
 * affair whose trigger.every is due. Concurrency-safe: defers when the
 * affair's context already has an active run. Backoff: doubles the
 * effective interval (capped 4×) on noop reviews; resets on activity.
 *
 * Scan cadence is monotonic. Persisted next_review_at_ms values are epoch
 * deadlines compared through the process's stable wall/monotonic anchor;
 * tests override both domains via FCLAW_FAKE_NOW_MS.
 *
 * The module accepts a back-pointer to the scheduler so it can check
 * "is this context already running" without going through the bus.
 *
 * Pass NULL to disable the gating check (used by unit tests that drive
 * the watcher in isolation). */
struct RtScheduler;
FcReactorModule *fc_affair_watcher_module_create(struct RtScheduler *scheduler);
void fc_affair_watcher_module_destroy(FcReactorModule *m);

#endif
