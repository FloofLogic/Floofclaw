#ifndef FCLAW_BUS_H
#define FCLAW_BUS_H

#include <stddef.h>

#define BUS_ID_MAX      32
#define BUS_CHANNEL_MAX 64
#define BUS_TYPE_MAX    64
#define BUS_PATH_MAX    1024
#define BUS_PAYLOAD_MAX 16384
#define BUS_WAKE_SOCK_PATH ".fclaw/run/bus.wake.sock"

/* Envelope routing bounds. Routing is transport metadata, never product
 * payload, so it is bounded far below BUS_PAYLOAD_MAX. The ref bound
 * matches the largest reply address the delivery path will carry. */
#define BUS_ROUTING_ADAPTER_MAX 64
#define BUS_ROUTING_CONTEXT_MAX 128
#define BUS_ROUTING_REF_MAX     512
/* Wide enough that any input inside the bounds above always serializes,
 * even when every byte escapes to \u00XX. Routing must never turn a
 * validation problem into a publication failure. */
#define BUS_ROUTING_JSON_MAX    2048

/* Custom typed-event grammar. A custom kind is an opaque token compared
 * byte-for-byte by floop `event_kind:` gates; the runtime never reads
 * meaning from it. Grammar: first byte [A-Za-z], remaining bytes
 * [A-Za-z0-9_.:-], total length 1..BUS_CUSTOM_TYPE_MAX. */
#define BUS_CUSTOM_TYPE_MAX 63

/* 1 when `type` satisfies the custom-kind token grammar above. */
int bus_type_token_valid(const char *type);

/* 1 when `type` is runtime-owned and may never be forged by a custom
 * producer. Reserves whole namespaces, not just today's spellings, so a
 * future lifecycle kind cannot be claimed by a product first. */
int bus_type_is_reserved(const char *type);

/* Transport routing carried beside — never inside — the domain payload.
 * Every field is optional; NULL or "" is omitted from the envelope.
 * ref_json must already be a JSON object when present. */
typedef struct {
  const char *adapter_id;
  const char *context_id;
  const char *ref_json;
} BusRouting;

/* Publish an event envelope onto the bus. The payload_json must be a valid
 * JSON object. On success, the envelope is written to workspace/bus/inbox/
 * AND appended to workspace/logs/bus.jsonl. The assigned envelope id is
 * written to envelope_id_out (e.g. "bus_000007"). */
int bus_publish(const char *channel, const char *type, const char *payload_json,
                char *envelope_id_out, size_t envelope_id_out_len);

/* Durable-publication support. Reserve allocates one ordinary numeric bus
 * identity without publishing it. Stable publish commits exactly that
 * identity and treats an identical existing inbox/processed envelope as
 * success; a conflicting envelope is a loud error. The location bitmask is
 * 1=inbox, 2=processed. Requeue moves a processed envelope back to inbox
 * when intake crashed before claiming a run.
 *
 * Returns 0 on commit (including an absorbed identical republish),
 * BUS_PUBLISH_CONFLICT when the reserved identity already holds a
 * different envelope, and BUS_PUBLISH_FAILED for any other failure. The
 * two are distinguished so a caller can report "this id is already
 * something else" separately from "the write did not land". */
#define BUS_PUBLISH_FAILED   (-1)
#define BUS_PUBLISH_CONFLICT (-2)
int bus_reserve_envelope_id(char *out, size_t out_len);
int bus_publish_stable(const char *envelope_id,
                       const char *channel, const char *type,
                       const char *payload_json);

/* Routed variants. Identical to the plain forms except that `routing` is
 * serialized into its own top-level envelope block, leaving `payload` the
 * caller's object byte-for-byte. NULL routing produces exactly the
 * envelope the plain forms produce. Routing participates in stable-publish
 * conflict detection: same id + different routing is a conflict. */
int bus_publish_routed(const char *channel, const char *type,
                       const char *payload_json, const BusRouting *routing,
                       char *envelope_id_out, size_t envelope_id_out_len);
int bus_publish_stable_routed(const char *envelope_id,
                              const char *channel, const char *type,
                              const char *payload_json,
                              const BusRouting *routing);
