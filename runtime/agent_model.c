#include "agent_exec.h"
#include "task_internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

static int append_json_string_value(char *out, size_t out_len, size_t *pos,
                                    const char *s) {
  char esc[RT_XL];
  if (json_escape(s ? s : "", esc, sizeof(esc)) != 0) return -1;
  if (rt_append_text(out, out_len, pos, "\"") != 0) return -1;
  if (rt_append_text(out, out_len, pos, esc) != 0) return -1;
  return rt_append_text(out, out_len, pos, "\"");
}

static int append_summary_recent(const RtContext *ctx, char *out, size_t out_len, size_t *pos) {
  char memory[RT_XL];
  JsonRef root, tail = {0}, summary, recent_summary;
  if (rt_memory_projection_json(ctx ? ctx->context_id : NULL, 20, memory, sizeof(memory)) != 0 ||
      json_ref_from_text(memory, &root) != 0) return -1;
  if (rt_append_text(out, out_len, pos, "\"summary\":") != 0) return -1;
  if (json_ref_object_get(&root, "conversation_summary", &summary) == 0) {
    char raw[RT_LARGE], compact[RT_LARGE];
    if (json_ref_value_copy(&summary, raw, sizeof(raw)) != 0 ||
        json_compact(raw, compact, sizeof(compact)) != 0 ||
        rt_append_text(out, out_len, pos, compact) != 0) return -1;
  } else if (rt_append_text(out, out_len, pos, "null") != 0) return -1;
  if (rt_append_text(out, out_len, pos, ",\"conversation_summary\":") != 0) return -1;
  if (json_ref_object_get(&root, "conversation_summary", &summary) == 0) {
    char raw[RT_LARGE], compact[RT_LARGE];
    if (json_ref_value_copy(&summary, raw, sizeof(raw)) != 0 ||
        json_compact(raw, compact, sizeof(compact)) != 0 ||
        rt_append_text(out, out_len, pos, compact) != 0) return -1;
  } else if (rt_append_text(out, out_len, pos, "null") != 0) return -1;
  if (rt_append_text(out, out_len, pos, ",\"recent_summary\":") != 0) return -1;
  if (json_ref_object_get(&root, "recent_summary", &recent_summary) == 0) {
    char raw[RT_LARGE], compact[RT_LARGE];
    if (json_ref_value_copy(&recent_summary, raw, sizeof(raw)) != 0 ||
        json_compact(raw, compact, sizeof(compact)) != 0 ||
        rt_append_text(out, out_len, pos, compact) != 0) return -1;
  } else if (rt_append_text(out, out_len, pos, "null") != 0) return -1;
  if (rt_append_text(out, out_len, pos, ",\"recent\":[") != 0) return -1;
  if (json_ref_object_get_array(&root, "recent", &tail) != 0)
    (void)json_ref_object_get_array(&root, "memory_tail", &tail);
  if (tail.type == JSON_REF_ARRAY) {
    int first = 1;
    for (size_t i = 0; i < json_ref_array_size(&tail); ++i) {
      JsonRef item;
      char role[RT_SMALL], msg[RT_LARGE];
      if (json_ref_array_get(&tail, i, &item) != 0 ||
          json_ref_object_get_string(&item, "role", role, sizeof(role)) != 0 ||
          json_ref_object_get_string(&item, "text", msg, sizeof(msg)) != 0) continue;
      if ((!first && rt_append_text(out, out_len, pos, ",") != 0) ||
          rt_append_text(out, out_len, pos, "[") != 0 ||
          append_json_string_value(out, out_len, pos, role) != 0 ||
          rt_append_text(out, out_len, pos, ",") != 0 ||
          append_json_string_value(out, out_len, pos, msg) != 0 ||
          rt_append_text(out, out_len, pos, "]") != 0) return -1;
      first = 0;
    }
  }
  return rt_append_text(out, out_len, pos, "]");
}

/* Read config/floofclaw_config.json affairs.cross_context_read. When
 * true, the affair projection ignores the current context_id and
 * returns every active affair — writes stay per-context, only reads
 * are unioned. Off by default: preserves the isolation guarantee
 * that a channel can't see affairs another channel opened.
 * Config-owned; see docs/concepts/floofclaw_floop.md. */
static int affair_cross_context_read(void) {
  char *text = NULL;
  JsonRef root, affairs;
  int flag = 0;
  if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  if (json_ref_first_object(text, &root) == 0 &&
      json_ref_object_get_object(&root, "affairs", &affairs) == 0)
    (void)json_ref_object_get_bool(&affairs, "cross_context_read", &flag);
  free(text);
  return flag ? 1 : 0;
}

