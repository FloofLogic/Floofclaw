/* Operation driver — the kernel's managed-operation lifecycle service.
 *
 * Identity is allocated by the runtime BEFORE the action executes:
 *
 *   allocate: operation_id ("op_<request_id>") + completion token +
 *             absolute deadline, committed durably (operation_started
 *             event + direct token bind) before the worker can exist;
 *   start:    the action returns finished(result) for the immediate
 *             path, or running(worker_handle) — the handle is recorded
 *             as kill/query metadata only, never as identity;
 *   finish:   a detached worker submits an operation_completed claim
 *             through the ordinary bus; the deadline claims timeout;
 *             an immediate finished(result) terminalizes in-run. All
 *             three causes compete through the single reducer-owned
 *             terminal gate in the operation store.
 *
 * The driver dispatches on the DECLARED contract, never on an action
 * or handler name, and never reads meaning out of result text. There
 * is no polling: the reactor sleeps until a completion envelope knocks
 * or the deadline becomes due. */

#include "run_internal.h"
#include "bus/bus.h"
#include "publication_outbox.h"
#include "support/heap_guard.h"
#include "support/json.h"
#include "support/timing.h"

#include <stdio.h>
#include <string.h>

#define OP_DEFAULT_TIMEOUT_MS (30LL * 60LL * 1000LL)
#define OP_FAILURE_TEXT_MAX 1800
#define OP_EXTERNAL_EXCERPT_MAX 500

_Static_assert(BUS_PAYLOAD_MAX == RT_EVENT_PAYLOAD_MAX,
               "correlated event and bus payload caps must match");

/* The bus channel completion claims travel on. The claim's routing is
 * ignored at intake — the result run's route comes exclusively from the
 * durable operation record — so claims share one internal channel and
 * workers never author routing. */
#define OP_CLAIM_CHANNEL "operations"

/* Runtime-owned operation id for one action call. Deterministic in the
 * durable request id so a recovered relaunch converges on the same
 * operation instead of allocating a second one. */
int rt_operation_id_for_request(const char *request_id,
                                char *out, size_t out_len) {
  size_t pos;
  if (!request_id || !*request_id || !out || out_len < 8) return -1;
  pos = (size_t)snprintf(out, out_len, "op_");
  for (const char *p = request_id; *p && pos + 1 < out_len; ++p) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')
      out[pos++] = c;
    else
      out[pos++] = '_';
  }
  out[pos] = '\0';
  return 0;
}

static void result_text_excerpt(const JsonRef *obj, char *out, size_t out_len) {
  JsonRef value;
  size_t cap;
  if (!out || out_len == 0) return;
  cap = out_len - 1U < RT_OPERATION_RESULT_TEXT_MAX
            ? out_len - 1U : RT_OPERATION_RESULT_TEXT_MAX;
  out[0] = '\0';
  if ((json_ref_object_get(obj, "text", &value) != 0 ||
       value.type != JSON_REF_STRING) &&
      (json_ref_object_get(obj, "final", &value) != 0 ||
       value.type != JSON_REF_STRING))
    return;
  memset(out, 0, cap + 1U);
  if (json_ref_string_copy(&value, out, cap + 1U) != 0)
    out[cap] = '\0';
}

static void failure_text_excerpt(const char *action, const char *type,
                                 const char *detail_json,
                                 char *out, size_t out_len) {
  JsonRef detail, error, errors;
  char message[OP_FAILURE_TEXT_MAX + 1] = "";
  char validation[OP_FAILURE_TEXT_MAX + 1] = "";
  const char *state = type && strcmp(type, "action_rejected") == 0
                          ? "was rejected" : "failed";
  size_t used;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (detail_json && json_ref_first_object(detail_json, &detail) == 0) {
    if (json_ref_object_get_string(&detail, "message", message,
                                   sizeof(message)) != 0 &&
        json_ref_object_get_object(&detail, "error", &error) == 0)
      (void)json_ref_object_get_string(&error, "message", message,
                                       sizeof(message));
    if (json_ref_object_get_array(&detail, "errors", &errors) == 0 &&
        json_ref_array_size(&errors) > 0)
      (void)json_ref_value_copy(&errors, validation, sizeof(validation));
  }
  if (!message[0]) snprintf(message, sizeof(message), "no error detail returned");
  snprintf(out, out_len, "Tool %s %s: ", action && *action ? action : "action",
           state);
  used = strlen(out);
  if (used < out_len - 1U)
    snprintf(out + used, out_len - used, "%.*s",
             (int)(out_len - used - 1U), message);
  used = strlen(out);
  if (validation[0] && used < out_len - 1U)
    snprintf(out + used, out_len - used, " Validation errors: %s",
             validation);
}

