#include "runtime.h"
#include "publication_outbox.h"
#include "support/heap_guard.h"
#include "support/timing.h"

#include <stdio.h>
#include <string.h>

static int value_compact(const JsonRef *v, char *out, size_t out_len) {
  char *compact = NULL;
  int rc;
  if (!v) return -1;
  compact = json_ref_value_compact_dup(v);
  if (!compact) return -1;
  rc = snprintf(out, out_len, "%s", compact) >= (int)out_len ? -1 : 0;
  fc_xfree(compact);
  return rc;
}

static int forbidden_artifact_key(const char *key) {
  return strcmp(key, "produced_by") == 0 || strcmp(key, "task_id") == 0 ||
         strcmp(key, "request_id") == 0 || strcmp(key, "run_id") == 0 ||
         strcmp(key, "source") == 0 || strcmp(key, "created_by") == 0;
}

static int part_keys_ok(const JsonRef *part, const char *kind) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef value;
  while (json_ref_object_iter(part, &cursor, key, sizeof(key), &value) == 1) {
    if (forbidden_artifact_key(key)) return 0;
    if (strcmp(key, "kind") == 0) continue;
    if (strcmp(kind, "file") == 0 &&
        (strcmp(key, "path") == 0 || strcmp(key, "mime") == 0 || strcmp(key, "bytes") == 0)) continue;
    if (strcmp(kind, "text") == 0 &&
        (strcmp(key, "text") == 0 || strcmp(key, "truncated") == 0)) continue;
    if (strcmp(kind, "data") == 0 && strcmp(key, "data") == 0) continue;
    if (strcmp(kind, "error") == 0 &&
        (strcmp(key, "tool") == 0 || strcmp(key, "reason") == 0 ||
         strcmp(key, "message") == 0)) continue;
    return 0;
  }
  return 1;
}

