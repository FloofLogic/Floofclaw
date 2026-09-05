#ifndef FLOOF_RUNTIME_H
#define FLOOF_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

#include "support/fsutil.h"
#include "support/json.h"

#define RT_MAX_STEPS 32
#define RT_SMALL 128
#define RT_MED 512
#define RT_LARGE 4096
#define RT_ACTION_SCHEMA_MAX 8192
#define RT_OPERATION_RESULT_TEXT_MAX 8192
/* A user-visible `message` delivers at most this many bytes of UTF-8. Every
 * copy on the reply path (fc_message, the terminal's delivery fragment, the
 * ledger line, each adapter's tailer, the CLI channel) is sized for it, and
 * fc_message refuses a longer text rather than truncating it; the
 * normalizer refuses it earlier still, where the model can shorten it. Its
 * JSON-escaped form is bounded to twice that. */
#define RT_MESSAGE_TEXT_MAX 8192
#define RT_MESSAGE_ESCAPED_MAX (RT_MESSAGE_TEXT_MAX * 2)
#define RT_EVENT_PAYLOAD_MAX 16384
#define RT_XL 65536
/* A memory compaction request travels as an event payload, is replayed
 * into ctx->memory_compaction_json, and is then rendered into an agent
 * input alongside the compactor's prompt -- every one of those is RT_XL.
 * The reserve is the prompt and the wrapper that share the last buffer. */
#define RT_MEMORY_COMPACTION_PAYLOAD_MAX (RT_XL - 4096)
#define RT_TASK_WORKING_MEMORY_MAX RT_LARGE
#define RT_DEFAULT_LOOP "floofclaw"
#define RT_MAX_ACTIONS 64
/* Presentation-order sentinel stored in RtAgentMeta.action_ids. Runtime action
 * ids occupy 0..RT_MAX_ACTIONS-1; this expands to the complete registry at
 * exactly the position where agent.json lists all_actions. */
#define RT_ACTION_PRESENT_ALL ((unsigned char)RT_MAX_ACTIONS)
/* One external work lineage may make at most this many model-selected
 * semantic action choices. Eight matches the complete controller-evidence
 * projection window and terminates well before the 32-pass emergency cap. */
#define RT_WORK_SEMANTIC_STEP_LIMIT 8
/* A bound work controller's floop-owned policy chooses how many repair
 * selections it may make after a rejected or failed action. The runtime
 * supplies the compatibility default and enforces this absolute safety
 * ceiling; it does not otherwise choose the product's persistence. */
#define RT_WORK_REPAIR_ATTEMPT_DEFAULT 1
#define RT_WORK_REPAIR_ATTEMPT_LIMIT 8
/* A normalizer rejection is a slip, not a verdict. Repairs are ordinary
 * agent jobs/provider calls; this is the static configuration ceiling. */
#define RT_AGENT_DECISION_REPAIR_LIMIT 4
#define RT_AGENT_DECISION_REPAIR_DEFAULT 2

/* Durable state-store ceilings. These are boundary contracts, not private
 * array trivia: overflow behavior is exercised by the robustness suite. */
#define OPERATION_MAX_ITEMS 128
#define AFFAIR_MAX_ITEMS     32
/* Completion-token length in hex characters (32 random bytes). */
#define RT_OPERATION_TOKEN_HEX 64

typedef struct {
  char id[RT_SMALL];
  char type[RT_SMALL];
  char target[RT_SMALL];
  char gate[RT_SMALL];
  /* Maintenance step: its failure is recorded (error event + trace)
   * but never fails the run and never blocks the processed event. */
  int  non_critical;
} RtStep;

typedef struct {
  int version;
  char name[RT_SMALL];
  char description[RT_LARGE];
  int step_count;
  RtStep steps[RT_MAX_STEPS];
  /* One event -> one run -> one complete profile pass. When set, the
   * end-of-profile quiescence check never rewinds to phase zero; only
   * a new event creates the next pass. */
  int one_pass;
  /* Serialize runs per context: a run does not advance while an older
   * non-terminal run exists for the same context_id. Other contexts
   * advance concurrently. */
  int serialize_contexts;
  /* Bounded per-event retry: when a phase job fails, rewind to phase
   * zero and re-run the profile up to this many extra attempts before
   * failing the run. Committed calls are memoized by signature, so a
   * retry pass reuses recorded results instead of re-executing. */
  int retry_attempts;
  /* Optional floop-level shortening of managed-operation completion
   * deadlines (ms). May only shorten an action's declared default;
   * never extends past the action's maximum. 0 = use action defaults. */
  int operation_timeout_ms;
} RtProfile;

