#ifndef FLOOF_PUBLICATION_OUTBOX_H
#define FLOOF_PUBLICATION_OUTBOX_H

#include "runtime.h"

#include <stddef.h>

/* Prepare a self-contained publication record before the source truth is
 * appended. `source_event_id` must be the exact next runtime-owned event id.
 * Reconciliation publishes only after that run log contains the same id,
 * type, and payload. */
int rt_publication_outbox_prepare(const RtContext *ctx,
                                  const char *source_event_id,
                                  const char *source_type,
                                  const char *source_payload_json,
                                  const char *channel,
                                  const char *wake_type,
                                  const char *wake_payload_json,
                                  char *wake_id_out,
                                  size_t wake_id_len);

/* Reconcile every prepared record. Called at startup and by a producer just
 * after it appended a record's source. A published record remains through
 * the exact claimed result run and is removed only after that run's terminal
 * state is durable — ordinarily by rt_publication_outbox_release(), which is
 * why this full pass is not on the reactor's per-tick path. */
int rt_publication_outbox_reconcile(void);

/* The intake tick's entry. Runs a full pass only when the previous pass or a
 * release could not resolve a record; otherwise it does nothing and returns
 * 0. That one flag is this module's only remembered state. */
int rt_publication_outbox_retry_if_pending(void);

/* Bind a record to the run that claimed its wake. Intake calls this once the
 * claiming run's control state is durable, so a crash before it leaves an
 * unbound record that reconciliation still resolves by scanning. Once bound,
 * every later claim check for the record costs one runstate read instead of
 * one read per retained run. */
int rt_publication_outbox_claim(const char *wake_id, const char *run_id);

/* Remove the record whose claiming run has just reached a durable terminal
 * state. The scheduler calls this as it retires that run — the moment the
 * record becomes garbage — after confirming nothing else claims the wake. A
 * record it cannot resolve is left, and the next tick runs a full pass. */
int rt_publication_outbox_release(const char *wake_id, const char *run_id);

/* Validate a processed internal wake before claiming a run. The record must
 * still exist and must exactly bind the numeric identity, channel, type,
 * canonical payload, and committed source truth. Returns 1 for an exact
 * internal wake, 0 when no record exists, and -1 for a mismatch/error. */
int rt_publication_outbox_validate(const char *wake_id,
                                   const char *channel,
                                   const char *wake_type,
                                   const char *wake_payload_json);

/* Work-ledger eviction guard. Returns 1 while any durable publication
 * record references this exact source event, including while its result run
 * is active; 0 after terminal claim cleanup; -1 on an unreadable outbox. */
int rt_publication_outbox_source_pending(const char *source_event_id);

/* Run-retention guard. A terminal source run remains forensic truth until
 * every publication record that names it has a terminal inbound claim. */
int rt_publication_outbox_run_pending(const char *run_id);

/* Processed-envelope retention guard. Returns 1 while `wake_id` still has
 * an outbox lifecycle record, 0 after terminal cleanup, and -1 when its
 * state is unreadable (callers must conservatively keep it). */
int rt_publication_outbox_wake_pending(const char *wake_id);

#endif
