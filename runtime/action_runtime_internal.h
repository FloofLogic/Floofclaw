#ifndef FCLAW_RUNTIME_ACTION_RUNTIME_INTERNAL_H
#define FCLAW_RUNTIME_ACTION_RUNTIME_INTERNAL_H

/* Private runtime-intrinsic implementations. action_runtime.c owns the
 * manifest-name dispatch table and completion lifecycle; family files own the
 * behavior of their related intrinsic actions. */

#include "action_internal.h"

typedef int (*RuntimeActionFn)(RtRun *, const char *, const RtActionDef *,
                               const char *, char *, size_t, char *, size_t);

int fc_list_directory(RtRun *, const char *, const RtActionDef *,
                      const char *, char *, size_t, char *, size_t);
int fc_read_file(RtRun *, const char *, const RtActionDef *,
                 const char *, char *, size_t, char *, size_t);
int fc_write_file(RtRun *, const char *, const RtActionDef *,
                  const char *, char *, size_t, char *, size_t);
int fc_rotate_file_intrinsic(RtRun *, const char *, const RtActionDef *,
                             const char *, char *, size_t, char *, size_t);
int fc_manage_codex(RtRun *, const char *, const RtActionDef *,
                    const char *, char *, size_t, char *, size_t);
int fc_manage_claude(RtRun *, const char *, const RtActionDef *,
                     const char *, char *, size_t, char *, size_t);

int rt_action_resolve_workspace_path(const RtRun *, const char *,
                                     char *, size_t, char *, size_t);

/* Replay reconciliation for a local (outside_world:false) intrinsic.
 *
 * A crash between action_started and the action's terminal leaves the
 * request replay-eligible, and recovery re-dispatches it under the same
 * stable request id. If the mutation event already committed, appending
 * it again would double the effect: a second note, a second counter
 * delta, a second affair, a second work revision. Every mutation
 * intrinsic therefore stamps its request id into the event it appends
 * and asks this first.
 *
 * Returns 1 with the committed payload copied into payload_out, 0 when
 * this request has not committed that event, -1 on error. */
int rt_action_committed_effect(const RtRun *r, const char *type,
                               const char *request_id,
                               char *payload_out, size_t payload_len);

int fc_work(RtRun *, const char *, const RtActionDef *,
            const char *, char *, size_t, char *, size_t);
int fc_working_memory_append(RtRun *, const char *, const RtActionDef *,
                             const char *, char *, size_t, char *, size_t);
int fc_work_complete(RtRun *, const char *, const RtActionDef *,
                     const char *, char *, size_t, char *, size_t);
int fc_work_blocked(RtRun *, const char *, const RtActionDef *,
                    const char *, char *, size_t, char *, size_t);

int fc_note_add(RtRun *, const char *, const RtActionDef *,
                const char *, char *, size_t, char *, size_t);
int fc_counter_bump(RtRun *, const char *, const RtActionDef *,
                    const char *, char *, size_t, char *, size_t);
int fc_affair_close(RtRun *, const char *, const RtActionDef *,
                    const char *, char *, size_t, char *, size_t);
int fc_defer(RtRun *, const char *, const RtActionDef *,
             const char *, char *, size_t, char *, size_t);
int fc_affair_open(RtRun *, const char *, const RtActionDef *,
                   const char *, char *, size_t, char *, size_t);

#endif
