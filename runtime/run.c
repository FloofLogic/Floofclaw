/* runtime/run.c — per-run state machine.
 *
 * Owns one RtRun's lifecycle: profile phase progression, lifecycle
 * events, advance accounting, wait-state transitions, and the
 * dispatch from steps to agent_runner / action_runner.
 *
 * Anything that crosses RtRun boundaries (allocating from the pool,
 * pumping finished jobs, retiring runs, the reactor pass itself)
 * lives in runtime/scheduler.c. Boundary I/O is in ports.c. */

#include "run_internal.h"
#include "support/json.h"
#include "support/heap_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tracked job launch — both agent_runner and action_runner use this to
 * launch their subprocesses. Pulls the rt_jobrunner_launch +
 * rt_run_waiting_job_add dance into one place. */
int rt_run_start_tracked_job(RtRun *r, RtJobRunner *runner,
                             const RtJobSpec *spec, RtJob **out_job) {
  RtJob *job = NULL;
  if (!r || !runner || !spec) return -1;
  if (rt_jobrunner_launch(runner,
                          spec->argv,
                          spec->stdin_path,
                          spec->stdout_path,
                          spec->stderr_path,
                          spec->kind,
                          spec->run_id,
                          spec->origin_event_id,
                          spec->step_id,
                          spec->request_id,
                          spec->bound_task_id,
                          spec->bound_work_rev,
                          spec->workspace_root,
                          spec->action_id,
                          spec->private_channel,
                          spec->env_extra,
                          spec->deadline_ms,
                          0, 0,
                          &job) != 0) return -1;
  if (job) rt_run_waiting_job_add(r, job->job_id);
  if (out_job) *out_job = job;
  return 0;
}

/* ----- advance accounting ----- */

static void advance_flags_reset(RtRun *r) {
  if (!r) return;
  r->advance_started_job = 0;
  r->advance_delivered_message = 0;
  r->advance_state_changed = 0;
  r->advance_work_progress = 0;
}

static void rt_run_set_phase_index(RtRun *r, size_t phase_index) {
  if (!r) return;
  r->phase_index = phase_index;
  (void)rt_run_persist_state(r);
}

static void rt_run_advance_phase(RtRun *r) {
  if (!r) return;
  rt_run_set_phase_index(r, r->phase_index + 1);
}

static int event_type_changes_state(const char *type) {
  if (!type) return 0;
  /* Memory record types and task reducer events durably mutate
   * workspace state. Only task mutations count as work progress. */
  return strcmp(type, "user") == 0 ||
         strcmp(type, "asst") == 0 ||
         strcmp(type, "fact") == 0 ||
         strcmp(type, "summ") == 0 ||
         strcmp(type, "memory_compacted") == 0 ||
         rt_task_is_event_type(type);
}

void rt_run_apply_output_events(RtRun *r, const char *source_agent, const char *output_json) {
  JsonRef root, events;
  size_t count, i;
  if (!output_json) return;
  if (json_ref_first_object(output_json, &root) != 0) return;
  if (json_ref_object_get_array(&root, "events", &events) != 0) return;
  count = json_ref_array_size(&events);
  for (i = 0; i < count; ++i) {
    JsonRef ev, payload_ref;
    JsonRef args_ref;
    char type[RT_SMALL] = "";
    char rid[RT_SMALL] = "", action[RT_SMALL] = "", task_id[RT_SMALL] = "";
    char trigger_event_id[RT_SMALL] = "";
    long long work_rev = 0;
    char *args = NULL;
    const char *effective_args = "{}";
    if (json_ref_array_get(&events, i, &ev) != 0) continue;
    if (json_ref_object_get_string(&ev, "type", type, sizeof(type)) != 0) continue;
    if (event_type_changes_state(type)) r->advance_state_changed = 1;
    if (rt_task_is_event_type(type)) {
      r->advance_work_progress = 1;
      continue;
    }
    if (strcmp(type, "action_request") != 0) continue;
    if (json_ref_object_get_object(&ev, "payload", &payload_ref) != 0) continue;
    (void)json_ref_object_get_string(&payload_ref, "request_id", rid, sizeof(rid));
    (void)json_ref_object_get_string(&payload_ref, "action", action, sizeof(action));
    (void)json_ref_object_get_string(&payload_ref, "task_id", task_id, sizeof(task_id));
    (void)json_ref_object_get_long(&payload_ref, "work_rev", &work_rev);
    (void)json_ref_object_get_string(&payload_ref, "trigger_event_id",
                                     trigger_event_id,
                                     sizeof(trigger_event_id));
    if (json_ref_object_get(&payload_ref, "args", &args_ref) == 0) {
      args = json_ref_value_compact_dup(&args_ref);
      if (args) effective_args = args;
    }
    int nr = rt_action_runner_note_request(r, source_agent, rid, action,
                                           effective_args, task_id, work_rev);
    if (nr == 1)
      rt_action_runner_note_trigger(r, rid, trigger_event_id);
    fc_xfree(args);
    if (nr == 1) {
      if (task_id[0] && strcmp(action, "message") != 0)
        r->advance_work_progress = 1;
    }
  }
}

