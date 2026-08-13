#include "agent_exec.h"
#include "floop.h"

#include <stdlib.h>
#include <string.h>

static int agent_json_path(const char *floop_name, const char *agent_id,
                           char *out, size_t out_len) {
  char dir[PATH_MAX];
  if (rt_floop_agent_dir(floop_name ? floop_name : rt_default_loop(),
                         agent_id, dir, sizeof(dir)) != 0) return -1;
  return snprintf(out, out_len, "%s/agent.json", dir) >= (int)out_len ? -1 : 0;
}

static int safe_action_id(const char *s) {
  if (!s || !*s) return 0;
  for (; *s; ++s)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
          (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
  return 1;
}

static int safe_listen_block(const char *s) {
  return s && (strcmp(s, "memory") == 0 ||
               strcmp(s, "tasks") == 0 ||
               strcmp(s, "affairs") == 0 ||
               strcmp(s, "event") == 0 ||
               strcmp(s, "usage") == 0);
}

static int has_memory_capability(const JsonRef *root) {
  JsonRef caps, item;
  if (!root || json_ref_object_get_array(root, "capabilities", &caps) != 0) return 0;
  for (size_t i = 0; i < json_ref_array_size(&caps); ++i) {
    char cap[RT_SMALL];
    if (json_ref_array_get(&caps, i, &item) == 0 &&
        json_ref_string_copy(&item, cap, sizeof(cap)) == 0 &&
        strcmp(cap, "memory") == 0) return 1;
  }
  return 0;
}

int rt_agent_read_meta(const char *floop_name, const char *agent_id,
                      char *executor, size_t executor_len,
                      char *model_ref, size_t model_ref_len,
                      char *err, size_t err_len) {
  char path[PATH_MAX];
  char *text = NULL;
  JsonRef root, model;
  if (executor && executor_len) executor[0] = '\0';
  if (model_ref && model_ref_len) model_ref[0] = '\0';
  if (!agent_id || !*agent_id) {
    if (err) snprintf(err, err_len,
                      "floop step is missing an agent id; fix: set its agent field in the floop's loop.json");
    return -1;
  }
  if (agent_json_path(floop_name, agent_id, path, sizeof(path)) != 0) {
    if (err) snprintf(err, err_len,
                      "could not resolve agent %s; fix: add floops/%s/agents/%s/agent.json or correct the floop step",
                      agent_id, floop_name, agent_id);
    return -1;
  }
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
    if (err) snprintf(err, err_len,
                      "could not read %s; fix: restore that agent.json or correct the floop step",
                      path);
    return -1;
  }
  if (json_ref_first_object(text, &root) != 0) {
    if (executor && executor_len) snprintf(executor, executor_len, "script");
    free(text);
    return 0;
  }
  if (executor && executor_len &&
      json_ref_object_get_string(&root, "executor", executor, executor_len) != 0)
    snprintf(executor, executor_len, "script");
  if (model_ref && model_ref_len &&
      json_ref_object_get_object(&root, "model", &model) == 0)
    (void)json_ref_object_get_string(&model, "ref", model_ref, model_ref_len);
  free(text);
  return 0;
}

