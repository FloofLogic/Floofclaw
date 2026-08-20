#include "agent_exec.h"
#include "floop.h"
#include "support/heap_guard.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static int load_bot_name(char *out, size_t out_len) {
  char *text = NULL;
  JsonRef root;
  int rc = -1;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  if (json_ref_first_object(text, &root) == 0 &&
      json_ref_object_get_string(&root, "bot_name", out, out_len) == 0 &&
      out[0]) {
    rc = 0;
  }
  free(text);
  return rc;
}

/* Standing operator-curated facts about the user, config/user.md.
 * Deployment-owned like floofclaw_config.json and unreachable from the
 * workspace-scoped file actions, so only the operator writes it. Absent
 * file renders empty so shipped floops may reference @{user}
 * unconditionally; a file the prompt boundary can never fit fails
 * loudly instead of silently dropping the operator's facts. */
static int load_user_info(char **out) {
  int rc;
  if (!out) return -1;
  *out = NULL;
  rc = fs_read_text("config/user.md", out, RT_XL);
  if (rc == FS_READ_TOO_LARGE) return -1;
  return 0;
}
/* Wall-clock now, in the deployment's local zone, phrased for a model
 * rather than for a parser: "It's 9:26 PM CDT on 7/28/26".
 *
 * A model cannot derive this and no prompt or action description can
 * supply it. Without it an agent asked to schedule "tonight" has to
 * invent a date, and it invents one from its training priors -- observed
 * live: a request for 10pm tonight became 2024-07-26T22:00:00-07:00, two
 * years stale and in the wrong zone, even though the schema example
 * carried the correct year and offset. Format teaches format; only a fact
 * teaches now.
 *
 * Built from tm fields rather than strftime's %-I/%-m, which are
 * extensions this codebase should not depend on across libc's. */
static int load_now_phrase(char *out, size_t out_len) {
  time_t t = time(NULL);
  struct tm tmv;
  char zone[64] = "";
  int hour12;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (!localtime_r(&t, &tmv)) return -1;
  if (strftime(zone, sizeof(zone), "%Z", &tmv) == 0) zone[0] = '\0';
  hour12 = tmv.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  return snprintf(out, out_len, "It's %d:%02d %s%s%s on %d/%d/%02d",
                  hour12, tmv.tm_min,
                  tmv.tm_hour < 12 ? "AM" : "PM",
                  zone[0] ? " " : "", zone,
                  tmv.tm_mon + 1, tmv.tm_mday,
                  (tmv.tm_year + 1900) % 100) < (int)out_len ? 0 : -1;
}

static int append_bytes(char *out, size_t out_len, size_t *pos,
                        const char *s, size_t n) {
  if (!out || !pos || !s) return -1;
  if (*pos + n >= out_len) return -1;
  memcpy(out + *pos, s, n);
  *pos += n;
  out[*pos] = '\0';
  return 0;
}

static size_t text_count(const char *text, const char *needle) {
  size_t count = 0;
  size_t n;
  const char *p;
  if (!text || !needle || !*needle) return 0;
  n = strlen(needle);
  p = text;
  while ((p = strstr(p, needle)) != NULL) {
    count++;
    p += n;
  }
  return count;
}

static int render_prompt_vars_with(const char *raw, const char *json,
                                   const char *tools, const char *user_info,
                                   char *out, size_t out_len, int *overflow) {
  size_t i = 0, pos = 0;
  char bot_name[RT_SMALL];
  int bot_name_loaded = 0;
  if (overflow) *overflow = 0;
  if (!raw || !out || out_len == 0) return -1;
  out[0] = '\0';
  while (raw[i]) {
    if (raw[i] == '{' && raw[i + 1] == '{') {
      const char *name = raw + i + 2;
      const char *end = strstr(name, "}}");
      size_t name_len;
      if (!end) return -1;
      name_len = (size_t)(end - name);
      if (name_len == 4 && strncmp(name, "json", 4) == 0) {
        if (rt_append_text(out, out_len, &pos, json ? json : "{}") != 0) {
          if (overflow) *overflow = 1;
          return -1;
        }
      } else if (name_len == 5 && strncmp(name, "tools", 5) == 0) {
        if (rt_append_text(out, out_len, &pos, tools ? tools : "") != 0) {
          if (overflow) *overflow = 1;
          return -1;
        }
      } else {
        return -1;
      }
      i = (size_t)(end - raw) + 2;
      continue;
    }
    if (raw[i] == '@' && raw[i + 1] == '{') {
      const char *name = raw + i + 2;
      const char *end = strchr(name, '}');
      size_t name_len;
      if (!end) return -1;
      name_len = (size_t)(end - name);
      if (name_len == 8 && strncmp(name, "bot_name", 8) == 0) {
        if (!bot_name_loaded) {
          if (load_bot_name(bot_name, sizeof(bot_name)) != 0) return -1;
          bot_name_loaded = 1;
        }
        if (rt_append_text(out, out_len, &pos, bot_name) != 0) {
          if (overflow) *overflow = 1;
          return -1;
        }
      } else if (name_len == 3 && strncmp(name, "now", 3) == 0) {
        char now_phrase[RT_SMALL];
        if (load_now_phrase(now_phrase, sizeof(now_phrase)) != 0) return -1;
        if (rt_append_text(out, out_len, &pos, now_phrase) != 0) {
          if (overflow) *overflow = 1;
          return -1;
        }
      } else if (name_len == 4 && strncmp(name, "user", 4) == 0) {
        if (rt_append_text(out, out_len, &pos, user_info ? user_info : "") != 0) {
          if (overflow) *overflow = 1;
          return -1;
        }
      } else {
        return -1;
      }
      i = (size_t)(end - raw) + 1;
      continue;
    }
    if (append_bytes(out, out_len, &pos, raw + i, 1) != 0) {
      if (overflow) *overflow = 1;
      return -1;
    }
    i++;
  }
  return 0;
}

