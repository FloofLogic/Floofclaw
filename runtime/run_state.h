#ifndef FCLAW_RUNTIME_RUN_STATE_H
#define FCLAW_RUNTIME_RUN_STATE_H

/* Persisted runstate status vocabulary. The serializer and every reader use
 * these predicates rather than reproducing lifecycle strings. */
int rt_run_status_is_valid(const char *status);
int rt_run_status_is_terminal(const char *status);

#endif
