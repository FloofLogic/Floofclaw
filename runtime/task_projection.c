#include "task_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int append_projected_artifact(const JsonRef *artifact,
                                     long long work_rev,
                                     char *out, size_t out_len,
                                     size_t *pos, int first) {
  size_t cursor = 0;
  char key[RT_SMALL], number[RT_SMALL];
  JsonRef value;
  int wrote = 0;
  if ((!first && rt_append_text(out, out_len, pos, ",") != 0) ||
      rt_append_text(out, out_len, pos, "{") != 0)
    return -1;
  while (json_ref_object_iter(artifact, &cursor, key, sizeof(key),
                              &value) == 1) {
    if (strcmp(key, "work_rev") == 0) continue;
    if (rt_task_json_append_pair(out, out_len, pos, key, &value,
                                 !wrote) != 0)
      return -1;
    wrote = 1;
  }
  snprintf(number, sizeof(number), "%s\"work_rev\":%lld",
           wrote ? "," : "", work_rev);
  if (rt_append_text(out, out_len, pos, number) != 0 ||
      rt_append_text(out, out_len, pos, "}") != 0)
    return -1;
  return 0;
}

static int append_agent_artifacts(const JsonRef *task,
                                  const JsonRef *artifacts,
                                  char *out, size_t out_len, size_t *pos) {
  long long current_work_rev = 0;
  int first;
  (void)rt_task_json_state_long(task, "work_rev", &current_work_rev);
  if (rt_append_text(out, out_len, pos, "{\"current\":[") != 0) return -1;
  first = 1;
  for (size_t i = 0; i < json_ref_array_size(artifacts); ++i) {
    JsonRef artifact;
    long long artifact_work_rev;
    if (json_ref_array_get(artifacts, i, &artifact) != 0 ||
        artifact.type != JSON_REF_OBJECT ||
        json_ref_object_get_long(&artifact, "work_rev",
                                 &artifact_work_rev) != 0 ||
        artifact_work_rev < 0 ||
        (current_work_rev > 0 && artifact_work_rev == 0))
      return -1;
    if (artifact_work_rev != current_work_rev) continue;
    if (append_projected_artifact(&artifact, artifact_work_rev,
                                  out, out_len, pos, first) != 0)
      return -1;
    first = 0;
  }
  if (rt_append_text(out, out_len, pos, "],\"historical\":[") != 0)
    return -1;
  first = 1;
  for (size_t i = 0; i < json_ref_array_size(artifacts); ++i) {
    JsonRef artifact;
    long long artifact_work_rev;
    if (json_ref_array_get(artifacts, i, &artifact) != 0 ||
        artifact.type != JSON_REF_OBJECT ||
        json_ref_object_get_long(&artifact, "work_rev",
                                 &artifact_work_rev) != 0 ||
        artifact_work_rev < 0 ||
        (current_work_rev > 0 && artifact_work_rev == 0))
      return -1;
    if (artifact_work_rev == current_work_rev) continue;
    if (append_projected_artifact(&artifact, artifact_work_rev,
                                  out, out_len, pos, first) != 0)
      return -1;
    first = 0;
  }
  return rt_append_text(out, out_len, pos, "]}");
}

static int append_agent_task_state(const JsonRef *task,
                                   const JsonRef *state,
                                   char *out, size_t out_len, size_t *pos) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef value;
  int first = 1;
  if (rt_append_text(out, out_len, pos, "{") != 0) return -1;
  while (json_ref_object_iter(state, &cursor, key, sizeof(key),
                              &value) == 1) {
    if ((!first && rt_append_text(out, out_len, pos, ",") != 0) ||
        rt_append_text(out, out_len, pos, "\"") != 0 ||
        rt_append_text(out, out_len, pos, key) != 0 ||
        rt_append_text(out, out_len, pos, "\":") != 0)
      return -1;
    if (strcmp(key, "artifacts") == 0 && value.type == JSON_REF_ARRAY) {
      if (append_agent_artifacts(task, &value,
                                 out, out_len, pos) != 0)
        return -1;
    } else {
      char raw[RT_XL], compact[RT_XL];
      if (json_ref_value_copy(&value, raw, sizeof(raw)) != 0 ||
          json_compact(raw, compact, sizeof(compact)) != 0 ||
          rt_append_text(out, out_len, pos, compact) != 0)
        return -1;
    }
    first = 0;
  }
  return rt_append_text(out, out_len, pos, "}");
}