static int render_prompt_vars(const char *raw, const char *json, const char *tools,
                              char *out, size_t out_len, int *overflow) {
  char *user_info = NULL;
  int rc;
  if (raw && strstr(raw, "@{user}")) {
    if (load_user_info(&user_info) != 0) return -1;
  }
  rc = render_prompt_vars_with(raw, json, tools, user_info,
                               out, out_len, overflow);
  free(user_info);
  return rc;
}

static int build_agent_input(const RtContext *ctx, const char *agent_dir, const char *executor,
                            const RtActionRegistry *actions,
                            const RtAgentMeta *agent_meta,
                            const char *bound_task_id,
                            long long bound_work_rev,
                            char *out, size_t out_len,
                            char *err, size_t err_len) {
  char prompt_path[PATH_MAX];
  char *prompt_text = NULL;
  char *rendered_prompt = NULL;
  char *model_input = NULL;
  char *available = NULL;
  const char *agent_id = agent_meta && agent_meta->id[0] ? agent_meta->id : "(unknown)";
  int rc = -1;
  if (err && err_len) err[0] = '\0';
  rendered_prompt = (char *)fc_xmalloc(RT_XL);
  model_input = (char *)fc_xmalloc(RT_XL);
  available = (char *)fc_xmalloc(RT_XL);
  if (!rendered_prompt || !model_input || !available) {
    if (err) snprintf(err, err_len,
                      "agent %s input allocation failed", agent_id);
    goto done;
  }
  if (rt_build_processor_input(ctx, agent_meta, bound_task_id, bound_work_rev,
                               model_input, RT_XL) != 0) {
    if (err) snprintf(err, err_len,
                      "agent %s model input projection failed; "
                      "fix: reduce its configured listen input", agent_id);
    goto done;
  }
  if (!executor || strcmp(executor, "llm") != 0) {
    rc = snprintf(out, out_len, "%s", model_input) >= (int)out_len ? -1 : 0;
    goto done;
  }
  if (rt_action_render_available(actions, agent_meta,
                                 available, RT_XL) != 0) {
    if (err) snprintf(err, err_len,
                      "agent %s action catalog exceeds the %d-byte prompt "
                      "boundary; fix: shorten source action descriptions or "
                      "schemas, or remove actions from that agent's catalog",
                      agent_id, RT_XL - 1);
    goto done;
  }
  snprintf(prompt_path, sizeof(prompt_path), "%s/prompt.md", agent_dir ? agent_dir : "");
  if (fs_read_text(prompt_path, &prompt_text, FS_READ_TEXT_DEFAULT_CAP) == 0 && prompt_text) {
    int n;
    int overflow = 0;
    int has_tools = strstr(prompt_text, "{{tools}}") != NULL;
    int has_json = strstr(prompt_text, "{{json}}") != NULL;
    if (text_count(prompt_text, "{{tools}}") > 1) {
      if (err) snprintf(err, err_len,
                        "agent %s prompt repeats {{tools}}; fix: place its "
                        "action catalog exactly once", agent_id);
      goto done;
    }
    if (render_prompt_vars(prompt_text, model_input, available,
                           rendered_prompt, RT_XL,
                           &overflow) != 0) {
      if (err) {
        if (overflow)
          snprintf(err, err_len,
                   "agent %s complete prompt and action catalog exceed the "
                   "%d-byte prompt boundary; fix: shorten its prompt, listen "
                   "input, or source action manifests",
                   agent_id, RT_XL - 1);
        else
          snprintf(err, err_len,
                   "agent %s prompt template is invalid; fix: use only "
                   "{{json}}, {{tools}}, @{bot_name}, @{now}, and @{user} "
                   "variables",
                   agent_id);
      }
      goto done;
    }
    if (agent_meta && (agent_meta->conversational_payload_only ||
                       agent_meta->affair_extraction_context_only ||
                       agent_meta->memory_compaction_context_only)) {
      n = snprintf(out, out_len, "%s", rendered_prompt);
      if (n < 0 || (size_t)n >= out_len) {
        if (err) snprintf(err, err_len,
                          "agent %s complete prompt and action catalog exceed "
                          "the %d-byte prompt boundary; fix: shorten its prompt, "
                          "listen input, or source action manifests",
                          agent_id, RT_XL - 1);
        goto done;
      }
      rc = 0;
      goto done;
    }
    n = snprintf(out, out_len,
                 "%s%s%s%s%s\n\n"
                 "Reply with one JSON object only.",
                 rendered_prompt,
                 has_tools ? "" : "\n\n=== available tools ===\n",
                 has_tools ? "" : available,
                 has_json ? "" : "\n=== model input ===\n",
                 has_json ? "" : model_input);
    if (n < 0 || (size_t)n >= out_len) {
      if (err) snprintf(err, err_len,
                        "agent %s complete prompt and action catalog exceed "
                        "the %d-byte prompt boundary; fix: shorten its prompt, "
                        "listen input, or source action manifests",
                        agent_id, RT_XL - 1);
      goto done;
    }
    rc = 0;
    goto done;
  }
  {
    int n = snprintf(out, out_len,
                     "=== available tools ===\n%s\n"
                     "=== model input ===\n%s\n\n"
                     "Reply with one JSON object only.",
                     available, model_input);
    if (n < 0 || (size_t)n >= out_len) {
      if (err) snprintf(err, err_len,
                        "agent %s complete prompt and action catalog exceed "
                        "the %d-byte prompt boundary; fix: shorten its prompt, "
                        "listen input, or source action manifests",
                        agent_id, RT_XL - 1);
      goto done;
    }
    rc = 0;
  }
done:
  free(prompt_text);
  fc_xfree(rendered_prompt);
  fc_xfree(model_input);
  fc_xfree(available);
  return rc;
}

