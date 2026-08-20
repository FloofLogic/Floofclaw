/* Action runner — public API + pending queue + validation + dispatch.
 *
 * Owns:
 *   - the pending-action queue (RtPendingAction[16] embedded in RtRun)
 *   - request-id and signature-based dedupe (terminal_action_*, completed_action_sig_*)
 *   - validation against action.json + agent allow-list
 *   - dispatch between runtime-intrinsic and subprocess paths
 *   - the rt_action_runner_* entry points called from run.c
 *
 * Companion files:
 *   action_events.c     — emit_started / _terminal / _rejected
 *   action_runtime*.c   — runtime-intrinsic dispatch and implementation families
 *   action_subprocess.c — fork+exec path for non-intrinsic actions
 *
 * Shared internals live in action_internal.h. */

#include "action_internal.h"
#include "action_coerce.h"
#include "ports.h"
#include "support/heap_guard.h"
#include "support/json.h"
#include "support/fsutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The action CLI is a transport adapter, not a model-backed agent. It uses
 * the same registry and runner with no presentation allowlist so
 * `fclaw action list` and `fclaw action exec` expose the installed catalog
 * directly. Work-terminal actions still enforce their own trusted task
 * binding in validate_pending_action. */
static const RtAgentMeta kActionCliMeta = {
  .id = "action_cli",
  .executor = "native",
  .has_action_allowlist = 0
};

/* ===== queue / signature helpers (used across action_*.c) ========= */

int rt_action_runner_pending_index(const RtRun *r, const char *rid) {
  if (!r || !rid) return -1;
  for (size_t i = 0; i < r->pending_action_count; ++i)
    if (strcmp(r->pending_actions[i].request_id, rid) == 0) return (int)i;
  return -1;
}

void rt_action_pending_remove_at(RtRun *r, size_t idx) {
  if (idx >= r->pending_action_count) return;
  fc_xfree(r->pending_actions[idx].args_json);
  r->pending_actions[idx].args_json = NULL;
  if (idx + 1 < r->pending_action_count)
    memmove(&r->pending_actions[idx], &r->pending_actions[idx + 1],
            (r->pending_action_count - idx - 1) * sizeof(*r->pending_actions));
  r->pending_action_count--;
  memset(&r->pending_actions[r->pending_action_count], 0, sizeof(*r->pending_actions));
}

int rt_action_terminal_index_of(RtRun *r, const char *rid) {
  if (!r || !rid || !*rid) return -1;
  for (size_t i = 0; i < r->completed_action_count; ++i)
    if (strcmp(r->completed_action_ids[i], rid) == 0) return (int)i;
  return -1;
}