static int project_task_for_agent(const JsonRef *task,
                                  char *out, size_t out_len) {
  size_t cursor = 0, pos = 0;
  char key[RT_SMALL];
  JsonRef value;
  int first = 1;
  if (!task || task->type != JSON_REF_OBJECT || !out || out_len == 0 ||
      rt_append_text(out, out_len, &pos, "{") != 0)
    return -1;
  while (json_ref_object_iter(task, &cursor, key, sizeof(key), &value) == 1) {
    if ((!first && rt_append_text(out, out_len, &pos, ",") != 0) ||
        rt_append_text(out, out_len, &pos, "\"") != 0 ||
        rt_append_text(out, out_len, &pos, key) != 0 ||
        rt_append_text(out, out_len, &pos, "\":") != 0)
      return -1;
    if (strcmp(key, "state") == 0 && value.type == JSON_REF_OBJECT) {
      if (append_agent_task_state(task, &value,
                                  out, out_len, &pos) != 0)
        return -1;
    } else {
      char raw[RT_XL], compact[RT_XL];
      if (json_ref_value_copy(&value, raw, sizeof(raw)) != 0 ||
          json_compact(raw, compact, sizeof(compact)) != 0 ||
          rt_append_text(out, out_len, &pos, compact) != 0)
        return -1;
    }
    first = 0;
  }
  return rt_append_text(out, out_len, &pos, "}");
}

static int task_visible_in_context(const JsonRef *task, const char *context_id) {
  char ctx[RT_MED] = "", state[RT_SMALL] = "";
  (void)rt_task_json_state_string(task, "context_id", ctx, sizeof(ctx));
  (void)rt_task_json_state_string(task, "state", state, sizeof(state));
  if (context_id && *context_id && strcmp(ctx, context_id) != 0) return 0;
  return strcmp(state, "archived") != 0;
}
int rt_tasks_active_state_json(const char *context_id, char *out, size_t out_len) {
  char state[RT_XL];
  JsonRef root, tasks;
  size_t pos = 0;
  int first = 1;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (rt_append_text(out, out_len, &pos, "{\"active\":[") != 0) return -1;
  if (rt_tasks_state_json(state, sizeof(state)) == 0 &&
      json_ref_from_text(state, &root) == 0 &&
      json_ref_object_get_array(&root, "tasks", &tasks) == 0) {
    for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
      JsonRef task;
      char raw[RT_XL], compact[RT_XL];
      if (json_ref_array_get(&tasks, i, &task) != 0 || task.type != JSON_REF_OBJECT ||
          !task_visible_in_context(&task, context_id)) continue;
      if (json_ref_value_copy(&task, raw, sizeof(raw)) != 0 ||
          json_compact(raw, compact, sizeof(compact)) != 0) return -1;
      if ((!first && rt_append_text(out, out_len, &pos, ",") != 0) ||
          rt_append_text(out, out_len, &pos, compact) != 0) return -1;
      first = 0;
    }
  }
  return rt_append_text(out, out_len, &pos, "]}");
}