static int dyn_reserve(char **buf, size_t *cap, size_t need_len) {
  size_t new_cap;
  char *grown;
  if (!buf || !cap) return -1;
  if (*buf && *cap > need_len + 1U) return 0;
  new_cap = *cap ? *cap : 256U;
  while (new_cap <= need_len + 1U) {
    if (new_cap > (size_t)-1 / 2U) return -1;
    new_cap *= 2U;
  }
  grown = *buf ? (char *)fc_xrealloc(*buf, new_cap) : (char *)fc_xmalloc(new_cap);
  if (!grown) return -1;
  if (!*buf) grown[0] = '\0';
  *buf = grown;
  *cap = new_cap;
  return 0;
}

static int dyn_append_bytes(char **buf, size_t *cap, size_t *pos,
                            const char *s, size_t n) {
  if (!buf || !cap || !pos || !s) return -1;
  if (dyn_reserve(buf, cap, *pos + n) != 0) return -1;
  memcpy(*buf + *pos, s, n);
  *pos += n;
  (*buf)[*pos] = '\0';
  return 0;
}

static int dyn_append_text(char **buf, size_t *cap, size_t *pos, const char *s) {
  return dyn_append_bytes(buf, cap, pos, s ? s : "", strlen(s ? s : ""));
}

static int dyn_append_json_pair(char **buf, size_t *cap, size_t *pos,
                                const char *key, const JsonRef *value, int first) {
  char ekey[RT_MED];
  char *compact = NULL;
  int rc = -1;
  if (json_escape(key, ekey, sizeof(ekey)) != 0) return -1;
  compact = json_ref_value_compact_dup(value);
  if (!compact) return -1;
  if ((!first && dyn_append_text(buf, cap, pos, ",") != 0) ||
      dyn_append_text(buf, cap, pos, "\"") != 0 ||
      dyn_append_text(buf, cap, pos, ekey) != 0 ||
      dyn_append_text(buf, cap, pos, "\":") != 0 ||
      dyn_append_text(buf, cap, pos, compact) != 0) goto cleanup;
  rc = 0;
cleanup:
  fc_xfree(compact);
  return rc;
}

static char *normalize_call_args_dup(const JsonRef *args_ref,
                                     char *task_id, size_t task_len) {
  size_t cursor = 0, pos = 0, cap = 0;
  char key[RT_SMALL];
  JsonRef value;
  int first = 1;
  char *args_no_task = NULL;
  if (!args_ref || args_ref->type != JSON_REF_OBJECT) return NULL;
  if (task_id && task_len) task_id[0] = '\0';
  if (dyn_append_text(&args_no_task, &cap, &pos, "{") != 0) goto fail;
  while (json_ref_object_iter(args_ref, &cursor, key, sizeof(key), &value) == 1) {
    if (strcmp(key, "event_id") == 0 || strcmp(key, "run_id") == 0 ||
        strcmp(key, "request_id") == 0 || strcmp(key, "job_id") == 0 ||
        strcmp(key, "source") == 0 || strcmp(key, "created_by") == 0 ||
        strcmp(key, "work_rev") == 0)
      goto fail;
    if (strcmp(key, "task_id") == 0) {
      if (!task_id || json_ref_string_copy(&value, task_id, task_len) != 0 || !task_id[0])
        goto fail;
      continue;
    }
    if (dyn_append_json_pair(&args_no_task, &cap, &pos, key, &value, first) != 0)
      goto fail;
    first = 0;
  }
  if (dyn_append_text(&args_no_task, &cap, &pos, "}") != 0) goto fail;
  return args_no_task;
fail:
  fc_xfree(args_no_task);
  return NULL;
}