/* ----- lifecycle ----- */

static int emit_lifecycle(RtRun *r, const char *type, const char *reason) {
  char payload[RT_LARGE];
  char profile_esc[RT_MED];
  unsigned long long mono = (unsigned long long)fc_now_ms();
  if (json_escape(r->profile->name, profile_esc, sizeof(profile_esc)) != 0) profile_esc[0] = '\0';
  if (reason && *reason) {
    char esc[RT_MED];
    if (json_escape(reason, esc, sizeof(esc)) != 0) esc[0] = '\0';
    return rt_append_event_format(
        &r->ctx, type, "kernel", payload, sizeof(payload), 0,
        "{\"run_id\":\"%s\",\"profile_name\":\"%s\",\"origin_event_id\":\"%s\","
        "\"monotonic_ms\":%llu,\"reason\":\"%s\"}",
        r->ctx.run_id, profile_esc, r->ctx.origin_event_id, mono, esc);
  } else {
    return rt_append_event_format(
        &r->ctx, type, "kernel", payload, sizeof(payload), 0,
        "{\"run_id\":\"%s\",\"profile_name\":\"%s\",\"origin_event_id\":\"%s\","
        "\"monotonic_ms\":%llu}",
        r->ctx.run_id, profile_esc, r->ctx.origin_event_id, mono);
  }
}

static void apply_run_terminal_event(RtRun *r, const char *type, const char *reason,
                                     RtRunState final_state) {
  if (!r || r->lifecycle_terminal_emitted) return;
  if (final_state == RT_RUN_FAILED && !r->last_error_code[0])
    rt_run_set_error(r, reason && *reason ? reason : "run_failed",
                     reason && *reason ? reason : "run failed", NULL);
  if (strcmp(type, "run_failed") == 0) {
    (void)rt_task_terminalize_open_message_tasks_for_run(&r->ctx, "task_failed");
  } else if (strcmp(type, "run_canceled") == 0) {
    (void)rt_task_terminalize_open_message_tasks_for_run(&r->ctx, "task_canceled");
  }
  if (emit_lifecycle(r, type, reason) != 0) {
    fprintf(stderr,
            "run: terminal event append/fsync failed for %s (%s); "
            "retiring failed\n",
            r->ctx.run_id, type);
    (void)rt_narrate(
        "RUN FAILED: %s (%s) — terminal event append/fsync failed while recording %s",
        r->ctx.run_id, r->ctx.event_kind[0] ? r->ctx.event_kind : "?", type);
    rt_run_set_error(r, "terminal_event_sync_failed",
                     "terminal event append/fsync failed", NULL);
    r->lifecycle_terminal_emitted = 1;
    rt_run_set_state(r, RT_RUN_FAILED);
    return;
  }
  if (strcmp(type, "run_failed") == 0)
    (void)rt_narrate("RUN FAILED: %s (%s) — %s", r->ctx.run_id,
                     r->ctx.event_kind[0] ? r->ctx.event_kind : "?",
                     reason && *reason ? reason : "unknown");
  r->lifecycle_terminal_emitted = 1;
  rt_run_set_state(r, final_state);
}

