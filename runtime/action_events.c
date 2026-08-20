/* Action terminal-event emission.
 *
 * The "say it on the event log" half of the action runner. Builds the
 * action_started / action_succeeded / action_failed / action_rejected
 * payloads, attaches delivery records for the message action, projects
 * records failed-action diagnostics, and updates pending-queue bookkeeping
 * via rt_action_runner_note_terminal. */

#include "action_internal.h"
#include "ports.h"
#include "publication_outbox.h"
#include "support/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rt_action_emit_started(RtRun *r, const char *rid, const char *action,
                          const char *job_id, const char *task_id) {
  char erid[RT_MED], eaction[RT_MED], ejob[RT_MED], etask[RT_MED];
  char bind[RT_LARGE] = "";
  char payload[RT_LARGE];
  int pidx = rt_action_runner_pending_index(r, rid);
  json_escape(rid ? rid : "", erid, sizeof(erid));
  json_escape(action ? action : "", eaction, sizeof(eaction));
  json_escape(job_id ? job_id : "", ejob, sizeof(ejob));
  json_escape(task_id ? task_id : "", etask, sizeof(etask));
  if (pidx >= 0 && r->pending_actions[pidx].trigger_event_id[0]) {
    char ectx[RT_LARGE], etrigger[RT_MED];
    if (json_escape(r->ctx.context_id, ectx, sizeof(ectx)) == 0 &&
        json_escape(r->pending_actions[pidx].trigger_event_id,
                    etrigger, sizeof(etrigger)) == 0)
      snprintf(bind, sizeof(bind),
               ",\"work_rev\":%lld,\"context_id\":\"%s\","
               "\"trigger_event_id\":\"%s\"",
               r->pending_actions[pidx].work_rev, ectx, etrigger);
  }
  if (snprintf(
          payload, sizeof(payload),
          "{\"request_id\":\"%s\",\"action\":\"%s\",\"run_id\":\"%s\","
          "\"job_id\":%s%s%s%s%s%s%s}",
          erid, eaction, r->ctx.run_id,
          job_id && *job_id ? "\"" : "", job_id && *job_id ? ejob : "null",
          job_id && *job_id ? "\"" : "",
          task_id && *task_id ? ",\"task_id\":\"" : "",
          task_id && *task_id ? etask : "",
          task_id && *task_id ? "\"" : "", bind) >= (int)sizeof(payload))
    return -1;
  if (rt_append_event_sync(&r->ctx, "action_started", "action_runner",
                           payload) != 0)
    return -1;
  rt_action_runner_note_started(r, rid);
  return 0;
}

/* 1 valid managed continuation, 0 not a managed terminal. Operation
 * identity is runtime-owned and preallocated at launch, so the result's
 * contract fields are only a shape check here: `handle` in a result is
 * the worker's own metadata, never durable identity. */
static int managed_contract_disposition(const RtActionDef *def,
                                        const RtPendingAction *pending,
                                        const char *type,
                                        const char *result_json) {
  JsonRef result;
  char status[RT_SMALL] = "";
  (void)pending;
  if (!def || !def->managed_operation ||
      strcmp(type ? type : "", "action_succeeded") != 0 ||
      !result_json || json_ref_top_object(result_json, &result) != 0 ||
      json_ref_object_get_string(&result, "status", status,
                                 sizeof(status)) != 0 ||
      (strcmp(status, "running") != 0 && strcmp(status, "finished") != 0))
    return 0;
  return 1;
}