static void infer_missing_task_id(RtContext *ctx, const char *bound_task_id,
                                  char *task_id, size_t task_len) {
  char inferred[RT_SMALL] = "";
  JsonRef payload;
  if (!ctx || !task_id || task_len == 0 || task_id[0]) return;

  /* Prefer exact runtime correlation over a context-wide guess. A chat may
   * legitimately retain another open input while this turn discovers or
   * runs an action. The triggering user run owns one deterministic input
   * task, and later operation_result events carry that task_id explicitly. */
  if (bound_task_id && *bound_task_id &&
      rt_task_reference_in_context(ctx->context_id, bound_task_id)) {
    snprintf(task_id, task_len, "%s", bound_task_id);
    return;
  }
  if (strcmp(ctx->event_kind, "user_message") == 0 &&
      rt_task_select_new_input_for_run(ctx->context_id, ctx->run_id,
                                       inferred, sizeof(inferred)) == 0) {
    snprintf(task_id, task_len, "%s", inferred);
    return;
  }
  if (ctx->event_payload_json[0] &&
      json_ref_top_object(ctx->event_payload_json, &payload) == 0 &&
      json_ref_object_get_string(&payload, "task_id", inferred,
                                 sizeof(inferred)) == 0 && inferred[0] &&
      rt_task_reference_in_context(ctx->context_id, inferred)) {
    snprintf(task_id, task_len, "%s", inferred);
    return;
  }
  if (rt_task_select_unique_open_input(ctx->context_id, inferred,
                                       sizeof(inferred)) == 0)
    snprintf(task_id, task_len, "%s", inferred);
}

static int append_task_payload(char *item, size_t item_len, const char *prefix,
                               const char *type, const char *task_id,
                               const char *args_no_task) {
  char etask[RT_MED];
  size_t len;
  const char *body = "";
  size_t inner = 0;
  if (!task_id || !*task_id || json_escape(task_id, etask, sizeof(etask)) != 0) return -1;
  len = strlen(args_no_task ? args_no_task : "{}");
  if (len >= 2 && args_no_task[0] == '{' && args_no_task[len - 1] == '}') {
    body = args_no_task + 1;
    inner = len - 2;
  }
  return snprintf(item, item_len, "%s{\"type\":\"%s\",\"payload\":{\"task_id\":\"%s\"%s%.*s}}",
                  prefix, type, etask, inner ? "," : "", (int)inner, body) >= (int)item_len ? -1 : 0;
}

static int agent_call_intent_name(const JsonRef *call,
                                  char *out, size_t out_len) {
  char alias[RT_SMALL] = "";
  int has_name, has_alias;
  if (!call || call->type != JSON_REF_OBJECT || !out || out_len == 0)
    return -1;
  out[0] = '\0';
  has_name = json_ref_object_get_string(call, "name", out, out_len) == 0 &&
             out[0];
  has_alias = json_ref_object_get_string(call, "action", alias,
                                         sizeof(alias)) == 0 && alias[0];
  if (has_name == has_alias) return -1;
  if (has_alias) snprintf(out, out_len, "%s", alias);
  return 0;
}

static int output_contract_task_id(const RtContext *ctx, char *out,
                                   size_t out_len) {
  char request_kind[RT_SMALL], text[RT_LARGE];
  JsonRef payload;
  if (!ctx || !out || out_len == 0) return -1;
  out[0] = '\0';
  if (strcmp(ctx->event_kind, "user_message") == 0)
    return rt_conversational_request_task(ctx, out, out_len,
                                           request_kind, sizeof(request_kind),
                                           text, sizeof(text));
  if (strcmp(ctx->event_kind, "operation_result") == 0) {
    if (json_ref_top_object(ctx->event_payload_json, &payload) != 0 ||
        json_ref_object_get_string(&payload, "task_id", out, out_len) != 0 ||
        !out[0] || !rt_task_reference_in_context(ctx->context_id, out))
      return -1;
    return 0;
  }
  return 1;
}