static int task_external_matches_attempt(const RtRun *r,
                                         const char *task_id,
                                         long long current_rev,
                                         const char *handle,
                                         long long attempt_rev) {
  char *task_json;
  JsonRef task, state, external;
  char stored_handle[RT_SMALL] = "";
  long long stored_rev = 0;
  int matches = 0;
  if (!r || !task_id || !*task_id || !handle || !*handle ||
      current_rev <= 0 || attempt_rev <= 0)
    return 0;
  task_json = (char *)fc_xmalloc(RT_XL);
  if (!task_json) return 0;
  if (rt_task_work_binding_json(r->ctx.context_id, task_id, current_rev, 1,
                                task_json, RT_XL) == 0 &&
      json_ref_from_text(task_json, &task) == 0 &&
      json_ref_object_get_object(&task, "state", &state) == 0 &&
      json_ref_object_get_object(&state, "external", &external) == 0 &&
      json_ref_object_get_string(&external, "handle", stored_handle,
                                 sizeof(stored_handle)) == 0 &&
      json_ref_object_get_long(&external, "work_rev", &stored_rev) == 0 &&
      strcmp(stored_handle, handle) == 0 && stored_rev == attempt_rev)
    matches = 1;
  fc_xfree(task_json);
  return matches;
}

/* Project the attempt record into the task's opaque state.external
 * slot via an ordinary task_updated event. The reducer merges it; an
 * external-only update never bumps work_rev. `handle` here is the
 * runtime operation id, so running and finished projections of one
 * attempt always match. */
static void update_task_external(RtRun *r, const char *task_id, const char *handle,
                                 const char *action, const char *status,
                                 long long work_rev, const char *excerpt) {
  char etask[RT_MED], eh[RT_MED], ea[RT_MED], es[RT_MED], ex[RT_MED * 2];
  char bounded[OP_EXTERNAL_EXCERPT_MAX + 1];
  char payload[RT_LARGE];
  long long current_rev = 0;
  if (!task_id || !*task_id) return;
  /* A detached operation belongs to the revision that started it. A late
   * result remains durable evidence but must never overwrite a newer
   * revision's current external projection. */
  if (work_rev > 0) {
    if (rt_task_work_rev_of(task_id, &current_rev) != 0)
      return;
    if (current_rev != work_rev &&
        !task_external_matches_attempt(r, task_id, current_rev,
                                       handle, work_rev))
      return;
  }
  snprintf(bounded, sizeof(bounded), "%s", excerpt ? excerpt : "");
  if (json_escape(task_id, etask, sizeof(etask)) != 0 ||
      json_escape(handle ? handle : "", eh, sizeof(eh)) != 0 ||
      json_escape(action ? action : "", ea, sizeof(ea)) != 0 ||
      json_escape(status ? status : "", es, sizeof(es)) != 0 ||
      json_escape(bounded, ex, sizeof(ex)) != 0) return;
  (void)rt_append_event_format(
      &r->ctx, "task_updated", "kernel", payload, sizeof(payload), 1,
      "{\"task_id\":\"%s\",\"state\":{\"external\":{\"handle\":\"%s\","
      "\"action\":\"%s\",\"status\":\"%s\",\"work_rev\":%lld,"
      "\"result_excerpt\":\"%s\"}}}",
      etask, eh, ea, es, work_rev, ex);
}

