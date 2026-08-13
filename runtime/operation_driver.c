/* Operation driver — the kernel's managed-operation lifecycle service.
 *
 * Observes terminal results of calls whose action declares the
 * managed-operation contract in its action.json:
 *
 *   start(args)  -> running(handle) | finished(result)
 *   poll(handle) -> running         | finished(result)
 *
 * and owns exactly three payload-opaque behaviors:
 *
 *   record   — a first running(handle) appends operation_started with
 *              full correlation and projects it into the task's
 *              state.external slot together with the work_rev the
 *              attempt started from;
 *   schedule — while an operation runs, at most one poll timer per
 *              handle (next_poll_at_ms in the operation store); a
 *              timer that fires after terminal is a no-op;
 *   publish  — a finished(result) appends exactly one operation_result
 *              per handle (guarded by the store's terminal state) and
 *              publishes it as a correlated bus event, so it enters
 *              the floop through normal intake as a brand-new run.
 *
 * The driver dispatches on the DECLARED contract, never on an action
 * or handler name, and never reads meaning out of result text. */

#include "run_internal.h"
#include "bus/bus.h"
#include "publication_outbox.h"
#include "support/heap_guard.h"
#include "support/json.h"
#include "support/timing.h"

#include <stdio.h>
#include <string.h>

#define OP_DEFAULT_POLL_INTERVAL_MS 5000
#define OP_FAILURE_TEXT_MAX 1800
#define OP_EXTERNAL_EXCERPT_MAX 500

_Static_assert(BUS_PAYLOAD_MAX == RT_EVENT_PAYLOAD_MAX,
               "correlated event and bus payload caps must match");

static long long op_poll_interval(const RtRun *r) {
  if (r && r->profile && r->profile->poll_interval_ms > 0)
    return r->profile->poll_interval_ms;
  return OP_DEFAULT_POLL_INTERVAL_MS;
}

/* Contract fields. `handle` accepts the legacy `op_id` spelling so the
 * existing adapters satisfy the contract without breaking their old
 * consumers. Everything else in the result stays opaque. */