int rt_agent_validate_output_contract(const RtContext *ctx,
                                      const RtAgentMeta *agent_meta,
                                      const char *output_json,
                                      char *reason, size_t reason_len) {
  JsonRef root, calls, call;
  char expected_task[RT_SMALL];
  size_t message_count = 0, update_count = 0, sidecar_count = 0;
  size_t substantive_count = 0;
  int task_rc;
  if (reason && reason_len) reason[0] = '\0';
  if (!agent_meta || !agent_meta->output_contract[0]) return 1;
  if (strcmp(agent_meta->output_contract,
             "task_action_or_finalize") != 0) {
    if (reason) snprintf(reason, reason_len,
                         "unknown configured output contract");
    return -1;
  }
  task_rc = output_contract_task_id(ctx, expected_task,
                                    sizeof(expected_task));
  if (task_rc > 0) return 1;
  if (task_rc < 0) {
    if (reason) snprintf(
        reason, reason_len,
        "the task-bearing event has no exact active task binding");
    return -1;
  }
  if (!output_json || json_ref_top_object(output_json, &root) != 0 ||
      json_ref_object_get_array(&root, "calls", &calls) != 0) {
    if (reason) snprintf(reason, reason_len,
                         "output must be one JSON object with a calls array");
    return -1;
  }
  for (size_t i = 0; i < json_ref_array_size(&calls); ++i) {
    char name[RT_SMALL];
    if (json_ref_array_get(&calls, i, &call) != 0 ||
        call.type != JSON_REF_OBJECT ||
        agent_call_intent_name(&call, name, sizeof(name)) != 0) {
      if (reason) snprintf(
          reason, reason_len,
          "every call must contain exactly one name or action");
      return -1;
    }
    if (strcmp(name, "message") == 0 ||
        strcmp(name, "message.send") == 0) {
      message_count++;
    } else if (strcmp(name, "task.update") == 0) {
      JsonRef args;
      char task_id[RT_SMALL] = "", state[RT_SMALL] = "";
      update_count++;
      if (json_ref_object_get_object(&call, "args", &args) != 0 ||
          json_ref_object_get_string(&args, "task_id", task_id,
                                     sizeof(task_id)) != 0 ||
          json_ref_object_get_string(&args, "state", state,
                                     sizeof(state)) != 0 ||
          strcmp(task_id, expected_task) != 0 ||
          strcmp(state, "completed") != 0) {
        if (reason) snprintf(
            reason, reason_len,
            "FINALIZE requires task.update with the exact current task_id "
            "and state completed");
        return -1;
      }
    } else if (strcmp(name, "working_memory_append") == 0) {
      sidecar_count++;
    } else {
      substantive_count++;
    }
  }
  if (message_count == 1 && update_count == 0 &&
      substantive_count == 0 && sidecar_count == 0) {
    if (reason) snprintf(
        reason, reason_len,
        "message-only response is invalid: it neither continues work nor "
        "finalizes the task");
    return -1;
  }
  if (substantive_count == 1 && update_count == 0 &&
      message_count <= 1 && sidecar_count <= 1)
    return 0;
  if (substantive_count == 0 && update_count == 1 &&
      message_count == 1 && sidecar_count == 0)
    return 0;
  if (reason) snprintf(
      reason, reason_len,
      "return exactly CONTINUE (one substantive action, optional message "
      "and working_memory_append, no task.update) or FINALIZE (one message "
      "plus exact completed task.update, no substantive action)");
  return -1;
}

