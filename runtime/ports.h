#ifndef FCLAW_PORTS_H
#define FCLAW_PORTS_H

/* Runtime boundary I/O — sources (bus inbox) and outputs (deliveries).
 * The scheduler asks ports.c to drain incoming envelopes and to emit
 * outgoing message / failure deliveries. ports.c knows about envelope
 * shapes, deliveries.jsonl, and the rejected-events log; the rest of
 * the runtime does not. */

#include "runtime_kernel.h"

#include <stddef.h>

/* Drain bounded number of inbox envelopes, allocating a run for each
 * accepted one. Replaces the old rt_scheduler_intake_some name —
 * "intake" was scheduler-named for what is really a port operation. */
void rt_ports_drain_sources(RtScheduler *s, int max);

/* One-shot variant: pop a single envelope. Returns 0 if one was
 * processed (run created or rejected and logged), 1 if the inbox was
 * empty, -1 on error. */
int  rt_ports_drain_one(RtScheduler *s);

/* Cold-start recovery boundary. Restore channel/origin fields from the
 * durable processed envelope without creating a new run. If the run died
 * before its triggering event reached a newline commit marker,
 * rt_ports_recommit_inbound() appends that event and its input task to the
 * existing run. */
int rt_ports_restore_origin(RtRun *r, const char *envelope_path);
int rt_ports_recommit_inbound(RtRun *r, const char *envelope_path);
/* Inspect the exact first behavioral event for a claimed processed
 * envelope. Returns 1 exact, 0 absent/torn, -1 conflict, -2 I/O error. */
int rt_ports_inbound_commit_state(const RtRun *r);

/* Returns 1 when the delivery ledger already contains this run/request,
 * 0 when it does not, and -1 on an unreadable ledger. */
int rt_ports_delivery_exists(const char *run_id, const char *request_id);

/* If `r` has a channel origin, build the ",\"delivery\":...\"
 * fragment for embedding in an action_succeeded event AND append the
 * matching line to deliveries.jsonl. The fragment starts with a
 * leading comma so it can be appended directly. On runs without
 * origin, the fragment is "" and no deliveries.jsonl line is
 * written. Increments r->ctx.next_delivery on success. */
void rt_ports_emit_message(RtRun *r,
                           const char *request_id,
                           const char *message_text,
                           char *out_fragment,
                           size_t out_fragment_len);

/* Append a "Run failed: <reason>" delivery line to deliveries.jsonl
 * for a run that retired in RT_RUN_FAILED. No-op for runs without a
 * channel origin. */
void rt_ports_emit_run_failure(RtRun *r);

/* Managed-operation poll pump: publish one correlated operation_poll
 * bus envelope for each due running operation whose context is idle,
 * then clear its single outstanding timer. Self-throttled; called from
 * every scheduler pass (gateway reactor and synchronous CLI alike). */
void rt_operation_pump(RtScheduler *s, uint64_t now_ms);

#endif