/* Correlation block from a durable operation record. Every result —
 * immediate, pushed, or timed out — resolves its route from the store,
 * never from what a worker or model submitted. */
static int correlation_fields_from_op(const RtOperationSnapshot *op,
                                      char *out, size_t out_len) {
  char ectx[RT_MED], ech[RT_MED], ead[RT_MED], etask[RT_MED], eorigin[RT_MED];
  int n;
  if (json_escape(op->context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(op->channel, ech, sizeof(ech)) != 0 ||
      json_escape(op->adapter_id, ead, sizeof(ead)) != 0 ||
      json_escape(op->task_id, etask, sizeof(etask)) != 0 ||
      json_escape(op->origin_event_id, eorigin, sizeof(eorigin)) != 0) return -1;
  n = snprintf(out, out_len,
               "\"task_id\":\"%s\",\"context_id\":\"%s\",\"channel\":\"%s\","
               "\"adapter_id\":\"%s\",\"origin_event_id\":\"%s\",\"ref\":%s",
               etask, ectx, ech, ead, eorigin,
               op->ref_json[0] ? op->ref_json : "null");
  return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

/* Effective completion deadline for one call. The action definition owns
 * the default and the hard maximum; a statically configured floop may
 * shorten it; nothing may extend past the action's maximum. */
static long long op_effective_timeout_ms(const RtRun *r,
                                         const RtActionDef *def) {
  long long base = def && def->default_timeout_ms > 0
                       ? def->default_timeout_ms : OP_DEFAULT_TIMEOUT_MS;
  if (r && r->profile && r->profile->operation_timeout_ms > 0 &&
      r->profile->operation_timeout_ms < base)
    base = r->profile->operation_timeout_ms;
  if (def && def->max_timeout_ms > 0 && base > def->max_timeout_ms)
    base = def->max_timeout_ms;
  return base;
}

/* Allocate identity + capability + deadline for one managed call and
 * commit the durable STARTING record before the action executes. Called
 * after action_started is durable and before dispatch reaches the
 * action. Recovery relaunches converge on the existing record.
 *
 * The token travels only through trusted memory (pending entry → child
 * env / forked worker state); it never appears in the event payload. */
int rt_operation_allocate_for_call(RtRun *r, const RtActionDef *def,
                                   RtPendingAction *p) {
  char op_id[RT_SMALL];
  char token[RT_OPERATION_TOKEN_HEX + 1] = "";
  char status[RT_SMALL] = "";
  long long deadline;
  int lookup;
  if (!r || !def || !p || !def->managed_operation) return -1;
  /* The action CLI transport consumes its terminal synchronously and the
   * driver never reacts to it; allocating would leave a dangling record. */
  if (strcmp(p->source_agent, "action_cli") == 0) return 0;
  if (rt_operation_id_for_request(p->request_id, op_id, sizeof(op_id)) != 0)
    return -1;
  lookup = rt_operation_status(op_id, status, sizeof(status));
  if (lookup < 0) return -1;
  if (lookup == 0) {
    /* Recovery relaunch of the same durable request: reuse the record.
     * A finished record means this request already has one terminal
     * truth; the relaunched call may still run, but it can never
     * produce a second result. */
    if (strcmp(status, "finished") == 0) {
      snprintf(p->op_id, sizeof(p->op_id), "%s", op_id);
      p->op_token[0] = '\0';
      return 0;
    }
    if (rt_operation_token_of(op_id, token, sizeof(token)) != 0 ||
        !token[0]) {
      if (rt_operation_generate_token(token, sizeof(token)) != 0 ||
          rt_operation_bind_secret(op_id, token) != 0)
        return -1;
    }
    snprintf(p->op_id, sizeof(p->op_id), "%s", op_id);
    snprintf(p->op_token, sizeof(p->op_token), "%s", token);
    return 0;
  }
  deadline = (long long)timing_stable_wall_ms() +
             op_effective_timeout_ms(r, def);
  {
    char eh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED];
    char corr[RT_LARGE], payload[RT_LARGE];
    RtOperationSnapshot op;
    long long work_rev = p->work_rev;
    if (work_rev <= 0 && p->task_id[0])
      (void)rt_task_work_rev_of(p->task_id, &work_rev);
    memset(&op, 0, sizeof(op));
    snprintf(op.context_id, sizeof(op.context_id), "%s", r->ctx.context_id);
    snprintf(op.channel, sizeof(op.channel), "%s", r->ctx.origin_channel);
    snprintf(op.adapter_id, sizeof(op.adapter_id), "%s",
             r->ctx.origin_adapter_id);
    snprintf(op.task_id, sizeof(op.task_id), "%s", p->task_id);
    snprintf(op.origin_event_id, sizeof(op.origin_event_id), "%s",
             r->ctx.origin_event_id);
    snprintf(op.ref_json, sizeof(op.ref_json), "%s",
             r->ctx.origin_ref_json[0] ? r->ctx.origin_ref_json : "");
    if (json_escape(op_id, eh, sizeof(eh)) != 0 ||
        json_escape(def->id, ea, sizeof(ea)) != 0 ||
        json_escape(p->request_id, erid, sizeof(erid)) != 0 ||
        json_escape(p->trigger_event_id, etrigger, sizeof(etrigger)) != 0 ||
        correlation_fields_from_op(&op, corr, sizeof(corr)) != 0)
      return -1;
    if (rt_append_event_format(
            &r->ctx, "operation_started", "kernel", payload, sizeof(payload), 1,
            "{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
            "\"trigger_event_id\":\"%s\",%s,"
            "\"work_rev\":%lld,\"deadline_ms\":%lld}",
            eh, ea, erid, etrigger, corr, work_rev, deadline) != 0)
      return -1;
  }
  if (rt_operation_generate_token(token, sizeof(token)) != 0 ||
      rt_operation_bind_secret(op_id, token) != 0)
    return -1;
  snprintf(p->op_id, sizeof(p->op_id), "%s", op_id);
  snprintf(p->op_token, sizeof(p->op_token), "%s", token);
  return 0;
}

/* One terminalization path for in-run causes (immediate finished,
 * immediate failure/rejection, and start-of-work echoes). Appends the
 * canonical self-sourced operation_result inside the current run; when
 * `deliver` is set and the record has a channel, the publication outbox
 * carries the correlated bus wake that opens the one result run.
 *
 * Bus-completion and timeout claims do not pass through here — they are
 * admitted at intake and their canonical operation_result is the first
 * event of the result run itself. All causes share the same gate: the
 * store's reducer flip, checked here and at admission. */
static void terminalize_in_run(RtRun *r, const char *op_id,
                               const char *status, const char *text,
                               int deliver) {
  RtOperationSnapshot op;
  char eh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED], estatus[RT_MED];
  char corr[RT_LARGE], source_event_id[RT_SMALL], esource[RT_MED];
  char *large = NULL;
  char *etext = NULL, *payload = NULL, *wake = NULL;
  int n;
  if (rt_operation_snapshot(op_id, &op) != 0) return;
  if (strcmp(op.status, "finished") == 0) return; /* exactly once */
  large = (char *)fc_xmalloc(RT_EVENT_PAYLOAD_MAX * 2U + BUS_PAYLOAD_MAX);
  if (!large) {
    (void)rt_emit_error(&r->ctx, "kernel",
                        "managed operation result allocation failed");
    return;
  }
  etext = large;
  payload = etext + RT_EVENT_PAYLOAD_MAX;
  wake = payload + RT_EVENT_PAYLOAD_MAX;
  if (json_escape(op.handle, eh, sizeof(eh)) != 0 ||
      json_escape(op.action, ea, sizeof(ea)) != 0 ||
      json_escape(op.request_id, erid, sizeof(erid)) != 0 ||
      json_escape(op.trigger_event_id, etrigger, sizeof(etrigger)) != 0 ||
      json_escape(status ? status : "succeeded", estatus, sizeof(estatus)) != 0 ||
      json_escape(text ? text : "", etext, RT_EVENT_PAYLOAD_MAX) != 0 ||
      rt_next_event_id(&r->ctx, source_event_id,
                       sizeof(source_event_id)) != 0 ||
      json_escape(source_event_id, esource, sizeof(esource)) != 0 ||
      correlation_fields_from_op(&op, corr, sizeof(corr)) != 0)
    goto done;
  n = snprintf(
          payload, RT_EVENT_PAYLOAD_MAX,
          "{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
          "\"trigger_event_id\":\"%s\",\"work_rev\":%lld,"
          "\"source_event_id\":\"%s\",%s,\"status\":\"%s\",\"text\":\"%s\"}",
          eh, ea, erid, etrigger, op.work_rev, esource, corr, estatus, etext);
  if (n < 0 || n >= RT_EVENT_PAYLOAD_MAX) {
    (void)rt_emit_error(&r->ctx, "kernel",
                        "managed operation result exceeds %d-byte event payload cap",
                        RT_EVENT_PAYLOAD_MAX);
    goto done;
  }
  if (deliver && op.channel[0]) {
    char etask[RT_MED], econtext[RT_MED], echannel[RT_MED];
    char eorigin[RT_MED], eadapter[RT_MED];
    const char *ref = op.ref_json[0] && strlen(op.ref_json) <= 512
                          ? op.ref_json : "null";
    if (json_escape(op.task_id, etask, sizeof(etask)) != 0 ||
        json_escape(op.context_id, econtext, sizeof(econtext)) != 0 ||
        json_escape(op.channel, echannel, sizeof(echannel)) != 0 ||
        json_escape(op.origin_event_id, eorigin, sizeof(eorigin)) != 0 ||
        json_escape(op.adapter_id, eadapter, sizeof(eadapter)) != 0)
      goto done;
    n = snprintf(
        wake, BUS_PAYLOAD_MAX,
        "{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
        "\"trigger_event_id\":\"%s\",\"task_id\":\"%s\","
        "\"work_rev\":%lld,\"source_event_id\":\"%s\","
        "\"context_id\":\"%s\",\"channel\":\"%s\",\"status\":\"%s\","
        "\"text\":\"%s\",\"origin_event_id\":\"%s\","
        "\"adapter_id\":\"%s\",\"ref\":%s}",
        eh, ea, erid, etrigger, etask, op.work_rev,
        esource, econtext, echannel, estatus, etext, eorigin, eadapter,
        ref);
    if (n < 0 || n >= BUS_PAYLOAD_MAX) {
      (void)rt_emit_error(&r->ctx, "kernel",
                          "managed operation result exceeds %d-byte bus payload cap",
                          BUS_PAYLOAD_MAX);
      goto done;
    }
    if (rt_publication_outbox_prepare(
            &r->ctx, source_event_id, "operation_result", payload,
            op.channel, "operation_result", wake, NULL, 0) != 0)
      goto done;
  }
  /* Append marks the store terminal via the reducer, closing the
   * exactly-once gate before the bus copy exists. */
  if (rt_append_event_sync(&r->ctx, "operation_result", "kernel",
                           payload) != 0)
    goto done;
  update_task_external(r, op.task_id, op.handle, op.action,
                       "finished", op.work_rev, text ? text : "");
  (void)rt_narrate("worker finished: %s (%s) — %.140s", op.handle,
                   status ? status : "succeeded",
                   text && text[0] ? text : "(no text)");
  if (deliver && op.channel[0]) (void)rt_publication_outbox_reconcile();
done:
  fc_xfree(large);
}

