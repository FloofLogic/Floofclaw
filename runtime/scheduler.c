/* runtime/scheduler.c — across-runs management.
 *
 * Owns the static run pool, profile-load + agent-meta preload at
 * init, the reactor pass split (intake → advance → retire → peaks),
 * the finished-job pump, the action-slot wake loop, and the orderly
 * shutdown.
 *
 * Per-run state machine lives in runtime/run.c.
 * Boundary I/O is in runtime/ports.c. */

#include "run_internal.h"
#include "ports.h"
#include "publication_outbox.h"
#include "support/heap_guard.h"
#include "support/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rt_next_run_number_for_workspace(const char *workspace);

/* `s` must arrive zeroed — allocate it with fc_xcalloc, or embed it in
 * calloc'd storage. This function deliberately does NOT memset it: the
 * struct is ~4.8 MB, almost all of it the fixed run pool, and memsetting
 * heap that another translation unit allocated faults every page at
 * startup (measured +4,768 KiB RSS versus +32 KiB for calloc, about 41%
 * of idle gateway RSS). calloc hands back lazily-faulted zero pages, and
 * rt_scheduler_alloc_run() already clears each slot as it is taken, so
 * only slots actually in use ever become resident. */
int rt_scheduler_init(RtScheduler *s, const char *loop_name, char *err, size_t err_len) {
  rt_jobrunner_init(&s->runner);
  snprintf(s->loop_name, sizeof(s->loop_name), "%s",
           loop_name ? loop_name : rt_default_loop());
  s->next_run_number = rt_next_run_number_for_workspace("workspace");
  if (s->next_run_number <= 0) {
    if (err) snprintf(err, err_len, "run allocator seed failed");
    return -1;
  }
  if (rt_profile_load(s->loop_name, &s->profile, err, err_len) != 0) {
    s->profile_loaded = 0;
    return -1;
  }
  s->profile_loaded = 1;
  if (rt_action_registry_load(&s->actions, err, err_len) != 0) return -1;
  for (int i = 0; i < s->profile.step_count; ++i) {
    const RtStep *step = &s->profile.steps[i];
    if (strcmp(step->type, "agent") != 0) continue;
    if (rt_agent_runner_preload_meta(s, step->target, err, err_len) != 0) return -1;
  }
  if (rt_work_state_rebuild_from_logs() != 0) {
    if (err) snprintf(err, err_len, "work-step ledger rebuild failed");
    return -1;
  }
  if (rt_scheduler_recover_runs(s, err, err_len) != 0) return -1;
  if (rt_publication_outbox_reconcile() != 0) {
    if (err) snprintf(err, err_len,
                      "publication outbox reconciliation failed");
    return -1;
  }
  /* A completion claim consumed from the inbox before a crash, with no
   * run bound to it, must survive the restart: requeue it for ordinary
   * intake before the reactor starts. */
  rt_ports_reconcile_orphaned_claims();
  return 0;
}

RtRun *rt_scheduler_alloc_run(RtScheduler *s) {
  if (!s) return NULL;
  for (int i = 0; i < RT_MAX_ACTIVE_RUNS; ++i) {
    if (!s->used[i]) {
      s->used[i] = 1;
      memset(&s->pool[i], 0, sizeof(RtRun));
      return &s->pool[i];
    }
  }
  return NULL;
}

void rt_scheduler_free_run(RtScheduler *s, RtRun *r) {
  if (!s || !r) return;
  ptrdiff_t idx = r - s->pool;
  if (idx >= 0 && idx < RT_MAX_ACTIVE_RUNS) {
    for (size_t i = 0; i < s->pool[idx].pending_action_count; ++i)
      fc_xfree(s->pool[idx].pending_actions[i].args_json);
    rt_context_clear_media_ref(&s->pool[idx].ctx);
    memset(&s->pool[idx], 0, sizeof(RtRun));
    s->used[idx] = 0;
  }
}

void rt_scheduler_destroy(RtScheduler *s) {
  if (!s) return;
  for (size_t i = 0; i < s->run_count; ++i) rt_scheduler_free_run(s, s->runs[i]);
  s->run_count = 0;
  rt_jobrunner_destroy(&s->runner);
}

int rt_scheduler_lookup_origin(const RtScheduler *s,
                               const char *origin_event_id,
                               char *run_id_out, size_t run_id_len) {
  if (run_id_out && run_id_len) run_id_out[0] = '\0';
  if (!s || !origin_event_id || !*origin_event_id) return 1;
  for (size_t i = 0; i < s->run_count; ++i) {
    const RtRun *r = s->runs[i];
    if (!r || strcmp(r->ctx.origin_event_id, origin_event_id) != 0)
      continue;
    if (run_id_out && run_id_len)
      snprintf(run_id_out, run_id_len, "%s", r->ctx.run_id);
    return 0;
  }
  return 1;
}