static const char *affair_projection_context(const RtContext *ctx) {
  return affair_cross_context_read() ? "" : (ctx ? ctx->context_id : "");
}

int rt_build_processor_input(const RtContext *ctx, const RtAgentMeta *agent_meta,
                             const char *bound_task_id,
                             long long bound_work_rev,
                             char *out, size_t out_len) {
  size_t pos = 0;
  int wrote = 0;
  if (!ctx || !out || out_len == 0) return -1;
  out[0] = '\0';
  if (agent_meta && agent_meta->memory_compaction_context_only)
    return rt_memory_compaction_input_json(ctx, out, out_len);
  if (agent_meta && agent_meta->bind_task_open_work) {
    char *task = NULL, *steps = NULL;
    int rc = -1;
    if (!bound_task_id || !*bound_task_id || bound_work_rev <= 0)
      return -1;
    task = (char *)fc_xmalloc(RT_XL);
    steps = (char *)fc_xmalloc(RT_XL);
    if (!task || !steps ||
        rt_task_work_agent_binding_json(ctx->context_id, bound_task_id,
                                        bound_work_rev, 0,
                                        task, RT_XL) != 0 ||
        rt_work_state_projection_json(bound_task_id, bound_work_rev,
                                      agent_meta->max_repair_attempts,
                                      steps, RT_XL) != 0)
      goto work_done;
    if (rt_append_text(out, out_len, &pos, "{\"event\":{\"kind\":") != 0 ||
        append_json_string_value(out, out_len, &pos,
                                 ctx->event_kind[0] ? ctx->event_kind : "user_message") != 0 ||
        rt_append_text(out, out_len, &pos, ",\"text\":") != 0 ||
        append_json_string_value(out, out_len, &pos, ctx->user_text) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"payload\":") != 0 ||
        rt_append_text(out, out_len, &pos,
                       ctx->event_payload_json[0] ? ctx->event_payload_json : "{}") != 0 ||
        rt_append_text(out, out_len, &pos, "},\"work_task\":") != 0 ||
        rt_append_text(out, out_len, &pos, task) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"work_steps\":") != 0 ||
        rt_append_text(out, out_len, &pos, steps) != 0 ||
        rt_append_text(out, out_len, &pos, "}\n") != 0)
      goto work_done;
    rc = 0;