typedef enum {
  RT_ACTION_EXEC_NONE = 0,
  RT_ACTION_EXEC_SUBPROCESS,
  RT_ACTION_EXEC_RUNTIME
} RtActionExecKind;

/* Declared non-secret config entries per action. */
#define RT_ACTION_CONFIG_MAX 8
#define RT_ACTION_CONFIG_KEY_MAX 64
#define RT_ACTION_TYPED_CONFIG_MAX 8

typedef enum {
  RT_ACTION_CONFIG_PLAIN = 0,
  RT_ACTION_CONFIG_EXISTING_DIRECTORY,
  RT_ACTION_CONFIG_STRING,
  RT_ACTION_CONFIG_PERMISSION_MODE_CEILING
} RtActionConfigKind;

typedef struct {
  /* Process-local registry slot. Durable records keep the string id. */
  unsigned char runtime_id;
  char id[RT_SMALL];
  char area_id[RT_SMALL];
  char dir[PATH_MAX];
  char manifest_path[PATH_MAX];
  char description[RT_MED];
  int outside_world;
  char args_schema[RT_ACTION_SCHEMA_MAX];
  RtActionExecKind exec_kind;
  char exec_argv0[RT_SMALL];
  char exec_arg[RT_MED];
  /* Optional trusted-operator entry point. It is resolved from this same
   * registry definition and is never exposed to model calls. */
  char operator_argv0[RT_SMALL];
  char operator_arg[RT_MED];
  int has_operator_exec;
  /* Opt-in private, writable credential channel. Legacy actions retain
   * read-only scoped environment injection until migrated. */
  int private_credentials;
  /* Deployment mask from config/floofclaw_config.json top-level
   * `force_disable`. The action stays registered — floop allowlists keep
   * declaring intent and still validate — but it is omitted from every
   * rendered catalog and rejected at dispatch. Listing an id the registry
   * does not hold is inert, so one config travels across branches. */
  int force_disabled;
  int timeout_ms;
  /* action.json "contract":"managed_operation" — the action implements
   * the detached-operation lifecycle (immediate finished(result) or
   * running(worker_handle) + bus completion) and the operation driver
   * reacts to its terminals. Declared capability; the kernel never
   * matches on the action's id. */
  int managed_operation;
  /* Completion-deadline policy owned by the action definition (ms).
   * default_timeout_ms is required with the managed-operation contract;
   * max_timeout_ms bounds any shortening/extension and defaults to the
   * declared default. */
  int default_timeout_ms;
  int max_timeout_ms;
  /* action.json "contract":"work_terminal" — this runtime intrinsic may
   * terminalize only the exact work task/revision bound by a controller
   * invocation. The action id remains opaque to the validator. */
  int work_terminal;
  /* action.json "config" — non-secret deployment settings the action
   * needs (an ssh host, a mail profile). The manifest DECLARES the names;
   * config/floofclaw_config.json actions.<id> SUPPLIES the values, so one
   * shared action pack never carries one deployment's facts. Plain values
   * reach subprocess actions as environment variables; typed values are
   * startup-cached for runtime actions. Credentials do NOT belong here —
   * those live in the action:<id>: secret namespace. */
  char config_names[RT_ACTION_CONFIG_MAX][RT_SMALL];
  /* Optional deployment-object keys and typed loaders. Plain entries use
   * the environment-facing name as their key. Typed values are resolved and
   * cached while the registry loads, before any action launch. */
  char config_keys[RT_ACTION_CONFIG_MAX][RT_ACTION_CONFIG_KEY_MAX];
  unsigned char config_kinds[RT_ACTION_CONFIG_MAX];
  unsigned char config_required[RT_ACTION_CONFIG_MAX];
  int config_count;
} RtActionDef;

typedef struct {
  char action_id[RT_SMALL];
  char key[RT_ACTION_CONFIG_KEY_MAX];
  char value[PATH_MAX];
} RtActionCachedConfig;

typedef struct {
  RtActionDef defs[RT_MAX_ACTIONS];
  size_t count;
  /* Typed deployment values are sparse and registry-owned. Keeping them out
   * of every action definition preserves the small-stack registry budget. */
  RtActionCachedConfig cached_configs[RT_ACTION_TYPED_CONFIG_MAX];
  size_t cached_config_count;
} RtActionRegistry;

