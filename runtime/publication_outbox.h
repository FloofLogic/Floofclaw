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

/* Reconcile every prepared record. Safe to call at startup and every intake
 * tick. A published record remains through the exact claimed result run and
 * is removed only after that run's terminal state is durable. */
int rt_publication_outbox_reconcile(void);

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