work_done:
    fc_xfree(task);
    fc_xfree(steps);
    return rc;
  }
  if (agent_meta && agent_meta->affair_extraction_context_only) {
    char text[RT_LARGE], tasks[RT_XL], affairs[RT_XL];
    if (!bound_task_id || !*bound_task_id ||
        rt_task_input_text(ctx->context_id, bound_task_id, text, sizeof(text)) != 0)
      return -1;
    if (rt_tasks_active_agent_state_json(ctx->context_id,
                                         tasks, sizeof(tasks)) != 0)
      snprintf(tasks, sizeof(tasks), "{\"active\":[]}");
    if (rt_affairs_active_state_json(affair_projection_context(ctx), affairs, sizeof(affairs)) != 0)
      snprintf(affairs, sizeof(affairs), "{\"active\":[]}");
    if (rt_append_text(out, out_len, &pos, "{") != 0 ||
        append_summary_recent(ctx, out, out_len, &pos) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"input\":{\"text\":") != 0 ||
        append_json_string_value(out, out_len, &pos, text) != 0 ||
        rt_append_text(out, out_len, &pos, "},\"tasks\":") != 0 ||
        rt_append_text(out, out_len, &pos, tasks) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"affairs\":") != 0 ||
        rt_append_text(out, out_len, &pos, affairs) != 0 ||
        rt_append_text(out, out_len, &pos, "}\n") != 0) return -1;
    return 0;
  }
  if (agent_meta && agent_meta->conversational_payload_only) {
    char task_id[RT_SMALL], request_kind[RT_SMALL], text[RT_LARGE];
    if (rt_append_text(out, out_len, &pos, "{") != 0 ||
        append_summary_recent(ctx, out, out_len, &pos) != 0) return -1;
    if (rt_append_text(out, out_len, &pos, ",\"request\":") != 0) return -1;
    /* Speaker-first + affair watcher: if the inbound user_message
     * carried affair_id, the speaker sees an affair_review request
     * instead of a user_message — with manage, notes, and task_ids
     * projected in. */
    if (ctx->inbound_affair_id[0]) {
      char review_json[RT_XL];
      if (rt_affair_review_input_json(ctx->inbound_affair_id,
                                      review_json, sizeof(review_json)) == 0) {
        if (rt_append_text(out, out_len, &pos, "{\"kind\":\"affair_review\",") != 0)
          return -1;
        /* review_json is "{affair_id, manage, notes, task_ids}" — drop
         * the leading "{" so it joins our object. */
        if (review_json[0] == '{') {
          if (rt_append_text(out, out_len, &pos, review_json + 1) != 0) return -1;
        } else if (rt_append_text(out, out_len, &pos, "}") != 0) return -1;
        return rt_append_text(out, out_len, &pos, "}\n");
      }
      /* fall through to standard projection if affair not found */
    }
    if (rt_conversational_request_task(ctx, task_id, sizeof(task_id),
                                       request_kind, sizeof(request_kind),
                                       text, sizeof(text)) == 0) {
      if (rt_append_text(out, out_len, &pos, "{\"kind\":") != 0 ||
          append_json_string_value(out, out_len, &pos, request_kind) != 0 ||
          rt_append_text(out, out_len, &pos, ",\"text\":") != 0 ||
          append_json_string_value(out, out_len, &pos, text) != 0 ||
          rt_append_text(out, out_len, &pos, "}") != 0) return -1;
    } else if (rt_append_text(out, out_len, &pos, "null") != 0) return -1;
    return rt_append_text(out, out_len, &pos, "}\n");
  }
  if (rt_append_text(out, out_len, &pos, "{") != 0) return -1;
  if (agent_meta && agent_meta->listen_event) {
    /* Triggering-event projection: kind + text + the opaque inbound
     * payload, unchanged. The kernel projects; it never interprets. */
    if (rt_append_text(out, out_len, &pos, "\"event\":{\"kind\":") != 0 ||
        append_json_string_value(out, out_len, &pos,
                                 ctx->event_kind[0] ? ctx->event_kind : "user_message") != 0 ||
        rt_append_text(out, out_len, &pos, ",\"text\":") != 0 ||
        append_json_string_value(out, out_len, &pos, ctx->user_text) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"payload\":") != 0 ||
        rt_append_text(out, out_len, &pos,
                    ctx->event_payload_json[0] ? ctx->event_payload_json : "{}") != 0 ||
        rt_append_text(out, out_len, &pos, "}") != 0) return -1;
    wrote = 1;
  }
  if (agent_meta && agent_meta->listen_memory) {
    char memory[RT_XL];
    if (rt_memory_projection_json(ctx->context_id, 20, memory, sizeof(memory)) != 0)
      snprintf(memory, sizeof(memory),
               "{\"conversation_summary\":null,\"recent_summary\":null,\"recent\":[],\"memory_tail\":[]}");
    if ((wrote && rt_append_text(out, out_len, &pos, ",") != 0) ||
        rt_append_text(out, out_len, &pos, "\"memory\":") != 0 ||
        rt_append_text(out, out_len, &pos, memory) != 0) return -1;
    wrote = 1;
  }
  if (agent_meta && agent_meta->listen_tasks) {
    char tasks[RT_XL];
    if (rt_tasks_active_agent_state_json(ctx->context_id,
                                         tasks, sizeof(tasks)) != 0)
      snprintf(tasks, sizeof(tasks), "{\"active\":[]}");
    if ((wrote && rt_append_text(out, out_len, &pos, ",") != 0) ||
        rt_append_text(out, out_len, &pos, "\"tasks\":") != 0 ||
        rt_append_text(out, out_len, &pos, tasks) != 0) return -1;
    wrote = 1;
  }
  if (agent_meta && agent_meta->listen_usage) {
    char usage[RT_LARGE];
    if (rt_usage_projection_json(usage, sizeof(usage)) != 0)
      snprintf(usage, sizeof(usage),
               "{\"llm\":{\"calls\":0,\"tokens_in\":0,\"tokens_out\":0},\"by_agent\":[],"
               "\"workers\":{\"operations\":0,\"running\":0}}");
    if ((wrote && rt_append_text(out, out_len, &pos, ",") != 0) ||
        rt_append_text(out, out_len, &pos, "\"usage\":") != 0 ||
        rt_append_text(out, out_len, &pos, usage) != 0) return -1;
    wrote = 1;
  }
  if (agent_meta && agent_meta->listen_affairs) {
    char affairs[RT_XL];
    if (rt_affairs_active_state_json(affair_projection_context(ctx), affairs, sizeof(affairs)) != 0)
      snprintf(affairs, sizeof(affairs), "{\"active\":[]}");
    if ((wrote && rt_append_text(out, out_len, &pos, ",") != 0) ||
        rt_append_text(out, out_len, &pos, "\"affairs\":") != 0 ||
        rt_append_text(out, out_len, &pos, affairs) != 0) return -1;
    wrote = 1;
  }
  if (rt_append_text(out, out_len, &pos, "}\n") != 0) return -1;
  return 0;
}