static int append_part(const JsonRef *part, char *out, size_t out_len, size_t *pos) {
  char kind[RT_SMALL], esc[RT_LARGE * 2], piece[RT_LARGE * 2];
  JsonRef data, bytes;
  int n;
  if (json_ref_object_get_string(part, "kind", kind, sizeof(kind)) != 0 ||
      !part_keys_ok(part, kind)) return -1;
  if (strcmp(kind, "file") == 0) {
    char path[RT_LARGE], mime[RT_SMALL] = "";
    if (json_ref_object_get_string(part, "path", path, sizeof(path)) != 0 ||
        json_escape(path, esc, sizeof(esc)) != 0) return -1;
    (void)json_ref_object_get_string(part, "mime", mime, sizeof(mime));
    n = snprintf(piece, sizeof(piece), "%s{\"kind\":\"file\",\"path\":\"%s\"",
                 *pos && out[*pos - 1] != '[' ? "," : "", esc);
    if (n < 0 || (size_t)n >= sizeof(piece) ||
        rt_append_text(out, out_len, pos, piece) != 0) return -1;
    if (mime[0]) {
      char emime[RT_MED];
      if (json_escape(mime, emime, sizeof(emime)) != 0) return -1;
      snprintf(piece, sizeof(piece), ",\"mime\":\"%s\"", emime);
      if (rt_append_text(out, out_len, pos, piece) != 0) return -1;
    }
    if (json_ref_object_get(part, "bytes", &bytes) == 0) {
      char raw[RT_SMALL];
      if (bytes.type != JSON_REF_NUMBER || json_ref_value_copy(&bytes, raw, sizeof(raw)) != 0) return -1;
      snprintf(piece, sizeof(piece), ",\"bytes\":%s", raw);
      if (rt_append_text(out, out_len, pos, piece) != 0) return -1;
    }
    return rt_append_text(out, out_len, pos, "}");
  }
  if (strcmp(kind, "text") == 0) {
    char *text = NULL;
    char *etext = NULL;
    int truncated = 0;
    size_t etext_cap, piece_cap;
    if (json_ref_object_get(part, "text", &data) != 0 ||
        (text = json_ref_string_dup(&data)) == NULL)
      return -1;
    etext_cap = strlen(text) * 6U + 1U;
    if (json_ref_object_get_bool(part, "truncated", &truncated) != 0) truncated = 0;
    etext = (char *)fc_xmalloc(etext_cap);
    piece_cap = strlen(text) * 6U + 96U;
    if (!etext || piece_cap > sizeof(piece) || json_escape(text, etext, etext_cap) != 0) {
      fc_xfree(etext);
      fc_xfree(text);
      return -1;
    }
    snprintf(piece, sizeof(piece), "%s{\"kind\":\"text\",\"text\":\"%s\"%s}",
             *pos && out[*pos - 1] != '[' ? "," : "", etext,
             truncated ? ",\"truncated\":true" : "");
    fc_xfree(etext);
    fc_xfree(text);
    return rt_append_text(out, out_len, pos, piece);
  }
  if (strcmp(kind, "data") == 0) {
    char raw[RT_LARGE];
    if (json_ref_object_get(part, "data", &data) != 0 ||
        (data.type != JSON_REF_OBJECT && data.type != JSON_REF_ARRAY) ||
        value_compact(&data, raw, sizeof(raw)) != 0) return -1;
    snprintf(piece, sizeof(piece), "%s{\"kind\":\"data\",\"data\":%s}",
             *pos && out[*pos - 1] != '[' ? "," : "", raw);
    return rt_append_text(out, out_len, pos, piece);
  }
  if (strcmp(kind, "error") == 0) {
    char tool[RT_SMALL] = "", reason[RT_SMALL] = "", msg[RT_LARGE];
    char etool[RT_MED], ereason[RT_MED], emsg[RT_LARGE * 2];
    if (json_ref_object_get_string(part, "message", msg, sizeof(msg)) != 0 ||
        json_escape(msg, emsg, sizeof(emsg)) != 0) return -1;
    (void)json_ref_object_get_string(part, "tool", tool, sizeof(tool));
    (void)json_ref_object_get_string(part, "reason", reason, sizeof(reason));
    if (json_escape(tool, etool, sizeof(etool)) != 0 ||
        json_escape(reason, ereason, sizeof(ereason)) != 0) return -1;
    n = snprintf(piece, sizeof(piece), "%s{\"kind\":\"error\"%s%s%s%s%s%s,\"message\":\"%s\"}",
                 *pos && out[*pos - 1] != '[' ? "," : "",
                 tool[0] ? ",\"tool\":\"" : "", tool[0] ? etool : "", tool[0] ? "\"" : "",
                 reason[0] ? ",\"reason\":\"" : "", reason[0] ? ereason : "", reason[0] ? "\"" : "",
                 emsg);
    if (n < 0 || (size_t)n >= sizeof(piece)) return -1;
    return rt_append_text(out, out_len, pos, piece);
  }
  return -1;
}

int rt_task_normalize_artifacts(const JsonRef *payload, const char *run_id,
                                int event_seq, long long work_rev,
                                char *out, size_t out_len) {
  JsonRef artifacts, artifact, parts, part;
  size_t pos = 0;
  if (json_ref_object_get_array(payload, "artifacts", &artifacts) != 0) {
    JsonRef state_obj;
    if (json_ref_object_get_object(payload, "state", &state_obj) != 0 ||
        json_ref_object_get_array(&state_obj, "artifacts", &artifacts) != 0) {
      snprintf(out, out_len, "[]");
      return 0;
    }
  }
  if (rt_append_text(out, out_len, &pos, "[") != 0) return -1;
  for (size_t i = 0; i < json_ref_array_size(&artifacts); ++i) {
    size_t cursor = 0, ppos = 0;
    char key[RT_SMALL], artifact_buf[RT_LARGE], prefix[RT_MED];
    JsonRef value;
    if (json_ref_array_get(&artifacts, i, &artifact) != 0 || artifact.type != JSON_REF_OBJECT ||
        json_ref_object_get_array(&artifact, "parts", &parts) != 0) return -1;
    while (json_ref_object_iter(&artifact, &cursor, key, sizeof(key), &value) == 1)
      if ((strcmp(key, "parts") != 0 &&
           strcmp(key, "artifact_id") != 0 &&
           strcmp(key, "work_rev") != 0) ||
          forbidden_artifact_key(key))
        return -1;
    artifact_buf[0] = '\0';
    if (rt_append_text(artifact_buf, sizeof(artifact_buf), &ppos, "[") != 0) return -1;
    for (size_t j = 0; j < json_ref_array_size(&parts); ++j) {
      if (json_ref_array_get(&parts, j, &part) != 0 || part.type != JSON_REF_OBJECT ||
          append_part(&part, artifact_buf, sizeof(artifact_buf), &ppos) != 0) return -1;
    }
    if (rt_append_text(artifact_buf, sizeof(artifact_buf), &ppos, "]") != 0) return -1;
    snprintf(prefix, sizeof(prefix),
             "%s{\"artifact_id\":\"art_%s_%06d_%03zu\","
             "\"work_rev\":%lld,\"parts\":",
             i ? "," : "", run_id && *run_id ? run_id : "validate",
             event_seq, i + 1, work_rev > 0 ? work_rev : 0);
    if (rt_append_text(out, out_len, &pos, prefix) != 0 ||
        rt_append_text(out, out_len, &pos, artifact_buf) != 0 ||
        rt_append_text(out, out_len, &pos, "}") != 0) return -1;
  }
  return rt_append_text(out, out_len, &pos, "]");
}