int rt_tasks_active_agent_state_json(const char *context_id,
                                     char *out, size_t out_len) {
  char state[RT_XL];
  JsonRef root, tasks;
  size_t pos = 0;
  int first = 1;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (rt_append_text(out, out_len, &pos, "{\"active\":[") != 0) return -1;
  if (rt_tasks_state_json(state, sizeof(state)) == 0 &&
      json_ref_from_text(state, &root) == 0 &&
      json_ref_object_get_array(&root, "tasks", &tasks) == 0) {
    for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
      JsonRef task;
      char projected[RT_XL];
      if (json_ref_array_get(&tasks, i, &task) != 0 ||
          task.type != JSON_REF_OBJECT ||
          !task_visible_in_context(&task, context_id))
        continue;
      if (project_task_for_agent(&task, projected, sizeof(projected)) != 0)
        return -1;
      if ((!first && rt_append_text(out, out_len, &pos, ",") != 0) ||
          rt_append_text(out, out_len, &pos, projected) != 0)
        return -1;
      first = 0;
    }
  }
  return rt_append_text(out, out_len, &pos, "]}");
}
static int task_ready_for_work(const JsonRef *tasks, const JsonRef *task) {
  char state[RT_SMALL] = "";
  (void)tasks;
  (void)rt_task_json_state_string(task, "state", state, sizeof(state));
  return strcmp(state, "open") == 0 || strcmp(state, "working") == 0;
}

static int task_belongs_to_run_id(const char *task_id, const char *run_id) {
  char prefix[RT_SMALL];
  if (!task_id || !*task_id || !run_id || !*run_id) return 0;
  snprintf(prefix, sizeof(prefix), "task_%s_", run_id);
  return strncmp(task_id, prefix, strlen(prefix)) == 0;
}

static int select_new_input(const char *context_id, const char *run_id,
                            char *task_id, size_t task_len) {
  char state[RT_XL];
  JsonRef root, tasks;
  if (task_id && task_len) task_id[0] = '\0';
  if (!context_id || !*context_id || !task_id || task_len == 0 ||
      rt_tasks_state_json(state, sizeof(state)) != 0 ||
      json_ref_from_text(state, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0)
    return 1;
  for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
    JsonRef task;
    char kind[RT_SMALL] = "", state_name[RT_SMALL] = "", id[RT_SMALL] = "";
    if (json_ref_array_get(&tasks, i, &task) != 0 || task.type != JSON_REF_OBJECT ||
        !task_visible_in_context(&task, context_id) ||
        rt_task_json_state_string(&task, "kind", kind, sizeof(kind)) != 0 ||
        rt_task_json_state_string(&task, "state", state_name, sizeof(state_name)) != 0 ||
        strcmp(kind, "input") != 0 || strcmp(state_name, "open") != 0 ||
        rt_task_json_state_string(&task, "task_id", id, sizeof(id)) != 0 ||
        (run_id && *run_id && !task_belongs_to_run_id(id, run_id)) ||
        !id[0]) continue;
    snprintf(task_id, task_len, "%s", id);
    return 0;
  }
  return 1;
}

int rt_task_select_new_input(const char *context_id, char *task_id, size_t task_len) {
  return select_new_input(context_id, NULL, task_id, task_len);
}

int rt_task_select_new_input_for_run(const char *context_id, const char *run_id,
                                     char *task_id, size_t task_len) {
  return select_new_input(context_id, run_id, task_id, task_len);
}

int rt_task_select_unique_open_input(const char *context_id, char *task_id, size_t task_len) {
  char state[RT_XL];
  JsonRef root, tasks;
  int matches = 0;
  if (task_id && task_len) task_id[0] = '\0';
  if (!context_id || !*context_id || !task_id || task_len == 0 ||
      rt_tasks_state_json(state, sizeof(state)) != 0 ||
      json_ref_from_text(state, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0)
    return 1;
  for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
    JsonRef task;
    char kind[RT_SMALL] = "", state_name[RT_SMALL] = "", id[RT_SMALL] = "";
    if (json_ref_array_get(&tasks, i, &task) != 0 || task.type != JSON_REF_OBJECT ||
        !task_visible_in_context(&task, context_id) ||
        rt_task_json_state_string(&task, "kind", kind, sizeof(kind)) != 0 ||
        rt_task_json_state_string(&task, "state", state_name, sizeof(state_name)) != 0 ||
        strcmp(kind, "input") != 0 || strcmp(state_name, "open") != 0 ||
        rt_task_json_state_string(&task, "task_id", id, sizeof(id)) != 0 || !id[0]) continue;
    matches++;
    if (matches > 1) {
      task_id[0] = '\0';
      return 1;
    }
    snprintf(task_id, task_len, "%s", id);
  }
  return matches == 1 ? 0 : 1;
}

