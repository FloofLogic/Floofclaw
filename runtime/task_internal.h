#ifndef FCLAW_RUNTIME_TASK_INTERNAL_H
#define FCLAW_RUNTIME_TASK_INTERNAL_H

/* Shared internals for the task reducer, readers, and archival transition.
 *
 * The durable task envelope has one compatibility rule: current fields live
 * below `state`, while older records may keep the same fields at the task
 * root (and may encode status as a root `state` string). Keep that rule here
 * so reducers and projections cannot drift into different interpretations. */

#include "runtime.h"

#define RT_TASK_STATE_PATH "workspace/memory/state/tasks.json"
#define RT_TASK_ARCHIVE_PATH "workspace/memory/state/tasks_archive.jsonl"

int rt_task_json_status(const JsonRef *task, char *out, size_t out_len);
int rt_task_json_state_string(const JsonRef *task, const char *key,
                              char *out, size_t out_len);
int rt_task_json_state_value(const JsonRef *task, const char *key,
                             JsonRef *out);
int rt_task_json_state_array(const JsonRef *task, const char *key,
                             JsonRef *out);
int rt_task_json_state_long(const JsonRef *task, const char *key,
                            long long *out);
int rt_task_json_append_pair(char *out, size_t out_len, size_t *pos,
                             const char *key, const JsonRef *value,
                             int first);

#endif