int rt_agent_normalize_output(RtContext *ctx, const RtStep *step,
                              const RtAgentMeta *agent_meta,
                              const char *bound_task_id,
                              long long bound_work_rev,
                              const char *output_json,
                              char *out, size_t out_len) {
  JsonRef root, calls, call, args_ref;
  char trusted_trigger[RT_SMALL] = "";
  size_t pos = 0, sidecar_count = 0, ordinary_count = 0;
  if (!ctx || !step || !output_json || !out || out_len == 0) return -1;
  if (agent_meta && agent_meta->conversational_payload_only)
    return rt_agent_normalize_conversational_output(ctx, step, output_json, out, out_len);
  if (agent_meta && agent_meta->affair_extraction_context_only)
    return rt_agent_normalize_affair_output(ctx, step, bound_task_id, output_json, out, out_len);
  if (agent_meta && agent_meta->memory_compaction_context_only)
    return rt_agent_normalize_memory_compaction_output(ctx, step, output_json, out, out_len);
  if (json_ref_first_object(output_json, &root) != 0 ||
      json_ref_object_get_array(&root, "calls", &calls) != 0) {
    rt_log_rejected_agent_event(step->target, "agent_output_missing_calls",
                                "agent output did not contain a calls array");
    return -1;
  }
  for (size_t i = 0; i < json_ref_array_size(&calls); ++i) {
    char name[RT_SMALL];
    if (json_ref_array_get(&calls, i, &call) != 0 ||
        agent_call_intent_name(&call, name, sizeof(name)) != 0) {
      rt_log_rejected_agent_event(step->target, "invalid_agent_call",
                                  "call must contain exactly one of name or action");
      return -1;
    }
    if (strcmp(name, "working_memory_append") == 0)
      sidecar_count++;
    else
      ordinary_count++;
  }
  if (sidecar_count > 1) {
    rt_log_rejected_agent_event(step->target, "invalid_sidecar_call_count",
                                "an agent response may append working_memory at most once");
    return -1;
  }
  if (agent_meta && agent_meta->bind_task_open_work) {
    char *exact_task = (char *)fc_xmalloc(RT_XL);
    if (!exact_task || !bound_task_id || !*bound_task_id ||
        bound_work_rev <= 0 ||
        rt_task_work_binding_json(ctx->context_id, bound_task_id,
                                  bound_work_rev, 0,
                                  exact_task, RT_XL) != 0) {
      fc_xfree(exact_task);
      rt_log_rejected_agent_event(step->target, "stale_work_binding",
                                  "bound work task or revision is no longer active");
      return -1;
    }
    fc_xfree(exact_task);
    if (strcmp(step->gate, "work_consequence") == 0) {
      char selected_task[RT_SMALL];
      long long selected_rev = 0;
      if (rt_task_select_work_consequence(
              ctx, selected_task, sizeof(selected_task), &selected_rev,
              trusted_trigger, sizeof(trusted_trigger)) != 0 ||
          strcmp(selected_task, bound_task_id) != 0 ||
          selected_rev != bound_work_rev) {
        rt_log_rejected_agent_event(
            step->target, "stale_work_consequence",
            "controller consequence is no longer exact or was already consumed");
        return -1;
      }
    }
    if (ordinary_count > 1 || (ordinary_count == 0 && sidecar_count == 0)) {
      rt_log_rejected_agent_event(step->target, "invalid_controller_call_count",
                                  ordinary_count == 0 && sidecar_count == 0
                                      ? "bound controller must choose a call; received zero"
                                      : "bound controller may choose at most one ordinary call");
      return -1;
    }
  }
  if (rt_append_text(out, out_len, &pos, "{\"events\":[") != 0) return -1;
  {
  size_t emitted = 0;
  int passes = agent_meta && agent_meta->bind_task_open_work && sidecar_count
                   ? 2 : 1;
  for (int pass = 0; pass < passes; ++pass) {
  for (size_t i = 0; i < json_ref_array_size(&calls); ++i) {
    char name[RT_SMALL] = "", ename[RT_MED];
    char task_id[RT_SMALL] = "", etask[RT_MED] = "", item[RT_XL];
    char work_rev_clause[RT_MED] = "";
    char *args_raw = NULL;
    size_t cursor = 0;
    char key[RT_SMALL];
    JsonRef value;
    int has_name;
    int has_action_alias;
    int is_sidecar;
    int n;
    if (json_ref_array_get(&calls, i, &call) != 0 ||
        call.type != JSON_REF_OBJECT) {
      rt_log_rejected_agent_event(step->target, "invalid_agent_call", "call missing name");
      return -1;
    }
    has_name =
        json_ref_object_get_string(&call, "name", name, sizeof(name)) == 0 &&
        name[0];
    has_action_alias =
        json_ref_object_get_string(&call, "action", ename, sizeof(ename)) == 0 &&
        ename[0];
    if (has_name && has_action_alias) {
      rt_log_rejected_agent_event(
          step->target, "invalid_agent_call",
          "call must not contain both name and action");
      return -1;
    }
    if (!has_name && has_action_alias)
      snprintf(name, sizeof(name), "%s", ename);
    if (!name[0]) {
      rt_log_rejected_agent_event(step->target, "invalid_agent_call",
                                  "call missing name");
      return -1;
    }
    is_sidecar = strcmp(name, "working_memory_append") == 0;
    if (passes == 2 && ((pass == 0 && !is_sidecar) ||
                        (pass == 1 && is_sidecar)))
      continue;
    while (json_ref_object_iter(&call, &cursor, key, sizeof(key), &value) == 1) {
      if (strcmp(key, "name") != 0 && strcmp(key, "action") != 0 &&
          strcmp(key, "args") != 0) {
        rt_log_rejected_agent_event(step->target, "invalid_agent_call", "call has non-intent key");
        return -1;
      }
    }
    if (json_ref_object_get(&call, "args", &args_ref) == 0) {
      args_raw = normalize_call_args_dup(&args_ref, task_id, sizeof(task_id));
      if (!args_raw) {
        rt_log_rejected_agent_event(step->target, "invalid_agent_call", "call args must be object");
        return -1;
      }
    } else {
      args_raw = fc_xstrdup("{}");
      if (!args_raw) return -1;
    }
    if (agent_meta && agent_meta->bind_task_open_work) {
      if (task_id[0] && strcmp(task_id, bound_task_id) != 0) {
        fc_xfree(args_raw);
        rt_log_rejected_agent_event(step->target, "invalid_agent_call",
                                    "controller task_id does not match its trusted work binding");
        return -1;
      }
      snprintf(task_id, sizeof(task_id), "%s", bound_task_id);
      if (!is_sidecar && trusted_trigger[0]) {
        char etrigger[RT_MED];
        if (json_escape(trusted_trigger, etrigger, sizeof(etrigger)) != 0) {
          fc_xfree(args_raw);
          return -1;
        }
        snprintf(work_rev_clause, sizeof(work_rev_clause),
                 ",\"work_rev\":%lld,\"trigger_event_id\":\"%s\"",
                 bound_work_rev, etrigger);
      } else if (!is_sidecar) {
        snprintf(work_rev_clause, sizeof(work_rev_clause),
                 ",\"work_rev\":%lld", bound_work_rev);
      }
      if (strcmp(name, "task.create") == 0 ||
          strcmp(name, "task.update") == 0 ||
          strncmp(name, "memory.", strlen("memory.")) == 0) {
        fc_xfree(args_raw);
        rt_log_rejected_agent_event(step->target, "invalid_agent_call",
                                    "bound controller must choose a registered action");
        return -1;
      }
    }
    if (task_id[0] && json_escape(task_id, etask, sizeof(etask)) != 0) {
      fc_xfree(args_raw);
      return -1;
    }
    if (json_escape(name, ename, sizeof(ename)) != 0) {
      fc_xfree(args_raw);
      return -1;
    }
    if (strcmp(name, "task.create") == 0) {
      if (task_id[0]) {
        fc_xfree(args_raw);
        rt_log_rejected_agent_event(step->target, "invalid_agent_call", "task.create must not include task_id");
        return -1;
      }
      n = snprintf(item, sizeof(item),
                   "%s{\"type\":\"task_created\",\"payload\":%s}", emitted ? "," : "", args_raw);
    } else if (strcmp(name, "task.update") == 0) {
      if (!task_id[0] || !rt_task_reference_in_context(ctx->context_id, task_id)) {
        fc_xfree(args_raw);
        rt_log_rejected_agent_event(step->target, "invalid_agent_call", "task.update requires existing task_id");
        return -1;
      }
      n = append_task_payload(item, sizeof(item), emitted ? "," : "",
                              "task_updated", task_id, args_raw);
    } else if (strcmp(name, "message.send") == 0 || strcmp(name, "message") == 0 ||
               strcmp(name, "note_add") == 0 ||
               strcmp(name, "counter_bump") == 0 ||
               strcmp(name, "affair_close") == 0 ||
               strcmp(name, "defer") == 0) {
      /* Affair-scoped or context-scoped actions (message, note_add,
       * counter_bump, affair_close, defer). Task binding is OPTIONAL — these
       * actions carry their own scoping (affair_id) or are proactive
       * (a review's message reply, an operation_result note_add).
       *
       * We STILL attempt to infer a task_id from the context (unique
       * open input task). When one exists (a live user_message run),
       * binding to it lets downstream hooks like
       * autocomplete_message_task_on_send fire correctly. When no
       * input task exists (operation_result runs), infer silently
       * returns nothing and the action still succeeds — task_id is
       * simply omitted from the payload.
       *
       * Without this list, agent_exec's ELSE branch demands a task_id
       * for any action call and REJECTS if inference fails. That
       * breaks result_manager on operation_result runs. */
      const char *action_name = strcmp(name, "message.send") == 0 ? "message" : name;
      infer_missing_task_id(ctx, bound_task_id, task_id, sizeof(task_id));
      if (task_id[0] && json_escape(task_id, etask, sizeof(etask)) != 0) {
        fc_xfree(args_raw);
        return -1;
      }
      if (task_id[0] && !rt_task_reference_in_context(ctx->context_id, task_id)) {
        fc_xfree(args_raw);
        rt_log_rejected_agent_event(step->target, "invalid_agent_call",
                                    "action task_id is not valid in context");
        return -1;
      }
      n = snprintf(item, sizeof(item),
                   "%s{\"type\":\"action_request\",\"payload\":{\"action\":\"%s\","
                   "\"args\":%s%s%s%s%s}}",
                   emitted ? "," : "", action_name, args_raw,
                   task_id[0] ? ",\"task_id\":\"" : "",
                   task_id[0] ? etask : "",
                   task_id[0] ? "\"" : "",
                   work_rev_clause);
    } else if (strcmp(name, "memory.recalled") == 0) {
      n = snprintf(item, sizeof(item),
                   "%s{\"type\":\"memory_recalled\",\"payload\":%s}",
                   emitted ? "," : "", args_raw);
    } else if (strcmp(name, "memory.user") == 0 || strcmp(name, "memory.asst") == 0 ||
               strcmp(name, "memory.fact") == 0 || strcmp(name, "memory.summ") == 0) {
      const char *type = name + strlen("memory.");
      if (task_id[0]) {
        fc_xfree(args_raw);
        rt_log_rejected_agent_event(step->target, "invalid_agent_call", "memory call must not include task_id");
        return -1;
      }
      n = snprintf(item, sizeof(item),
                   "%s{\"type\":\"%s\",\"payload\":%s}",
                   emitted ? "," : "", type, args_raw);
    } else {
      infer_missing_task_id(ctx, bound_task_id, task_id, sizeof(task_id));
      if (task_id[0] && json_escape(task_id, etask, sizeof(etask)) != 0) {
        fc_xfree(args_raw);
        return -1;
      }
      if (!task_id[0] || !rt_task_reference_in_context(ctx->context_id, task_id)) {
        fc_xfree(args_raw);
        rt_log_rejected_agent_event(step->target, "invalid_agent_call", "action call requires existing task_id");
        return -1;
      }
      n = snprintf(item, sizeof(item),
                   "%s{\"type\":\"action_request\",\"payload\":{\"action\":\"%s\","
                   "\"args\":%s,\"task_id\":\"%s\"%s}}",
                   emitted ? "," : "", ename, args_raw, etask, work_rev_clause);
    }
    fc_xfree(args_raw);
    if (n < 0 || (size_t)n >= sizeof(item) ||
        rt_append_text(out, out_len, &pos, item) != 0) return -1;
    emitted++;
  }
  }
  }
  return rt_append_text(out, out_len, &pos, "]}");
}