static int task_belongs_to_current_run(const RtContext *ctx, const JsonRef *task) {
  char task_id[RT_SMALL] = "";
  char prefix[RT_SMALL];
  if (!ctx || !ctx->run_id[0] || !task) return 0;
  if (json_ref_object_get_string(task, "task_id", task_id, sizeof(task_id)) != 0 || !task_id[0])
    return 0;
  snprintf(prefix, sizeof(prefix), "task_%s_", ctx->run_id);
  return strncmp(task_id, prefix, strlen(prefix)) == 0;
}

int rt_conversational_request_task(const RtContext *ctx, char *task_id,
                                   size_t task_len, char *request_kind,
                                   size_t kind_len, char *text, size_t text_len) {
  char tasks_json[RT_XL];
  JsonRef root, active;
  if (!task_id || task_len == 0 || !request_kind || kind_len == 0 ||
      !text || text_len == 0) return -1;
  if (task_id && task_len) task_id[0] = '\0';
  if (request_kind && kind_len) request_kind[0] = '\0';
  if (text && text_len) text[0] = '\0';
  if (!ctx || rt_tasks_active_state_json(ctx->context_id, tasks_json, sizeof(tasks_json)) != 0 ||
      json_ref_from_text(tasks_json, &root) != 0 ||
      json_ref_object_get_array(&root, "active", &active) != 0) return -1;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < json_ref_array_size(&active); ++i) {
      char kind[RT_SMALL] = "", state[RT_SMALL] = "", input_type[RT_SMALL] = "";
      JsonRef task, input;
      if (json_ref_array_get(&active, i, &task) != 0 || task.type != JSON_REF_OBJECT ||
          !task_belongs_to_current_run(ctx, &task) ||
          json_ref_object_get_string(&task, "kind", kind, sizeof(kind)) != 0 ||
          rt_task_json_state_string(&task, "state", state,
                                    sizeof(state)) != 0)
        continue;
      if (pass == 0 && strcmp(kind, "input") == 0 && strcmp(state, "open") == 0 &&
          json_ref_object_get_object(&task, "input", &input) == 0 &&
          json_ref_object_get_string(&input, "type", input_type, sizeof(input_type)) == 0 &&
          strcmp(input_type, "message") == 0 &&
          json_ref_object_get_string(&task, "task_id", task_id, task_len) == 0 &&
          json_ref_object_get_string(&input, "text", text, text_len) == 0) {
        snprintf(request_kind, kind_len, "user_message");
        return 0;
      }
      if (pass == 1 && strcmp(kind, "notification") == 0 && strcmp(state, "open") == 0 &&
          json_ref_object_get_object(&task, "input", &input) == 0 &&
          json_ref_object_get_string(&input, "type", input_type, sizeof(input_type)) == 0 &&
          strcmp(input_type, "message") == 0 &&
          json_ref_object_get_string(&task, "task_id", task_id, task_len) == 0 &&
          json_ref_object_get_string(&input, "text", text, text_len) == 0) {
        snprintf(request_kind, kind_len, "notification");
        return 0;
      }
    }
  }
  return -1;
}

static int latest_provider_raw_response(const RtContext *ctx, char *out, size_t out_len) {
  char best[PATH_MAX] = "";
  if (!ctx || !out || out_len == 0) return -1;
  out[0] = '\0';
  for (int seq = 1; seq < 999; ++seq) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/provider_calls/%03d_raw_response.json", ctx->run_dir, seq);
    if (!fs_file_exists(path)) {
      if (seq == 1) continue;
      break;
    }
    snprintf(best, sizeof(best), "%s", path);
  }
  if (!best[0]) return -1;
  {
    char *text = NULL;
    int rc = fs_read_text(best, &text, FS_READ_TEXT_DEFAULT_CAP);
    if (rc != 0 || !text) return -1;
    snprintf(out, out_len, "%s", text);
    free(text);
  }
  return 0;
}

