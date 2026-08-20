#ifndef FCLAW_RUNTIME_KERNEL_H
#define FCLAW_RUNTIME_KERNEL_H

#include "runtime.h"
#include "error_codes.h"
#include "run_state.h"
#include "gateway/jobrunner.h"

#include <stddef.h>
#include <stdint.h>

/* Static-shape capacity caps and tunables for the runtime kernel.
 * Every "interesting number" the kernel uses lives in this header so
 * sizing decisions are visible in one place. The kernel has no heap
 * allocations on the hot path; these caps shape the static pool. */

#define RT_MAX_ACTIVE_RUNS              16
#define RT_MAX_PHASE_ADVANCES_PER_TICK   8
#define RT_INTAKE_PER_TICK               4

/* Hard cap on pending actions *per run*. Picked to be comfortably above
 * any current loop's emit-per-step count (most loops emit 0–2). Hitting
 * this is an explicit error; the runtime will not silently truncate or
 * drop an action_request. */
#define RT_MAX_PENDING_ACTIONS           16
#define RT_MAX_WAITING_JOBS             16
#define RT_MAX_COMPLETED_ACTIONS         32

/* Hard cap on full profile passes for one run. A normal run completes
 * when a whole pass produces no accepted work and no async effects are
 * pending. This cap only protects the reactor from misconfigured loops
 * that keep producing progress forever. */
#define RT_MAX_PROFILE_PASSES           32

/* Per-job monotonic elapsed-time limits (ms). Agents get the larger budget
 * because LLM calls dominate; action subprocesses are local and
 * should return quickly. Tunable here, not per-runner. */
#define RT_AGENT_TIMEOUT_MS_DEFAULT     120000
#define RT_ACTION_TIMEOUT_MS_DEFAULT     60000

/* Per-run state ------------------------------------------------------- */

typedef enum {
  RT_RUN_READY,                /* can advance synchronously */
  RT_RUN_RUNNING,              /* currently advancing through profile steps */
  RT_RUN_WAITING_JOBS,         /* paused on one or more jobrunner jobs */
  RT_RUN_WAITING_EVENT,        /* claimed inbound (outbox wake or completion
                                * claim) whose first behavioral append is not
                                * yet proven; drained by the inbound-claim
                                * retry in ports.c */
  RT_RUN_WAITING_ACTION_SLOT,   /* action request pending; slot held by another run */
  RT_RUN_DONE,
  RT_RUN_FAILED
} RtRunState;

typedef enum {
  RT_STEP_ADVANCED,
  RT_STEP_WAITING_JOB,
  RT_STEP_WAITING_ACTION_SLOT,
  RT_STEP_DONE,
  RT_STEP_FAILED
} RtStepResult;

typedef enum {
  RT_WAIT_REASON_NONE,
  RT_WAIT_REASON_AGENT_JOB,
  RT_WAIT_REASON_ACTION_JOB,
  RT_WAIT_REASON_ACTION_SLOT
} RtRunWaitReason;

/* Pending action request, maintained in-memory by apply_action_* functions
 * (no event-log scanning). Scalar ids stay fixed-size; unbounded args
 * move as heap-owned compact JSON so large tool payloads do not get
 * truncated by the queue itself. Empty entries have request_id[0] == '\0'. */
typedef struct {
  char request_id[RT_SMALL];
  char source_agent[RT_SMALL];
  char action[RT_SMALL];
  char *args_json;      /* compact JSON object; NULL means "{}" */
  char sig[RT_MED];    /* source+action+args hash, used to catch duplicate intents */
  char task_id[RT_SMALL]; /* optional existing runtime-owned task reference */
  long long work_rev;  /* trusted controller binding; 0 for ordinary calls */
  char trigger_event_id[RT_SMALL]; /* exact work consequence; empty for legacy */
  int  started;        /* action_started seen */
  /* Managed-operation identity + capability, allocated by the runtime
   * before dispatch reaches the action. In-memory only: the durable
   * copies live in the operation store, and the token must never reach
   * events, artifacts, or model-visible text. */
  char op_id[RT_SMALL];
  char op_token[RT_OPERATION_TOKEN_HEX + 1];
} RtPendingAction;