/* Post-admission hook for a bus completion/timeout claim: the canonical
 * operation_result is already the result run's first event (the reducer
 * has flipped the store); project the terminal into the owning task and
 * narrate. Called from intake after the claim run's first behavioral
 * append commits. */
void rt_operation_on_claim_committed(RtRun *r, const char *op_id,
                                     const char *status,
                                     const char *excerpt) {
  RtOperationSnapshot op;
  if (!r || !op_id || rt_operation_snapshot(op_id, &op) != 0) return;
  update_task_external(r, op.task_id, op.handle, op.action,
                       "finished", op.work_rev, excerpt ? excerpt : "");
  (void)rt_narrate("worker finished: %s (%s) — %.140s", op.handle,
                   status ? status : "?",
                   excerpt && excerpt[0] ? excerpt : "(no text)");
}

/* Publish one operation_completed claim through the ordinary bus. Safe
 * from detached worker processes and from the gateway alike: a durable
 * inbox envelope plus the disposable wake knock, no gateway required.
 * Duplicate submissions mint fresh envelopes; idempotent admission and
 * the terminal gate absorb them. */
int rt_operation_publish_worker_claim(const char *operation_id,
                                      const char *token,
                                      const char *status,
                                      const char *text) {
  char envelope_id[BUS_ID_MAX];
  char eop[RT_MED], etoken[RT_OPERATION_TOKEN_HEX * 2 + 2], estatus[RT_MED];
  char bounded[RT_OPERATION_RESULT_TEXT_MAX + 1];
  char *etext = NULL, *payload = NULL;
  int n, rc = -1;
  if (!operation_id || !*operation_id || !token || !*token ||
      !status || !*status)
    return -1;
  /* Result payload overflow fails loudly at the boundary; it must never
   * truncate silently into a smaller canonical result. */
  if (text && strlen(text) > RT_OPERATION_RESULT_TEXT_MAX) return -1;
  snprintf(bounded, sizeof(bounded), "%s", text ? text : "");
  etext = (char *)fc_xmalloc(RT_EVENT_PAYLOAD_MAX + BUS_PAYLOAD_MAX);
  if (!etext) return -1;
  payload = etext + RT_EVENT_PAYLOAD_MAX;
  if (json_escape(operation_id, eop, sizeof(eop)) != 0 ||
      json_escape(token, etoken, sizeof(etoken)) != 0 ||
      json_escape(status, estatus, sizeof(estatus)) != 0 ||
      json_escape(bounded, etext, RT_EVENT_PAYLOAD_MAX) != 0)
    goto done;
  n = snprintf(payload, BUS_PAYLOAD_MAX,
               "{\"operation_id\":\"%s\",\"completion_token\":\"%s\","
               "\"cause\":\"worker\",\"result\":{\"status\":\"%s\","
               "\"text\":\"%s\"}}",
               eop, etoken, estatus, etext);
  if (n < 0 || n >= BUS_PAYLOAD_MAX) goto done;
  if (bus_reserve_envelope_id(envelope_id, sizeof(envelope_id)) != 0)
    goto done;
  rc = bus_publish_stable(envelope_id, OP_CLAIM_CHANNEL,
                          "operation_completed", payload);
done:
  fc_xfree(etext);
  return rc;
}