static int select_unique_work(const char *context_id, int allow_blocked,
                              char *task_id, size_t task_len,
                              long long *work_rev) {
  char state_json[RT_XL];
  JsonRef root, tasks;
  int matches = 0;
  if (task_id && task_len) task_id[0] = '\0';
  if (work_rev) *work_rev = 0;
  if (!context_id || !*context_id || !task_id || task_len == 0 ||
      rt_tasks_state_json(state_json, sizeof(state_json)) != 0 ||
      json_ref_from_text(state_json, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0)
    return -1;
  for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
    JsonRef task;
    char kind[RT_SMALL] = "", state[RT_SMALL] = "", id[RT_SMALL] = "";
    long long rev = 0;
    if (json_ref_array_get(&tasks, i, &task) != 0 ||
        task.type != JSON_REF_OBJECT ||
        !task_visible_in_context(&task, context_id) ||
        rt_task_json_state_string(&task, "kind", kind, sizeof(kind)) != 0 ||
        strcmp(kind, "work") != 0 ||
        rt_task_json_state_string(&task, "state", state, sizeof(state)) != 0 ||
        (strcmp(state, "open") != 0 && strcmp(state, "working") != 0 &&
         (!allow_blocked || strcmp(state, "blocked") != 0)) ||
        rt_task_json_state_string(&task, "task_id", id, sizeof(id)) != 0 ||
        rt_task_json_state_long(&task, "work_rev", &rev) != 0 || rev <= 0)
      continue;
    matches++;
    if (matches > 1) {
      task_id[0] = '\0';
      if (work_rev) *work_rev = 0;
      return 2;
    }
    snprintf(task_id, task_len, "%s", id);
    if (work_rev) *work_rev = rev;
  }
  return matches == 1 ? 0 : 1;
}

int rt_task_select_unique_open_work(const char *context_id,
                                    char *task_id, size_t task_id_len,
                                    long long *work_rev) {
  return select_unique_work(context_id, 0, task_id, task_id_len, work_rev);
}

int rt_task_select_unique_revisable_work(const char *context_id,
                                         char *task_id, size_t task_id_len,
                                         long long *work_rev) {
  return select_unique_work(context_id, 1, task_id, task_id_len, work_rev);
}

typedef struct {
  char task_id[RT_SMALL];
  char trigger_event_id[RT_SMALL];
  long long work_rev;
} WorkConsequence;