static int resolve_media_manifest_path(const RtContext *ctx,
                                       char *out, size_t out_len) {
  JsonRef media;
  char relative[PATH_MAX];
  const char *workspace;
  static const char prefix[] = "media/inbound/";
  const char *leaf;
  int n;
  if (!ctx || !ctx->media_ref_json || !out || out_len == 0) return -1;
  out[0] = '\0';
  if (json_ref_first_object(ctx->media_ref_json, &media) != 0 ||
      json_ref_object_get_string(&media, "manifest",
                                 relative, sizeof(relative)) != 0 ||
      strncmp(relative, prefix, sizeof(prefix) - 1U) != 0 ||
      relative[0] == '/' || strchr(relative, '\\') ||
      strstr(relative, "..") || strstr(relative, "//"))
    return -1;
  leaf = relative + sizeof(prefix) - 1U;
  if (!*leaf || strchr(leaf, '/')) return -1;
  workspace = ctx->workspace[0] ? ctx->workspace : "workspace";
  n = snprintf(out, out_len, "%s/%s", workspace, relative);
  return n >= 0 && (size_t)n < out_len ? 0 : -1;
}

int rt_agent_prepare_invocation(RtContext *ctx, const char *floop_name, const RtStep *step,
                               const char *executor,
                               const RtActionRegistry *actions,
                               const RtAgentMeta *agent_meta,
                               const char *bound_task_id,
                               long long bound_work_rev,
                               RtAgentInvocation *out) {
  char out_dir[PATH_MAX];
  char input_err[RT_LARGE] = "";
  int seq;
  if (!ctx || !step || !out) return -1;
  memset(out, 0, sizeof(*out));
  snprintf(out->agent_id, sizeof(out->agent_id), "%s", step->target);
  snprintf(out->executor, sizeof(out->executor), "%s", executor && *executor ? executor : "script");
  if (rt_floop_agent_dir(floop_name ? floop_name : rt_default_loop(),
                         step->target, out->agent_dir, sizeof(out->agent_dir)) != 0) {
    (void)rt_emit_error(ctx, "kernel", "agent %s directory not found", step->target);
    return -1;
  }
  snprintf(out->script_path, sizeof(out->script_path), "%s/run.sh", out->agent_dir);
  snprintf(out_dir, sizeof(out_dir), "%s/agent_outputs", ctx->run_dir);
  if (fs_mkdir_p(out_dir) != 0) return -1;
  seq = rt_agent_artifact_seq(ctx->run_dir, out->executor);
  snprintf(out->in_path,  sizeof(out->in_path),  "%s/%03d_%s.input.json", out_dir, seq, step->id);
  snprintf(out->out_path, sizeof(out->out_path), "%s/%03d_%s.json",       out_dir, seq, step->id);
  snprintf(out->err_path, sizeof(out->err_path), "%s/%03d_%s.stderr",     out_dir, seq, step->id);
  if (bound_task_id) snprintf(out->bound_task_id, sizeof(out->bound_task_id), "%s", bound_task_id);
  out->bound_work_rev = bound_work_rev;
  if (strcmp(out->executor, "llm") == 0 && agent_meta &&
      agent_meta->listen_event &&
      strcmp(ctx->event_kind, "user_message") == 0 &&
      ctx->media_ref_json &&
      resolve_media_manifest_path(ctx, out->media_manifest_path,
                                  sizeof(out->media_manifest_path)) != 0) {
    (void)rt_emit_error(
        ctx, "kernel",
        "agent %s received an invalid ingress media manifest reference",
        step->target);
    return -1;
  }
  if (build_agent_input(ctx, out->agent_dir, out->executor, actions, agent_meta,
                        out->bound_task_id, out->bound_work_rev,
                        out->input, sizeof(out->input),
                        input_err, sizeof(input_err)) != 0) {
    (void)rt_emit_error(ctx, "kernel", "%s",
                        input_err[0] ? input_err : "agent input render failed");
    return -1;
  }
  return fs_write_text(out->in_path, out->input);
}