typedef struct {
  RtContext ctx;
  /* Borrowed back-pointer to the scheduler. Used only to read cached
   * data (profile, agent metadata, …). Set at slot allocation time. */
  struct RtScheduler *scheduler;
  /* Borrowed pointer to scheduler-owned RtProfile (loaded once at
   * rt_scheduler_init). Not heap-owned by the run. */
  const RtProfile *profile;
  size_t phase_index;
  RtRunState state;
  RtRunWaitReason wait_reason;
  int run_started_emitted;
  int lifecycle_terminal_emitted;
  /* Set when state is WAITING_JOB or WAITING_ACTION_SLOT. */
  char wait_step_id[RT_SMALL];
  char wait_request_id[RT_SMALL];
  char wait_action[RT_SMALL];
  /* Per-run pending-action queue. The array is fixed-size; each nonempty
   * args_json payload is a bounded-lifetime heap allocation owned by its
   * entry. Hitting RT_MAX_PENDING_ACTIONS emits an error event; the runtime
   * never silently drops an action_request. */
  RtPendingAction pending_actions[RT_MAX_PENDING_ACTIONS];
  size_t pending_action_count;
  char completed_action_ids[RT_MAX_COMPLETED_ACTIONS][RT_SMALL];
  size_t completed_action_count;
  char completed_action_sigs[RT_MAX_COMPLETED_ACTIONS][RT_MED];
  /* request_id of the first completed call per signature, parallel to
   * completed_action_sigs. §5.G: lets an equivalent repeat be memoized
   * to that prior call's recorded terminal outcome. */
  char completed_action_sig_rids[RT_MAX_COMPLETED_ACTIONS][RT_SMALL];
  size_t completed_action_sig_count;
  /* Runstate spec v1.3.8 envelope fields. `phase_index` above is the
   * runtime-owned next-phase cursor and is persisted under cursors. */
  char context_id[RT_MED];
  char created_from_event_id[RT_SMALL];
  char created_from_type[RT_SMALL];
  unsigned int advance;
  /* Bounded per-event retry counter: number of times a failed phase
   * job caused this run to rewind to phase zero and re-run. Capped by
   * the profile's retry_attempts. */
  unsigned int event_attempts;
  char waiting_job_ids[RT_MAX_WAITING_JOBS][RT_SMALL];
  size_t waiting_job_count;
  char last_error_code[RT_SMALL];
  FcErrorCode last_error_class;
  char last_error_message[RT_MED];
  char last_error_job_id[RT_SMALL];
  int advance_started_job;
  int advance_delivered_message;
  int advance_state_changed;
  int advance_work_progress;
  /* A direct action CLI envelope owns only its pending effect. It uses the
   * active scheduler and registry but never walks product profile steps. */
  int action_cli_only;
} RtRun;

/* Scheduler ----------------------------------------------------------- */

typedef struct RtScheduler {
  RtJobRunner runner;
  /* Static pool of run slots. The runtime never malloc/frees an RtRun;
   * slots are reused across the gateway lifetime. */
  RtRun pool[RT_MAX_ACTIVE_RUNS];
  int   used[RT_MAX_ACTIVE_RUNS];   /* 1 = slot occupied, 0 = free */
  RtRun *runs[RT_MAX_ACTIVE_RUNS];  /* compacted active list (points into pool) */
  size_t run_count;
  size_t run_count_peak;       /* peak concurrent runs seen */
  size_t action_count_peak;     /* peak concurrent action jobs seen */
  uint64_t runs_completed;     /* total runs retired (DONE or FAILED) */
  uint64_t runs_succeeded;
  uint64_t runs_failed;
  char last_retired_origin_event_id[RT_SMALL];
  char last_retired_run_id[RT_SMALL];
  RtRunState last_retired_state;
  char loop_name[RT_SMALL];
  /* Profile loaded once at rt_scheduler_init. Borrowed by every RtRun
   * via run->profile. No file read or JSON parse per event. */
  RtProfile profile;
  int profile_loaded;
  /* Action contracts loaded once at scheduler init. Prompt rendering,
   * authorization, launch resolution, and args validation all read this
   * registry instead of scanning actions/ during the hot path. */
  RtActionRegistry actions;
  int next_run_number;
  /* Mock fixture sequence index. Test infrastructure only: when the
   * scheduler's environment carries LLM_MOCK_RESPONSE_PATHS, the
   * parent picks the next token before each LLM child launch and
   * passes it down as singular LLM_MOCK_RESPONSE_PATH. The child
   * mock provider knows nothing about sequencing — that's the
   * caller's job. Zero in production (env var unset). */
  int mock_seq_index;
  /* Agent metadata cache: one entry per "agent" step in the profile.
   * Populated alongside the profile at init. launch_agent_job looks up
   * by agent id instead of reading floop-local agent.json each event. */
  RtAgentMeta agent_meta[RT_MAX_STEPS];
  size_t agent_meta_count;
} RtScheduler;