int rt_scheduler_cancel_origin(RtScheduler *s,
                               const char *origin_event_id,
                               const char *reason) {
  if (!s || !origin_event_id || !*origin_event_id) return 1;
  for (size_t i = 0; i < s->run_count; ++i) {
    RtRun *r = s->runs[i];
    if (!r || strcmp(r->ctx.origin_event_id, origin_event_id) != 0)
      continue;
    (void)rt_jobrunner_cancel_run(&s->runner, r->ctx.run_id, fc_now_ms());
    rt_run_cancel(r, reason && *reason ? reason : "client_canceled");
    return 0;
  }
  return 1;
}

/* One finder for both agent and action jobs. Dispatches on the job's
 * kind. */
static RtRun *rt_scheduler_find_run_for_job(RtScheduler *s, const RtJob *job) {
  if (!s || !job) return NULL;
  for (size_t i = 0; i < s->run_count; ++i) {
    RtRun *r = s->runs[i];
    if (strcmp(r->ctx.run_id, job->run_id) != 0) continue;
    if (job->kind == JOB_AGENT) {
      if (r->state == RT_RUN_WAITING_JOBS &&
          r->wait_reason == RT_WAIT_REASON_AGENT_JOB &&
          strcmp(r->wait_step_id, job->step_id) == 0)
        return r;
    } else {
      if (r->state == RT_RUN_WAITING_JOBS &&
          (strcmp(r->wait_request_id, job->request_id) == 0 ||
           rt_action_runner_pending_index(r, job->request_id) >= 0))
        return r;
    }
  }
  return NULL;
}

/* ----- the reactor pass ----- */

static void wake_run_after_job(RtRun *r, RtJob *j, int handler_rc) {
  if (handler_rc != 0) {
    rt_run_waiting_job_remove(r, j->job_id);
    if (j->kind == JOB_AGENT &&
        strcmp(j->step_id, "memory.after") == 0 &&
        r->advance_delivered_message) {
      rt_run_finish(r);
      return;
    }
    if (j->kind == JOB_AGENT && r->profile) {
      const RtStep *step = rt_run_find_step_by_id(r->profile, j->step_id);
      if (step && step->non_critical) {
        /* Maintenance failure: recorded by the completion handler, but
         * it never fails the processed event. Resume from the next
         * phase (the cursor already advanced past this step). */
        (void)rt_write_phase_skipped_trace(&r->ctx, r->advance, j->step_id,
                                           "non_critical_step_failed");
        rt_run_set_state(r, r->waiting_job_count ? RT_RUN_WAITING_JOBS : RT_RUN_RUNNING);
        rt_run_wait_clear(r);
        return;
      }
      if (r->profile->retry_attempts > 0 &&
          r->event_attempts < (unsigned)r->profile->retry_attempts) {
        /* Bounded per-event retry: rewind to phase zero and re-run the
         * profile for the same origin event. Calls already committed in
         * this run are memoized by signature, so committed effects are
         * reused, not repeated. */
        r->event_attempts++;
        (void)rt_emit_error(&r->ctx, "kernel",
                            "phase %s failed; retrying event (attempt %u of %d)",
                            j->step_id, r->event_attempts, r->profile->retry_attempts);
        rt_run_wait_clear(r);
        r->phase_index = 0;
        rt_run_set_state(r, RT_RUN_READY);
        return;
      }
    }
    rt_run_fail(r, j->kind == JOB_AGENT ? j->step_id : r->wait_action);
    return;
  }
  rt_run_waiting_job_remove(r, j->job_id);
  if (j->kind == JOB_AGENT) {
    rt_run_set_state(r, RT_RUN_RUNNING);
  } else {
    rt_run_set_state(r, r->waiting_job_count ? RT_RUN_WAITING_JOBS : RT_RUN_READY);
  }
  rt_run_wait_clear(r);
}

static void pump_finished_jobs(RtScheduler *s) {
  RtJob *j;
  while ((j = rt_jobrunner_pop_finished(&s->runner)) != NULL) {
    RtRun *r = rt_scheduler_find_run_for_job(s, j);
    if (r) {
      int rc = (j->kind == JOB_AGENT) ? rt_agent_runner_complete_job(r, j) : rt_action_runner_complete_job(r, j);
      wake_run_after_job(r, j, rc);
    }
    rt_jobrunner_release(&s->runner, j);
  }
}

static void wake_action_slot_waiters(RtScheduler *s) {
  if (rt_jobrunner_count_active(&s->runner, JOB_ACTION) != 0) return;
  for (size_t i = 0; i < s->run_count; ++i) {
    RtRun *r = s->runs[i];
    if (r->state == RT_RUN_WAITING_ACTION_SLOT) {
      rt_run_set_state(r, RT_RUN_READY);
      rt_run_wait_clear(r);
    }
  }
}