static unsigned long long sig_hash_add(unsigned long long h, const char *s) {
  if (!s) s = "";
  for (; *s; ++s) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

void rt_action_signature(char *out, size_t out_len, const char *source,
                        const char *action, const char *args,
                        const char *task_id, long long work_rev) {
  unsigned long long h = 1469598103934665603ULL;
  char rev[64];
  h = sig_hash_add(h, source);
  h = sig_hash_add(h, "\037");
  h = sig_hash_add(h, action);
  h = sig_hash_add(h, "\037");
  h = sig_hash_add(h, args && *args ? args : "{}");
  h = sig_hash_add(h, "\037");
  h = sig_hash_add(h, task_id);
  h = sig_hash_add(h, "\037");
  snprintf(rev, sizeof(rev), "%lld", work_rev);
  h = sig_hash_add(h, rev);
  snprintf(out, out_len, "%s:%s:%016llx",
           source ? source : "", action ? action : "",
           (unsigned long long)h);
}

int rt_action_completed_sig_index_of(RtRun *r, const char *sig) {
  if (!r || !sig || !*sig) return -1;
  for (size_t i = 0; i < r->completed_action_sig_count; ++i)
    if (strcmp(r->completed_action_sigs[i], sig) == 0) return (int)i;
  return -1;
}

int rt_action_pending_sig_index_of(RtRun *r, const char *sig) {
  if (!r || !sig || !*sig) return -1;
  for (size_t i = 0; i < r->pending_action_count; ++i)
    if (strcmp(r->pending_actions[i].sig, sig) == 0) return (int)i;
  return -1;
}

void rt_action_completed_sig_add(RtRun *r, const char *sig, const char *rid) {
  size_t i;
  if (!r || !sig || !*sig || rt_action_completed_sig_index_of(r, sig) >= 0) return;
  if (r->completed_action_sig_count >= RT_MAX_COMPLETED_ACTIONS) return;
  i = r->completed_action_sig_count++;
  snprintf(r->completed_action_sigs[i], sizeof(r->completed_action_sigs[0]), "%s", sig);
  snprintf(r->completed_action_sig_rids[i], sizeof(r->completed_action_sig_rids[0]),
           "%s", rid ? rid : "");
}

const char *rt_action_completed_sig_rid(RtRun *r, const char *sig) {
  int idx = rt_action_completed_sig_index_of(r, sig);
  if (idx < 0 || !r->completed_action_sig_rids[idx][0]) return NULL;
  return r->completed_action_sig_rids[idx];
}

void rt_action_terminal_add(RtRun *r, const char *rid) {
  if (!r || !rid || !*rid || rt_action_terminal_index_of(r, rid) >= 0) return;
  if (r->completed_action_count >= RT_MAX_COMPLETED_ACTIONS) return;
  snprintf(r->completed_action_ids[r->completed_action_count++],
           sizeof(r->completed_action_ids[0]), "%s", rid);
}

const RtAgentMeta *rt_action_agent_meta_lookup(const RtScheduler *s,
                                              const char *agent_id) {
  if (!agent_id) return NULL;
  if (strcmp(agent_id, kActionCliMeta.id) == 0) return &kActionCliMeta;
  if (!s) return NULL;
  for (size_t i = 0; i < s->agent_meta_count; ++i)
    if (strcmp(s->agent_meta[i].id, agent_id) == 0) return &s->agent_meta[i];
  return NULL;
}

/* §5.G: read a prior call's recorded terminal outcome by request_id
 * from the event log so an equivalent repeat can be memoized to it.
 * Fills type ("action_succeeded"/"action_failed") and the raw `result`
 * / `error` JSON values of the most recent terminal for prior_rid.
 * Returns 0 if found, -1 otherwise. Pure read of already-logged
 * events, so a memoized re-emit stays replay-deterministic. */
static int memo_read_outcome_by_rid(const RtContext *ctx, const char *prior_rid,
                                     char *type_out, size_t type_len,
                                     char *result_out, size_t result_len,
                                     char *error_out, size_t error_len) {
  char path[PATH_MAX], *text = NULL;
  int found = 0;
  if (!ctx || !prior_rid || !*prior_rid || !type_out || !result_out || !error_out)
    return -1;
  rt_event_log_path(ctx, path, sizeof(path));
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  for (char *line = text; line && *line;) {
    char *next = strchr(line, '\n');
    JsonRef event, payload, val;
    char type[RT_SMALL] = "", rid[RT_SMALL] = "";
    if (next) *next = '\0';
    if (json_ref_first_object(line, &event) == 0 &&
        json_ref_object_get_string(&event, "type", type, sizeof(type)) == 0 &&
        (strcmp(type, "action_succeeded") == 0 || strcmp(type, "action_failed") == 0) &&
        json_ref_object_get_object(&event, "payload", &payload) == 0 &&
        json_ref_object_get_string(&payload, "request_id", rid, sizeof(rid)) == 0 &&
        strcmp(rid, prior_rid) == 0) {
      snprintf(type_out, type_len, "%s", type);
      if (json_ref_object_get(&payload, "result", &val) != 0 ||
          json_ref_value_copy(&val, result_out, result_len) != 0)
        snprintf(result_out, result_len, "{}");
      if (json_ref_object_get(&payload, "error", &val) != 0 ||
          json_ref_value_copy(&val, error_out, error_len) != 0)
        snprintf(error_out, error_len, "null");
      found = 1;
    }
    if (!next) break;
    line = next + 1;
  }
  free(text);
  return found ? 0 : -1;
}

/* ===== public entry points (called from run.c) =================== */

int rt_action_runner_note_request(RtRun *r, const char *source_agent,
                                  const char *rid, const char *action,
                                  const char *args, const char *task_id,
                                  long long work_rev) {
  char sig[RT_MED] = "";
  if (!r || !rid || !*rid) return 0;
  /* Every call gets an effect signature — including message. Within
   * one run, an identical repeated call (e.g. re-emitted on a bounded
   * per-event retry pass) is memoized to its recorded outcome instead
   * of executing or delivering twice. */
  rt_action_signature(sig, sizeof(sig), source_agent, action, args,
                      task_id, work_rev);
  if (rt_action_runner_pending_index(r, rid) >= 0) return 0;
  if (rt_action_terminal_index_of(r, rid) >= 0) {
    return rt_action_emit_rejected(
               r, rid, action, task_id, "duplicate_terminal_request_id",
               "Request id already has a terminal action outcome.", "[]", sig) == 0
               ? 0 : -1;
  }
  if (sig[0] && rt_action_pending_sig_index_of(r, sig) >= 0) {
    /* An equivalent call is still in flight: there is no terminal
     * outcome to memoize yet, and running it again concurrently is
     * meaningless. Keep the historical hard-reject — this is a
     * correctness guard, not the behavioral one §5.G relaxes. */
    return rt_action_emit_rejected(
               r, rid, action, task_id, "duplicate_equivalent_action_call",
               "Equivalent action call is already in flight.", "[]", sig) == 0
               ? 0 : -1;
  }
  if (sig[0] && rt_action_completed_sig_index_of(r, sig) >= 0) {
    /* §5.G: an equivalent call already completed. Do NOT re-run the
     * tool and do NOT hard-reject (which would end the worker's
     * turn). Memoize: re-emit the prior call's recorded terminal
     * outcome for this request_id. The worker transparently gets the
     * same result back; the profile pass cap remains the spin bound.
     * Replay-deterministic: the
     * re-emitted event is a pure function of a prior logged event. */
    const char *prior = rt_action_completed_sig_rid(r, sig);
    char mtype[RT_SMALL] = "", mresult[RT_XL] = "{}", merror[RT_LARGE] = "null";
    if (prior && memo_read_outcome_by_rid(&r->ctx, prior, mtype, sizeof(mtype),
                                          mresult, sizeof(mresult),
                                          merror, sizeof(merror)) == 0) {
      /* A memoized re-emit is the SAME result, not new work. Let it
       * appear on the log but do not let it count as
       * outer-profile work progress — otherwise the profile loop can
       * never quiesce and burns to RT_MAX_PROFILE_PASSES (run_failed)
       * instead of settling, exactly as the old duplicate-reject did.
       * the old duplicate rejection did. */
      int prev_awp = r->advance_work_progress;
      if (rt_action_emit_terminal(r, mtype, rid, action, NULL, task_id,
                                  "memoized_duplicate", mresult,
                                  merror) != 0)
        return -1;
      r->advance_work_progress = prev_awp;
      return 2;
    }
    /* No recoverable prior outcome (should not happen) — fall back to
     * the historical hard-reject so behavior stays defined. */
    return rt_action_emit_rejected(
               r, rid, action, task_id, "duplicate_equivalent_action_call",
               "Equivalent action call already has an outcome in this run.",
               "[]", sig) == 0 ? 0 : -1;
  }
  if (r->pending_action_count >= RT_MAX_PENDING_ACTIONS) {
    char p[RT_LARGE];
    (void)rt_append_event_format(
        &r->ctx, "error", "kernel", p, sizeof(p), 0,
        "{\"message\":\"pending_action overflow: cap=%d\",\"action\":\"%s\","
        "\"request_id\":\"%s\"}",
        RT_MAX_PENDING_ACTIONS, action ? action : "", rid);
    return 0;
  }
  RtPendingAction *e = &r->pending_actions[r->pending_action_count++];
  snprintf(e->request_id, sizeof(e->request_id), "%s", rid);
  if (source_agent) snprintf(e->source_agent, sizeof(e->source_agent), "%s", source_agent);
  if (action) snprintf(e->action, sizeof(e->action), "%s", action);
  if (args && *args && strcmp(args, "{}") != 0) {
    e->args_json = fc_xstrdup(args);
    if (!e->args_json) {
      r->pending_action_count--;
      memset(e, 0, sizeof(*e));
      return -1;
    }
  } else {
    e->args_json = NULL;
  }
  if (task_id && *task_id) snprintf(e->task_id, sizeof(e->task_id), "%s", task_id);
  e->work_rev = work_rev;
  if (sig[0]) snprintf(e->sig, sizeof(e->sig), "%s", sig);
  e->started = 0;
  return 1;
}

void rt_action_runner_note_trigger(RtRun *r, const char *rid,
                                   const char *trigger_event_id) {
  int idx = rt_action_runner_pending_index(r, rid);
  if (idx >= 0 && trigger_event_id && *trigger_event_id)
    snprintf(r->pending_actions[idx].trigger_event_id,
             sizeof(r->pending_actions[idx].trigger_event_id), "%s",
             trigger_event_id);
}

void rt_action_runner_note_started(RtRun *r, const char *rid) {
  int idx = rt_action_runner_pending_index(r, rid);
  if (idx >= 0) r->pending_actions[idx].started = 1;
}

void rt_action_runner_note_terminal(RtRun *r, const char *rid) {
  int idx = rt_action_runner_pending_index(r, rid);
  rt_action_terminal_add(r, rid);
  if (idx >= 0 && r->pending_actions[idx].sig[0])
    rt_action_completed_sig_add(r, r->pending_actions[idx].sig,
                               r->pending_actions[idx].request_id);
  if (idx >= 0) rt_action_pending_remove_at(r, (size_t)idx);
}

int rt_action_runner_recover_started(RtRun *r) {
  size_t i = 0;
  if (!r) return -1;
  while (i < r->pending_action_count) {
    RtPendingAction *p = &r->pending_actions[i];
    if (!p->started) { i++; continue; }
    if (strcmp(p->action, "message") == 0) {
      int delivered = rt_ports_delivery_exists(r->ctx.run_id, p->request_id);
      if (delivered < 0) return -1;
      if (delivered) {
        char message[RT_LARGE] = "";
        char escaped[RT_LARGE * 2] = "";
        char result[RT_LARGE * 2 + 32];
        (void)rt_json_get_string(p->args_json ? p->args_json : "{}",
                                 "message", message, sizeof(message));
        if (json_escape(message, escaped, sizeof(escaped)) != 0) return -1;
        snprintf(result, sizeof(result), "{\"message\":\"%s\"}", escaped);
        /* The ledger proves the outside-facing effect already happened.
         * Commit the missing terminal without handing it to the adapter a
         * second time; rt_action_emit_terminal removes this pending entry. */
        if (rt_action_emit_terminal(r, "action_succeeded", p->request_id,
                                    p->action, NULL, p->task_id,
                                    "memoized_duplicate", result,
                                    "null") != 0)
          return -1;
        continue;
      }
      /* message is the one outside-world action whose effect crosses a
       * runtime-owned, queryable commit boundary: deliveries.jsonl. If the
       * stable (run_id, request_id) record is absent, the effect did not
       * commit and the same request may resume. Every other outside-world
       * action remains ambiguous after action_started. */
      p->started = 0;
      i++;
      continue;
    }
    {
      const RtActionDef *def =
          rt_action_registry_find(&r->scheduler->actions, p->action);
      if (!def || def->outside_world) {
        char request_id[RT_SMALL], action[RT_SMALL], task_id[RT_SMALL];
        char trigger_event_id[RT_SMALL];
        char reason[RT_LARGE];
        long long work_rev = p->work_rev;
        int blocked = 1;
        snprintf(request_id, sizeof(request_id), "%s", p->request_id);
        snprintf(action, sizeof(action), "%s", p->action);
        snprintf(task_id, sizeof(task_id), "%s", p->task_id);
        snprintf(trigger_event_id, sizeof(trigger_event_id), "%s",
                 p->trigger_event_id);
        snprintf(
            reason, sizeof(reason),
            "ambiguous_completion: %s crossed action_started before the "
            "gateway stopped, so its outside-world effect will not be "
            "replayed",
            action[0] ? action : "(unknown action)");
        /* A preallocated managed-operation record for this request must
         * close with the ambiguity blocker as its terminal truth —
         * otherwise the deadline would later add a second, conflicting
         * timeout consequence for the same request. */
        if (def && def->managed_operation) {
          char op_id[RT_SMALL];
          if (rt_operation_id_for_request(request_id, op_id,
                                          sizeof(op_id)) == 0)
            (void)rt_operation_close_without_result(op_id);
        }
        if (task_id[0] && work_rev > 0 && trigger_event_id[0]) {
          blocked = rt_task_block_bound_work(
              &r->ctx, task_id, work_rev, request_id, action,
              trigger_event_id, reason,
              "Confirm the outside-world result, then send a new external "
              "instruction to create a newer work revision.");
          if (blocked < 0) return -1;
        }
        if (blocked == 0) {
          /* work_blocked is the durable terminal for this selected request.
           * Replay consumes it through replay_action_record below. */
          rt_action_runner_note_terminal(r, request_id);
          continue;
        }
        /* Unbound or stale work cannot safely receive a current-revision
         * blocker. It still needs a durable terminal so recovery never
         * relaunches the outside-world call. */
        if (rt_action_emit_terminal(
                r, "action_failed", request_id, action, NULL, task_id,
                "ambiguous_completion", "{}",
                "{\"message\":\"outside-world completion is ambiguous; "
                "the action was not replayed\"}") != 0)
          return -1;
        continue;
      }
    }
    /* No committed terminal and no separately durable message delivery.
     * A registry-declared local action may be replayed under its stable
     * request id. Outside-world actions are terminalized above. */
    p->started = 0;
    i++;
  }
  return 0;
}

/* ===== validation ================================================ */

static int validate_pending_action(RtRun *r, RtPendingAction *p,
                                  const RtActionDef **out_def) {
  const RtActionDef *def = rt_action_registry_find(&r->scheduler->actions, p->action);
  const RtAgentMeta *meta = rt_action_agent_meta_lookup(r->scheduler, p->source_agent);
  char errors[RT_LARGE] = "[]";
  char message[RT_MED] = "";
  int working_memory_sidecar =
      p && strcmp(p->action, "working_memory_append") == 0;
  if (!def) {
    return rt_action_emit_rejected(
               r, p->request_id, p->action, p->task_id, "unknown_action",
               "Action is not registered.", "[]", p->sig) == 0 ? -1 : -2;
  }
  /* The catalog renderer already hides force-disabled actions; a call that
   * arrives anyway (stale context, hallucinated id) is rejected with the
   * deployment fact rather than executed. The action_cli transport is the
   * operator's own hands — `fclaw action exec` is how setup gets verified
   * before an id is removed from force_disable — so it bypasses the mask. */
  if (def->force_disabled && strcmp(p->source_agent, "action_cli") != 0) {
    return rt_action_emit_rejected(
               r, p->request_id, p->action, p->task_id, "force_disabled",
               "Action is listed in config force_disable.", "[]",
               p->sig) == 0 ? -1 : -2;
  }
  if (!rt_agent_allows_action(meta, def)) {
    return rt_action_emit_rejected(
               r, p->request_id, p->action, p->task_id, "not_allowed",
               "Agent is not allowed to call this action.", "[]", p->sig) == 0
               ? -1 : -2;
  }
  if (meta && meta->bind_task_open_work && working_memory_sidecar) {
    if (!p->task_id[0] || p->work_rev != 0 || p->trigger_event_id[0] ||
        !rt_task_reference_in_context(r->ctx.context_id, p->task_id)) {
      return rt_action_emit_rejected(
                 r, p->request_id, p->action, p->task_id,
                 "invalid_working_memory_sidecar_binding",
                 "Working-memory sidecars require the exact bound task and no semantic work revision.",
                 "[]", p->sig) == 0 ? -1 : -2;
    }
  } else if ((meta && meta->bind_task_open_work) || p->work_rev > 0 ||
             def->work_terminal) {
    char task_json[RT_XL];
    if (!meta || !meta->bind_task_open_work || p->work_rev <= 0 ||
        !p->task_id[0]) {
      return rt_action_emit_rejected(
                 r, p->request_id, p->action, p->task_id,
                 "trusted_work_binding_required",
                 "Action requires a runtime-bound work controller task and revision.",
                 "[]", p->sig) == 0 ? -1 : -2;
    }
    if (rt_task_work_binding_json(r->ctx.context_id, p->task_id,
                                  p->work_rev, 0,
                                  task_json, sizeof(task_json)) != 0) {
      return rt_action_emit_rejected(
                 r, p->request_id, p->action, p->task_id,
                 "stale_work_binding",
                 "Bound work task or revision is no longer active.",
                 "[]", p->sig) == 0 ? -1 : -2;
    }
  }
  /* Coerce-then-validate: nudge primitive type mistakes in place so a
   * type-sloppy-but-correct call fires instead of being rejected. The
   * coerced args are what both validation and execution see (both read
   * p->args_json). */
  if (p->args_json && p->args_json[0])
    (void)rt_action_coerce_args(def->args_schema, p->args_json, strlen(p->args_json) + 1U);
  if (rt_action_validate_args(def, p->args_json && p->args_json[0] ? p->args_json : "{}",
                             errors, sizeof(errors),
                             message, sizeof(message)) != 0) {
    return rt_action_emit_rejected(
               r, p->request_id, p->action, p->task_id, "invalid_args",
               message[0] ? message : "Invalid args for action.", errors,
               p->sig) == 0 ? -1 : -2;
  }
  *out_def = def;
  return 0;
}

/* ===== dispatch (runtime vs subprocess) ========================== */

static int launch_action_job(RtRun *r, const char *rid, const char *args,
                            RtJobRunner *runner) {
  int idx = rt_action_runner_pending_index(r, rid);
  const RtActionDef *def = NULL;
  const char *eff_args;
  if (idx < 0) return -1;
  {
    int validation = validate_pending_action(
        r, &r->pending_actions[idx], &def);
    if (validation != 0) return validation == -2 ? -1 : 0;
  }
  /* Execute with the pending action's (possibly coerced) args, not the
   * pre-coercion snapshot the caller passed. */
  (void)args;
  eff_args = (r->pending_actions[idx].args_json && r->pending_actions[idx].args_json[0])
           ? r->pending_actions[idx].args_json : "{}";
  if (def->exec_kind == RT_ACTION_EXEC_RUNTIME)
    return rt_action_runtime_complete(r, rid, def, r->pending_actions[idx].task_id, eff_args);
  return rt_action_subprocess_launch(r, rid, def, r->pending_actions[idx].task_id, eff_args, runner);
}

int rt_action_runner_peek_next(const RtRun *r,
                              char *out_rid, size_t rid_len,
                              char *out_action, size_t action_len,
                              char *out_args, size_t args_len) {
  if (!r) return 1;
  for (size_t i = 0; i < r->pending_action_count; ++i) {
    const RtPendingAction *e = &r->pending_actions[i];
    if (e->started) continue;
    snprintf(out_rid, rid_len, "%s", e->request_id);
    snprintf(out_action, action_len, "%s", e->action);
    snprintf(out_args, args_len, "%s", e->args_json && e->args_json[0] ? e->args_json : "{}");
    return 0;
  }
  return 1;
}

int rt_action_runner_launch(RtRun *r, const char *rid, const char *action,
                           const char *args, RtJobRunner *runner) {
  (void)action;
  return launch_action_job(r, rid, args, runner);
}

int rt_action_runner_complete_job(RtRun *r, RtJob *job) {
  const char *rid = job->request_id[0] ? job->request_id : r->wait_request_id;
  const char *action = r->wait_action;
  char *output = NULL;
  char result[RT_LARGE] = "{}";
  char error_obj[RT_LARGE] = "null";
  const char *reason = NULL;
  int ok;
  int emit_rc = 0;
  int idx = rt_action_runner_pending_index(r, rid);
  const char *task_id = idx >= 0 ? r->pending_actions[idx].task_id : "";
  if ((!action || !*action) && idx >= 0) action = r->pending_actions[idx].action;
  if (!action || !*action) action = "unknown";
  ok = job->state == JOB_FINISHED && !job->killed_for_timeout &&
       !job->killed_for_output_limit;
  reason = ok ? NULL : job->killed_for_timeout ? "timeout" :
           job->killed_for_output_limit ? "output_limit_exceeded" : "process_failed";
  if (fs_read_text(job->out_path, &output, FS_READ_TEXT_DEFAULT_CAP) == 0 && output) {
    const RtActionDef *def = rt_action_registry_find(&r->scheduler->actions, action);
    int is_managed_op = def ? def->managed_operation : 0;
    if (rt_action_output_extract(output, job->out_path, result, sizeof(result),
                                error_obj, sizeof(error_obj), is_managed_op) != 0) {
      emit_rc = rt_action_emit_terminal(
          r, "action_failed", rid, action, job->job_id, task_id,
          ok ? "invalid_action_output" : reason, "{}", error_obj);
    } else {
      if (!ok && strcmp(error_obj, "null") == 0) {
        snprintf(error_obj, sizeof(error_obj),
                 "{\"message\":\"action %s failed\",\"exit_status\":%d,\"reason\":\"%s\"}",
                 action, job->exit_status, reason ? reason : "process_failed");
      }
      /* Auto-wrap managed-op subprocess actions the same way intrinsics
       * are wrapped in rt_action_runtime_complete. Subprocess actions can
       * still emit status/handle themselves; the wrap respects existing
       * status fields and is a no-op if present. */
      if (ok && is_managed_op) {
        rt_action_wrap_finished_result(result, sizeof(result), rid);
      }
      emit_rc = rt_action_emit_terminal(
          r, ok ? "action_succeeded" : "action_failed",
          rid, action, job->job_id, task_id, ok ? NULL : reason,
          ok ? result : "{}", ok ? "null" : error_obj);
    }
  } else if (!ok) {
    snprintf(error_obj, sizeof(error_obj),
             "{\"message\":\"action %s failed\",\"exit_status\":%d,\"reason\":\"%s\"}",
             action, job->exit_status, reason);
    emit_rc = rt_action_emit_terminal(
        r, "action_failed", rid, action, job->job_id, task_id,
        reason, "{}", error_obj);
  } else {
    snprintf(error_obj, sizeof(error_obj),
             "{\"message\":\"action %s produced no output\"}", action);
    emit_rc = rt_action_emit_terminal(
        r, "action_failed", rid, action, job->job_id,
        task_id, "no_output", "{}", error_obj);
  }
  if (output) fc_xfree(output);
  return emit_rc;
}