static int current_run_work_consequences(const RtContext *ctx,
                                         WorkConsequence *items,
                                         size_t items_cap,
                                         size_t *out_count) {
  char path[PATH_MAX], *text = NULL, *line;
  size_t count = 0;
  if (out_count) *out_count = 0;
  if (!ctx || !ctx->run_id[0] || !items || items_cap == 0 || !out_count)
    return -1;
  if (rt_event_log_path(ctx, path, sizeof(path)) != 0) return -1;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  line = text;
  while (*line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload, state;
    char type[RT_SMALL] = "", id[RT_SMALL] = "", kind[RT_SMALL] = "";
    char event_id[RT_SMALL] = "";
    long long work_rev = 0;
    int eligible = 0;
    if (next) *next = '\0';
    if (json_ref_top_object(line, &event) == 0 &&
        json_ref_object_get_string(&event, "type", type,
                                   sizeof(type)) == 0 &&
        json_ref_object_get_string(&event, "event_id", event_id,
                                   sizeof(event_id)) == 0 &&
        json_ref_object_get_object(&event, "payload", &payload) == 0) {
      if (strcmp(type, "task_created") == 0 &&
          json_ref_object_get_string(&payload, "kind", kind,
                                     sizeof(kind)) == 0 &&
          strcmp(kind, "work") == 0)
        eligible = 1;
      else if (strcmp(type, "task_updated") == 0 &&
               json_ref_object_get_object(&payload, "state", &state) == 0) {
        JsonRef value;
        eligible = json_ref_object_get(&state, "work", &value) == 0 ||
                   json_ref_object_get(&state, "done_when", &value) == 0;
      }
      if (eligible &&
          json_ref_object_get_string(&payload, "task_id", id,
                                     sizeof(id)) == 0 && id[0] &&
          json_ref_object_get_long(&payload, "work_rev",
                                   &work_rev) == 0 &&
          work_rev > 0) {
        size_t existing;
        for (existing = 0; existing < count; ++existing)
          if (strcmp(items[existing].task_id, id) == 0) break;
        if (existing == count) {
          if (count >= items_cap) {
            free(text);
            return -1;
          }
          count++;
        }
        /* Multiple revisions of one task in one admission run are one
         * consequence: bind the latest recorded revision and trigger. */
        snprintf(items[existing].task_id, sizeof(items[existing].task_id),
                 "%s", id);
        snprintf(items[existing].trigger_event_id,
                 sizeof(items[existing].trigger_event_id), "%s", event_id);
        items[existing].work_rev = work_rev;
      }
    }
    if (!next) break;
    line = next + 1;
  }
  free(text);
  *out_count = count;
  return 0;
}

static int current_run_trigger_consumed(const RtContext *ctx,
                                        const char *task_id,
                                        long long work_rev,
                                        const char *trigger_event_id) {
  char path[PATH_MAX], *text = NULL, *line;
  int consumed = 0;
  if (!ctx || !task_id || !*task_id || work_rev <= 0 ||
      !trigger_event_id || !*trigger_event_id)
    return -1;
  if (rt_event_log_path(ctx, path, sizeof(path)) != 0) return -1;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  line = text;
  while (*line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload;
    char type[RT_SMALL] = "", bound_task[RT_SMALL] = "";
    char bound_trigger[RT_SMALL] = "";
    long long bound_rev = 0;
    if (next) *next = '\0';
    if (json_ref_top_object(line, &event) == 0 &&
        json_ref_object_get_string(&event, "type", type,
                                   sizeof(type)) == 0 &&
        strcmp(type, "action_request") == 0 &&
        json_ref_object_get_object(&event, "payload", &payload) == 0 &&
        json_ref_object_get_string(&payload, "task_id", bound_task,
                                   sizeof(bound_task)) == 0 &&
        json_ref_object_get_long(&payload, "work_rev", &bound_rev) == 0 &&
        json_ref_object_get_string(&payload, "trigger_event_id",
                                   bound_trigger,
                                   sizeof(bound_trigger)) == 0 &&
        strcmp(bound_task, task_id) == 0 && bound_rev == work_rev &&
        strcmp(bound_trigger, trigger_event_id) == 0) {
      consumed = 1;
      break;
    }
    if (!next) break;
    line = next + 1;
  }
  free(text);
  return consumed;
}

static int exact_work_binding(const char *context_id, const char *task_id,
                              long long work_rev) {
  char *binding = (char *)malloc(RT_XL);
  int result;
  if (!binding) return -1;
  result = rt_task_work_binding_json(context_id, task_id, work_rev, 0,
                                     binding, RT_XL);
  free(binding);
  return result;
}

