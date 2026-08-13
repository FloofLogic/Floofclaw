/* Durable-work runtime intrinsics and their recovery-safe helpers. */

#include "action_runtime_internal.h"
#include "publication_outbox.h"
#include "support/fsutil.h"
#include "support/heap_guard.h"
#include "support/json.h"
#include "support/timing.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Recovery may replay a locally safe intrinsic after action_started when its
 * terminal event is missing. The append event carries the stable request id,
 * so that replay returns the already-committed result instead of duplicating
 * text. */
static int working_memory_append_committed(const RtRun *r, const char *rid,
                                           const char *task_id,
                                           const char *incoming) {
  char path[PATH_MAX];
  char *text = NULL;
  char *line;
  int found = 0;
  if (!r || !rid || !*rid || !task_id || !incoming) return -1;
  rt_event_log_path(&r->ctx, path, sizeof(path));
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? 0 : -1;
  line = text;
  while (line && *line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload;
    char type[RT_SMALL] = "", request_id[RT_SMALL] = "";
    char event_task[RT_SMALL] = "";
    char event_text[RT_TASK_WORKING_MEMORY_MAX] = "";
    if (next) *next = '\0';
    if (*line && json_ref_top_object(line, &event) == 0 &&
        json_ref_object_get_string(&event, "type", type,
                                   sizeof(type)) == 0 &&
        strcmp(type, "task_working_memory_appended") == 0 &&
        json_ref_object_get_object(&event, "payload", &payload) == 0 &&
        json_ref_object_get_string(&payload, "request_id", request_id,
                                   sizeof(request_id)) == 0 &&
        strcmp(request_id, rid) == 0) {
      if (json_ref_object_get_string(&payload, "task_id", event_task,
                                     sizeof(event_task)) != 0 ||
          json_ref_object_get_string(&payload, "working_memory", event_text,
                                     sizeof(event_text)) != 0 ||
          strcmp(event_task, task_id) != 0 ||
          strcmp(event_text, incoming) != 0) {
        found = -1;
      } else {
        found = 1;
      }
      break;
    }
    line = next ? next + 1 : NULL;
  }
  fc_xfree(text);
  return found;
}

/* Append opaque, task-owned working text. Task identity is trusted routing
 * metadata stamped onto the pending request by the normalizer (or inherited
 * by a managed worker's action CLI), never an action content argument. */