/* Durable managed-operation record. A completion claim trusts only the
 * inbound operation id + token, then resolves every other field from
 * this snapshot. `handle` is the runtime-owned operation id;
 * `worker_handle` is implementation-specific kill/query metadata. The
 * completion token is deliberately NOT part of the snapshot. */
typedef struct {
  char handle[RT_SMALL];
  char worker_handle[RT_SMALL];
  char action[RT_SMALL];
  char request_id[RT_SMALL];
  char trigger_event_id[RT_SMALL];
  char task_id[RT_SMALL];
  char context_id[RT_MED];
  char channel[RT_SMALL];
  char adapter_id[RT_SMALL];
  char origin_event_id[RT_SMALL];
  char ref_json[RT_MED];
  char status[RT_SMALL];
  long long work_rev;
  long long deadline_ms;
} RtOperationSnapshot;

/* Reducer-owned budget/correlation summary for one work revision. A
 * controller-created revision inherits lineage_root_rev and semantic_steps;
 * an external revision begins a fresh lineage at zero. */
typedef struct {
  char context_id[RT_MED];
  char task_id[RT_SMALL];
  long long work_rev;
  long long lineage_root_rev;
  int semantic_steps;
  int repair_attempts;
  char request_id[RT_SMALL];
  char action[RT_SMALL];
  char trigger_event_id[RT_SMALL];
} RtWorkBudgetSnapshot;

typedef struct {
  char id[RT_SMALL];
  char executor[RT_SMALL];
  char model_ref[RT_SMALL];
  int has_action_allowlist;
  /* Resolved once at startup. Entries retain exact agent.json presentation
   * order; RT_ACTION_PRESENT_ALL expands in place without deduplication. The
   * mask is the independent hard authorization grant. */
  uint64_t action_allow_mask;
  unsigned char action_ids[RT_MAX_ACTIONS];
  size_t action_count;
  int listen_memory;
  int listen_tasks;
  int listen_affairs;
  int listen_event;
  int listen_usage;
  int listen_declared;
  /* Product-neutral controller binding. When true, the runner selects the
   * unique same-context open work task and carries its reducer-owned
   * task_id/work_rev through the invocation and action request. */
  int bind_task_open_work;
  /* agent.json work_policy.max_repair_attempts. Applies only to an
   * open-work-bound controller; defaults to one for compatibility. */
  int max_repair_attempts;
  /* Configured handler binding for native phases that dispatch work to
   * a configured action (e.g. the workers phase). Opaque to the
   * kernel; read only by the native agent that declared it. */
  char handler[RT_SMALL];
  int recent;
  int autocomplete_message_task_on_send;
  /* Bounded repair budget for an LLM normalizer rejection. agent.json
   * "repair_attempts". */
  int decision_repair_attempts;
  int conversational_payload_only;
  int affair_extraction_context_only;
  int memory_compaction_context_only;
  int can_write_memory;
  int memory_summary_enabled;
  int memory_compact_after_messages;
  int memory_compact_after_tokens;
  int memory_keep_recent_messages;
  int memory_min_compact_messages;
  int memory_summary_max_tokens;
  int memory_recent_summary_max_tokens;
} RtAgentMeta;