static int result_handle(const JsonRef *obj, char *out, size_t out_len) {
  if (json_ref_object_get_string(obj, "handle", out, out_len) == 0 && out[0]) return 0;
  if (json_ref_object_get_string(obj, "op_id", out, out_len) == 0 && out[0]) return 0;
  return -1;
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
 * external-only update never bumps work_rev. */
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
    /* A stale immediate result must not replace a newer revision's
     * projection. A running older attempt that already owns the projection
     * may, however, record its own terminal state: the legacy fixed worker
     * uses that exact (handle, revision) transition to start the newer
     * revision once. */
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

/* Correlation block from the current run's origin. The initial start
 * happens inside the user-facing run, so the origin route here IS the
 * user's delivery route; poll/result runs restore the same route from
 * the operation envelope at intake. */
static int correlation_fields(RtRun *r, const char *task_id,
                              char *out, size_t out_len) {
  char ectx[RT_MED], ech[RT_MED], ead[RT_MED], etask[RT_MED], eorigin[RT_MED];
  int n;
  if (json_escape(r->ctx.context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(r->ctx.origin_channel, ech, sizeof(ech)) != 0 ||
      json_escape(r->ctx.origin_adapter_id, ead, sizeof(ead)) != 0 ||
      json_escape(task_id ? task_id : "", etask, sizeof(etask)) != 0 ||
      json_escape(r->ctx.origin_event_id, eorigin, sizeof(eorigin)) != 0) return -1;
  n = snprintf(out, out_len,
               "\"task_id\":\"%s\",\"context_id\":\"%s\",\"channel\":\"%s\","
               "\"adapter_id\":\"%s\",\"origin_event_id\":\"%s\",\"ref\":%s",
               etask, ectx, ech, ead, eorigin,
               r->ctx.origin_ref_json[0] ? r->ctx.origin_ref_json : "null");
  return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

static void record_started(RtRun *r, const char *action, const char *task_id,
                           const char *request_id,
                           const char *trigger_event_id,
                           long long selected_work_rev,
                           const char *handle, long long now, long long interval) {
  char eh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED];
  char corr[RT_LARGE], payload[RT_LARGE];
  long long work_rev = selected_work_rev;
  if (work_rev <= 0) (void)rt_task_work_rev_of(task_id, &work_rev);
  if (json_escape(handle, eh, sizeof(eh)) != 0 ||
      json_escape(action ? action : "", ea, sizeof(ea)) != 0 ||
      json_escape(request_id ? request_id : "", erid, sizeof(erid)) != 0 ||
      json_escape(trigger_event_id ? trigger_event_id : "",
                  etrigger, sizeof(etrigger)) != 0 ||
      correlation_fields(r, task_id, corr, sizeof(corr)) != 0) return;
  if (rt_append_event_format(
          &r->ctx, "operation_started", "kernel", payload, sizeof(payload), 1,
          "{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
          "\"trigger_event_id\":\"%s\",%s,"
          "\"work_rev\":%lld,\"poll_interval_ms\":%lld,\"next_poll_at_ms\":%lld}",
          eh, ea, erid, etrigger, corr, work_rev,
          interval, now + interval) != 0)
    return;
  update_task_external(r, task_id, handle, action, "running", work_rev, "");
  (void)rt_narrate("worker started: %s via %s (task %s, rev %lld)",
                   handle, action ? action : "?", task_id ? task_id : "?", work_rev);
}

static void block_missing_poll_metadata(RtRun *r, const RtActionDef *def,
                                        const char *task_id,
                                        const char *request_id,
                                        const char *trigger_event_id,
                                        long long selected_work_rev,
                                        const char *handle) {
  RtOperationSnapshot op;
  char reason_text[RT_LARGE], needed[RT_LARGE];
  char *large = (char *)fc_xcalloc(3U, RT_XL);
  char *error_json, *ereason, *task_json;
  long long work_rev = selected_work_rev;
  if (!large) return;
  error_json = large;
  ereason = large + RT_XL;
  task_json = large + RT_XL * 2U;
  memset(&op, 0, sizeof(op));
  if (work_rev <= 0) (void)rt_task_work_rev_of(task_id, &work_rev);
  snprintf(op.handle, sizeof(op.handle), "%s", handle ? handle : "");
  snprintf(op.action, sizeof(op.action), "%s", def ? def->id : "");
  snprintf(op.request_id, sizeof(op.request_id), "%s",
           request_id ? request_id : "");
  snprintf(op.trigger_event_id, sizeof(op.trigger_event_id), "%s",
           trigger_event_id ? trigger_event_id : "");
  snprintf(op.task_id, sizeof(op.task_id), "%s", task_id ? task_id : "");
  snprintf(op.context_id, sizeof(op.context_id), "%s", r->ctx.context_id);
  snprintf(op.channel, sizeof(op.channel), "%s", r->ctx.origin_channel);
  snprintf(op.adapter_id, sizeof(op.adapter_id), "%s",
           r->ctx.origin_adapter_id);
  snprintf(op.origin_event_id, sizeof(op.origin_event_id), "%s",
           r->ctx.origin_event_id);
  snprintf(op.ref_json, sizeof(op.ref_json), "%s",
           r->ctx.origin_ref_json[0] ? r->ctx.origin_ref_json : "null");
  snprintf(op.status, sizeof(op.status), "running");
  op.work_rev = work_rev;
  snprintf(reason_text, sizeof(reason_text),
           "managed operation %s returned status=running but %s omits "
           "poll_handle_arg",
           def ? def->id : "(unknown)",
           def && def->manifest_path[0] ? def->manifest_path : "its action.json");
  snprintf(needed, sizeof(needed),
           "fix: add \"poll_handle_arg\":\"op_id\" (or the action's string "
           "handle property) to %s",
           def && def->manifest_path[0] ? def->manifest_path : "action.json");
  (void)rt_emit_error(&r->ctx, "kernel", "%s; %s", reason_text, needed);
  if (work_rev > 0 &&
      rt_task_work_binding_json(r->ctx.context_id, task_id, work_rev, 0,
                                task_json, RT_XL) == 0 &&
      json_escape(reason_text, ereason, RT_XL) == 0) {
    snprintf(error_json, RT_XL,
             "{\"message\":\"%s\"}", ereason);
    (void)rt_task_append_action_failure(&r->ctx, def ? def->id : "",
                                        task_id, work_rev,
                                        "missing_poll_handle_arg",
                                        error_json);
  }
  (void)rt_task_block_unpollable_operation(&r->ctx, &op,
                                           reason_text, needed);
  fc_xfree(large);
}

static void publish_result(RtRun *r, const char *action, const char *task_id,
                           const char *request_id,
                           const char *trigger_event_id,
                           long long selected_work_rev,
                           const char *handle, const JsonRef *result) {
  RtOperationSnapshot op;
  char eh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED];
  char corr[RT_LARGE], source_event_id[RT_SMALL], esource[RT_MED];
  char *large = NULL;
  char *excerpt = NULL, *etext = NULL, *payload = NULL, *wake = NULL;
  char channel[RT_SMALL] = "";
  const char *original_request = request_id ? request_id : "";
  const char *original_trigger = trigger_event_id ? trigger_event_id : "";
  const char *resolved_action = action ? action : "";
  const char *resolved_task = task_id ? task_id : "";
  const char *resolved_context = r->ctx.context_id;
  const char *resolved_origin = r->ctx.origin_event_id;
  const char *resolved_adapter = r->ctx.origin_adapter_id;
  const char *resolved_ref =
      r->ctx.origin_ref_json[0] ? r->ctx.origin_ref_json : "null";
  long long work_rev = 0;
  int have_op = rt_operation_snapshot(handle, &op) == 0;
  int n;
  large = (char *)fc_xmalloc(
      RT_OPERATION_RESULT_TEXT_MAX + 1U + RT_EVENT_PAYLOAD_MAX * 2U +
      BUS_PAYLOAD_MAX);
  if (!large) {
    (void)rt_emit_error(&r->ctx, "kernel",
                        "managed operation result allocation failed");
    goto done;
  }
  excerpt = large;
  etext = excerpt + RT_OPERATION_RESULT_TEXT_MAX + 1U;
  payload = etext + RT_EVENT_PAYLOAD_MAX;
  wake = payload + RT_EVENT_PAYLOAD_MAX;
  result_text_excerpt(result, excerpt, RT_OPERATION_RESULT_TEXT_MAX + 1U);
  /* The attempt record keeps the revision the attempt STARTED from —
   * that is what the attempt rule compares against the task's current
   * revision. A start-that-finished-immediately has no record, so it
   * uses the trusted pending revision; legacy unbound callers alone
   * fall back to the task's current revision. */
  if (have_op) {
    work_rev = op.work_rev;
    original_request = op.request_id;
    original_trigger = op.trigger_event_id;
    resolved_action = op.action;
    resolved_task = op.task_id;
    resolved_context = op.context_id;
    resolved_origin = op.origin_event_id;
    resolved_adapter = op.adapter_id;
    resolved_ref = op.ref_json[0] ? op.ref_json : "null";
    snprintf(channel, sizeof(channel), "%s", op.channel);
  } else if (rt_operation_work_rev_of(handle, &work_rev) != 0 ||
             work_rev <= 0) {
    work_rev = selected_work_rev;
    if (work_rev <= 0) (void)rt_task_work_rev_of(task_id, &work_rev);
  }
  if (json_escape(handle, eh, sizeof(eh)) != 0 ||
      json_escape(resolved_action, ea, sizeof(ea)) != 0 ||
      json_escape(original_request, erid, sizeof(erid)) != 0 ||
      json_escape(original_trigger, etrigger, sizeof(etrigger)) != 0 ||
      json_escape(excerpt, etext, RT_EVENT_PAYLOAD_MAX) != 0 ||
      rt_next_event_id(&r->ctx, source_event_id,
                       sizeof(source_event_id)) != 0 ||
      json_escape(source_event_id, esource, sizeof(esource)) != 0)
    goto done;
  if (have_op) {
    char ectx[RT_MED], ech[RT_MED], ead[RT_MED], etask[RT_MED];
    char eorigin[RT_MED];
    if (json_escape(op.context_id, ectx, sizeof(ectx)) != 0 ||
        json_escape(op.channel, ech, sizeof(ech)) != 0 ||
        json_escape(op.adapter_id, ead, sizeof(ead)) != 0 ||
        json_escape(op.task_id, etask, sizeof(etask)) != 0 ||
        json_escape(op.origin_event_id, eorigin, sizeof(eorigin)) != 0 ||
        snprintf(corr, sizeof(corr),
                 "\"task_id\":\"%s\",\"context_id\":\"%s\","
                 "\"channel\":\"%s\",\"adapter_id\":\"%s\","
                 "\"origin_event_id\":\"%s\",\"ref\":%s",
                 etask, ectx, ech, ead, eorigin, resolved_ref) >=
            (int)sizeof(corr))
      goto done;
  } else if (correlation_fields(r, resolved_task, corr,
                                sizeof(corr)) != 0) {
    goto done;
  }
  n = snprintf(
          payload, RT_EVENT_PAYLOAD_MAX,
          "{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
          "\"trigger_event_id\":\"%s\",\"work_rev\":%lld,"
          "\"source_event_id\":\"%s\",%s,\"text\":\"%s\"}",
          eh, ea, erid, etrigger, work_rev, esource, corr, etext);
  if (n < 0 || n >= RT_EVENT_PAYLOAD_MAX) {
    (void)rt_emit_error(&r->ctx, "kernel",
                        "managed operation result exceeds %d-byte event payload cap",
                        RT_EVENT_PAYLOAD_MAX);
    goto done;
  }
  if (!channel[0]) {
    if (r->ctx.origin_channel[0])
      snprintf(channel, sizeof(channel), "%s", r->ctx.origin_channel);
    else
      (void)rt_operation_channel_of(handle, channel, sizeof(channel));
  }
  {
    char etask[RT_MED], econtext[RT_MED], echannel[RT_MED];
    char eorigin[RT_MED], eadapter[RT_MED];
    if (json_escape(resolved_task, etask, sizeof(etask)) != 0 ||
        json_escape(resolved_context, econtext, sizeof(econtext)) != 0 ||
        json_escape(channel, echannel, sizeof(echannel)) != 0 ||
        json_escape(resolved_origin, eorigin, sizeof(eorigin)) != 0 ||
        json_escape(resolved_adapter, eadapter,
                    sizeof(eadapter)) != 0)
      goto done;
    n = snprintf(
        wake, BUS_PAYLOAD_MAX,
        "{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
        "\"trigger_event_id\":\"%s\",\"task_id\":\"%s\","
        "\"work_rev\":%lld,\"source_event_id\":\"%s\","
        "\"context_id\":\"%s\",\"channel\":\"%s\","
        "\"text\":\"%s\",\"origin_event_id\":\"%s\","
        "\"adapter_id\":\"%s\",\"ref\":%s}",
        eh, ea, erid, etrigger, etask, work_rev,
        esource, econtext, echannel, etext, eorigin, eadapter,
        resolved_ref[0] && strlen(resolved_ref) <= 512
            ? resolved_ref : "null");
    if (n < 0 || n >= BUS_PAYLOAD_MAX) {
      (void)rt_emit_error(&r->ctx, "kernel",
                          "managed operation result exceeds %d-byte bus payload cap",
                          BUS_PAYLOAD_MAX);
      goto done;
    }
  }
  if (channel[0] &&
      rt_publication_outbox_prepare(
          &r->ctx, source_event_id, "operation_result", payload,
          channel, "operation_result", wake, NULL, 0) != 0)
    goto done;
  /* Append marks the store terminal via the reducer, closing the
   * exactly-once gate before the bus copy exists. */
  if (rt_append_event_sync(&r->ctx, "operation_result", "kernel",
                           payload) != 0)
    goto done;
  update_task_external(r, resolved_task, handle, resolved_action,
                       "finished", work_rev, excerpt);
  (void)rt_narrate("worker finished: %s — %.140s", handle,
                   excerpt[0] ? excerpt : "(no text)");
  if (channel[0]) (void)rt_publication_outbox_reconcile();
done:
  fc_xfree(large);
}

static void publish_failure_result(RtRun *r, const char *type,
                                   const char *action, const char *task_id,
                                   const char *request_id,
                                   const char *trigger_event_id,
                                   long long work_rev,
                                   const char *detail_json) {
  char handle[RT_SMALL], stored[RT_SMALL] = "";
  char text[OP_FAILURE_TEXT_MAX + 1], ehandle[RT_MED], etext[RT_LARGE];
  char result_json[RT_LARGE * 2];
  JsonRef result;
  if (!r || !request_id || !*request_id || work_rev > 0 ||
      (trigger_event_id && *trigger_event_id))
    return;
  snprintf(handle, sizeof(handle), "fail_%.122s", request_id);
  if (rt_operation_status(handle, stored, sizeof(stored)) == 0 &&
      strcmp(stored, "finished") == 0)
    return;
  failure_text_excerpt(action, type, detail_json, text, sizeof(text));
  if (json_escape(handle, ehandle, sizeof(ehandle)) != 0 ||
      json_escape(text, etext, sizeof(etext)) != 0 ||
      snprintf(result_json, sizeof(result_json),
               "{\"status\":\"finished\",\"handle\":\"%s\","
               "\"text\":\"%s\"}", ehandle, etext) >=
          (int)sizeof(result_json) ||
      json_ref_first_object(result_json, &result) != 0)
    return;
  publish_result(r, action, task_id, request_id, trigger_event_id, work_rev,
                 handle, &result);
}

void rt_operation_driver_on_terminal(RtRun *r, const char *type, const char *action,
                                     const char *request_id,
                                     const char *trigger_event_id,
                                     const char *task_id, long long work_rev,
                                     const char *args_json,
                                     const char *result_json) {
  const RtActionDef *def;
  long long now, interval;
  if (!r || !r->scheduler || !action || !*action) return;
  /* The driver is enabled per floop by configuring poll_interval_ms in
   * the profile. Floops that manage external operations through their
   * own phases leave it
   * unset and the driver stays inert for them. */
  if (!r->profile || r->profile->poll_interval_ms <= 0) return;
  def = rt_action_registry_find(&r->scheduler->actions, action);
  if (!def || !def->managed_operation) return;
  now = (long long)timing_stable_wall_ms();
  interval = op_poll_interval(r);
  if (strcmp(type, "action_failed") == 0) {
    /* The adapter call itself failed (crash, timeout). The external
     * operation may still be alive: keep polling so a transient
     * adapter fault cannot strand a running operation. The failure is
     * already recorded as a normal action terminal. */
    JsonRef args;
    char handle[RT_SMALL] = "";
    char status[RT_SMALL] = "";
    if (args_json && json_ref_first_object(args_json, &args) == 0 &&
        result_handle(&args, handle, sizeof(handle)) == 0 &&
        rt_operation_status(handle, status, sizeof(status)) == 0 &&
        strcmp(status, "running") == 0) {
      (void)rt_operation_arm_poll(handle, now + interval);
      return;
    }
    publish_failure_result(r, type, action, task_id, request_id,
                           trigger_event_id, work_rev, result_json);
    return;
  }
  if (strcmp(type, "action_rejected") == 0) {
    publish_failure_result(r, type, action, task_id, request_id,
                           trigger_event_id, work_rev, result_json);
    return;
  }
  if (strcmp(type, "action_succeeded") != 0) return;
  {
    JsonRef result;
    char status[RT_SMALL] = "", handle[RT_SMALL] = "";
    char stored[RT_SMALL] = "";
    int lookup;
    if (!result_json || json_ref_first_object(result_json, &result) != 0) return;
    if (json_ref_object_get_string(&result, "status", status, sizeof(status)) != 0)
      return; /* not speaking the contract this call — nothing to drive */
    if (result_handle(&result, handle, sizeof(handle)) != 0) {
      (void)rt_emit_error(&r->ctx, "kernel",
                          "managed operation result from %s carries no handle", action);
      return;
    }
    lookup = rt_operation_status(handle, stored, sizeof(stored));
    if (strcmp(status, "running") == 0) {
      if (lookup == 1 && !def->poll_handle_arg[0]) {
        block_missing_poll_metadata(r, def, task_id, request_id,
                                    trigger_event_id, work_rev, handle);
      } else if (lookup == 1) {
        record_started(r, action, task_id, request_id, trigger_event_id,
                       work_rev, handle, now, interval);
      } else if (lookup == 0 && strcmp(stored, "running") == 0) {
        (void)rt_operation_arm_poll(handle, now + interval);
      }
      /* running-after-finished is a stale adapter echo: ignore. */
      return;
    }
    if (strcmp(status, "finished") == 0) {
      if (lookup == 0 && strcmp(stored, "finished") == 0) return; /* exactly once */
      publish_result(r, action, task_id, request_id, trigger_event_id,
                     work_rev, handle, &result);
      return;
    }
    /* Unknown status value: contract violation, say so loudly. */
    (void)rt_emit_error(&r->ctx, "kernel",
                        "managed operation %s returned unknown status", action);
  }
}