static int prepare_step_wake(RtRun *r, const RtPendingAction *pending,
                             const char *source_event_id,
                             const char *source_type,
                             const char *source_payload,
                             const char *action,
                             const char *status,
                             const char *detail) {
  char etask[RT_MED], erid[RT_MED], eaction[RT_MED], esource[RT_MED];
  char eadapter[RT_MED], eorigin[RT_MED], edetail[RT_LARGE];
  char wake[4096];
  const char *ref = "null";
  int n;
  if (!pending || !pending->trigger_event_id[0] ||
      !r->ctx.origin_channel[0])
    return 0;
  if (r->ctx.origin_ref_json[0] && strlen(r->ctx.origin_ref_json) <= 512)
    ref = r->ctx.origin_ref_json;
  if (json_escape(pending->task_id, etask, sizeof(etask)) != 0 ||
      json_escape(pending->request_id, erid, sizeof(erid)) != 0 ||
      json_escape(action ? action : "", eaction, sizeof(eaction)) != 0 ||
      json_escape(source_event_id, esource, sizeof(esource)) != 0 ||
      json_escape(r->ctx.origin_adapter_id, eadapter, sizeof(eadapter)) != 0 ||
      json_escape(r->ctx.origin_event_id, eorigin, sizeof(eorigin)) != 0)
    return -1;
  {
    char bounded[385];
    snprintf(bounded, sizeof(bounded), "%.384s", detail ? detail : "");
    if (json_escape(bounded, edetail, sizeof(edetail)) != 0) return -1;
  }
  n = snprintf(
      wake, sizeof(wake),
      "{\"task_id\":\"%s\",\"work_rev\":%lld,"
      "\"request_id\":\"%s\",\"action\":\"%s\","
      "\"source_event_id\":\"%s\",\"status\":\"%s\","
      "\"detail\":\"%s\",\"text\":\"%s\","
      "\"origin_event_id\":\"%s\",\"adapter_id\":\"%s\",\"ref\":%s}",
      etask, pending->work_rev, erid, eaction, esource,
      status ? status : "", edetail, edetail, eorigin, eadapter, ref);
  if (n < 0 || (size_t)n >= sizeof(wake)) return -1;
  return rt_publication_outbox_prepare(
             &r->ctx, source_event_id, source_type, source_payload,
             r->ctx.origin_channel, "work_step_result", wake, NULL, 0) == 0
             ? 1 : -1;
}