typedef struct {
  char workspace[PATH_MAX];
  char run_id[RT_SMALL];
  char run_dir[PATH_MAX];
  /* event_log_path / state_path / trace_path are not stored — derive
   * them from run_dir at the (rare) point of use via the inline
   * helpers rt_event_log_path / rt_state_path / rt_trace_path
   * below. Saves 3 × PATH_MAX (3 KB) per RtContext = 96 KB across
   * the static run pool. */
  int next_event;
  int next_delivery;
  int replay_mode;
  char user_text[RT_LARGE];
  char latest_action[RT_SMALL];
  char latest_result_text[RT_LARGE];
  char latest_message[RT_LARGE];
  /* Origin attribution from the inbound user_message envelope (channel
   * adapters care about these; deterministic CLI runs leave them empty). */
  char origin_event_id[RT_SMALL];
  char origin_channel[RT_SMALL];
  char origin_adapter_id[RT_SMALL];
  char origin_ref_json[RT_LARGE];   /* raw JSON of the ref object, e.g. {"nick":"rick", ...} */
  char context_id[RT_MED];
  char state_json[RT_XL];
  /* Compact JSON copy of the most recent memory_recalled payload,
   * folded in by the run-state projector. This is trusted runtime state
   * for native memory handling; processor inputs do not expose a global
   * memory root.
   * Sized to RT_LARGE because rt_append_events_from_output already
   * caps an event's compact payload at RT_LARGE — anything larger
   * never reaches the projector. */
  char memory_json[RT_LARGE];
  int memory_compaction_due;
  char memory_compaction_json[RT_XL];
  /* Affair watcher: when the inbound user_message carried an affair_id
   * (manual review or watcher fire), the runtime stashes it here so
   * downstream phases can project an affair_review request and link
   * any new work tasks back to the affair. Empty = not an affair turn. */
  char inbound_affair_id[RT_SMALL];
  /* Triggering-event projection: the inbound envelope's kind and its
   * opaque payload, preserved verbatim for event-kind gates and the
   * "event" listen block. The kernel never interprets the payload. */
  char event_kind[RT_SMALL];
  char event_payload_json[RT_EVENT_PAYLOAD_MAX];
  /* Optional compact reference to an immutable ingress-media manifest.
   * Text-only runs leave this NULL and allocate nothing. The pointed-to
   * JSON object is owned by this context and must be released before its
   * enclosing run-pool slot is zeroed. Attachment bytes never live here. */
  char *media_ref_json;
  /* Runtime-owned runstate spec v1.3.8 envelope for the current run. */
  char run_state_json[RT_LARGE];
} RtContext;

int rt_json_escape(const char *src, char *out, size_t out_len);
int rt_json_get_string(const char *json, const char *key, char *out, size_t out_len);
int rt_json_get_int(const char *json, const char *key, int *out);
int rt_json_get_array_raw(const char *json, const char *key, char *out, size_t out_len);
int rt_action_output_extract(const char *output_json, const char *artifact_path,
                            char *result_out, size_t result_len,
                            char *error_out, size_t error_len,
                            int is_managed_op);

/* Inject status:"finished" and the durable request_id as handle into a raw
 * action result JSON object, in place. Idempotent (skips if status already
 * present). Used by both intrinsic and subprocess dispatch paths to uniformly
 * wrap managed-op action returns. */
void rt_action_wrap_finished_result(char *result, size_t result_len,
                                    const char *request_id);

int rt_profile_load(const char *name_or_path, RtProfile *out, char *error, size_t error_len);
const char *rt_default_loop(void);
int rt_next_run_number_for_workspace(const char *workspace);
int rt_workspace_next_run_task_collision(const char *workspace,
                                         char *run_id_out,
                                         size_t run_id_len);
/* Path derivers — small inline helpers so callers don't keep
 * stamping the same suffix over and over. PATH_MAX-sized output
 * buffers are the caller's responsibility. */
static inline int rt_event_log_path(const RtContext *ctx, char *out,
                                    size_t out_len) {
  return ctx ? fs_join(out, out_len, ctx->run_dir, "event_log.jsonl") : -1;
}
static inline int rt_state_path(const RtContext *ctx, char *out,
                                size_t out_len) {
  return ctx ? fs_join(out, out_len, ctx->run_dir, "state.json") : -1;
}
static inline int rt_trace_path(const RtContext *ctx, char *out,
                                size_t out_len) {
  return ctx ? fs_join(out, out_len, ctx->run_dir, "trace.jsonl") : -1;
}

int rt_append_event(RtContext *ctx, const char *type, const char *source, const char *payload_json);
/* Forced-sync variant for an event that proves a prepared durable wake. */
int rt_append_event_sync(RtContext *ctx, const char *type,
                         const char *source, const char *payload_json);
int rt_next_event_id(const RtContext *ctx, char *out, size_t out_len);
#include "support/append.h"
int rt_append_events_from_output(RtContext *ctx, const char *source, int source_can_write_memory,
                                 const char *output_json,
                                 char *committed_out, size_t committed_len);
void rt_log_rejected_agent_event(const char *source, const char *reason, const char *detail);
const char *rt_last_rejected_agent_detail(void);
void rt_clear_rejected_agent_detail(void);
/* printf-style "error" event emitter. Builds payload `{"message":"<msg>"}`
 * with proper JSON escaping and forwards to rt_append_event with the
 * given source ("kernel" if NULL). Use this for message-only error
 * events; for events that need extra structured fields, snprintf the
 * payload manually and call rt_append_event directly. */