static int has_other_active_context_run(RtScheduler *s, RtRun *skip) {
  if (!s || !skip || !skip->ctx.context_id[0]) return 0;
  for (size_t i = 0; i < s->run_count; ++i) {
    RtRun *r = s->runs[i];
    if (!r || r == skip) continue;
    if ((r->state == RT_RUN_DONE || r->state == RT_RUN_FAILED) ||
        strcmp(r->ctx.context_id, skip->ctx.context_id) != 0) continue;
    return 1;
  }
  return 0;
}

/* Per-context serialization (profile option): a run must not advance
 * while an older non-terminal run exists for the same context. Intake
 * order is creation order and retire compaction preserves it, so
 * "older" is simply a lower index in s->runs[]. */
static int serialized_behind_older_run(const RtScheduler *s, size_t idx) {
  const RtRun *r = s->runs[idx];
  if (!s->profile.serialize_contexts || !r->ctx.context_id[0]) return 0;
  for (size_t j = 0; j < idx; ++j) {
    const RtRun *o = s->runs[j];
    if (!o || o->state == RT_RUN_DONE || o->state == RT_RUN_FAILED) continue;
    if (strcmp(o->ctx.context_id, r->ctx.context_id) == 0) return 1;
  }
  return 0;
}

static void step_ready_runs(RtScheduler *s) {
  for (size_t i = 0; i < s->run_count; ++i) {
    RtRun *r = s->runs[i];
    int advances = 0;
    if (serialized_behind_older_run(s, i)) continue;
    while ((r->state == RT_RUN_READY || r->state == RT_RUN_RUNNING) &&
           advances < RT_MAX_PHASE_ADVANCES_PER_TICK) {
      RtStepResult res = rt_run_step_ready(r, &s->runner);
      if (res == RT_STEP_ADVANCED) { advances++; continue; }
      break; /* terminal or waiting; rt_run_step_ready already mutated state */
    }
  }
}

static void retire_terminal_runs(RtScheduler *s) {
  size_t out = 0;
  for (size_t i = 0; i < s->run_count; ++i) {
    RtRun *r = s->runs[i];
    if (r->state == RT_RUN_DONE || r->state == RT_RUN_FAILED) {
      char log_line[256];
      snprintf(log_line, sizeof(log_line), "gateway: completed run %s\n", r->ctx.run_id);
      (void)fs_append_text("workspace/logs/gateway.log", log_line);
      if (r->state == RT_RUN_DONE) s->runs_succeeded++;
      else {
        rt_ports_emit_run_failure(r);
        s->runs_failed++;
      }
      snprintf(s->last_retired_origin_event_id,
               sizeof(s->last_retired_origin_event_id), "%s", r->ctx.origin_event_id);
      snprintf(s->last_retired_run_id,
               sizeof(s->last_retired_run_id), "%s", r->ctx.run_id);
      s->last_retired_state = r->state;
      if (!has_other_active_context_run(s, r))
        (void)rt_tasks_archive_completed_for_context(r->ctx.context_id);
      rt_scheduler_free_run(s, r);
      s->runs_completed++;
    } else {
      s->runs[out++] = r;
    }
  }
  s->run_count = out;
}

static void update_peaks(RtScheduler *s) {
  int active_actions;
  if (s->run_count > s->run_count_peak) s->run_count_peak = s->run_count;
  active_actions = rt_jobrunner_count_active(&s->runner, JOB_ACTION);
  if ((size_t)active_actions > s->action_count_peak) s->action_count_peak = (size_t)active_actions;
}

void rt_scheduler_advance_and_retire(RtScheduler *s, uint64_t now_ms) {
  if (!s) return;
  /* Intake runs before the runtime module. Capture the actual admitted
   * concurrency before a synchronous profile can finish and retire every
   * run in this same tick. The trailing sample still catches jobs launched
   * during stepping. */
  update_peaks(s);
  rt_operation_pump(s, now_ms);
  pump_finished_jobs(s);
  wake_action_slot_waiters(s);
  step_ready_runs(s);
  retire_terminal_runs(s);
  update_peaks(s);
}

void rt_scheduler_tick(RtScheduler *s, uint64_t now_ms) {
  if (!s) return;
  rt_jobrunner_tick(&s->runner, now_ms);
  rt_ports_drain_sources(s, RT_INTAKE_PER_TICK);
  rt_scheduler_advance_and_retire(s, now_ms);
}

void rt_scheduler_shutdown(RtScheduler *s, uint64_t grace_ms) {
  if (!s) return;
  rt_jobrunner_shutdown_all(&s->runner, grace_ms);
  for (size_t i = 0; i < s->run_count; ++i) {
    rt_run_cancel(s->runs[i], "gateway_shutdown");
    rt_scheduler_free_run(s, s->runs[i]);
  }
  s->run_count = 0;
}
