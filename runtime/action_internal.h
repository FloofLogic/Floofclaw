#ifndef FCLAW_RUNTIME_ACTION_INTERNAL_H
#define FCLAW_RUNTIME_ACTION_INTERNAL_H

/* Shared internals across the action_* implementation files.
 *
 * action_runner.c   — public API + pending queue + validation + dispatch
 * action_events.c   — emit_action_started / _terminal / _rejected
 * action_runtime*.c — runtime-function dispatch and intrinsic families
 * action_subprocess.c — fork+exec path for non-intrinsic actions
 *
 * Anything declared here is "action module internal" — not part of the
 * runtime/runtime.h public surface, but shared between the files that
 * jointly implement the action_runner contract. */

#include "run_internal.h"
#include <limits.h>
#include <sys/types.h>

typedef struct {
  int  seq;
  char call_id[RT_MED];
  char in_path[PATH_MAX];
  char out_path[PATH_MAX];
  char err_path[PATH_MAX];
  char body_path[PATH_MAX];
  char body_rel_path[PATH_MAX];
} RtActionCallPaths;

/* ===== queue / signature helpers (defined in action_runner.c) ====== */
void rt_action_pending_remove_at(RtRun *r, size_t idx);
int  rt_action_terminal_index_of(RtRun *r, const char *rid);
int  rt_action_completed_sig_index_of(RtRun *r, const char *sig);
int  rt_action_pending_sig_index_of(RtRun *r, const char *sig);
void rt_action_completed_sig_add(RtRun *r, const char *sig, const char *rid);
/* The request_id of the first completed call with this signature, or
 * NULL. Used to memoize an equivalent repeat to its prior outcome. */
const char *rt_action_completed_sig_rid(RtRun *r, const char *sig);
void rt_action_terminal_add(RtRun *r, const char *rid);
void rt_action_signature(char *out, size_t out_len, const char *source,
                        const char *action, const char *args,
                        const char *task_id, long long work_rev);
const RtAgentMeta *rt_action_agent_meta_lookup(const RtScheduler *s,
                                              const char *agent_id);
int rt_action_call_paths_next(const RtRun *r, const char *rid,
                             RtActionCallPaths *out);

/* ===== event emitters (defined in action_events.c) ================ */
int rt_action_emit_started(RtRun *r, const char *rid, const char *action,
                          const char *job_id, const char *task_id);
int rt_action_emit_terminal(RtRun *r, const char *type, const char *rid,
                            const char *action, const char *job_id,
                            const char *task_id, const char *reason,
                            const char *result_json, const char *error_json);
int rt_action_emit_rejected(RtRun *r, const char *rid, const char *action,
                            const char *task_id, const char *reason,
                            const char *message, const char *errors_json,
                            const char *effect_sig);

/* ===== runtime-function dispatch (defined in action_runtime.c) ===== */
int rt_action_runtime_complete(RtRun *r, const char *rid, const RtActionDef *def,
                              const char *task_id, const char *args);

/* ===== subprocess dispatch (defined in action_subprocess.c) ======== */
int rt_action_subprocess_launch(RtRun *r, const char *rid, const RtActionDef *def,
                               const char *task_id, const char *args,
                               RtJobRunner *runner);

#endif