void rt_run_finish(RtRun *r)                    { apply_run_terminal_event(r, "run_done",     NULL,   RT_RUN_DONE);   }
void rt_run_fail  (RtRun *r, const char *why)   { apply_run_terminal_event(r, "run_failed",   why,    RT_RUN_FAILED); }
void rt_run_cancel(RtRun *r, const char *why)   { apply_run_terminal_event(r, "run_canceled", why,    RT_RUN_FAILED); }

/* ----- profile lookup ----- */

const RtStep *rt_run_find_step_by_id(const RtProfile *p, const char *id) {
  if (!p || !id || !*id) return NULL;
  for (int i = 0; i < p->step_count; ++i)
    if (strcmp(p->steps[i].id, id) == 0) return &p->steps[i];
  return NULL;
}

/* ----- wait state -----
 * One owner per kind, one helper to clear. The previous code had
 * inline wait_* pokes scattered across launch callers, wake handlers,
 * and the slot-waiter wake loop — those forgot to clear individual
 * fields and quietly left stale data behind. */

void rt_run_wait_clear(RtRun *r) {
  if (!r) return;
  r->wait_reason = RT_WAIT_REASON_NONE;
  r->wait_step_id[0]    = '\0';
  r->wait_request_id[0] = '\0';
  r->wait_action[0]      = '\0';
}

static void rt_run_wait_for_agent(RtRun *r, const char *step_id) {
  if (!r) return;
  rt_run_set_state(r, RT_RUN_WAITING_JOBS);
  r->wait_reason = RT_WAIT_REASON_AGENT_JOB;
  snprintf(r->wait_step_id, sizeof(r->wait_step_id), "%s", step_id ? step_id : "");
  r->wait_request_id[0] = '\0';
  r->wait_action[0]      = '\0';
}

static void rt_run_wait_for_action_job(RtRun *r, const char *rid, const char *action) {
  if (!r) return;
  rt_run_set_state(r, RT_RUN_WAITING_JOBS);
  r->wait_reason = RT_WAIT_REASON_ACTION_JOB;
  snprintf(r->wait_request_id, sizeof(r->wait_request_id), "%s", rid ? rid : "");
  snprintf(r->wait_action,      sizeof(r->wait_action),      "%s", action ? action : "");
  r->wait_step_id[0] = '\0';
}

static void rt_run_wait_for_action_slot(RtRun *r, const char *rid, const char *action) {
  if (!r) return;
  rt_run_set_state(r, RT_RUN_WAITING_ACTION_SLOT);
  r->wait_reason = RT_WAIT_REASON_ACTION_SLOT;
  snprintf(r->wait_request_id, sizeof(r->wait_request_id), "%s", rid ? rid : "");
  snprintf(r->wait_action,      sizeof(r->wait_action),      "%s", action ? action : "");
  r->wait_step_id[0] = '\0';
}

/* ----- step machine ----- */

static const RtAgentMeta *run_agent_meta_lookup(const RtRun *r, const char *agent_id) {
  if (!r || !r->scheduler || !agent_id) return NULL;
  for (size_t i = 0; i < r->scheduler->agent_meta_count; ++i)
    if (strcmp(r->scheduler->agent_meta[i].id, agent_id) == 0)
      return &r->scheduler->agent_meta[i];
  return NULL;
}

static int affair_extraction_needed(RtRun *r) {
  if (!r) return 0;
  /* When the inbound user_message already names the affair (manual
   * review or watcher fire), skip extraction — we already know the
   * affair this turn is about. Otherwise let the floop's extractor
   * agent decide via its prompt; the engine has no business judging
   * which user inputs look "concern-shaped" or "work-shaped." */
  return r->ctx.inbound_affair_id[0] ? 0 : 1;
}

/* Declarative event-kind gate: gate value "event_kind:a,b,c" opens the
 * step only when the run's triggering event kind matches one of the
 * configured tokens. The tokens are opaque configuration strings; the
 * kernel only compares bytes. */
static int event_kind_gate_matches(const RtRun *r, const char *list) {
  const char *kind = r ? r->ctx.event_kind : NULL;
  const char *p = list;
  size_t klen;
  if (!kind || !kind[0] || !p) return 0;
  klen = strlen(kind);
  while (*p) {
    const char *end = strchr(p, ',');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == klen && strncmp(p, kind, n) == 0) return 1;
    if (!end) break;
    p = end + 1;
  }
  return 0;
}

