/* Runtime-function action dispatch.
 *
 * Some actions are intrinsics: their "exec" entry in action.json is
 *   "exec": ["runtime", "fc_name()"]
 * The kernel dispatches them through a compiled function table rather
 * than fork+exec'ing a run.sh. This file owns:
 *   - the kRuntimeActions[] table
 *   - the generic message and action-catalog intrinsics
 *   - rt_action_runtime_complete() — the runner-side glue that emits
 *     action_started + action_succeeded/_failed around the call.
 * Related intrinsic families live in action_runtime_*.c. */

#include "action_runtime_internal.h"
#include "support/fsutil.h"
#include "support/heap_guard.h"
#include "support/json.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *name;
  RuntimeActionFn fn;
} RuntimeActionEntry;

static int fc_message(RtRun *, const char *, const RtActionDef *,
                      const char *, char *, size_t, char *, size_t);
static int fc_action_list(RtRun *, const char *, const RtActionDef *,
                          const char *, char *, size_t, char *, size_t);
static const RuntimeActionEntry kRuntimeActions[] = {
  { "fc_message()", fc_message },
  { "fc_action_list()", fc_action_list },
  { "fc_list_directory()", fc_list_directory },
  { "fc_read_file()", fc_read_file },
  { "fc_write_file()", fc_write_file },
  { "fc_rotate_file()", fc_rotate_file_intrinsic },
  { "fc_manage_codex()", fc_manage_codex },
  { "fc_manage_claude()", fc_manage_claude },
  { "fc_note_add()", fc_note_add },
  { "fc_counter_bump()", fc_counter_bump },
  { "fc_affair_close()", fc_affair_close },
  { "fc_defer()", fc_defer },
  { "fc_work()", fc_work },
  { "fc_working_memory_append()", fc_working_memory_append },
  { "fc_work_complete()", fc_work_complete },
  { "fc_work_blocked()", fc_work_blocked },
  { "fc_affair_open()", fc_affair_open },
  { NULL, NULL }
};

/* Scan this run's committed event log for the effect `request_id`
 * already appended. One linear pass over a log recovery has just read
 * anyway; a local intrinsic runs once per request, so this is not a hot
 * path. */
int rt_action_committed_effect(const RtRun *r, const char *type,
                               const char *request_id,
                               char *payload_out, size_t payload_len) {
  char path[PATH_MAX];
  char *text = NULL;
  char *line;
  int found = 0;
  if (!r || !type || !*type || !request_id || !*request_id ||
      !payload_out || payload_len == 0)
    return -1;
  payload_out[0] = '\0';
  if (rt_event_log_path(&r->ctx, path, sizeof(path)) != 0) return -1;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? 0 : -1;
  line = text;
  while (line && *line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload;
    char event_type[RT_SMALL] = "", event_rid[RT_SMALL] = "";
    if (next) *next = '\0';
    if (*line && json_ref_top_object(line, &event) == 0 &&
        json_ref_object_get_string(&event, "type", event_type,
                                   sizeof(event_type)) == 0 &&
        strcmp(event_type, type) == 0 &&
        json_ref_object_get_object(&event, "payload", &payload) == 0 &&
        json_ref_object_get_string(&payload, "request_id", event_rid,
                                   sizeof(event_rid)) == 0 &&
        strcmp(event_rid, request_id) == 0) {
      found = json_ref_value_copy(&payload, payload_out, payload_len) == 0
                  ? 1 : -1;
      break;
    }
    line = next ? next + 1 : NULL;
  }
  fc_xfree(text);
  return found;
}

int rt_action_registry_runtime_function_known(const char *name) {
  for (int i = 0; kRuntimeActions[i].name; ++i)
    if (strcmp(kRuntimeActions[i].name, name) == 0) return 1;
  return 0;
}

static RuntimeActionFn runtime_action_lookup(const char *name) {
  for (int i = 0; kRuntimeActions[i].name; ++i)
    if (strcmp(kRuntimeActions[i].name, name) == 0) return kRuntimeActions[i].fn;
  return NULL;
}

/* The reply itself. A text the delivery path cannot carry is a failure
 * that names the ceiling, never an empty or shortened delivery recorded
 * as success: the runtime's word about what it delivered has to be true. */
static int fc_message(RtRun *r, const char *rid, const RtActionDef *def,
                      const char *args, char *result, size_t result_len,
                      char *error, size_t error_len) {
  char message_text[RT_MESSAGE_TEXT_MAX + 1] = "";
  char escaped_text[RT_MESSAGE_ESCAPED_MAX + 1];
  JsonRef root, value;
  (void)r; (void)rid; (void)def;
  if (json_ref_first_object(args && *args ? args : "{}", &root) != 0 ||
      json_ref_object_get(&root, "message", &value) != 0 ||
      value.type != JSON_REF_STRING) {
    snprintf(error, error_len, "{\"message\":\"message must be a string\"}");
    return -1;
  }
  /* json_ref_string_copy refuses rather than truncates. */
  if (json_ref_string_copy(&value, message_text, sizeof(message_text)) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"message is longer than %d bytes or is not valid "
             "text; a message delivers at most %d bytes, so shorten it\"}",
             RT_MESSAGE_TEXT_MAX, RT_MESSAGE_TEXT_MAX);
    return -1;
  }
  if (json_escape(message_text, escaped_text, sizeof(escaped_text)) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"message escapes to more than %d bytes; "
             "shorten it\"}",
             RT_MESSAGE_ESCAPED_MAX);
    return -1;
  }
  snprintf(result, result_len, "{\"message\":\"%s\"}", escaped_text);
  snprintf(error, error_len, "null");
  return 0;
}

