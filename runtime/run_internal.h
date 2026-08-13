#ifndef FCLAW_RUN_INTERNAL_H
#define FCLAW_RUN_INTERNAL_H

/* Cross-file private declarations for the runtime kernel split.
 * Symbols here are NOT public API; gateway modules and channel
 * adapters must not include this header. The runtime_kernel.h header
 * is the public surface for the rest of the binary. */

#include "runtime_kernel.h"

/* RtJobSpec packs the variable args to rt_jobrunner_launch so both
 * agent and action launchers share rt_run_start_tracked_job instead of
 * each doing the launch + waiting_job_add dance. stdout_cap and
 * stderr_cap are not exposed; both callers want 0 today. */
typedef struct {
  char *const *argv;
  const char *stdin_path;     /* NULL = no stdin redirect */
  const char *stdout_path;
  const char *stderr_path;
  JobKind kind;
  const char *run_id;
  const char *origin_event_id;
  const char *step_id;        /* NULL for action jobs */
  const char *request_id;     /* NULL for agent jobs */
  const char *bound_task_id;   /* agent jobs only; hidden selected-task binding */
  long long bound_work_rev;    /* agent jobs only; reducer-owned work revision */
  const char *workspace_root;  /* subprocess action jobs; exported as env */
  const char *action_id;       /* subprocess action jobs; selects the
                                * action:<id>: secret namespace injected
                                * into the child's env. NULL = none. */
  int private_channel;         /* expose scoped read/list/set/delete broker
                                * instead of legacy environment injection */
  char *const *env_extra;      /* NULL-terminated NAME=VALUE list set in the
                                * child before exec; resolved declared
                                * config. Caller-owned, must outlive the
                                * launch call. NULL = none. */
  uint64_t deadline_ms;
} RtJobSpec;

/* Scheduler pool slot management. Callable from ports.c when a new
 * envelope creates a run. */
RtRun *rt_scheduler_alloc_run(RtScheduler *s);
void   rt_scheduler_free_run(RtScheduler *s, RtRun *r);
int    rt_scheduler_recover_runs(RtScheduler *s, char *err, size_t err_len);

/* Shared helpers used across scheduler.c / run.c / agent_runner.c /
 * action_runner.c / ports.c. Implemented in run.c.
 *   rt_run_start_tracked_job:    rt_jobrunner_launch + waiting-job add.
 *   rt_run_apply_output_events:  thread an agent's emitted events into
 *                                run state via the in-memory projector.
 *   rt_run_find_step_by_id:      scan the run's profile by step id. */
int  rt_run_start_tracked_job(RtRun *r, RtJobRunner *runner,
                              const RtJobSpec *spec, RtJob **out_job);
void rt_run_apply_output_events(RtRun *r, const char *source_agent, const char *output_json);
const RtStep *rt_run_find_step_by_id(const RtProfile *p, const char *id);

/* Wait state — cleared by scheduler after a job retires; the
 * per-kind setters stay private to run.c's step machine. */
void rt_run_wait_clear(RtRun *r);

/* Agent runner — implemented in agent_runner.c. */
int rt_agent_runner_preload_meta(RtScheduler *s, const char *agent_id,
                                 char *err, size_t err_len);
int rt_agent_runner_start_step(RtRun *r, const RtStep *step, RtJobRunner *runner);
int rt_agent_runner_complete_job(RtRun *r, RtJob *job);

/* Action runner — implemented in action_runner.c.
 *   note_request: action_request seen, queue it.
 *   note_started: action_started seen, mark queue entry as live.
 *   note_terminal: action_rejected/succeeded/failed seen, complete queue entry.
 *   peek_next:    next not-yet-started pending request, into out bufs.
 *                 Returns 0 if found, 1 if queue empty.
 *   launch:       start the subprocess (or synthesize if builtin).
 *                 Returns 0 launched/synthesized, 1 slot-busy, -1 fail.
 *   complete_job: scheduler hands a finished JOB_ACTION job back. */
int  rt_action_runner_note_request(RtRun *r, const char *source_agent, const char *rid,
                                   const char *action, const char *args,
                                   const char *task_id, long long work_rev);
void rt_action_runner_note_trigger(RtRun *r, const char *rid,
                                   const char *trigger_event_id);
void rt_action_runner_note_started(RtRun *r, const char *rid);
void rt_action_runner_note_terminal(RtRun *r, const char *rid);
int  rt_action_runner_peek_next(const RtRun *r,
                               char *out_rid,   size_t rid_len,
                               char *out_action, size_t action_len,
                               char *out_args,  size_t args_len);
int  rt_action_runner_launch(RtRun *r, const char *rid, const char *action,
                            const char *args, RtJobRunner *runner);
int  rt_action_runner_complete_job(RtRun *r, RtJob *job);
int  rt_action_runner_pending_index(const RtRun *r, const char *rid);
/* Cold-start repair for action_started records with no committed terminal.
 * Message delivery is reconciled against the ports-owned delivery ledger;
 * outside-world calls become ambiguous_completion blockers and are never
 * replayed; registry-declared local calls become launchable again. */
int  rt_action_runner_recover_started(RtRun *r);

#endif