static const char *step_gate_skip_reason(RtRun *r, const RtStep *step) {
  if (!step || !step->gate[0] || strcmp(step->gate, "always") == 0) return NULL;
  if (strncmp(step->gate, "event_kind:", 11) == 0)
    return event_kind_gate_matches(r, step->gate + 11) ? NULL : "event_kind_mismatch";
  if (strcmp(step->gate, "work_consequence") == 0) {
    char task_id[RT_SMALL];
    long long work_rev = 0;
    int selected = rt_task_select_work_consequence(
        r ? &r->ctx : NULL, task_id, sizeof(task_id), &work_rev,
        NULL, 0);
    if (selected == 0) return NULL;
    if (selected == 1) return "no_work_consequence";
    if (selected == 2) return "multiple_work_consequences";
    if (selected == 3) return "stale_work_revision";
    if (selected == 4) return "uncorrelated_work_result";
    if (selected == 5) return "work_consequence_consumed";
    return "work_consequence_lookup_failed";
  }
  if (strcmp(step->gate, "terminal_work_outcome") == 0) {
    int matched = r ? rt_work_state_outcome_matches(&r->ctx) : -1;
    if (matched == 1) return NULL;
    return matched == 0 ? "uncorrelated_work_outcome"
                        : "work_outcome_lookup_failed";
  }
  if (strcmp(step->gate, "after_message_result") == 0)
    return (r && r->advance_delivered_message) ? NULL : "config_noop";
  if (strcmp(step->gate, "memory_compaction_due") == 0)
    return (r && r->ctx.memory_compaction_due) ? NULL : "memory_compaction_not_due";
  if (strcmp(step->gate, "affair_extraction_maybe_needed") == 0)
    return affair_extraction_needed(r) ? NULL : "affair_extraction_not_needed";
  return "config_noop";
}

static int step_gate_allows(RtRun *r, const RtStep *step) {
  return step_gate_skip_reason(r, step) == NULL;
}

static int step_is_explicit_action_runner(const RtStep *step) {
  return step && strcmp(step->type, "builtin") == 0 &&
         strcmp(step->target, "action_runner") == 0;
}

static int post_reply_bookkeeping_failed(const RtRun *r, const char *step_id) {
  return r && r->advance_delivered_message &&
         step_id && strcmp(step_id, "memory.after") == 0;
}

/* Drain the pending-action queue. Loops until: queue empty (DONE),
 * a launch needs a slot (WAITING_ACTION_SLOT), or a launch returned
 * but a subprocess is now in flight (WAITING_JOB). Runtime-intrinsic
 * launches complete synchronously inside the launch call, so this
 * loop is the "drain-all" semantics — what used to take two
 * action_runner phases now happens in one. */
static RtStepResult drain_pending_effect(RtRun *r, RtJobRunner *runner, int implicit) {
  int launched_any = 0;
  for (;;) {
    char rid[RT_SMALL] = "", action[RT_SMALL] = "", args[RT_LARGE] = "";
    int lr;
    if (rt_action_runner_peek_next(r, rid, sizeof(rid),
                                  action, sizeof(action),
                                  args, sizeof(args)) == 1)
      return launched_any ? RT_STEP_ADVANCED : RT_STEP_DONE;
    lr = rt_action_runner_launch(r, rid, action, args, runner);
    if (lr == 1) {
      (void)rt_write_loop_decision_trace(&r->ctx, "wait", "pending_effect", r->advance);
      rt_run_wait_for_action_slot(r, rid, action);
      return RT_STEP_WAITING_ACTION_SLOT;
    }
    if (lr == 0) {
      launched_any = 1;
      if (r->waiting_job_count > 0) {
        /* Parking on a subprocess is not a reason to restart the recipe.
         * A one-pass profile has already declared "one event -> one run ->
         * one complete pass" (see finish_advance), so hold the phase and
         * resume at the dispatch step: it re-drains an empty queue and
         * walks on to the steps after the wait. Rewinding here re-ran every
         * upstream step, and an event_kind: gate cannot close -- the run's
         * event_kind is fixed for its lifetime -- so its handler fired
         * again, while the steps below the wait never ran in the first
         * pass at all. A looping profile still rewinds; that is its
         * declared behavior, not a property of the wait. */
        if (!implicit && !r->action_cli_only &&
            !(r->profile && r->profile->one_pass))
          r->phase_index = 0;
        (void)rt_write_loop_decision_trace(&r->ctx, "wait", "pending_effect", r->advance);
        rt_run_wait_for_action_job(r, rid, action);
        return RT_STEP_WAITING_JOB;
      }
      /* Runtime intrinsic completed synchronously — keep draining. */
      continue;
    }
    rt_run_fail(r, "action_launch");
    return RT_STEP_FAILED;
  }
}