int rt_task_append_action_failure(RtContext *ctx, const char *action,
                                  const char *task_id, long long work_rev,
                                  const char *reason,
                                  const char *error_json) {
  char etask[RT_MED], etool[RT_MED], ereason[RT_MED], emsg[RT_LARGE * 2];
  char payload[RT_XL], artifacts[RT_LARGE];
  char *message = NULL;
  JsonRef error, value, root;
  int n;
  int rc = -1;
  if (!ctx || !task_id || !*task_id || !action) return 1;
  if (error_json && json_ref_first_object(error_json, &error) == 0 &&
      json_ref_object_get(&error, "message", &value) == 0)
    message = json_ref_string_dup(&value);
  if (json_escape(task_id, etask, sizeof(etask)) != 0 ||
      json_escape(action, etool, sizeof(etool)) != 0 ||
      json_escape(reason ? reason : "", ereason, sizeof(ereason)) != 0 ||
      json_escape(message && *message ? message : "action failed", emsg,
                  sizeof(emsg)) != 0)
    goto cleanup;
  n = snprintf(payload, sizeof(payload),
               "{\"task_id\":\"%s\",\"artifacts\":[{\"parts\":[{"
               "\"kind\":\"error\"%s%s%s%s%s%s,\"message\":\"%s\"}]}]}",
               etask,
               action[0] ? ",\"tool\":\"" : "", action[0] ? etool : "",
               action[0] ? "\"" : "",
               reason && *reason ? ",\"reason\":\"" : "",
               reason && *reason ? ereason : "",
               reason && *reason ? "\"" : "", emsg);
  if (n < 0 || (size_t)n >= sizeof(payload) ||
      json_ref_from_text(payload, &root) != 0 || root.type != JSON_REF_OBJECT ||
      rt_task_normalize_artifacts(&root, ctx->run_id, ctx->next_event,
                                  work_rev, artifacts,
                                  sizeof(artifacts)) != 0)
    goto cleanup;
  rc = rt_append_event_format(
      ctx, "task_updated", "action_runner", payload, sizeof(payload), 0,
      "{\"task_id\":\"%s\",\"state\":{\"updated_ms\":%llu,"
      "\"artifacts\":%s}}",
      etask, (unsigned long long)timing_wall_ms(), artifacts);
cleanup:
  fc_xfree(message);
  return rc;
}