static int next_provider_seq_preview(const char *run_dir) {
  char probe[PATH_MAX];
  if (!run_dir || !*run_dir) return 1;
  for (int seq = 1; seq < 999; ++seq) {
    snprintf(probe, sizeof(probe), "%s/provider_calls/%03d_request.json", run_dir, seq);
    if (!fs_file_exists(probe)) return seq;
  }
  return 999;
}

static int agent_output_seq_exists(const char *dir, int seq) {
  DIR *d = opendir(dir);
  struct dirent *ent;
  char prefix[8];
  int found = 0;
  if (!d) return 0;
  snprintf(prefix, sizeof(prefix), "%03d_", seq);
  while ((ent = readdir(d)) != NULL) {
    size_t n = strlen(ent->d_name);
    if (strncmp(ent->d_name, prefix, 4) == 0 && n > strlen(".input.json") &&
        strcmp(ent->d_name + n - strlen(".input.json"), ".input.json") == 0) {
      found = 1;
      break;
    }
  }
  closedir(d);
  return found;
}

int rt_agent_artifact_seq(const char *run_dir, const char *executor) {
  char dir[PATH_MAX];
  if (executor && strcmp(executor, "llm") == 0) return next_provider_seq_preview(run_dir);
  snprintf(dir, sizeof(dir), "%s/agent_outputs", run_dir ? run_dir : "");
  for (int seq = 1; seq < 999; ++seq)
    if (!agent_output_seq_exists(dir, seq)) return seq;
  return 999;
}

static int append_json_value_or_string(char *out, size_t out_len, size_t *pos,
                                       const char *text) {
  JsonRef ref;
  if (text && json_ref_from_text(text, &ref) == 0)
    return rt_append_text(out, out_len, pos, text);
  return append_json_string_value(out, out_len, pos, text ? text : "");
}

static int llm_response_path_from_output(const char *output_path,
                                         char *out, size_t out_len) {
  size_t n;
  if (!output_path || !out || out_len == 0) return -1;
  n = strlen(output_path);
  if (n > strlen(".json") && strcmp(output_path + n - strlen(".json"), ".json") == 0) {
    size_t stem = n - strlen(".json");
    if (stem + strlen(".llm_response.json") + 1 > out_len) return -1;
    memcpy(out, output_path, stem);
    out[stem] = '\0';
    strcat(out, ".llm_response.json");
    return 0;
  }
  return snprintf(out, out_len, "%s.llm_response.json", output_path) >= (int)out_len ? -1 : 0;
}

int rt_agent_write_llm_response_artifact(RtContext *ctx, const RtStep *step,
                                         const char *agent_output_path,
                                         const char *model_text,
                                         const char *normalized) {
  char path[PATH_MAX], provider[RT_XL], events[RT_XL], doc[RT_XL];
  char eagent[RT_MED], estep[RT_MED];
  size_t pos = 0;
  if (!ctx || !step || !agent_output_path || !model_text || !normalized) return -1;
  if (rt_json_get_array_raw(normalized, "events", events, sizeof(events)) != 0) return -1;
  if (json_escape(step->target, eagent, sizeof(eagent)) != 0 ||
      json_escape(step->id, estep, sizeof(estep)) != 0) return -1;
  if (llm_response_path_from_output(agent_output_path, path, sizeof(path)) != 0) return -1;
  doc[0] = '\0';
  if (rt_append_text(doc, sizeof(doc), &pos,
                  "{\"runtime_meta\":{\"agent\":\"") != 0 ||
      rt_append_text(doc, sizeof(doc), &pos, eagent) != 0 ||
      rt_append_text(doc, sizeof(doc), &pos, "\",\"step\":\"") != 0 ||
      rt_append_text(doc, sizeof(doc), &pos, estep) != 0 ||
      rt_append_text(doc, sizeof(doc), &pos, "\",\"normalized_runtime_events\":") != 0 ||
      rt_append_text(doc, sizeof(doc), &pos, events) != 0 ||
      rt_append_text(doc, sizeof(doc), &pos, "},\"provider_response\":") != 0)
    return -1;
  if (latest_provider_raw_response(ctx, provider, sizeof(provider)) == 0) {
    if (append_json_value_or_string(doc, sizeof(doc), &pos, provider) != 0) return -1;
  } else if (rt_append_text(doc, sizeof(doc), &pos, "null") != 0) {
    return -1;
  }
  if (rt_append_text(doc, sizeof(doc), &pos, ",\"model_text\":") != 0 ||
      append_json_value_or_string(doc, sizeof(doc), &pos, model_text) != 0 ||
      rt_append_text(doc, sizeof(doc), &pos, "}\n") != 0) return -1;
  return fs_write_text(path, doc);
}