int rt_emit_error(RtContext *ctx, const char *source, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));
/* Same, plus the run-relative artifact holding the failure's evidence
 * (e.g. "agent_outputs/003_step.stderr") so `view -d` resolves it exactly. */
int rt_emit_error_artifact(RtContext *ctx, const char *source,
                           const char *artifact_rel, const char *fmt, ...)
  __attribute__((format(printf, 4, 5)));

/* Run-state projector: applies one event's effect to ctx fields
 * (latest_*, user_text). Pure in-memory. Called automatically from
 * rt_append_event after a successful append. */
void rt_apply_event_to_context(RtContext *ctx, const char *type, const char *payload_json);
/* Release the optional run-owned compact media reference. Safe on NULL and
 * idempotent; scheduler retirement is the ordinary owner boundary. */
void rt_context_clear_media_ref(RtContext *ctx);
/* Write the per-run state.json from current ctx fields. No event-log
 * re-read; ctx is already up to date because rt_apply_event_to_context
 * runs at every append. */
int rt_write_state_snapshot(RtContext *ctx);

typedef struct {
  size_t malformed_lines;
  size_t malformed_nonfinal_lines;
  size_t first_malformed_nonfinal_line;
} RtReplayReport;

/* Cold path: replay an event-log text and produce a final state JSON.
 * Used by `fclaw replay`. Independent of RtContext. The report variant is
 * pure; the ordinary wrapper narrates malformed-line diagnostics. */
int rt_reduce_event_log_text_report(const char *event_text,
                                    char *out_state, size_t out_len,
                                    RtReplayReport *report);
int rt_reduce_event_log_text(const char *event_text, char *out_state, size_t out_len);

/* Durable subsystems. Each owns its files under workspace/memory/.
 * Routed from rt_append_event when the event type matches; not called
 * by the run-state projector. */
/* True when `type` is one of the four memory record types
 * (user/asst/fact/summ). The runtime uses this in rt_append_event
 * to gate routing into rt_memory_apply. */
int  rt_memory_is_record_type(const char *type);
int  rt_memory_is_control_type(const char *type);
/* Append one flat memory record line to workspace/memory/memory.jsonl.
 * payload_json must carry a non-empty "text"; summ records may also
 * carry "through". Returns 0 on success, -1 on malformed payload. The
 * caller (rt_append_event) emits a kernel error on -1. */
int  rt_memory_apply(RtContext *ctx, const char *type, const char *payload_json,
	                     const char *ts, const char *run_id);
int  rt_memory_apply_control(const char *type, const char *payload_json);
int  rt_memory_maybe_request_compaction(RtContext *ctx, const RtAgentMeta *meta);
int  rt_memory_compaction_input_json(const RtContext *ctx, char *out, size_t out_len);
int  rt_memory_compaction_cutoff(const RtContext *ctx, char *out, size_t out_len);
int  rt_task_is_event_type(const char *type);
int  rt_task_normalize_event(RtContext *ctx, const char *type,
                             const JsonRef *payload,
                             int event_seq,
                             char *out, size_t out_len,
                             char *err, size_t err_len);
int  rt_task_normalize_artifacts(const JsonRef *payload, const char *run_id,
                                 int event_seq, long long work_rev,
                                 char *out, size_t out_len);
/* Persist operator-visible error context for a failed action. Failure
 * diagnostics are deliberately separate from the restricted success-proof
 * path above. */
int  rt_task_append_action_failure(RtContext *ctx, const char *action,
                                   const char *task_id, long long work_rev,
                                   const char *reason,
                                   const char *error_json);
int  rt_task_apply_event(const char *type, const char *payload_json);
enum {
  RT_TASK_CANCEL_OK = 0,
  RT_TASK_CANCEL_NOT_FOUND = 1,
  RT_TASK_CANCEL_NOT_OPEN = 2,
  RT_TASK_CANCEL_FAILED = -1
};
/* Operator control path. Only an exact open task may transition; the
 * canonical task_canceled payload is applied by the task reducer. */
int  rt_task_cancel_operator(const char *task_id,
                             char *current_status, size_t status_len);
int  rt_task_work_rev_of(const char *task_id, long long *out_rev);
int  rt_task_working_memory_of(const char *task_id,
                               char *out, size_t out_len);
/* Bounded reducer-owned controller evidence and work-lineage budgets. */
int  rt_work_state_is_event_type(const char *type);
int  rt_work_state_apply_event(const RtContext *ctx, const char *type,
                               const char *payload_json,
                               const char *event_id);