int rt_agent_read_action_allowlist(const char *floop_name, const char *agent_id, const char *executor,
                                  const RtActionRegistry *registry,
                                  RtAgentMeta *meta,
                                  char *err, size_t err_len) {
  char path[PATH_MAX];
  char *text = NULL;
  JsonRef root, actions, item;
  int saw_all_actions = 0;
  if (!agent_id || !meta || !registry) return -1;
  if (agent_json_path(floop_name, agent_id, path, sizeof(path)) != 0 ||
      fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text ||
      json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_array(&root, "actions", &actions) != 0) {
    if (text) free(text);
    meta->has_action_allowlist = executor && strcmp(executor, "llm") == 0;
    meta->action_count = 0;
    return 0;
  }
  meta->has_action_allowlist = 1;
  meta->action_count = 0;
  meta->action_allow_mask = 0;
  if (json_ref_array_size(&actions) > RT_MAX_ACTIONS) {
    if (err) snprintf(err, err_len,
                      "agent %s actions catalog exceeds registry maximum %d; "
                      "fix: remove entries from actions in %s",
                      agent_id, RT_MAX_ACTIONS, path);
    free(text);
    return -1;
  }
  for (size_t i = 0; i < json_ref_array_size(&actions); ++i) {
    char id[RT_SMALL];
    const RtActionDef *def;
    if (json_ref_array_get(&actions, i, &item) != 0 ||
        json_ref_string_copy(&item, id, sizeof(id)) != 0 || !safe_action_id(id)) {
      if (err) snprintf(err, err_len,
                        "agent %s has an invalid actions entry; fix: use an id listed by `fclaw actions` in %s",
                        agent_id, path);
      free(text);
      return -1;
    }
    if (strcmp(id, "all_actions") == 0) {
      uint64_t mask = registry->count == RT_MAX_ACTIONS
                          ? UINT64_MAX
                          : (UINT64_C(1) << registry->count) - UINT64_C(1);
      if (saw_all_actions) {
        if (err) snprintf(err, err_len,
                          "agent %s lists duplicate all_actions; fix: remove the duplicate from actions in %s",
                          agent_id, path);
        free(text);
        return -1;
      }
      saw_all_actions = 1;
      meta->action_ids[meta->action_count++] = RT_ACTION_PRESENT_ALL;
      meta->action_allow_mask |= mask;
      continue;
    }
    def = rt_action_registry_find(registry, id);
    if (!def) {
      if (err) snprintf(err, err_len,
                        "agent %s allows unknown action %s; fix: put its directory under actions/ "
                        "(check `fclaw actions`, and that the name is not _-prefixed), "
                        "or remove it from %s",
                        agent_id, id, path);
      free(text);
      return -1;
    }
    for (size_t j = 0; j < meta->action_count; ++j) {
      if (meta->action_ids[j] == def->runtime_id) {
        if (err) snprintf(err, err_len,
                          "agent %s lists duplicate action %s; "
                          "fix: remove the duplicate from actions in %s",
                          agent_id, id, path);
        free(text);
        return -1;
      }
    }
    meta->action_ids[meta->action_count++] = def->runtime_id;
    meta->action_allow_mask |= UINT64_C(1) << def->runtime_id;
  }
  free(text);
  return 0;
}