int fc_working_memory_append(RtRun *r, const char *rid,
                                    const RtActionDef *def,
                                    const char *args,
                                    char *result, size_t result_len,
                                    char *error, size_t error_len) {
  JsonRef root;
  char incoming[RT_TASK_WORKING_MEMORY_MAX] = "";
  char current[RT_TASK_WORKING_MEMORY_MAX] = "";
  char eincoming[RT_TASK_WORKING_MEMORY_MAX * 2];
  char etask[RT_MED], erid[RT_MED];
  char payload[RT_XL];
  const char *task_id = "";
  size_t needed;
  int pidx;
  (void)def;
  if (!r || !rid || !args || json_ref_first_object(args, &root) != 0 ||
      json_ref_object_get_string(&root, "working_memory", incoming,
                                 sizeof(incoming)) != 0 ||
      !incoming[0]) {
    snprintf(error, error_len,
             "{\"message\":\"args.working_memory must be a nonempty string\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  pidx = rt_action_runner_pending_index(r, rid);
  if (pidx >= 0) task_id = r->pending_actions[pidx].task_id;
  if (!task_id[0] ||
      rt_task_working_memory_of(task_id, current, sizeof(current)) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"working_memory_append requires an existing task binding\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  {
    int committed = working_memory_append_committed(
        r, rid, task_id, incoming);
    if (committed > 0) {
      if (json_escape(task_id, etask, sizeof(etask)) != 0) return -1;
      snprintf(result, result_len,
               "{\"task_id\":\"%s\",\"appended\":true,"
               "\"bytes\":%zu,\"recovered\":true}",
               etask, strlen(incoming));
      snprintf(error, error_len, "null");
      return 0;
    }
    if (committed < 0) {
      snprintf(error, error_len,
               "{\"message\":\"working_memory request-id collision\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
  }
  needed = strlen(current) + (current[0] ? 1U : 0U) + strlen(incoming);
  if (needed >= RT_TASK_WORKING_MEMORY_MAX) {
    snprintf(error, error_len,
             "{\"message\":\"working_memory append exceeds the %d-byte task limit\"}",
             RT_TASK_WORKING_MEMORY_MAX - 1);
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (json_escape(task_id, etask, sizeof(etask)) != 0 ||
      json_escape(rid, erid, sizeof(erid)) != 0 ||
      json_escape(incoming, eincoming, sizeof(eincoming)) != 0 ||
      rt_append_event_format(
          &r->ctx, "task_working_memory_appended", "action_runner",
          payload, sizeof(payload), 0,
          "{\"task_id\":\"%s\",\"request_id\":\"%s\","
          "\"working_memory\":\"%s\","
          "\"updated_ms\":%llu}",
          etask, erid, eincoming,
          (unsigned long long)timing_wall_ms()) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"working_memory append could not be committed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(result, result_len,
           "{\"task_id\":\"%s\",\"appended\":true,\"bytes\":%zu}",
           etask, strlen(incoming));
  snprintf(error, error_len, "null");
  return 0;
}

/* work — mechanical create-or-revise of the durable background work
 * intent. Asynchronous by construction: it durably records the task
 * and returns immediately; it never starts external work, never
 * inspects prose to guess whether a message "sounds like" a follow-up,
 * and never touches the configured backend. The caller (the manager
 * LLM) makes the fuzzy create-vs-revise choice by including or
 * omitting a runtime-supplied task_id. */
int fc_work(RtRun *r, const char *rid, const RtActionDef *def,
                   const char *args, char *result, size_t result_len,
                   char *error, size_t error_len) {
  JsonRef root;
  char work[RT_LARGE] = "", done[RT_LARGE] = "", task_id[RT_SMALL] = "";
  char ework[RT_LARGE * 2], edone[RT_LARGE * 2], etask[RT_MED];
  char revision_clause[RT_LARGE] = "";
  long long prior_rev = 0, new_rev = 0;
  char payload[RT_XL];
  int pending_idx = -1;
  (void)def;
  if (!r || !args || json_ref_first_object(args, &root) != 0 ||
      json_ref_object_get_string(&root, "work", work, sizeof(work)) != 0 || !work[0] ||
      json_ref_object_get_string(&root, "done_when", done, sizeof(done)) != 0 || !done[0]) {
    snprintf(error, error_len,
             "{\"message\":\"args.work and args.done_when are required\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  /* The normalized action_request owns the explicit target. The runtime
   * function recovers it from the pending entry because RuntimeActionFn
   * intentionally receives only validated args. A same-context work target
   * wins exactly, including blocked. If the request was instead bound to an
   * input task or unbound, a unique revisable work task is safe; ambiguity is
   * loud and never selects the first task. The input may already be terminal
   * here when the same accepted agent batch placed task.update after this work
   * intent: normalization validated the binding before either event committed,
   * so dispatch must not invalidate the earlier accepted call. */
  {
    int pidx = rt_action_runner_pending_index(r, rid);
    const char *pending_task =
        pidx >= 0 ? r->pending_actions[pidx].task_id : "";
    char snapshot[RT_XL];
    int sr;
    pending_idx = pidx;
    task_id[0] = '\0';
    if (pending_task[0] &&
        rt_task_work_binding_json(r->ctx.context_id, pending_task, 0, 1,
                                  snapshot, sizeof(snapshot)) == 0) {
      snprintf(task_id, sizeof(task_id), "%s", pending_task);
      (void)rt_task_work_rev_of(task_id, &prior_rev);
    } else if (pending_task[0] &&
               rt_task_reference_in_context(r->ctx.context_id, pending_task) &&
               !rt_task_reference_is_ready_for_context(r->ctx.context_id,
                                                       pending_task) &&
               rt_task_input_text(r->ctx.context_id, pending_task,
                                  snapshot, sizeof(snapshot)) != 0) {
      snprintf(error, error_len,
               "{\"message\":\"explicit task_id is not an active or blocked work task\"}");
      snprintf(result, result_len, "{}");
      return -1;
    } else {
      sr = rt_task_select_unique_revisable_work(
          r->ctx.context_id, task_id, sizeof(task_id), &prior_rev);
      if (sr == 2) {
        snprintf(error, error_len,
                 "{\"message\":\"work revision is ambiguous: multiple same-context active or blocked work tasks\"}");
        snprintf(result, result_len, "{}");
        return -1;
      }
      if (sr < 0) {
        snprintf(error, error_len,
                 "{\"message\":\"work revision target lookup failed\"}");
        snprintf(result, result_len, "{}");
        return -1;
      }
      if (sr == 1) task_id[0] = '\0';
    }
  }
  if (json_escape(work, ework, sizeof(ework)) != 0 ||
      json_escape(done, edone, sizeof(edone)) != 0) {
    snprintf(error, error_len, "{\"message\":\"work json escape failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (task_id[0]) {
    /* A revision always reopens a blocked task and always bumps work_rev,
     * even if the restated work prose happens to be byte-identical. */
    if (json_escape(task_id, etask, sizeof(etask)) != 0) return -1;
    if (pending_idx >= 0) {
      const RtPendingAction *pending = &r->pending_actions[pending_idx];
      if (pending->work_rev == prior_rev &&
          pending->trigger_event_id[0] &&
          strcmp(pending->task_id, task_id) == 0 &&
          strcmp(pending->action, "work") == 0) {
        char erid[RT_MED], eaction[RT_MED], etrigger[RT_MED];
        char econtext[RT_LARGE];
        if (json_escape(pending->request_id, erid, sizeof(erid)) != 0 ||
            json_escape(pending->action, eaction, sizeof(eaction)) != 0 ||
            json_escape(pending->trigger_event_id, etrigger,
                        sizeof(etrigger)) != 0 ||
            json_escape(r->ctx.context_id, econtext, sizeof(econtext)) != 0)
          return -1;
        snprintf(revision_clause, sizeof(revision_clause),
                 ",\"revision_origin\":\"controller\","
                 "\"parent_work_rev\":%lld,\"request_id\":\"%s\","
                 "\"action\":\"%s\",\"trigger_event_id\":\"%s\","
                 "\"context_id\":\"%s\"",
                 pending->work_rev, erid, eaction, etrigger, econtext);
      }
    }
    if (rt_append_event_format(
            &r->ctx, "task_updated", "action", payload, sizeof(payload), 0,
            "{\"task_id\":\"%s\",\"work_rev\":%lld,"
            "\"state\":{\"status\":\"open\","
            "\"work\":\"%s\",\"done_when\":\"%s\"}%s}",
            etask, prior_rev + 1, ework, edone, revision_clause) != 0) {
      snprintf(error, error_len, "{\"message\":\"failed to append task_updated\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    if (rt_task_work_rev_of(task_id, &new_rev) != 0 ||
        new_rev != prior_rev + 1) {
      snprintf(error, error_len,
               "{\"message\":\"task revision did not advance work_rev\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    snprintf(result, result_len,
             "{\"task_id\":\"%s\",\"work_rev\":%lld,\"revised\":true}",
             etask, new_rev);
    snprintf(error, error_len, "null");
    (void)rt_narrate("work revised: %s — %.100s", task_id, work);
    return 0;
  }
  {
    char context[RT_MED], ectx[RT_MED], event_id[RT_SMALL];
    char affair_clause[RT_MED + 32] = "";
    long long ms = (long long)timing_wall_ms();
    int event_seq = r->ctx.next_event;
    snprintf(context, sizeof(context), "%s", r->ctx.context_id);
    snprintf(task_id, sizeof(task_id), "task_%s_%06d", r->ctx.run_id, event_seq);
    snprintf(event_id, sizeof(event_id), "evt_%s_%06d", r->ctx.run_id, event_seq);
    if (json_escape(context, ectx, sizeof(ectx)) != 0 ||
        json_escape(task_id, etask, sizeof(etask)) != 0) return -1;
    /* Work admitted from an affair review durably carries its origin
     * affair through task state, so a later operation_result turn can
     * correlate exactly instead of matching affair prose. Replay-safe:
     * the id rides the task_created payload. */
    if (r->ctx.inbound_affair_id[0]) {
      char eaff[RT_MED];
      if (json_escape(r->ctx.inbound_affair_id, eaff, sizeof(eaff)) != 0)
        return -1;
      snprintf(affair_clause, sizeof(affair_clause),
               "\"affair_id\":\"%s\",", eaff);
    }
    if (rt_append_event_format(
            &r->ctx, "task_created", "action", payload, sizeof(payload), 0,
            "{\"task_id\":\"%s\",\"context_id\":\"%s\",\"kind\":\"work\","
            "\"work_rev\":1,"
            "\"input\":null,\"created_event_id\":\"%s\",\"created_ms\":%lld,"
            "\"state\":{%s\"status\":\"open\",\"work\":\"%s\",\"done_when\":\"%s\","
            "\"artifacts\":[],\"updated_ms\":%lld}}",
            etask, ectx, event_id, ms, affair_clause, ework, edone, ms) != 0) {
      snprintf(error, error_len, "{\"message\":\"failed to append task_created\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    snprintf(result, result_len,
             "{\"task_id\":\"%s\",\"work_rev\":1,\"created\":true}", etask);
    snprintf(error, error_len, "null");
    (void)rt_narrate("work queued: %s — %.100s", task_id, work);
    return 0;
  }
}

static int compact_task_state_value(const JsonRef *task, const char *key,
                                    const char *fallback,
                                    char *out, size_t out_len) {
  JsonRef state, value;
  char raw[RT_LARGE];
  if (!task || !key || !out || out_len == 0) return -1;
  if (json_ref_object_get_object(task, "state", &state) != 0 ||
      json_ref_object_get(&state, key, &value) != 0) {
    return snprintf(out, out_len, "%s", fallback ? fallback : "null") >=
                   (int)out_len
               ? -1
               : 0;
  }
  if (json_ref_value_copy(&value, raw, sizeof(raw)) != 0 ||
      json_compact(raw, out, out_len) != 0)
    return -1;
  return 0;
}

static int terminalize_bound_work(RtRun *r, const char *rid,
                                  const char *args, int blocked,
                                  char *result, size_t result_len,
                                  char *error, size_t error_len) {
  int pidx = rt_action_runner_pending_index(r, rid);
  RtPendingAction *pending;
  char *large = NULL;
  char *snapshot, *esummary, *eblocker, *eneeded, *payload;
  char summary[RT_LARGE] = "", blocker[RT_LARGE] = "";
  char needed[RT_LARGE] = "", affair_id[RT_SMALL] = "";
  char artifacts[RT_LARGE] = "[]", operation[RT_LARGE] = "null";
  char etask[RT_MED], ectx[RT_LARGE], eaffair[RT_MED];
  char echannel[RT_MED], eadapter[RT_MED], eorigin[RT_MED];
  char erid[RT_MED], eaction[RT_MED], etrigger[RT_MED], esource[RT_MED];
  char source_event_id[RT_SMALL], wake[4096];
  char *evidence = NULL;
  JsonRef task, state;
  long long updated_ms = 0;
  const char *event_type = blocked ? "work_blocked" : "work_completed";
  const char *status = blocked ? "blocked" : "completed";
  if (!r || pidx < 0) {
    snprintf(error, error_len,
             "{\"message\":\"trusted work binding is missing\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  pending = &r->pending_actions[pidx];
  large = (char *)fc_xcalloc(5U, RT_XL);
  if (!large) {
    snprintf(error, error_len,
             "{\"message\":\"terminal work scratch allocation failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snapshot = large;
  esummary = large + RT_XL;
  eblocker = large + RT_XL * 2U;
  eneeded = large + RT_XL * 3U;
  payload = large + RT_XL * 4U;
  if (!pending->task_id[0] || pending->work_rev <= 0 ||
      rt_task_work_binding_json(r->ctx.context_id, pending->task_id,
                                pending->work_rev, 0,
                                snapshot, RT_XL) != 0 ||
      json_ref_from_text(snapshot, &task) != 0 ||
      json_ref_object_get_object(&task, "state", &state) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"bound work task or revision is stale\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(large);
    return -1;
  }
  if (json_ref_object_get_long(&state, "updated_ms", &updated_ms) != 0)
    updated_ms = 0;
  updated_ms++;
  (void)json_ref_object_get_string(&state, "affair_id",
                                   affair_id, sizeof(affair_id));
  if (compact_task_state_value(&task, "artifacts", "[]",
                               artifacts, sizeof(artifacts)) != 0 ||
      compact_task_state_value(&task, "external", "null",
                               operation, sizeof(operation)) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"bound work evidence projection failed\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(large);
    return -1;
  }
  evidence = (char *)fc_xmalloc(RT_XL);
  if (!evidence ||
      (pending->trigger_event_id[0]
           ? rt_work_state_evidence_refs_json(
                 pending->task_id, pending->work_rev, evidence, RT_XL)
           : (snprintf(evidence, RT_XL, "%s", artifacts) >= RT_XL
                  ? -1 : 0)) != 0) {
    fc_xfree(evidence);
    fc_xfree(large);
    snprintf(error, error_len,
             "{\"message\":\"bound work ledger projection failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (blocked) {
    (void)rt_json_get_string(args && *args ? args : "{}", "blocker",
                             blocker, sizeof(blocker));
    (void)rt_json_get_string(args && *args ? args : "{}", "needed",
                             needed, sizeof(needed));
    snprintf(summary, sizeof(summary), "%s", blocker);
  } else {
    (void)rt_json_get_string(args && *args ? args : "{}", "summary",
                             summary, sizeof(summary));
  }
  if (!summary[0] || (blocked && !needed[0]) ||
      json_escape(pending->task_id, etask, sizeof(etask)) != 0 ||
      json_escape(r->ctx.context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(affair_id, eaffair, sizeof(eaffair)) != 0 ||
      json_escape(r->ctx.origin_channel, echannel, sizeof(echannel)) != 0 ||
      json_escape(r->ctx.origin_adapter_id, eadapter, sizeof(eadapter)) != 0 ||
      json_escape(r->ctx.origin_event_id, eorigin, sizeof(eorigin)) != 0 ||
      json_escape(pending->request_id, erid, sizeof(erid)) != 0 ||
      json_escape(pending->action, eaction, sizeof(eaction)) != 0 ||
      json_escape(pending->trigger_event_id, etrigger,
                  sizeof(etrigger)) != 0 ||
      rt_next_event_id(&r->ctx, source_event_id,
                       sizeof(source_event_id)) != 0 ||
      json_escape(source_event_id, esource, sizeof(esource)) != 0 ||
      json_escape(summary, esummary, RT_XL) != 0 ||
      json_escape(blocker, eblocker, RT_XL) != 0 ||
      json_escape(needed, eneeded, RT_XL) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"terminal work outcome is invalid\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(evidence);
    fc_xfree(large);
    return -1;
  }
  if (snprintf(
          payload, RT_XL,
          "{\"task_id\":\"%s\",\"work_rev\":%lld,\"context_id\":\"%s\","
          "\"request_id\":\"%s\",\"action\":\"%s\","
          "\"trigger_event_id\":\"%s\",\"source_event_id\":\"%s\","
          "\"status\":\"%s\","
          "\"affair_id\":%s%s%s,\"channel\":\"%s\",\"adapter_id\":\"%s\","
          "\"origin_event_id\":\"%s\",\"ref\":%s,\"summary\":\"%s\"%s%s%s%s%s%s,"
          "\"operation_ref\":%s,\"artifacts\":%s,\"evidence_refs\":%s,"
          "\"state\":{\"status\":\"%s\",\"updated_ms\":%lld}}",
          etask, pending->work_rev, ectx, erid, eaction, etrigger, esource,
          status,
          affair_id[0] ? "\"" : "", affair_id[0] ? eaffair : "null",
          affair_id[0] ? "\"" : "",
          echannel, eadapter, eorigin,
          r->ctx.origin_ref_json[0] ? r->ctx.origin_ref_json : "null",
          esummary,
          blocked ? ",\"blocker\":\"" : "",
          blocked ? eblocker : "",
          blocked ? "\"" : "",
          blocked && needed[0] ? ",\"needed\":\"" : "",
          blocked && needed[0] ? eneeded : "",
          blocked && needed[0] ? "\"" : "",
          operation, artifacts, evidence, status, updated_ms) >=
      RT_XL) {
    snprintf(error, error_len,
             "{\"message\":\"terminal work outcome exceeds event boundary\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(evidence);
    fc_xfree(large);
    return -1;
  }
  {
    char bounded[385], bounded_needed[385];
    char ebounded[RT_LARGE], ebounded_needed[RT_LARGE];
    const char *ref = r->ctx.origin_ref_json[0] &&
                              strlen(r->ctx.origin_ref_json) <= 512
                          ? r->ctx.origin_ref_json : "null";
    int n;
    snprintf(bounded, sizeof(bounded), "%.384s", summary);
    snprintf(bounded_needed, sizeof(bounded_needed), "%.384s", needed);
    if (json_escape(bounded, ebounded, sizeof(ebounded)) != 0 ||
        json_escape(bounded_needed, ebounded_needed,
                    sizeof(ebounded_needed)) != 0) {
      fc_xfree(evidence);
      fc_xfree(large);
      return -1;
    }
    n = snprintf(
        wake, sizeof(wake),
        "{\"task_id\":\"%s\",\"work_rev\":%lld,\"request_id\":\"%s\","
        "\"context_id\":\"%s\","
        "\"action\":\"%s\",\"trigger_event_id\":\"%s\","
        "\"source_event_id\":\"%s\",\"status\":\"%s\","
        "\"affair_id\":%s%s%s,\"summary\":\"%s\",\"text\":\"%s\""
        "%s%s%s%s%s%s,"
        "\"origin_event_id\":\"%s\",\"adapter_id\":\"%s\",\"ref\":%s}",
        etask, pending->work_rev, erid, ectx, eaction, etrigger, esource,
        status,
        affair_id[0] ? "\"" : "", affair_id[0] ? eaffair : "null",
        affair_id[0] ? "\"" : "",
        ebounded, ebounded,
        blocked ? ",\"blocker\":\"" : "",
        blocked ? ebounded : "",
        blocked ? "\"" : "",
        blocked ? ",\"needed\":\"" : "",
        blocked ? ebounded_needed : "",
        blocked ? "\"" : "",
        eorigin, eadapter, ref);
    if (n < 0 || (size_t)n >= sizeof(wake) ||
        (r->ctx.origin_channel[0] && pending->trigger_event_id[0] &&
         rt_publication_outbox_prepare(
             &r->ctx, source_event_id, event_type, payload,
             r->ctx.origin_channel, "work_outcome", wake, NULL, 0) != 0)) {
      snprintf(error, error_len,
               "{\"message\":\"failed to prepare terminal work outcome\"}");
      snprintf(result, result_len, "{}");
      fc_xfree(evidence);
      fc_xfree(large);
      return -1;
    }
  }
  if (rt_append_event_sync(&r->ctx, event_type, "action", payload) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"failed to append terminal work outcome\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(evidence);
    fc_xfree(large);
    return -1;
  }
  fc_xfree(evidence);
  fc_xfree(large);
  if (r->ctx.origin_channel[0] && pending->trigger_event_id[0])
    (void)rt_publication_outbox_reconcile();
  snprintf(result, result_len,
           "{\"task_id\":\"%s\",\"work_rev\":%lld,\"status\":\"%s\"}",
           etask, pending->work_rev, status);
  snprintf(error, error_len, "null");
  if (blocked)
    (void)rt_narrate("work blocked: %s — %.160s", pending->task_id, blocker);
  else
    (void)rt_narrate("work completed: %s — %.160s", pending->task_id, summary);
  return 0;
}

int fc_work_complete(RtRun *r, const char *rid,
                            const RtActionDef *def, const char *args,
                            char *result, size_t result_len,
                            char *error, size_t error_len) {
  (void)def;
  return terminalize_bound_work(r, rid, args, 0,
                                result, result_len, error, error_len);
}

int fc_work_blocked(RtRun *r, const char *rid,
                           const RtActionDef *def, const char *args,
                           char *result, size_t result_len,
                           char *error, size_t error_len) {
  (void)def;
  return terminalize_bound_work(r, rid, args, 1,
                                result, result_len, error, error_len);
}