int rt_task_select_work_consequence(const RtContext *ctx,
                                    char *task_id, size_t task_id_len,
                                    long long *work_rev,
                                    char *trigger_event_id,
                                    size_t trigger_event_id_len) {
  if (task_id && task_id_len) task_id[0] = '\0';
  if (work_rev) *work_rev = 0;
  if (trigger_event_id && trigger_event_id_len) trigger_event_id[0] = '\0';
  if (!ctx || !ctx->context_id[0] || !task_id || task_id_len == 0 ||
      !work_rev)
    return -1;
  if (strcmp(ctx->event_kind, "work_step_result") == 0 ||
      strcmp(ctx->event_kind, "operation_result") == 0) {
    JsonRef payload;
    char id[RT_SMALL] = "", rid[RT_SMALL] = "", action[RT_SMALL] = "";
    char source_event_id[RT_SMALL] = "";
    long long rev = 0, effective_rev = 0;
    int match;
    if (json_ref_top_object(ctx->event_payload_json, &payload) != 0 ||
        json_ref_object_get_string(&payload, "task_id", id,
                                   sizeof(id)) != 0 ||
        json_ref_object_get_long(&payload, "work_rev", &rev) != 0 ||
        json_ref_object_get_string(&payload, "request_id", rid,
                                   sizeof(rid)) != 0 ||
        json_ref_object_get_string(&payload, "source_event_id",
                                   source_event_id,
                                   sizeof(source_event_id)) != 0 ||
        !id[0] || rev <= 0 || !rid[0] || !source_event_id[0])
      return 4;
    (void)json_ref_object_get_string(&payload, "action",
                                     action, sizeof(action));
    match = rt_work_state_result_matches(id, rev, rid, action,
                                         source_event_id,
                                         ctx->event_kind,
                                         &effective_rev);
    if (match <= 0) return match < 0 ? -1 : 4;
    if (exact_work_binding(ctx->context_id, id, effective_rev) != 0)
      return 3;
    snprintf(task_id, task_id_len, "%s", id);
    *work_rev = effective_rev;
    if (trigger_event_id && trigger_event_id_len)
      snprintf(trigger_event_id, trigger_event_id_len, "%s",
               source_event_id);
    if (current_run_trigger_consumed(ctx, id, effective_rev,
                                     source_event_id) == 1)
      return 5;
    return 0;
  }
  {
    WorkConsequence items[RT_MAX_ACTIONS];
    size_t count = 0;
    if (current_run_work_consequences(ctx, items,
                                      RT_MAX_ACTIONS,
                                      &count) != 0)
      return -1;
    if (count == 0) return 1;
    if (count > 1) return 2;
    if (exact_work_binding(ctx->context_id, items[0].task_id,
                           items[0].work_rev) != 0)
      return 3;
    snprintf(task_id, task_id_len, "%s", items[0].task_id);
    *work_rev = items[0].work_rev;
    if (trigger_event_id && trigger_event_id_len)
      snprintf(trigger_event_id, trigger_event_id_len, "%s",
               items[0].trigger_event_id);
    if (current_run_trigger_consumed(ctx, items[0].task_id,
                                     items[0].work_rev,
                                     items[0].trigger_event_id) == 1)
      return 5;
    return 0;
  }
}

static int find_task_json(const char *context_id, const char *task_id,
                          char *out, size_t out_len) {
  char *state = NULL;
  char *raw = NULL;
  JsonRef root, tasks;
  int result = -1;
  if (!context_id || !*context_id || !task_id || !*task_id ||
      !out || out_len == 0)
    return -1;
  state = (char *)malloc(RT_XL);
  raw = (char *)malloc(RT_XL);
  if (!state || !raw ||
      rt_tasks_state_json(state, RT_XL) != 0 ||
      json_ref_from_text(state, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0)
    goto done;
  out[0] = '\0';
  for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
    JsonRef task;
    char id[RT_SMALL] = "";
    if (json_ref_array_get(&tasks, i, &task) != 0 || task.type != JSON_REF_OBJECT ||
        !task_visible_in_context(&task, context_id) ||
        rt_task_json_state_string(&task, "task_id", id, sizeof(id)) != 0 ||
        strcmp(id, task_id) != 0) continue;
    if (json_ref_value_copy(&task, raw, RT_XL) != 0 ||
        json_compact(raw, out, out_len) != 0)
      goto done;
    result = 0;
    break;
  }
done:
  free(raw);
  free(state);
  return result;
}