int rt_action_emit_terminal(RtRun *r, const char *type, const char *rid,
                            const char *action, const char *job_id, const char *task_id,
                            const char *reason, const char *result_json, const char *error_json) {
  char delivery[RT_XL] = "", erid[RT_MED], eaction[RT_MED], ejob[RT_MED], ereason[RT_MED], etask[RT_MED];
  char bind[RT_LARGE] = "", continuation[RT_SMALL] = "";
  char source_event_id[RT_SMALL] = "";
  char payload[RT_XL];
  int append_rc, wake_prepared = 0;
  const char *emit_type = type;
  const char *emit_reason = reason;
  const char *emit_result = result_json;
  const char *emit_error = error_json;
  int ok = strcmp(type, "action_succeeded") == 0;
  int message_action = strcmp(action ? action : "", "message") == 0;
  int advances_work = !message_action &&
                      strcmp(action ? action : "",
                             "working_memory_append") != 0;
  int pidx = rt_action_runner_pending_index(r, rid);
  RtPendingAction *pending = pidx >= 0 ? &r->pending_actions[pidx] : NULL;
  const RtAgentMeta *source_meta = pidx >= 0 ? rt_action_agent_meta_lookup(r->scheduler, r->pending_actions[pidx].source_agent) : NULL;
  const RtActionDef *def =
      r && r->scheduler ? rt_action_registry_find(&r->scheduler->actions,
                                                   action ? action : "")
                        : NULL;
  json_escape(rid ? rid : "", erid, sizeof(erid)); json_escape(action ? action : "", eaction, sizeof(eaction));
  json_escape(job_id ? job_id : "", ejob, sizeof(ejob));
  json_escape(emit_reason ? emit_reason : "", ereason, sizeof(ereason));
  json_escape(task_id ? task_id : "", etask, sizeof(etask));
  if (pending && pending->trigger_event_id[0]) {
    char ectx[RT_LARGE], etrigger[RT_MED], esource[RT_MED];
    if (rt_next_event_id(&r->ctx, source_event_id,
                         sizeof(source_event_id)) != 0 ||
        json_escape(r->ctx.context_id, ectx, sizeof(ectx)) != 0 ||
        json_escape(pending->trigger_event_id, etrigger,
                    sizeof(etrigger)) != 0 ||
        json_escape(source_event_id, esource, sizeof(esource)) != 0)
      return -1;
    snprintf(bind, sizeof(bind),
             ",\"work_rev\":%lld,\"context_id\":\"%s\","
             "\"trigger_event_id\":\"%s\",\"source_event_id\":\"%s\"",
             pending->work_rev, ectx, etrigger, esource);
    if (ok && def && def->work_terminal)
      snprintf(continuation, sizeof(continuation), "work_terminal");
    else if (managed_contract_disposition(def, pending, type,
                                          result_json) > 0)
      snprintf(continuation, sizeof(continuation), "managed_operation");
  }
  if (strcmp(action ? action : "", "message") == 0 && ok) r->advance_delivered_message = 1;
  /* A memoized duplicate message is the SAME already-delivered message:
   * record the terminal, but never hand the adapter a second delivery. */
  if (strcmp(action ? action : "", "message") == 0 && r->ctx.origin_channel[0] && ok &&
      strcmp(reason ? reason : "", "memoized_duplicate") != 0) {
    char message_text[RT_LARGE] = "";
    JsonRef result_ref;
    if (result_json && json_ref_first_object(result_json, &result_ref) == 0)
      (void)json_ref_object_get_string(&result_ref, "message", message_text, sizeof(message_text));
    rt_ports_emit_message(r, rid, message_text, delivery, sizeof(delivery));
  }
  if (snprintf(
      payload, sizeof(payload),
      "{\"request_id\":\"%s\",\"action\":\"%s\",\"run_id\":\"%s\",\"job_id\":%s%s%s,"
      "%s%s%s%s%s%s\"result\":%s,\"error\":%s%s}",
      erid, eaction, r->ctx.run_id,
      job_id && *job_id ? "\"" : "", job_id && *job_id ? ejob : "null",
      job_id && *job_id ? "\"" : "",
      task_id && *task_id ? "\"task_id\":\"" : "",
      task_id && *task_id ? etask : "",
      task_id && *task_id ? "\"," : "",
      ok ? "" : "\"reason\":\"", ok ? "" : ereason, ok ? "" : "\",",
      emit_result && emit_result[0] ? emit_result : "{}",
      emit_error && emit_error[0] ? emit_error : "null",
      delivery) >= (int)sizeof(payload))
    return -1;
  if (bind[0]) {
    size_t len = strlen(payload);
    if (len == 0 || payload[len - 1] != '}' ||
        len - 1U + strlen(bind) +
                (continuation[0] ? strlen(continuation) + 20U : 0U) +
                2U >= sizeof(payload))
      return -1;
    payload[len - 1] = '\0';
    strcat(payload, bind);
    if (continuation[0]) {
      strcat(payload, ",\"continuation\":\"");
      strcat(payload, continuation);
      strcat(payload, "\"");
    }
    strcat(payload, "}");
  }
  if (pending && pending->trigger_event_id[0] && !continuation[0]) {
    const char *detail = ok ? emit_result
                           : (emit_error && strcmp(emit_error, "null") != 0
                                  ? emit_error : emit_reason);
    wake_prepared = prepare_step_wake(
        r, pending, source_event_id, emit_type, payload, action,
        ok ? "succeeded" : "failed", detail);
    if (wake_prepared < 0) {
      (void)rt_narrate("work-step wake preparation failed for %s", rid);
      return -1;
    }
  }
  append_rc = (pending && pending->trigger_event_id[0])
                  ? rt_append_event_sync(&r->ctx, emit_type, "action_runner",
                                         payload)
                  : rt_append_event(&r->ctx, emit_type, "action_runner",
                                    payload);
  if (append_rc != 0) return -1;
  if (!ok)
    (void)rt_task_append_action_failure(&r->ctx, action, task_id,
                                        pending ? pending->work_rev : 0,
                                        emit_reason, emit_error);
  if (message_action && ok && source_meta &&
      source_meta->autocomplete_message_task_on_send) {
    (void)rt_task_complete_message_on_send(&r->ctx, task_id);
  }
  if (task_id && *task_id && advances_work) r->advance_work_progress = 1;
  /* Managed-operation driver: reacts to terminals of contract-declaring
   * actions (record / schedule / publish). Dispatch is by declared
   * contract; the args snapshot is read before the pending entry is
   * retired below. */
  /* `action_cli` consumes the correlated action terminal synchronously.
   * Do not also turn an immediate managed-operation result into a second
   * operation_result run for an LLM agent. The action itself still travels
   * through the ordinary registry, validation, dispatch, and terminal path. */
  if (!r->ctx.replay_mode &&
      (!source_meta || strcmp(source_meta->id, "action_cli") != 0)) {
    int dpidx = rt_action_runner_pending_index(r, rid);
    const char *dargs = dpidx >= 0 && r->pending_actions[dpidx].args_json
                      ? r->pending_actions[dpidx].args_json : "{}";
    long long dwork_rev =
        dpidx >= 0 ? r->pending_actions[dpidx].work_rev : 0;
    const char *dtrigger =
        dpidx >= 0 ? r->pending_actions[dpidx].trigger_event_id : "";
    rt_operation_driver_on_terminal(
        r, emit_type, action, rid, dtrigger, task_id, dwork_rev, dargs,
        ok ? (emit_result && emit_result[0] ? emit_result : "{}")
           : (emit_error && strcmp(emit_error, "null") != 0
                  ? emit_error : "{}"));
  }
  if (wake_prepared > 0)
    (void)rt_publication_outbox_reconcile();
  rt_action_runner_note_terminal(r, rid);
  return 0;
}