/* Abort path: a runtime-side caller that only knows the worker's own
 * identity terminates the operation through the same claim admission as
 * any worker completion. Trusted-caller only (the store's token is read
 * back); never reachable from model-authored input. */
int rt_operation_submit_abort_claim(const char *worker_handle,
                                    const char *text) {
  char op_id[RT_SMALL];
  char token[RT_OPERATION_TOKEN_HEX + 1] = "";
  if (rt_operation_find_running_by_worker_handle(worker_handle, op_id,
                                                 sizeof(op_id)) != 0)
    return 1;
  if (rt_operation_token_of(op_id, token, sizeof(token)) != 0 || !token[0])
    return -1;
  return rt_operation_publish_worker_claim(op_id, token, "failed", text);
}

void rt_operation_driver_on_terminal(RtRun *r, const char *type, const char *action,
                                     const char *request_id,
                                     const char *trigger_event_id,
                                     const char *task_id, long long work_rev,
                                     const char *args_json,
                                     const char *result_json) {
  const RtActionDef *def;
  char op_id[RT_SMALL];
  char stored[RT_SMALL] = "";
  int lookup;
  (void)task_id;
  (void)args_json;
  if (!r || !r->scheduler || !action || !*action ||
      !request_id || !*request_id)
    return;
  def = rt_action_registry_find(&r->scheduler->actions, action);
  if (!def || !def->managed_operation) return;
  if (rt_operation_id_for_request(request_id, op_id, sizeof(op_id)) != 0)
    return;
  lookup = rt_operation_status(op_id, stored, sizeof(stored));
  if (strcmp(type, "action_failed") == 0 ||
      strcmp(type, "action_rejected") == 0) {
    char text[OP_FAILURE_TEXT_MAX + 1];
    if (lookup == 0 && strcmp(stored, "finished") == 0) return;
    if ((trigger_event_id && *trigger_event_id) || work_rev > 0) {
      /* The work-step wake (or the bound controller's own repair loop)
       * owns this failure's consequence. The action terminal is the
       * behavioral truth; closing any preallocated record is view
       * maintenance so the deadline can never fire for it. */
      if (lookup == 0) (void)rt_operation_close_without_result(op_id);
      return;
    }
    if (lookup == 1) {
      /* Rejected or failed before dispatch ever allocated (validation
       * boundary, unknown action, launch failure). Allocate now so the
       * failure still returns to the floop as one ordinary result run. */
      int pidx = rt_action_runner_pending_index(r, request_id);
      if (pidx < 0 ||
          rt_operation_allocate_for_call(
              r, def, &r->pending_actions[pidx]) != 0 ||
          rt_operation_status(op_id, stored, sizeof(stored)) != 0)
        return;
    } else if (lookup != 0) {
      return;
    }
    failure_text_excerpt(action, type, result_json, text, sizeof(text));
    terminalize_in_run(r, op_id, "failed", text, 1);
    return;
  }
  if (lookup != 0) return; /* action_cli transport or allocation failure */
  if (strcmp(type, "action_succeeded") != 0) return;
  {
    JsonRef result;
    char status[RT_SMALL] = "", worker_handle[RT_SMALL] = "";
    if (!result_json || json_ref_first_object(result_json, &result) != 0) return;
    if (json_ref_object_get_string(&result, "status", status, sizeof(status)) != 0)
      return; /* not speaking the contract this call — nothing to drive */
    if (strcmp(status, "running") == 0) {
      if (strcmp(stored, "finished") == 0) return; /* stale echo */
      if (json_ref_object_get_string(&result, "handle", worker_handle,
                                     sizeof(worker_handle)) != 0)
        (void)json_ref_object_get_string(&result, "op_id", worker_handle,
                                         sizeof(worker_handle));
      (void)rt_operation_set_worker_handle(op_id, worker_handle);
      {
        RtOperationSnapshot op;
        if (rt_operation_snapshot(op_id, &op) == 0) {
          update_task_external(r, op.task_id, op.handle, op.action,
                               "running", op.work_rev, "");
          (void)rt_narrate("worker started: %s via %s (worker %s, task %s, rev %lld)",
                           op.handle, op.action,
                           worker_handle[0] ? worker_handle : "?",
                           op.task_id[0] ? op.task_id : "?", op.work_rev);
        }
      }
      return;
    }
    if (strcmp(status, "finished") == 0) {
      char excerpt[RT_OPERATION_RESULT_TEXT_MAX + 1];
      char state[RT_SMALL] = "";
      const char *result_status = "succeeded";
      if (strcmp(stored, "finished") == 0) return; /* exactly once */
      /* An action that reports its own terminal state (codex/claude
       * result shape) keeps it; otherwise a successful finished result
       * is a success. The runtime still never reads meaning from text. */
      if (json_ref_object_get_string(&result, "state", state,
                                     sizeof(state)) == 0 &&
          strcmp(state, "failed") == 0)
        result_status = "failed";
      result_text_excerpt(&result, excerpt, sizeof(excerpt));
      terminalize_in_run(r, op_id, result_status, excerpt, 1);
      return;
    }
    /* Unknown status value: contract violation, say so loudly. */
    (void)rt_emit_error(&r->ctx, "kernel",
                        "managed operation %s returned unknown status", action);
  }
}