int rt_task_work_binding_json(const char *context_id, const char *task_id,
                              long long work_rev, int allow_blocked,
                              char *out, size_t out_len) {
  char *task_json = NULL;
  char kind[RT_SMALL] = "", state[RT_SMALL] = "";
  JsonRef task;
  long long current_rev = 0;
  int result = 1;
  if (out && out_len) out[0] = '\0';
  if (!out || out_len == 0) return 1;
  task_json = (char *)malloc(RT_XL);
  if (!task_json ||
      find_task_json(context_id, task_id, task_json, RT_XL) != 0 ||
      json_ref_from_text(task_json, &task) != 0 ||
      rt_task_json_state_string(&task, "kind", kind, sizeof(kind)) != 0 ||
      strcmp(kind, "work") != 0 ||
      rt_task_json_state_string(&task, "state", state, sizeof(state)) != 0 ||
      (strcmp(state, "open") != 0 && strcmp(state, "working") != 0 &&
       (!allow_blocked || strcmp(state, "blocked") != 0)) ||
      rt_task_json_state_long(&task, "work_rev", &current_rev) != 0 ||
      current_rev <= 0 || (work_rev > 0 && current_rev != work_rev))
    goto done;
  result = snprintf(out, out_len, "%s", task_json) >= (int)out_len ? -1 : 0;
done:
  free(task_json);
  return result;
}

int rt_task_work_agent_binding_json(const char *context_id,
                                    const char *task_id,
                                    long long work_rev, int allow_blocked,
                                    char *out, size_t out_len) {
  char *task_json = NULL;
  JsonRef task;
  int result;
  if (out && out_len) out[0] = '\0';
  task_json = (char *)malloc(RT_XL);
  if (!task_json) return -1;
  result = rt_task_work_binding_json(context_id, task_id, work_rev,
                                     allow_blocked,
                                     task_json, RT_XL);
  if (result == 0) {
    if (json_ref_from_text(task_json, &task) != 0)
      result = -1;
    else
      result = project_task_for_agent(&task, out, out_len);
  }
  free(task_json);
  return result;
}

int rt_task_input_text(const char *context_id, const char *task_id,
                       char *out, size_t out_len) {
  JsonRef task, input;
  char task_json[RT_XL];
  char type[RT_SMALL] = "";
  if (out && out_len) out[0] = '\0';
  if (!out || out_len == 0 ||
      find_task_json(context_id, task_id, task_json, sizeof(task_json)) != 0 ||
      json_ref_from_text(task_json, &task) != 0 ||
      json_ref_object_get_object(&task, "input", &input) != 0 ||
      json_ref_object_get_string(&input, "type", type, sizeof(type)) != 0 ||
      strcmp(type, "message") != 0 ||
      json_ref_object_get_string(&input, "text", out, out_len) != 0)
    return -1;
  return 0;
}

int rt_task_context_has_actionable(const char *context_id) {
  char state[RT_XL];
  JsonRef root, tasks;
  if (!context_id || !*context_id ||
      rt_tasks_state_json(state, sizeof(state)) != 0 ||
      json_ref_from_text(state, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0)
    return 0;
  for (size_t i = 0; i < json_ref_array_size(&tasks); ++i) {
    JsonRef task;
    char state_name[RT_SMALL] = "";
    if (json_ref_array_get(&tasks, i, &task) != 0 || task.type != JSON_REF_OBJECT ||
        !task_visible_in_context(&task, context_id)) continue;
    (void)rt_task_json_state_string(&task, "state", state_name, sizeof(state_name));
    if (strcmp(state_name, "open") == 0 || task_ready_for_work(&tasks, &task)) return 1;
  }
  return 0;
}