/* Begin a fresh profile pass if we just rolled back to phase zero.
 * Returns 0 normally, -1 if the pass cap is hit. The cap is the
 * runtime's only infinite-loop guard; it does not judge what an agent
 * emits during a single pass. */
static int begin_advance_if_needed(RtRun *r) {
  if (!r) return 0;
  if (r->phase_index == 0 && r->state == RT_RUN_READY) {
    if (r->advance >= RT_MAX_PROFILE_PASSES) {
      rt_run_set_error(r, "max_profile_passes_exceeded",
                       "profile pass cap reached without quiescing",
                       NULL);
      rt_run_fail(r, "max_profile_passes_exceeded");
      return -1;
    }
    r->advance++;
    advance_flags_reset(r);
  }
  if (r->state == RT_RUN_READY) rt_run_set_state(r, RT_RUN_RUNNING);
  return 0;
}

/* End-of-profile is a quiescence checkpoint. If the pass produced
 * accepted work, cycle and let the agents observe the new state. If a
 * full pass produces no accepted work and no async effects are pending,
 * the run is done. */
static RtStepResult finish_advance(RtRun *r) {
  int actionable;
  if (!r) return RT_STEP_FAILED;
  if (r->waiting_job_count > 0) {
    (void)rt_write_loop_decision_trace(&r->ctx, "wait", "pending_effect", r->advance);
    rt_run_set_state(r, RT_RUN_WAITING_JOBS);
    return RT_STEP_WAITING_JOB;
  }
  /* One event -> one run -> one complete pass: never rewind to phase
   * zero. Whatever work state exists, only a new event creates the
   * next pass through this profile. */
  if (r->action_cli_only || (r->profile && r->profile->one_pass)) {
    (void)rt_write_loop_decision_trace(&r->ctx, "done", "one_pass", r->advance);
    rt_run_finish(r);
    return RT_STEP_DONE;
  }
  actionable = rt_task_context_has_actionable(r->ctx.context_id);
  if (r->advance_work_progress && actionable) {
    (void)rt_write_loop_decision_trace(&r->ctx, "continue", "work_progress", r->advance);
    r->phase_index = 0;
    rt_run_set_state(r, RT_RUN_READY);
    return RT_STEP_ADVANCED;
  }
  if (actionable)
    (void)rt_write_loop_decision_trace(&r->ctx, "quiesce", "no_progress", r->advance);
  else
    (void)rt_write_loop_decision_trace(&r->ctx, "done", "no_live_work", r->advance);
  rt_run_finish(r);
  return RT_STEP_DONE;
}