int rt_action_emit_rejected(RtRun *r, const char *rid, const char *action,
                            const char *task_id, const char *reason,
                            const char *message, const char *errors_json,
                            const char *effect_sig) {
  char erid[RT_MED], eaction[RT_MED], etask[RT_MED], ereason[RT_MED], emsg[RT_LARGE];
  char bind[RT_LARGE] = "", source_event_id[RT_SMALL] = "";
  char payload[RT_XL];
  int pidx = rt_action_runner_pending_index(r, rid), wake_prepared = 0;
  RtPendingAction *pending = pidx >= 0 ? &r->pending_actions[pidx] : NULL;
  const RtAgentMeta *source_meta =
      pidx >= 0 ? rt_action_agent_meta_lookup(
                      r->scheduler, r->pending_actions[pidx].source_agent)
                : NULL;
  json_escape(rid ? rid : "", erid, sizeof(erid));
  json_escape(action ? action : "", eaction, sizeof(eaction));
  json_escape(task_id ? task_id : "", etask, sizeof(etask));
  json_escape(reason ? reason : "", ereason, sizeof(ereason));
  json_escape(message ? message : "", emsg, sizeof(emsg));
  if (pending && pending->trigger_event_id[0]) {
    char ectx[RT_LARGE], etrigger[RT_MED], esource[RT_MED];
    if (rt_next_event_id(&r->ctx, source_event_id,
                         sizeof(source_event_id)) != 0 ||
        json_escape(r->ctx.context_id, ectx, sizeof(ectx)) != 0 ||
        json_escape(pending->trigger_event_id, etrigger,
                    sizeof(etrigger)) != 0 ||
        json_escape(source_event_id, esource, sizeof(esource)) != 0)
      return -1;
    snprintf(bind, sizeof(bind),
             ",\"work_rev\":%lld,\"context_id\":\"%s\","
             "\"trigger_event_id\":\"%s\",\"source_event_id\":\"%s\"",
             pending->work_rev, ectx, etrigger, esource);
  }
  if (snprintf(
      payload, sizeof(payload),
      "{\"request_id\":\"%s\",\"action\":\"%s\",\"run_id\":\"%s\","
      "%s%s%s\"reason\":\"%s\",\"message\":\"%s\",\"errors\":%s%s}",
      erid, eaction, r->ctx.run_id,
      task_id && *task_id ? "\"task_id\":\"" : "",
      task_id && *task_id ? etask : "",
      task_id && *task_id ? "\"," : "",
      ereason, emsg,
      errors_json && errors_json[0] ? errors_json : "[]", bind) >=
      (int)sizeof(payload))
    return -1;
  if (pending && pending->trigger_event_id[0]) {
    wake_prepared = prepare_step_wake(
        r, pending, source_event_id, "action_rejected", payload, action,
        "rejected", message && *message ? message : reason);
    if (wake_prepared < 0) {
      (void)rt_narrate("work-step rejection wake preparation failed for %s",
                       rid);
      return -1;
    }
  }
  if ((pending && pending->trigger_event_id[0]
           ? rt_append_event_sync(&r->ctx, "action_rejected",
                                  "action_runner", payload)
           : rt_append_event(&r->ctx, "action_rejected",
                             "action_runner", payload)) != 0)
    return -1;
  if (task_id && *task_id && strcmp(action ? action : "", "message") != 0 &&
      (!effect_sig || !*effect_sig || rt_action_completed_sig_index_of(r, effect_sig) < 0))
    r->advance_work_progress = 1;
  if (!r->ctx.replay_mode &&
      (!source_meta || strcmp(source_meta->id, "action_cli") != 0)) {
    char detail[RT_LARGE * 2];
    const char *dargs = pending && pending->args_json
                            ? pending->args_json : "{}";
    const char *dtrigger = pending ? pending->trigger_event_id : "";
    long long dwork_rev = pending ? pending->work_rev : 0;
    if (snprintf(detail, sizeof(detail),
                 "{\"message\":\"%s\",\"errors\":%s}", emsg,
                 errors_json && errors_json[0] ? errors_json : "[]") <
        (int)sizeof(detail))
      rt_operation_driver_on_terminal(r, "action_rejected", action, rid,
                                      dtrigger, task_id, dwork_rev, dargs,
                                      detail);
  }
  if (wake_prepared > 0)
    (void)rt_publication_outbox_reconcile();
  rt_action_runner_note_terminal(r, rid);
  return 0;
}