int rt_agent_read_listen_config(const char *floop_name, const char *agent_id, const char *executor,
                                RtAgentMeta *meta, char *err, size_t err_len) {
  char path[PATH_MAX];
  char *text = NULL;
  JsonRef root, listen, item;
  long long recent = 10;
  if (!agent_id || !meta) return -1;
  meta->listen_memory = meta->listen_tasks = meta->listen_affairs = meta->listen_declared = 0;
  meta->listen_event = 0;
  meta->listen_usage = 0;
  meta->bind_task_open_work = 0;
  meta->max_repair_attempts = RT_WORK_REPAIR_ATTEMPT_DEFAULT;
  meta->handler[0] = '\0';
  meta->recent = 10;
  meta->autocomplete_message_task_on_send = meta->conversational_payload_only = 0;
  meta->output_contract[0] = '\0';
  meta->output_repair_attempts = 0;
  meta->affair_extraction_context_only = 0;
  meta->memory_compaction_context_only = 0;
  meta->can_write_memory = 0;
  meta->memory_summary_enabled = 0;
  meta->memory_compact_after_messages = 48;
  meta->memory_compact_after_tokens = 12000;
  meta->memory_keep_recent_messages = 20;
  meta->memory_min_compact_messages = 16;
  meta->memory_summary_max_tokens = 1600;
  meta->memory_recent_summary_max_tokens = 500;
  if (agent_json_path(floop_name, agent_id, path, sizeof(path)) != 0 ||
      fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text ||
      json_ref_first_object(text, &root) != 0) {
    if (text) free(text);
    if (err) snprintf(err, err_len,
                      "could not read %s; fix: restore that agent.json or correct the floop step",
                      path);
    return -1;
  }
  if (json_ref_object_get_long(&root, "recent", &recent) == 0 && recent > 0 && recent < 1000)
    meta->recent = (int)recent;
  meta->can_write_memory = has_memory_capability(&root);
  (void)json_ref_object_get_bool(&root, "autocomplete_message_task_on_send",
                                 &meta->autocomplete_message_task_on_send);
  if (!meta->autocomplete_message_task_on_send)
    (void)json_ref_object_get_bool(&root, "autocomplete_input_task_on_send",
                                   &meta->autocomplete_message_task_on_send);
  (void)json_ref_object_get_bool(&root, "conversational_payload_only", &meta->conversational_payload_only);
  (void)json_ref_object_get_bool(&root, "affair_extraction_context_only", &meta->affair_extraction_context_only);
  (void)json_ref_object_get_bool(&root, "memory_compaction_context_only", &meta->memory_compaction_context_only);
  {
    JsonRef contract;
    if (json_ref_object_get(&root, "output_contract", &contract) == 0) {
      JsonRef repair_value;
      long long repair_attempts = 0;
      if (contract.type != JSON_REF_OBJECT ||
          json_ref_object_get_string(&contract, "kind", meta->output_contract,
                                     sizeof(meta->output_contract)) != 0 ||
          strcmp(meta->output_contract, "task_action_or_finalize") != 0) {
        if (err) snprintf(
            err, err_len,
            "agent %s has invalid output_contract; fix: use kind "
            "\"task_action_or_finalize\" or remove it from %s",
            agent_id, path);
        free(text);
        return -1;
      }
      if (executor && strcmp(executor, "llm") != 0) {
        if (err) snprintf(err, err_len,
                          "agent %s declares output_contract but is not executor=llm; "
                          "fix: remove output_contract from %s",
                          agent_id, path);
        free(text);
        return -1;
      }
      if (json_ref_object_get(&contract, "repair_attempts",
                              &repair_value) == 0) {
        if (json_ref_get_long(&repair_value, &repair_attempts) != 0 ||
            repair_attempts < 0 ||
            repair_attempts > RT_AGENT_OUTPUT_REPAIR_LIMIT) {
          if (err) snprintf(
              err, err_len,
              "agent %s has invalid output_contract.repair_attempts; fix: use "
              "an integer from 0 through %d in %s",
              agent_id, RT_AGENT_OUTPUT_REPAIR_LIMIT, path);
          free(text);
          return -1;
        }
      }
      meta->output_repair_attempts = (int)repair_attempts;
    }
  }
  {
    JsonRef bind_ref;
    if (json_ref_object_get(&root, "bind_task", &bind_ref) == 0) {
      char bind_task[RT_SMALL] = "";
      if (json_ref_string_copy(&bind_ref, bind_task, sizeof(bind_task)) != 0 ||
          strcmp(bind_task, "open_work") != 0) {
        if (err) snprintf(err, err_len,
                          "agent %s has invalid bind_task; fix: set bind_task to \"open_work\" or remove it from %s",
                          agent_id, path);
        free(text);
        return -1;
      }
      meta->bind_task_open_work = 1;
    }
  }
  {
    JsonRef policy;
    if (json_ref_object_get(&root, "work_policy", &policy) == 0) {
      JsonRef repair_value;
      long long max_repair_attempts = RT_WORK_REPAIR_ATTEMPT_DEFAULT;
      if (policy.type != JSON_REF_OBJECT) {
        if (err) snprintf(err, err_len,
                          "agent %s has invalid work_policy; fix: make it an "
                          "object or remove it from %s",
                          agent_id, path);
        free(text);
        return -1;
      }
      if (!meta->bind_task_open_work) {
        if (err) snprintf(err, err_len,
                          "agent %s declares work_policy without bind_task "
                          "\"open_work\"; fix: add the binding or remove "
                          "work_policy from %s",
                          agent_id, path);
        free(text);
        return -1;
      }
      if (json_ref_object_get(&policy, "max_repair_attempts",
                              &repair_value) == 0) {
        if (json_ref_get_long(&repair_value, &max_repair_attempts) != 0 ||
            max_repair_attempts < 0 ||
            max_repair_attempts > RT_WORK_REPAIR_ATTEMPT_LIMIT) {
          if (err) snprintf(
              err, err_len,
              "agent %s has invalid work_policy.max_repair_attempts; "
              "fix: use an integer from 0 through %d in %s",
              agent_id, RT_WORK_REPAIR_ATTEMPT_LIMIT, path);
          free(text);
          return -1;
        }
        meta->max_repair_attempts = (int)max_repair_attempts;
      }
    }
  }
  if (meta->bind_task_open_work &&
      (meta->conversational_payload_only ||
       meta->affair_extraction_context_only ||
       meta->memory_compaction_context_only)) {
    const char *conflict =
        meta->conversational_payload_only
            ? "conversational_payload_only"
            : meta->affair_extraction_context_only
                  ? "affair_extraction_context_only"
                  : "memory_compaction_context_only";
    if (err) snprintf(
        err, err_len,
        "agent %s cannot combine bind_task \"open_work\" with %s; "
        "fix: remove %s or remove bind_task from %s",
        agent_id, conflict, conflict, path);
    free(text);
    return -1;
  }
  /* Configured handler binding for native dispatch phases. The value
   * must name a registered action; the kernel never matches on it. */
  {
    char handler[RT_SMALL] = "";
    if (json_ref_object_get_string(&root, "handler", handler, sizeof(handler)) == 0 &&
        handler[0] && safe_action_id(handler))
      snprintf(meta->handler, sizeof(meta->handler), "%s", handler);
  }
  {
    JsonRef summary;
    long long v = 0;
    if (json_ref_object_get_object(&root, "summary", &summary) == 0) {
      (void)json_ref_object_get_bool(&summary, "enabled", &meta->memory_summary_enabled);
      if (json_ref_object_get_long(&summary, "compact_after_messages", &v) == 0 && v >= 0 && v < 1000000)
        meta->memory_compact_after_messages = (int)v;
      if (json_ref_object_get_long(&summary, "compact_after_tokens", &v) == 0 && v >= 0 && v < 100000000)
        meta->memory_compact_after_tokens = (int)v;
      if (json_ref_object_get_long(&summary, "keep_recent_messages", &v) == 0 && v >= 0 && v < 1000000)
        meta->memory_keep_recent_messages = (int)v;
      if (json_ref_object_get_long(&summary, "min_compact_messages", &v) == 0 && v >= 0 && v < 1000000)
        meta->memory_min_compact_messages = (int)v;
      if (json_ref_object_get_long(&summary, "summary_max_tokens", &v) == 0 && v >= 0 && v < 1000000)
        meta->memory_summary_max_tokens = (int)v;
      if (json_ref_object_get_long(&summary, "recent_summary_max_tokens", &v) == 0 && v >= 0 && v < 1000000)
        meta->memory_recent_summary_max_tokens = (int)v;
    }
  }
  if (json_ref_object_get_array(&root, "listen", &listen) != 0) {
    free(text);
    if (executor && (strcmp(executor, "llm") == 0 || strcmp(executor, "script") == 0)) {
      if (err) snprintf(err, err_len,
                        "agent %s is missing listen; fix: add a listen array to %s",
                        agent_id, path);
      return -1;
    }
    return 0;
  }
  meta->listen_declared = 1;
  for (size_t i = 0; i < json_ref_array_size(&listen); ++i) {
    char block[RT_SMALL];
    if (json_ref_array_get(&listen, i, &item) != 0 ||
        json_ref_string_copy(&item, block, sizeof(block)) != 0 ||
        !safe_listen_block(block)) {
      if (err) snprintf(err, err_len,
                        "agent %s has an invalid listen entry; fix: use memory, tasks, affairs, event, or usage in %s",
                        agent_id, path);
      free(text);
      return -1;
    }
    if (strcmp(block, "memory") == 0) meta->listen_memory = 1;
    if (strcmp(block, "tasks") == 0) meta->listen_tasks = 1;
    if (strcmp(block, "affairs") == 0) meta->listen_affairs = 1;
    if (strcmp(block, "event") == 0) meta->listen_event = 1;
    if (strcmp(block, "usage") == 0) meta->listen_usage = 1;
  }
  free(text);
  return 0;
}