int  rt_work_state_projection_json(const char *task_id, long long work_rev,
                                   int max_repair_attempts,
                                   char *out, size_t out_len);
int  rt_work_state_evidence_refs_json(const char *task_id,
                                      long long work_rev,
                                      char *out, size_t out_len);
int  rt_work_state_result_matches(const char *task_id, long long work_rev,
                                  const char *request_id,
                                  const char *action,
                                  const char *source_event_id,
                                  const char *wake_type,
                                  long long *effective_work_rev);
int  rt_work_state_rebuild_from_logs(void);
int  rt_work_state_outcome_matches(const RtContext *ctx);
int  rt_work_state_budget_snapshot(const char *task_id, long long work_rev,
                                   RtWorkBudgetSnapshot *out);
/* 0 means the selected result is not repairable, 1 permits another repair
 * selection under the supplied floop policy, 2 means the configured repair
 * budget is exhausted and must now block. source_event_id may be empty only
 * for the reducer-owned latest selection. */
int  rt_work_state_repair_disposition(const char *task_id,
                                      long long work_rev,
                                      const char *request_id,
                                      const char *source_event_id,
                                      int max_repair_attempts);
/* Managed-operation store (runtime/operation_state.c). Records are one
 * per runtime-owned operation id; every value is opaque bytes to the
 * kernel. Direct writes cover the completion capability and worker
 * metadata; behavioral truth stays in operation_started /
 * operation_result events. */
int  rt_operation_is_event_type(const char *type);
int  rt_operation_apply_event(const char *type, const char *payload_json);
int  rt_operation_status(const char *operation_id, char *status_out, size_t status_len);
int  rt_operation_snapshot(const char *operation_id, RtOperationSnapshot *out);
int  rt_operation_bind_secret(const char *operation_id, const char *token);
int  rt_operation_close_without_result(const char *operation_id);
int  rt_operation_id_for_request(const char *request_id,
                                 char *out, size_t out_len);
int  rt_operation_publish_worker_claim(const char *operation_id,
                                       const char *token,
                                       const char *status,
                                       const char *text);
int  rt_operation_submit_abort_claim(const char *worker_handle,
                                     const char *text);
int  rt_operation_set_worker_handle(const char *operation_id,
                                    const char *worker_handle);
int  rt_operation_bind_claim_envelope(const char *operation_id,
                                      const char *envelope_id);
int  rt_operation_claim_envelope_of(const char *operation_id,
                                    char *out, size_t out_len);
int  rt_operation_token_matches(const char *operation_id, const char *token);
int  rt_operation_token_of(const char *operation_id, char *out, size_t out_len);
int  rt_operation_find_running_by_worker_handle(const char *worker_handle,
                                                char *operation_id_out,
                                                size_t out_len);
long long rt_operation_earliest_deadline_ms(void);
int  rt_operation_list_deadline_due(long long now_ms, char ids[][RT_SMALL],
                                    size_t max, size_t *out_count);
int  rt_operation_any_running(void);
int  rt_operation_work_rev_of(const char *operation_id, long long *out_rev);
int  rt_operation_channel_of(const char *operation_id, char *out, size_t out_len);
int  rt_operation_context_id_of(const char *operation_id, char *out, size_t out_len);
int  rt_operation_generate_token(char *out, size_t out_len);
/* Janitor retention guard for workspace/bus/processed pruning: 1 while
 * the envelope is a completion claim whose operation is still running.
 * Implemented in ports.c (it owns envelope shapes). */
int  rt_ports_completion_claim_pinned(const char *envelope_id);
/* Task-domain terminal for an operation that cannot continue safely.
 * Returns 0 when appended, 1 when the stored work binding is already
 * stale/terminal, and -1 on malformed input/I/O. */
int  rt_task_block_unpollable_operation(const RtContext *ctx,
                                        const RtOperationSnapshot *op,
                                        const char *reason,
                                        const char *needed);
/* Mechanically terminalize an exact bound work revision. The request/action
 * identity must come from reducer-owned selection state, never model text. */
int  rt_task_block_bound_work(const RtContext *ctx,
                              const char *task_id, long long work_rev,
                              const char *request_id, const char *action,
                              const char *trigger_event_id,
                              const char *reason, const char *needed);
int  rt_task_open_work_snapshot(const char *context_id,
                                char *task_id, size_t task_id_len,
                                char *work, size_t work_len,
                                char *done_when, size_t done_len,
                                long long *work_rev,
                                char *external_json, size_t external_len);