int bus_envelope_locations(const char *envelope_id);
int bus_requeue_processed(const char *envelope_id);

/* Publish with bounded exponential-backoff retry. Same as bus_publish but
 * retries up to `max_attempts` on transient failures (typical: disk full
 * blip, permission race, inbox rmdir mid-write). Delays follow 10ms, 100ms,
 * 1000ms — clamped by max_attempts. Returns 0 on eventual success, -1 if
 * every attempt failed (last error narrated).
 *
 * Used from operation_driver's publish_result path where a silent
 * bus_publish failure means the LLM's next-turn envelope never arrives
 * and the conversation stalls. Callers that don't
 * care about publish durability (fire-and-forget narration, etc.) use
 * plain bus_publish. */
int bus_publish_with_retry(const char *channel, const char *type,
                           const char *payload_json,
                           char *envelope_id_out, size_t envelope_id_out_len,
                           int max_attempts);

/* Best-effort wake signal for the process that owns the bus driver. This is
 * not truth and may be lost; the file-backed inbox/log remain canonical. */
void bus_signal_wake(void);

/* Pop the oldest pending envelope from inbox. Returns 0 on success and fills
 * envelope_path_out with the path of the consumed file (already moved out of
 * the inbox). Returns 1 if the inbox is empty. -1 on error.
 *
	 * Runtime-owner path uses bus_drain_inbox below to amortize the dirent
	 * scan over many envelopes per tick. Command paths must not pop inbox
	 * envelopes directly into runs. */
int bus_pop_oldest(char *envelope_path_out, size_t path_len);

/* Per-envelope callback for bus_drain_inbox. Return 0 to keep draining,
 * nonzero to stop the batch (e.g. when the caller has run out of capacity
 * to accept more envelopes). The path is already inside processed/. */
typedef int (*BusDrainHandler)(const char *envelope_path, void *user_data);

/* Scan + sort the inbox once, then for each filename in oldest-first order
 * (up to `budget`): atomically rename the file from inbox/ to processed/,
 * then invoke `handler` with the new path. This collapses many
 * bus_pop_oldest opendir+qsort calls into one per tick. Stops when:
 *   - `budget` envelopes have been consumed, OR
 *   - the inbox is empty, OR
 *   - the handler returns nonzero.
 * Returns the count of envelopes successfully renamed and handed to the
 * handler, or -1 on hard error. Each rename is independent: a crash
 * mid-batch leaves remaining envelopes in inbox/ exactly as before
 * (preserves prior crash semantics).
 *
 * The 1024-name scan ceiling and oldest-by-filename ordering match
 * bus_pop_oldest exactly. */
int bus_drain_inbox(int budget, BusDrainHandler handler, void *user_data);

/* Return 1 if the inbox directory has any pending envelope (cheap probe — a
 * single readdir until the first non-dot entry), 0 if empty, -1 on error.
 * Used by bus_intake_module's next_deadline_ms to short-circuit poll
 * sleeps when there's an envelope ready to import. */
int bus_inbox_has_pending(void);
int bus_inbox_count(void);

/* Print the last `tail_n` entries of workspace/logs/bus.jsonl. tail_n <= 0
 * means show everything. Returns 0 on success. */
int bus_log_show(int tail_n);

/* Filtered + tail-followable bus-log inspector. Each filter field is
 * optional: NULL strings mean "any", last_n <= 0 means "all", follow != 0
 * means "tail and keep printing new lines as they arrive until *stop_flag
 * goes nonzero". stop_flag may be NULL (then follow runs forever).
 *
 * Output is one envelope per line, exactly as stored in bus.jsonl. */
typedef struct {
  int last_n;
  const char *channel;
  const char *type;
  int follow;
  int human;
  volatile int *stop_flag; /* set to nonzero by signal handler to stop follow */
} BusLogFilter;

int bus_log_show_filtered(const BusLogFilter *filter);

/* Return the current byte size of workspace/logs/bus.jsonl, or 0 if the
 * file doesn't exist yet. Used by `fclaw bus log --follow`; channel adapters
 * tail their own delivery/narration logs instead of this bus CLI surface. */
long long bus_log_size_bytes(void);

#endif