int rt_agent_append_output(RtContext *ctx, const RtStep *step,
                           const RtAgentMeta *agent_meta,
                           const char *bound_task_id,
                           long long bound_work_rev,
                           const char *output_json,
                           char *committed_out, size_t committed_len) {
  char intent[RT_XL];
  if (rt_agent_normalize_output(ctx, step, agent_meta, bound_task_id,
                                bound_work_rev,
                                output_json, intent, sizeof(intent)) != 0)
    return -1;
  return rt_append_events_from_output(ctx, step->target,
                                      agent_meta && agent_meta->can_write_memory,
                                      intent,
                                      committed_out, committed_len);
}

int rt_agent_append_output_file(RtContext *ctx, const RtStep *step,
                                const char *executor, const RtAgentMeta *agent_meta,
                                const char *bound_task_id,
                                long long bound_work_rev,
                                const char *path,
                                char *normalized_out, size_t normalized_len) {
  char *output = NULL;
  char intent[RT_XL];
  char local[RT_XL];
  char *committed = normalized_out && normalized_len ? normalized_out : local;
  size_t committed_size = normalized_out && normalized_len ? normalized_len : sizeof(local);
  int rc;
  if (!ctx || !step || !path || fs_read_text(path, &output, FS_READ_TEXT_DEFAULT_CAP) != 0 || !output) {
    if (ctx && step) (void)rt_emit_error(ctx, step->target, "agent %s output unreadable", step->target);
    return -1;
  }
  rc = rt_agent_normalize_output(ctx, step, agent_meta, bound_task_id,
                                 bound_work_rev,
                                 output, intent, sizeof(intent));
  if (rc == 0)
    rc = rt_append_events_from_output(ctx, step->target,
                                      agent_meta && agent_meta->can_write_memory,
                                      intent,
                                      committed, committed_size);
  if (rc == 0 && executor && strcmp(executor, "llm") == 0)
    (void)rt_agent_write_llm_response_artifact(ctx, step, path, output, committed);
  free(output);
  return rc;
}
