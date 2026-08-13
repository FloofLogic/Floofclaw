/* Task archival owns the terminal active-store -> archive transition.
 * Projections remain read-only; the task subsystem owns the current-store
 * envelope and its compatibility accessors. */

#include "task_internal.h"
#include "support/timing.h"

#include <stdlib.h>
#include <string.h>

static int append_json_string_value(char *out, size_t out_len, size_t *pos,
                                    const char *value) {
  char escaped[RT_XL];
  if (json_escape(value ? value : "", escaped, sizeof(escaped)) != 0 ||
      rt_append_text(out, out_len, pos, "\"") != 0 ||
      rt_append_text(out, out_len, pos, escaped) != 0)
    return -1;
  return rt_append_text(out, out_len, pos, "\"");
}

static int archive_line_for_task(const JsonRef *task, long long ms,
                                 char *out, size_t out_len) {
  size_t cursor = 0, pos = 0;
  char key[RT_SMALL];
  JsonRef value;
  int first = 1;
  if (rt_append_text(out, out_len, &pos, "{") != 0) return -1;
  while (json_ref_object_iter(task, &cursor, key, sizeof(key), &value) == 1) {
    if (strcmp(key, "state") == 0 ||
        strcmp(key, "final_state") == 0 ||
        strcmp(key, "archived_ms") == 0)
      continue;
    if (rt_task_json_append_pair(out, out_len, &pos, key, &value,
                                 first) != 0)
      return -1;
    first = 0;
  }
  {
    char work[RT_LARGE] = "", done[RT_LARGE] = "";
    char working_memory[RT_TASK_WORKING_MEMORY_MAX] = "";
    char artifacts[RT_LARGE] = "[]", tail[RT_SMALL];
    JsonRef array;
    (void)rt_task_json_state_string(task, "work", work, sizeof(work));
    (void)rt_task_json_state_string(task, "done_when", done, sizeof(done));
    (void)rt_task_json_state_string(task, "working_memory", working_memory,
                                    sizeof(working_memory));
    if (rt_task_json_state_array(task, "artifacts", &array) == 0) {
      char raw[RT_LARGE];
      if (json_ref_value_copy(&array, raw, sizeof(raw)) != 0 ||
          json_compact(raw, artifacts, sizeof(artifacts)) != 0)
        return -1;
    }
    if ((!first && rt_append_text(out, out_len, &pos, ",") != 0) ||
        rt_append_text(out, out_len, &pos,
                       "\"state\":{\"status\":\"archived\",\"work\":") != 0)
      return -1;
    if (work[0]) {
      if (append_json_string_value(out, out_len, &pos, work) != 0) return -1;
    } else if (rt_append_text(out, out_len, &pos, "null") != 0) {
      return -1;
    }
    if (rt_append_text(out, out_len, &pos, ",\"done_when\":") != 0)
      return -1;
    if (done[0]) {
      if (append_json_string_value(out, out_len, &pos, done) != 0) return -1;
    } else if (rt_append_text(out, out_len, &pos, "null") != 0) {
      return -1;
    }
    if (rt_append_text(out, out_len, &pos, ",\"working_memory\":") != 0 ||
        append_json_string_value(out, out_len, &pos, working_memory) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"artifacts\":") != 0 ||
        rt_append_text(out, out_len, &pos, artifacts) != 0)
      return -1;
    snprintf(tail, sizeof(tail),
             ",\"updated_ms\":%lld},\"final_state\":\"completed\","
             "\"archived_ms\":%lld}\n", ms, ms);
    return rt_append_text(out, out_len, &pos, tail);
  }
}

int rt_tasks_archive_completed_for_context(const char *context_id) {
  char *state = NULL, *out = NULL, *raw = NULL, *compact = NULL, *line = NULL;
  JsonRef root, tasks;
  size_t pos = 0;
  int first = 1;
  int rc = -1;
  long long ms = timing_system_wall_ms();
  if (!context_id || !*context_id) return 0;
  state = (char *)malloc(RT_XL);
  out = (char *)malloc(RT_XL);
  raw = (char *)malloc(RT_XL);
  compact = (char *)malloc(RT_XL);
  line = (char *)malloc(RT_XL);
  if (!state || !out || !raw || !compact || !line) goto done;
  if (rt_tasks_state_json(state, RT_XL) != 0 ||
      json_ref_from_text(state, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0) {
    rc = 0;
    goto done;
  }
  if (rt_append_text(out, RT_XL, &pos, "{\"tasks\":[") != 0) goto done;
  for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
    JsonRef task;
    char task_context[RT_MED] = "", task_state[RT_SMALL] = "";
    if (json_ref_array_get(&tasks, i, &task) != 0 ||
        task.type != JSON_REF_OBJECT)
      continue;
    (void)rt_task_json_state_string(&task, "context_id", task_context,
                                    sizeof(task_context));
    (void)rt_task_json_state_string(&task, "state", task_state,
                                    sizeof(task_state));
    if (strcmp(task_context, context_id) == 0 &&
        strcmp(task_state, "completed") == 0) {
      if (archive_line_for_task(&task, ms, line, RT_XL) != 0 ||
          fs_append_text(RT_TASK_ARCHIVE_PATH, line) != 0)
        goto done;
      continue;
    }
    if (json_ref_value_copy(&task, raw, RT_XL) != 0 ||
        json_compact(raw, compact, RT_XL) != 0)
      goto done;
    if ((!first && rt_append_text(out, RT_XL, &pos, ",") != 0) ||
        rt_append_text(out, RT_XL, &pos, compact) != 0)
      goto done;
    first = 0;
  }
  if (rt_append_text(out, RT_XL, &pos, "]}\n") != 0) goto done;
  rc = fs_write_text_atomic(RT_TASK_STATE_PATH, out);
done:
  free(state);
  free(out);
  free(raw);
  free(compact);
  free(line);
  return rc;
}