/* Exact work-controller binding helpers. The selector returns 0 for one
 * match, 1 for none, 2 for ambiguity, and -1 on malformed state. The JSON
 * lookup returns 0 only when task/context/revision/status all match. */
int  rt_task_select_unique_open_work(const char *context_id,
                                     char *task_id, size_t task_id_len,
                                     long long *work_rev);
int  rt_task_select_unique_revisable_work(const char *context_id,
                                          char *task_id, size_t task_id_len,
                                          long long *work_rev);
/* Select the exact work consequence for one run. Returns 0 one, 1 none,
 * 2 multiple, 3 stale revision, 4 uncorrelated result, 5 already consumed,
 * and -1 malformed. `trigger_event_id` is the runtime-owned source event
 * that the controller action_request must bind. */
int  rt_task_select_work_consequence(const RtContext *ctx,
                                     char *task_id, size_t task_id_len,
                                     long long *work_rev,
                                     char *trigger_event_id,
                                     size_t trigger_event_id_len);
int  rt_task_work_binding_json(const char *context_id, const char *task_id,
                               long long work_rev, int allow_blocked,
                               char *out, size_t out_len);
int  rt_task_work_agent_binding_json(const char *context_id,
                                     const char *task_id,
                                     long long work_rev, int allow_blocked,
                                     char *out, size_t out_len);
int  rt_tasks_state_json(char *out, size_t out_len);
int  rt_tasks_array_json(char *out, size_t out_len);
int  rt_tasks_active_state_json(const char *context_id, char *out, size_t out_len);
int  rt_tasks_active_agent_state_json(const char *context_id,
                                      char *out, size_t out_len);
int  rt_task_select_new_input(const char *context_id, char *task_id, size_t task_len);
int  rt_task_select_unique_open_input(const char *context_id, char *task_id, size_t task_len);
int  rt_task_select_new_input_for_run(const char *context_id, const char *run_id,
                                      char *task_id, size_t task_len);
int  rt_task_input_text(const char *context_id, const char *task_id,
                        char *out, size_t out_len);
int  rt_conversational_request_task(const RtContext *ctx, char *task_id,
                                    size_t task_len, char *request_kind,
                                    size_t kind_len, char *text,
                                    size_t text_len);
int  rt_tasks_archive_completed_for_context(const char *context_id);
/* Resolve an action's declared config into "NAME=VALUE" strings.
 * out_buf is char[RT_ACTION_CONFIG_MAX][entry_len]. Returns the count
 * written, or -1 if a required name has no deployment value. */
int  rt_action_resolve_config(const RtActionDef *def, void *out_buf,
                              size_t entry_len);
const char *rt_action_cached_config(const RtActionRegistry *reg,
                                    const char *action_id,
                                    const char *name);
int  rt_task_reference_exists(const char *task_id);
int  rt_task_ids_exist_for_run(const char *run_id);
int  rt_task_reference_is_ready_for_context(const char *context_id,
                                            const char *task_id);
int  rt_task_reference_in_context(const char *context_id,
                                  const char *task_id);
int  rt_task_reference_active_in_context(const char *context_id,
                                         const char *task_id);
int  rt_task_create_input_for_user_message(RtContext *ctx,
                                           const char *text);
int  rt_task_complete_message_on_send(RtContext *ctx, const char *task_id);
int  rt_task_terminalize_open_message_tasks_for_run(RtContext *ctx,
                                                    const char *task_event_type);
int  rt_task_context_has_actionable(const char *context_id);
int  rt_affair_is_event_type(const char *type);
int  rt_affair_normalize_event(RtContext *ctx, const JsonRef *payload,
                               int event_seq, char *out, size_t out_len);
int  rt_affair_apply_event(const char *type, const char *payload_json);
int  rt_affairs_active_state_json(const char *context_id, char *out, size_t out_len);
int  rt_affair_reference_in_context(const char *context_id, const char *affair_id);
int  rt_affair_first_active_in_context(const char *context_id,
                                       char *out, size_t out_len);
/* Affair watcher + CLI helpers. The scheduling field is one long long:
 * next_review_at_ms (0 = manual-only). The watcher fires when
 * now_ms >= next_review_at_ms and clears it via affair_reviewed;
 * the speaker re-arms via affair_patch action:update, or the CLI via
 * rt_affair_set_next_review_at. */