static int block_correlated_work(const RtContext *ctx,
                                 const RtOperationSnapshot *op,
                                 const char *reason,
                                 const char *needed,
                                 int include_operation_ref) {
  RtContext *mutable_ctx = (RtContext *)ctx;
  char affair_id[RT_SMALL] = "";
  char etask[RT_MED], ectx[RT_LARGE], eaffair[RT_MED];
  char echannel[RT_MED], eadapter[RT_MED], eorigin[RT_MED];
  char ehandle[RT_MED], eaction[RT_MED];
  char erid[RT_MED], etrigger[RT_MED], esource[RT_MED];
  char *large = NULL, *task_json, *ereason, *eneeded, *payload;
  char source_event_id[RT_SMALL], wake[4096];
  char *evidence = NULL;
  char operation_ref[RT_LARGE] = "null";
  JsonRef task, state;
  long long updated_ms = 0;
  int rc = -1;
  if (!ctx || !op || !op->task_id[0] || !op->context_id[0] ||
      op->work_rev <= 0 || !reason || !*reason || !needed || !*needed)
    return -1;
  large = (char *)fc_xcalloc(4U, RT_XL);
  if (!large) return -1;
  task_json = large;
  ereason = large + RT_XL;
  eneeded = large + RT_XL * 2U;
  payload = large + RT_XL * 3U;
  if (strcmp(ctx->context_id, op->context_id) != 0 ||
      rt_task_work_binding_json(op->context_id, op->task_id, op->work_rev, 0,
                                task_json, RT_XL) != 0) {
    rc = 1;
    goto cleanup;
  }
  if (json_ref_from_text(task_json, &task) == 0 &&
      json_ref_object_get_object(&task, "state", &state) == 0) {
    (void)json_ref_object_get_string(&state, "affair_id",
                                     affair_id, sizeof(affair_id));
    (void)json_ref_object_get_long(&state, "updated_ms", &updated_ms);
  }
  updated_ms++;
  if (json_escape(op->task_id, etask, sizeof(etask)) != 0 ||
      json_escape(op->context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(affair_id, eaffair, sizeof(eaffair)) != 0 ||
      json_escape(op->channel, echannel, sizeof(echannel)) != 0 ||
      json_escape(op->adapter_id, eadapter, sizeof(eadapter)) != 0 ||
      json_escape(op->origin_event_id, eorigin, sizeof(eorigin)) != 0 ||
      json_escape(op->handle, ehandle, sizeof(ehandle)) != 0 ||
      json_escape(op->action, eaction, sizeof(eaction)) != 0 ||
      json_escape(op->request_id, erid, sizeof(erid)) != 0 ||
      json_escape(op->trigger_event_id, etrigger, sizeof(etrigger)) != 0 ||
      rt_next_event_id(ctx, source_event_id, sizeof(source_event_id)) != 0 ||
      json_escape(source_event_id, esource, sizeof(esource)) != 0 ||
      json_escape(reason, ereason, RT_XL) != 0 ||
      json_escape(needed, eneeded, RT_XL) != 0)
    goto cleanup;
  evidence = (char *)fc_xmalloc(RT_XL);
  if (!evidence ||
      rt_work_state_evidence_refs_json(op->task_id, op->work_rev,
                                       evidence, RT_XL) != 0) {
    goto cleanup;
  }
  if (include_operation_ref && (op->handle[0] || op->action[0])) {
    int n = snprintf(operation_ref, sizeof(operation_ref),
                     "{\"handle\":\"%s\",\"action\":\"%s\"}",
                     ehandle, eaction);
    if (n < 0 || (size_t)n >= sizeof(operation_ref)) goto cleanup;
  }
  if (snprintf(
          payload, RT_XL,
          "{\"task_id\":\"%s\",\"work_rev\":%lld,\"context_id\":\"%s\","
          "\"request_id\":\"%s\",\"action\":\"%s\","
          "\"trigger_event_id\":\"%s\",\"source_event_id\":\"%s\","
          "\"status\":\"blocked\","
          "\"affair_id\":%s%s%s,\"channel\":\"%s\",\"adapter_id\":\"%s\","
          "\"origin_event_id\":\"%s\",\"ref\":%s,\"summary\":\"%s\","
          "\"blocker\":\"%s\",\"needed\":\"%s\",\"operation_ref\":%s,"
          "\"evidence_refs\":%s,\"state\":{\"status\":\"blocked\","
          "\"updated_ms\":%lld}}",
          etask, op->work_rev, ectx, erid, eaction, etrigger, esource,
          affair_id[0] ? "\"" : "", affair_id[0] ? eaffair : "null",
          affair_id[0] ? "\"" : "",
          echannel, eadapter, eorigin,
          op->ref_json[0] ? op->ref_json : "null",
          ereason, ereason, eneeded, operation_ref, evidence,
          updated_ms) >= RT_XL)
    goto cleanup;
  {
    char bounded[385], bounded_needed[385];
    char ebounded[RT_LARGE], ebounded_needed[RT_LARGE];
    const char *ref = op->ref_json[0] && strlen(op->ref_json) <= 512
                          ? op->ref_json : "null";
    int n;
    snprintf(bounded, sizeof(bounded), "%.384s", reason);
    snprintf(bounded_needed, sizeof(bounded_needed), "%.384s", needed);
    if (json_escape(bounded, ebounded, sizeof(ebounded)) != 0 ||
        json_escape(bounded_needed, ebounded_needed,
                    sizeof(ebounded_needed)) != 0)
      goto cleanup;
    n = snprintf(
        wake, sizeof(wake),
        "{\"task_id\":\"%s\",\"work_rev\":%lld,\"request_id\":\"%s\","
        "\"context_id\":\"%s\","
        "\"action\":\"%s\",\"trigger_event_id\":\"%s\","
        "\"source_event_id\":\"%s\",\"status\":\"blocked\","
        "\"affair_id\":%s%s%s,\"summary\":\"%s\",\"text\":\"%s\","
        "\"blocker\":\"%s\",\"needed\":\"%s\","
        "\"origin_event_id\":\"%s\",\"adapter_id\":\"%s\",\"ref\":%s}",
        etask, op->work_rev, erid, ectx, eaction, etrigger, esource,
        affair_id[0] ? "\"" : "", affair_id[0] ? eaffair : "null",
        affair_id[0] ? "\"" : "",
        ebounded, ebounded, ebounded, ebounded_needed,
        eorigin, eadapter, ref);
    if (n < 0 || (size_t)n >= sizeof(wake) ||
        (op->channel[0] && op->trigger_event_id[0] &&
         rt_publication_outbox_prepare(
            ctx, source_event_id, "work_blocked", payload,
             op->channel, "work_outcome", wake, NULL, 0) != 0)) {
      goto cleanup;
    }
  }
  if (rt_append_event_sync(mutable_ctx, "work_blocked", "kernel",
                           payload) != 0)
    goto cleanup;
  if (op->channel[0] && op->trigger_event_id[0])
    (void)rt_publication_outbox_reconcile();
  (void)rt_narrate("work blocked: %s — %.160s", op->task_id, reason);
  rc = 0;
cleanup:
  fc_xfree(evidence);
  fc_xfree(large);
  return rc;
}

int rt_task_block_unpollable_operation(const RtContext *ctx,
                                       const RtOperationSnapshot *op,
                                       const char *reason,
                                       const char *needed) {
  return block_correlated_work(ctx, op, reason, needed, 1);
}

int rt_task_block_bound_work(const RtContext *ctx,
                             const char *task_id, long long work_rev,
                             const char *request_id, const char *action,
                             const char *trigger_event_id,
                             const char *reason, const char *needed) {
  RtOperationSnapshot identity;
  if (!ctx || !task_id || !*task_id || work_rev <= 0 ||
      !request_id || !*request_id || !action || !*action ||
      !trigger_event_id || !*trigger_event_id)
    return -1;
  memset(&identity, 0, sizeof(identity));
  snprintf(identity.task_id, sizeof(identity.task_id), "%s", task_id);
  snprintf(identity.context_id, sizeof(identity.context_id), "%s",
           ctx->context_id);
  snprintf(identity.request_id, sizeof(identity.request_id), "%s",
           request_id);
  snprintf(identity.action, sizeof(identity.action), "%s", action);
  snprintf(identity.trigger_event_id, sizeof(identity.trigger_event_id),
           "%s", trigger_event_id);
  snprintf(identity.channel, sizeof(identity.channel), "%s",
           ctx->origin_channel);
  snprintf(identity.adapter_id, sizeof(identity.adapter_id), "%s",
           ctx->origin_adapter_id);
  snprintf(identity.origin_event_id, sizeof(identity.origin_event_id), "%s",
           ctx->origin_event_id);
  snprintf(identity.ref_json, sizeof(identity.ref_json), "%s",
           ctx->origin_ref_json);
  identity.work_rev = work_rev;
  return block_correlated_work(ctx, &identity, reason, needed, 0);
}