RtStepResult rt_run_step_ready(RtRun *r, RtJobRunner *runner) {
  if (!r->run_started_emitted) {
    emit_lifecycle(r, "run_started", NULL);
    r->run_started_emitted = 1;
  }
  if (rt_write_state_snapshot(&r->ctx) != 0) {
    rt_run_fail(r, "snapshot_failed");
    return RT_STEP_FAILED;
  }
  if (begin_advance_if_needed(r) != 0) return RT_STEP_FAILED;
  if (r->pending_action_count > 0) {
    const RtStep *next = r->phase_index < (size_t)r->profile->step_count
                       ? &r->profile->steps[r->phase_index] : NULL;
    if (!step_is_explicit_action_runner(next))
      return drain_pending_effect(r, runner, 1);
  }
  if (r->phase_index >= (size_t)r->profile->step_count) {
    return finish_advance(r);
  }
  const RtStep *step = &r->profile->steps[r->phase_index];
  if (strcmp(step->gate, "work_consequence") == 0) {
    char task_id[RT_SMALL];
    long long work_rev = 0;
    int selected = rt_task_select_work_consequence(
        &r->ctx, task_id, sizeof(task_id), &work_rev, NULL, 0);
    if (selected == 2 || selected < 0) {
      (void)rt_emit_error(
          &r->ctx, "kernel",
          selected == 2
              ? "step %s requires exactly one current-run work consequence; found multiple"
              : "step %s work consequence lookup failed",
          step->id);
      rt_run_fail(r, selected == 2
                      ? "multiple_work_consequences"
                      : "work_consequence_lookup_failed");
      return RT_STEP_FAILED;
    }
  }
  if (strcmp(step->gate, "terminal_work_outcome") == 0 &&
      rt_work_state_outcome_matches(&r->ctx) < 0) {
    (void)rt_emit_error(&r->ctx, "kernel",
                        "step %s terminal work-outcome lookup failed",
                        step->id);
    rt_run_fail(r, "work_outcome_lookup_failed");
    return RT_STEP_FAILED;
  }
  if (!step_gate_allows(r, step)) {
    const char *reason = step_gate_skip_reason(r, step);
    (void)rt_write_phase_skipped_trace(&r->ctx, r->advance, step->id,
                                       reason ? reason : "config_noop");
    rt_run_advance_phase(r);
    return RT_STEP_ADVANCED;
  }
  (void)rt_write_trace(&r->ctx, step->id, step->type);

  if (strcmp(step->type, "agent") == 0) {
    int lr = rt_agent_runner_start_step(r, step, runner);
    if (lr < 0) {
      if (step->non_critical) {
        /* Maintenance step: record the failure, keep the run alive.
         * Nothing already committed in this run may be replayed
         * because of a maintenance error. */
        (void)rt_write_phase_skipped_trace(&r->ctx, r->advance, step->id,
                                           "non_critical_step_failed");
        rt_run_advance_phase(r);
        return RT_STEP_ADVANCED;
      }
      if (post_reply_bookkeeping_failed(r, step->id)) {
        rt_run_finish(r);
        return RT_STEP_DONE;
      }
      rt_run_fail(r, step->id);
      return RT_STEP_FAILED;
    }
    if (lr == 0) {
      /* native: phase already finished, no subprocess. A native agent
       * can't emit the {"tool":null} sentinel mid-cycle, so repeat
       * does not apply — advance as usual. */
      if (strcmp(step->id, "memory.after") == 0) {
        const RtAgentMeta *meta = run_agent_meta_lookup(r, step->target);
        if (rt_memory_maybe_request_compaction(&r->ctx, meta) != 0) {
          if (post_reply_bookkeeping_failed(r, step->id)) {
            rt_run_finish(r);
            return RT_STEP_DONE;
          }
          rt_run_fail(r, "memory_compaction_request");
          return RT_STEP_FAILED;
        }
      }
      rt_run_advance_phase(r);
      return RT_STEP_ADVANCED;
    }
    rt_run_advance_phase(r);
    rt_run_wait_for_agent(r, step->id);
    return RT_STEP_WAITING_JOB;
  }
  if (strcmp(step->type, "builtin") == 0 && strcmp(step->target, "action_runner") == 0) {
    RtStepResult drained = drain_pending_effect(r, runner, 0);
    if (drained == RT_STEP_DONE) {
      rt_run_advance_phase(r);
      return RT_STEP_ADVANCED;
    }
    if (drained == RT_STEP_ADVANCED) {
      rt_run_advance_phase(r);
      return RT_STEP_ADVANCED;
    }
    return drained;
  }
  {
    char p[RT_LARGE];
    (void)rt_append_event_format(
        &r->ctx, "error", "kernel", p, sizeof(p), 0,
        "{\"message\":\"unknown step type or builtin\",\"step\":\"%s\"}",
        step->id);
  }
  rt_run_fail(r, "unknown_step");
  return RT_STEP_FAILED;
}