static int fc_action_list(RtRun *r, const char *rid, const RtActionDef *def,
                          const char *args, char *result, size_t result_len,
                          char *error, size_t error_len) {
  const RtAgentMeta *meta = NULL;
  const RtActionDef *selected = NULL;
  char name[RT_SMALL] = "";
  char *catalog = NULL;
  char *escaped = NULL;
  size_t escaped_len;
  int idx = rt_action_runner_pending_index(r, rid);
  (void)def;
  if (idx >= 0)
    meta = rt_action_agent_meta_lookup(
        r->scheduler, r->pending_actions[idx].source_agent);
  if (!meta) {
    snprintf(error, error_len,
             "{\"message\":\"action_list could not resolve its calling agent\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  (void)rt_json_get_string(args && *args ? args : "{}", "name", name,
                           sizeof(name));
  catalog = (char *)fc_xmalloc(RT_XL);
  if (!catalog) {
    snprintf(error, error_len, "{\"message\":\"action_list allocation failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (name[0]) {
    selected = rt_action_registry_find(&r->scheduler->actions, name);
    if (!selected || !rt_agent_allows_action(meta, selected)) {
      snprintf(error, error_len,
               "{\"message\":\"requested action is not in this agent's allowlist\"}");
      snprintf(result, result_len, "{}");
      fc_xfree(catalog);
      return -1;
    }
    if (selected->force_disabled) {
      snprintf(error, error_len,
               "{\"message\":\"requested action is listed in config force_disable\"}");
      snprintf(result, result_len, "{}");
      fc_xfree(catalog);
      return -1;
    }
    if (rt_action_render_definition(selected, catalog, RT_XL) != 0) {
      snprintf(error, error_len,
               "{\"message\":\"requested action contract is too large\"}");
      snprintf(result, result_len, "{}");
      fc_xfree(catalog);
      return -1;
    }
  } else if (rt_action_render_allowed_ids(&r->scheduler->actions, meta,
                                          catalog, RT_XL) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"allowed action id list is too large\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(catalog);
    return -1;
  }
  if (strlen(catalog) + strlen(name) + 32U >
      RT_OPERATION_RESULT_TEXT_MAX) {
    snprintf(error, error_len,
             "{\"message\":\"complete action contract exceeds the managed-operation result limit\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(catalog);
    return -1;
  }
  escaped_len = strlen(catalog) * 2U + 1U;
  escaped = (char *)fc_xmalloc(escaped_len);
  if (!escaped || json_escape(catalog, escaped, escaped_len) != 0) {
    snprintf(error, error_len, "{\"message\":\"action_list escape failed\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(escaped);
    fc_xfree(catalog);
    return -1;
  }
  if (snprintf(result, result_len, "{\"text\":\"%s%s:\\n%s\"}",
               name[0] ? "Action contract " : "Allowed action IDs",
               name[0] ? name : "", escaped) >= (int)result_len) {
    snprintf(error, error_len,
             "{\"message\":\"complete action_list result is too large\"}");
    snprintf(result, result_len, "{}");
    fc_xfree(escaped);
    fc_xfree(catalog);
    return -1;
  }
  snprintf(error, error_len, "null");
  fc_xfree(escaped);
  fc_xfree(catalog);
  return 0;
}

int rt_action_runtime_complete(RtRun *r, const char *rid, const RtActionDef *def,
                              const char *task_id, const char *args) {
  RuntimeActionFn fn = runtime_action_lookup(def->exec_arg);
  char result[RT_XL] = "{}";
  char error[RT_LARGE] = "null";
  int idx = rt_action_runner_pending_index(r, rid);
  int rc;
  if (!fn) {
    return rt_action_emit_rejected(
        r, rid, def->id, task_id, "unknown_action",
        "Runtime action function is not registered.", "[]",
        idx >= 0 ? r->pending_actions[idx].sig : "");
  }
  if (rt_action_emit_started(r, rid, def->id, NULL, task_id) != 0)
    return -1;
  /* Preallocate the operation identity + completion capability +
   * deadline durably before the intrinsic can spawn a worker. */
  if (def->managed_operation && idx >= 0 &&
      rt_operation_allocate_for_call(r, def, &r->pending_actions[idx]) != 0) {
    return rt_action_emit_terminal(
        r, "action_failed", rid, def->id, NULL, task_id,
        "operation_allocation_failed", "{}",
        "{\"message\":\"managed-operation identity allocation failed\"}");
  }
  rc = fn(r, rid, def, args && *args ? args : "{}", result, sizeof(result), error, sizeof(error));
  /* Auto-wrap managed-op intrinsics into the finished-immediately contract.
   * The operation driver publishes the result on a later operation_result
   * event; current OpenClaw and FloofClaw both use that event-driven path. */
  if (rc == 0 && def->managed_operation) {
    rt_action_wrap_finished_result(result, sizeof(result), rid);
  }
  return rt_action_emit_terminal(
      r, rc == 0 ? "action_succeeded" : "action_failed",
      rid, def->id, NULL, task_id,
      rc == 0 ? NULL : "runtime_function_failed",
      rc == 0 ? result : "{}", rc == 0 ? "null" : error);
}
