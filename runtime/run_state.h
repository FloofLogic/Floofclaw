#ifndef FCLAW_RUNTIME_RUN_STATE_H
#define FCLAW_RUNTIME_RUN_STATE_H

#include <stddef.h>

/* The persisted runstate key that carries lifecycle status. The serializer
 * and every reader spell it through this token, because they did not: the
 * janitor read "state" while the serializer wrote "status", so
 * `run_is_terminal` was always false and every deployment retained 100% of
 * its run directories with the janitor reporting zero errors. */
#define RT_RUNSTATE_STATUS_KEY "status"

/* Persisted runstate status vocabulary. The serializer and every reader use
 * these predicates rather than reproducing lifecycle strings. */
int rt_run_status_is_valid(const char *status);
int rt_run_status_is_terminal(const char *status);

/* Read the lifecycle status out of a runstate.json document. Returns 0 and
 * fills `out` on success; -1 when the document carries no readable status,
 * leaving `out` empty so the caller's predicate sees "not terminal". */
int rt_run_state_status_from_json(const char *runstate_json,
                                  char *out, size_t out_len);

#endif