int  rt_affair_create_manual(const char *context_id, const char *manage,
                             long long initial_next_review_at_ms,
                             char *out_affair_id, size_t out_len);
int  rt_affair_set_status(const char *affair_id, const char *status);
int  rt_affair_set_next_review_at(const char *affair_id, long long next_review_at_ms);
int  rt_affair_get_manage(const char *affair_id, char *out, size_t out_len,
                          char *context_id_out, size_t context_id_len);
int  rt_affair_review_input_json(const char *affair_id, char *out, size_t out_len);
int  rt_affair_list_due(long long now, char ids[][RT_SMALL], size_t max,
                        size_t *out_count);
int  rt_affair_context_id_of(const char *affair_id, char *out, size_t out_len);
int  rt_affair_each(int (*visitor)(const char *affair_id, const char *context_id,
                                   const char *manage, const char *status,
                                   long long next_review_at_ms,
                                   size_t note_count, size_t task_id_count,
                                   void *user),
                    void *user);
int  rt_affair_main(int argc, char **argv);
int  rt_task_main(int argc, char **argv);
int  rt_memory_state_json(char *out, size_t out_len);
int  rt_memory_projection_json(const char *context_id, int recent_limit,
                               char *out, size_t out_len);
int rt_write_trace(RtContext *ctx, const char *phase, const char *detail);
/* Append a one-line note to workspace/logs/narration.jsonl. Channel
 * adapters may mirror narration into a configured ops channel. */
int rt_narrate(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Retry the first narration that could not be persisted during a transient
 * filesystem outage. The gateway calls this on a bounded cadence. */
int rt_narrate_flush_pending(void);
int rt_write_loop_decision_trace(RtContext *ctx, const char *decision,
                                 const char *reason, unsigned int pass);
int rt_write_phase_skipped_trace(RtContext *ctx, unsigned int pass,
                                 const char *phase, const char *reason);
typedef int (*RtActionVisitor)(const char *id, const char *area_id,
                              const char *dir, const char *manifest_path,
                              const char *script_path, void *user);
int rt_action_registry_load(RtActionRegistry *reg, char *err, size_t err_len);
const RtActionDef *rt_action_registry_find(const RtActionRegistry *reg, const char *action_id);
int rt_action_registry_runtime_function_known(const char *name);
int rt_agent_allows_action(const RtAgentMeta *meta, const RtActionDef *def);
int rt_action_validate_args(const RtActionDef *def, const char *args_json,
                           char *errors_json, size_t errors_len,
                           char *message, size_t message_len);
int rt_action_render_registry(const RtActionRegistry *reg,
                              char *out, size_t out_len);
int rt_action_render_available(const RtActionRegistry *reg, const RtAgentMeta *meta,
                              char *out, size_t out_len);
int rt_action_render_allowed_ids(const RtActionRegistry *reg,
                                 const RtAgentMeta *meta,
                                 char *out, size_t out_len);
int rt_action_render_definition(const RtActionDef *def,
                                char *out, size_t out_len);
int rt_action_foreach_registered(RtActionVisitor visitor, void *user);
int rt_view_main(int argc, char **argv);
int rt_usage_main(int argc, char **argv);
int rt_usage_projection_json(char *out, size_t out_len);
int rt_usage_client_json(char *out, size_t out_len);
int rt_pulse_projection_json(const char *root, long long now_ms,
                             int message_limit, int run_limit,
                             char *out, size_t out_len,
                             char *err, size_t err_len);
int rt_cacheview_main(int argc, char **argv);
int rt_internal_main(int argc, char **argv);
const char *rt_exe_path(void);
int rt_clear_main(int argc, char **argv);
/* The three extension surfaces, listed the same way. `fclaw tools` is the
 * former name of `fclaw actions` and stays as a silent alias. */
int rt_tools_main(int argc, char **argv);
int rt_adapters_main(int argc, char **argv);
int rt_floops_main(int argc, char **argv);
int rt_action_main(int argc, char **argv);
int rt_operation_cli_main(int argc, char **argv);
int rt_setup_main(int argc, char **argv);
int rt_config_main(int argc, char **argv);
int rt_version_main(int argc, char **argv);
int rt_channel_main(int argc, char **argv);
int rt_mcp_main(int argc, char **argv);
int rt_chat_interactive(int argc, char **argv);
int rt_auth_main(int argc, char **argv);
int rt_bus_main(int argc, char **argv);
int rt_gateway_main(int argc, char **argv);

#endif
