#include "agent_exec.h"
#include "support/heap_guard.h"

#include <string.h>

static int runtime_owned_key(const char *key) {
  return key &&
         (strcmp(key, "task_id") == 0 || strcmp(key, "context_id") == 0 ||
          strcmp(key, "kind") == 0 || strcmp(key, "state") == 0 ||
          strcmp(key, "created_event_id") == 0 || strcmp(key, "created_ms") == 0 ||
          strcmp(key, "updated_ms") == 0 || strcmp(key, "event_id") == 0 ||
          strcmp(key, "run_id") == 0 || strcmp(key, "request_id") == 0 ||
          strcmp(key, "job_id") == 0 || strcmp(key, "source") == 0 ||
          strcmp(key, "created_by") == 0 || strcmp(key, "ts") == 0);
}

static int dyn_reserve(char **buf, size_t *cap, size_t need_len) {
  size_t new_cap;
  char *grown;
  if (!buf || !cap) return -1;
  if (*cap > need_len + 1U && *buf) return 0;
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

/* Speaker output normalizer: a dumb router. Reads `message` (required;
 * always wrapped as a `message` action_request bound to the active input
 * task). For every other top-level key K, emits one action_request with
 * action=K and args=V (scalars wrap as {"value": V}). This normalizer has
 * NO knowledge of specific conversational keys. Each
 * action validates its own args against its action.json schema and checks
 * its own preconditions (e.g. fc_note_add requires an active affair). */
int rt_agent_normalize_conversational_output(RtContext *ctx, const RtStep *step,
                                             const char *output_json,
                                             char *out, size_t out_len) {
  JsonRef root, msg_ref, value;
  char task_id[RT_SMALL], request_kind[RT_SMALL], text[RT_LARGE], etask[RT_MED];
  char key[RT_SMALL];
  char *message = NULL, *emsg = NULL, *buf = NULL;
  size_t cap = 0, pos = 0, cursor = 0;
  int rc = -1;

  if (!ctx || !step || !output_json || !out || out_len == 0) return -1;
  if (json_ref_first_object(output_json, &root) != 0 ||
      json_ref_object_get(&root, "message", &msg_ref) != 0) {
    rt_log_rejected_agent_event(step->target, "agent_output_missing_message",
                                "conversational output did not contain message");
    return -1;
  }
  /* {"message": null} — no inbound request to answer. Emit no events. */
  if (msg_ref.type == JSON_REF_NULL)
    return snprintf(out, out_len, "{\"events\":[]}") >= (int)out_len ? -1 : 0;

  message = json_ref_string_dup(&msg_ref);
  if (rt_conversational_request_task(ctx, task_id, sizeof(task_id),
                                     request_kind, sizeof(request_kind),
                                     text, sizeof(text)) != 0) {
    if (message) {
      rt_log_rejected_agent_event(step->target, "conversational_message_without_request",
                                  "speaker emitted a message when request was null");
      rc = snprintf(out, out_len, "{\"events\":[]}") >= (int)out_len ? -1 : 0;
    } else {
      rt_log_rejected_agent_event(step->target, "invalid_conversational_output",
                                  "message must be a string or null");
    }
    goto cleanup;
  }

  /* Message action_request — bound to task_id so the channel routes the
   * reply to the correct conversation. */
  {
    size_t emsg_cap = message ? strlen(message) * 6U + 1U : 0U;
    emsg = emsg_cap ? (char *)fc_xmalloc(emsg_cap) : NULL;
    if (!message || !emsg ||
        json_escape(message, emsg, emsg_cap) != 0 ||
        json_escape(task_id, etask, sizeof(etask)) != 0) {
      rt_log_rejected_agent_event(step->target, "invalid_conversational_output",
                                  "message must be a string for an active input task");
      goto cleanup;
    }
  }
  if (dyn_append_text(&buf, &cap, &pos, "{\"events\":[") != 0) goto cleanup;

  /* Every non-message top-level key → one action_request emitted FIRST.
   * The action name is the key; the args are the value (objects pass
   * through verbatim; scalars wrap as {"value": V} so args is always an
   * object the action's args_schema can validate). Runtime-owned keys
   * (task_id, ts, etc.) are ignored — the speaker shouldn't stamp them,
   * and a hallucinated one shouldn't take down the whole turn.
   *
   * Message is emitted LAST so side-effecting action requests are committed
   * before the reply completes the input task. Order in the events array is
   * the drain-queue order. */
  {
    int first = 1;
    while (json_ref_object_iter(&root, &cursor, key, sizeof(key), &value) == 1) {
      char ekey[RT_MED];
      char *args = NULL;
      int append_rc;
      if (strcmp(key, "message") == 0 || runtime_owned_key(key)) continue;
      if (json_escape(key, ekey, sizeof(ekey)) != 0) goto cleanup;
      if (value.type == JSON_REF_OBJECT) {
        args = json_ref_value_compact_dup(&value);
      } else {
        char *v = json_ref_value_compact_dup(&value);
        if (v) {
          size_t need = strlen(v) + 16U;
          args = (char *)fc_xmalloc(need);
          if (args) snprintf(args, need, "{\"value\":%s}", v);
          fc_xfree(v);
        }
      }
      if (!args) goto cleanup;
      append_rc = 0;
      if (!first) append_rc = dyn_append_text(&buf, &cap, &pos, ",");
      if (append_rc == 0) append_rc = dyn_append_text(&buf, &cap, &pos,
                                                      "{\"type\":\"action_request\",\"payload\":{\"action\":\"");
      if (append_rc == 0) append_rc = dyn_append_text(&buf, &cap, &pos, ekey);
      if (append_rc == 0) append_rc = dyn_append_text(&buf, &cap, &pos, "\",\"args\":");
      if (append_rc == 0) append_rc = dyn_append_text(&buf, &cap, &pos, args);
      if (append_rc == 0) append_rc = dyn_append_text(&buf, &cap, &pos, "}}");
      fc_xfree(args);
      if (append_rc != 0) goto cleanup;
      first = 0;
    }
    if (!first && dyn_append_text(&buf, &cap, &pos, ",") != 0) goto cleanup;
  }

  /* Message action_request — bound to task_id so the channel routes the
   * reply to the correct conversation. Emitted LAST (see above). */
  if (dyn_append_text(&buf, &cap, &pos,
                      "{\"type\":\"action_request\",\"payload\":{"
                      "\"action\":\"message\",\"args\":{\"message\":\"") != 0 ||
      dyn_append_text(&buf, &cap, &pos, emsg) != 0 ||
      dyn_append_text(&buf, &cap, &pos, "\"},\"task_id\":\"") != 0 ||
      dyn_append_text(&buf, &cap, &pos, etask) != 0 ||
      dyn_append_text(&buf, &cap, &pos, "\"}}") != 0) goto cleanup;

  if (dyn_append_text(&buf, &cap, &pos, "]}") != 0) goto cleanup;
  rc = pos >= out_len ? -1 : (snprintf(out, out_len, "%s", buf), 0);

cleanup:
  fc_xfree(buf);
  fc_xfree(emsg);
  fc_xfree(message);
  return rc;
}

int rt_agent_normalize_affair_output(RtContext *ctx, const RtStep *step,
                                     const char *bound_task_id,
                                     const char *output_json,
                                     char *out, size_t out_len) {
  JsonRef root, affair, value;
  char key[RT_SMALL], action[RT_SMALL] = "";
  char *payload = NULL;
  size_t cursor = 0;
  int rc = -1;
  (void)ctx;
  (void)bound_task_id;
  if (!step || !output_json || !out || out_len == 0) return -1;
  if (json_ref_first_object(output_json, &root) != 0 ||
      json_ref_object_get(&root, "affair", &affair) != 0) {
    rt_log_rejected_agent_event(step->target, "agent_output_missing_affair",
                                "affair output did not contain affair");
    return -1;
  }
  while (json_ref_object_iter(&root, &cursor, key, sizeof(key), &value) == 1) {
    if (strcmp(key, "affair") != 0) {
      rt_log_rejected_agent_event(step->target, "invalid_affair_output",
                                  "affair output had extra root key");
      return -1;
    }
  }
  if (affair.type == JSON_REF_NULL)
    return snprintf(out, out_len, "{\"events\":[]}") >= (int)out_len ? -1 : 0;
  if (affair.type != JSON_REF_OBJECT ||
      json_ref_object_get_string(&affair, "action", action, sizeof(action)) != 0 ||
      !action[0]) {
    rt_log_rejected_agent_event(step->target, "invalid_affair_output",
                                "affair must contain an action");
    return -1;
  }
  if (strcmp(action, "none") == 0)
    return snprintf(out, out_len, "{\"events\":[]}") >= (int)out_len ? -1 : 0;
  if (strcmp(action, "create") != 0 && strcmp(action, "link") != 0 &&
      strcmp(action, "update") != 0) {
    rt_log_rejected_agent_event(step->target, "invalid_affair_output",
                                "affair action must be create, link, update, or none");
    return -1;
  }
  payload = json_ref_value_compact_dup(&affair);
  if (!payload) goto cleanup;
  rc = snprintf(out, out_len,
                "{\"events\":[{\"type\":\"affair_patch\",\"payload\":%s}]}",
                payload) >= (int)out_len ? -1 : 0;
cleanup:
  fc_xfree(payload);
  return rc;
}

int rt_agent_normalize_memory_compaction_output(RtContext *ctx, const RtStep *step,
                                                const char *output_json,
                                                char *out, size_t out_len) {
  JsonRef root, value;
  char cutoff[RT_SMALL] = "", expected[RT_SMALL] = "";
  char context_id[RT_MED] = "";
  char *conversation = NULL, *recent = NULL;
  char *econversation = NULL, *erecent = NULL;
  size_t econv_cap = 0, erecent_cap = 0;
  char ectx[RT_MED * 2], ecut[RT_MED];
  int rc = -1;
  (void)step;
  if (!ctx || !output_json || !out || out_len == 0) return -1;
  if (json_ref_first_object(output_json, &root) != 0 ||
      json_ref_object_get(&root, "conversation_summary", &value) != 0 ||
      (conversation = json_ref_string_dup(&value)) == NULL ||
      json_ref_object_get(&root, "recent_summary", &value) != 0 ||
      (recent = json_ref_string_dup(&value)) == NULL ||
      json_ref_object_get_string(&root, "covered_through_message_id", cutoff, sizeof(cutoff)) != 0 ||
      !cutoff[0]) {
    rt_log_rejected_agent_event(step ? step->target : "memory_compactor",
                                "invalid_memory_compaction_output",
                                "compactor output must include summaries and covered_through_message_id");
    rc = snprintf(out, out_len, "{\"events\":[]}") >= (int)out_len ? -1 : 0;
    goto cleanup;
  }
  if (rt_memory_compaction_cutoff(ctx, expected, sizeof(expected)) != 0 ||
      strcmp(cutoff, expected) != 0) {
    rt_log_rejected_agent_event(step ? step->target : "memory_compactor",
                                "memory_compaction_cutoff_mismatch",
                                "compactor covered_through_message_id did not match runtime cutoff");
    rc = snprintf(out, out_len, "{\"events\":[]}") >= (int)out_len ? -1 : 0;
    goto cleanup;
  }
  {
    JsonRef req;
    if (json_ref_first_object(ctx->memory_compaction_json, &req) == 0)
      (void)json_ref_object_get_string(&req, "context_id", context_id, sizeof(context_id));
  }
  if (!context_id[0]) snprintf(context_id, sizeof(context_id), "%s",
                               ctx->context_id[0] ? ctx->context_id : "chat:cli");
  econv_cap = strlen(conversation) * 6U + 1U;
  erecent_cap = strlen(recent) * 6U + 1U;
  econversation = (char *)fc_xmalloc(econv_cap);
  erecent = (char *)fc_xmalloc(erecent_cap);
  if (!econversation || !erecent ||
      json_escape(conversation, econversation, econv_cap) != 0 ||
      json_escape(recent, erecent, erecent_cap) != 0 ||
      json_escape(context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(cutoff, ecut, sizeof(ecut)) != 0)
    goto cleanup;
  rc = snprintf(out, out_len,
                "{\"events\":[{\"type\":\"memory_compacted\",\"payload\":{"
                "\"context_id\":\"%s\",\"conversation_summary\":\"%s\","
                "\"recent_summary\":\"%s\",\"covered_through_message_id\":\"%s\"}}]}",
                ectx, econversation, erecent, ecut) >= (int)out_len ? -1 : 0;
cleanup:
  fc_xfree(erecent);
  fc_xfree(econversation);
  fc_xfree(recent);
  fc_xfree(conversation);
  return rc;
}