/* Loads the loop profile into s->profile. Returns 0 on success, -1
 * if the profile failed to load (err filled). The gateway aborts
 * startup on failure rather than running with an unloaded profile. */
int  rt_scheduler_init(RtScheduler *s, const char *loop_name, char *err, size_t err_len);
void rt_scheduler_destroy(RtScheduler *s);

/* Reactor pass split into module-sized helpers. Each is bounded and
 * nonblocking. Bus intake (rt_ports_drain_*) lives in ports.h. */
void rt_scheduler_advance_and_retire(RtScheduler *s, uint64_t now_ms); /* runtime_module */

/* Single-tick helper used by the synchronous CLI (`fclaw run --text X`).
 * The gateway reactor uses the split helpers above. */
void rt_scheduler_tick(RtScheduler *s, uint64_t now_ms);
void rt_scheduler_shutdown(RtScheduler *s, uint64_t grace_ms);

/* Narrow runtime-control/correlation boundary for local channel adapters.
 * The adapter owns client ids; the scheduler resolves only its stable
 * origin_event_id. lookup returns 0 active, 1 not active. cancel returns
 * 0 when cancellation was committed, 1 when no active run exists. */
int rt_scheduler_lookup_origin(const RtScheduler *s,
                               const char *origin_event_id,
                               char *run_id_out, size_t run_id_len);
int rt_scheduler_cancel_origin(RtScheduler *s,
                               const char *origin_event_id,
                               const char *reason);

/* Step a READY run forward by one phase. Returns one of the RT_STEP_*
 * results; the run's state has already been mutated to match. */
RtStepResult rt_run_step_ready(RtRun *run, RtJobRunner *runner);

/* Terminal lifecycle helpers. Emit run_done / run_failed / run_canceled
 * exactly once and set the run's final state. No-op on a run that has
 * already emitted a terminal event. */
void rt_run_finish(RtRun *run);
void rt_run_fail  (RtRun *run, const char *reason);
void rt_run_cancel(RtRun *run, const char *reason);

/* Runstate spec v1.3.8 envelope helpers. The scheduler owns the RtRun, these
 * helpers only serialize/update its durable control fields. */
const char *rt_run_status_name(RtRunState state);
int  rt_run_persist_state(RtRun *run);
void rt_run_set_error_code(RtRun *run, FcErrorCode class_code,
                           const char *code, const char *message, const char *job_id);
void rt_run_set_error(RtRun *run, const char *code, const char *message, const char *job_id);
void rt_run_set_state(RtRun *run, RtRunState state);
int  rt_run_waiting_job_index_of(RtRun *run, const char *job_id);
void rt_run_waiting_job_add(RtRun *run, const char *job_id);
void rt_run_waiting_job_remove(RtRun *run, const char *job_id);

/* Operation driver (runtime/operation_driver.c): reacts to terminals of
 * managed-operation contract actions. Called from the action terminal
 * emitter; dispatches on the declared contract only. */
void rt_operation_driver_on_terminal(RtRun *r, const char *type, const char *action,
                                     const char *request_id,
                                     const char *trigger_event_id,
                                     const char *task_id, long long work_rev,
                                     const char *args_json,
                                     const char *result_json);
/* Preallocate operation_id + completion token + deadline for a managed
 * call, durably, before dispatch reaches the action. Idempotent for a
 * recovery relaunch of the same durable request id. */
int rt_operation_allocate_for_call(RtRun *r, const RtActionDef *def,
                                   RtPendingAction *p);
/* Post-admission projection hook for a bus completion/timeout claim:
 * the claim run's first behavioral event has already flipped the store;
 * this projects the terminal into the owning task and narrates. */
void rt_operation_on_claim_committed(RtRun *r, const char *op_id,
                                     const char *status,
                                     const char *excerpt);

#endif
