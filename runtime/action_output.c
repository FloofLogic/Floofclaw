#include "runtime.h"

#include <stdio.h>
#include <string.h>

/* Wrap an action's raw result into the managed-operation contract:
 * inject "status":"finished" and use the durable action request_id as its
 * handle, preserving all existing fields as peers. Idempotent: skipped if the
 * result already carries a status field.
 *
 * Called by:
 *   - rt_action_runtime_complete (intrinsics)
 *   - rt_action_runner_complete_job (subprocess actions)
 * ...when the action's def declares contract:"managed_operation".
 *
 * The point: every managed-op action, whether an intrinsic or a subprocess
 * script, emits an operation_result-compatible envelope. The operation
 * driver then publishes it uniformly. See the unified-architecture doc. */
void rt_action_wrap_finished_result(char *result, size_t result_len,
                                    const char *request_id) {
  char wrapped[RT_XL];
  size_t rl = result ? strlen(result) : 0;
  char handle[RT_SMALL];
  int has_inner;
  int n;
  if (!result || result_len == 0 || rl < 2 ||
      !request_id || !request_id[0]) return;
  if (result[0] != '{' || result[rl - 1] != '}') return;
  /* Respect explicit status if the action already speaks the contract
   * (web_read, manage_codex, and similar managed operations). */
  if (strstr(result, "\"status\"")) return;
  if (json_escape(request_id, handle, sizeof(handle)) != 0) return;
  has_inner = (rl > 2);
  n = snprintf(wrapped, sizeof(wrapped),
               "{\"status\":\"finished\",\"handle\":\"%s\"%s%.*s",
               handle, has_inner ? "," : "",
               (int)(rl - 1), result + 1);
  if (n < 0 || (size_t)n >= sizeof(wrapped)) return;
  snprintf(result, result_len, "%s", wrapped);
}

static void artifact_ref_obj(const char *artifact_path, size_t output_bytes,
                             const char *error_code, char *out, size_t out_len) {
  char esc_path[PATH_MAX * 2];
  char esc_error[RT_SMALL * 2];
  if (!out || out_len == 0) return;
  if (json_escape(artifact_path ? artifact_path : "", esc_path, sizeof(esc_path)) != 0)
    esc_path[0] = '\0';
  if (json_escape(error_code ? error_code : "action_output_error",
                  esc_error, sizeof(esc_error)) != 0)
    esc_error[0] = '\0';
  snprintf(out, out_len,
           "{\"truncated\":true,\"artifact\":\"%s\",\"output_bytes\":%zu,\"error\":\"%s\"}",
           esc_path, output_bytes, esc_error);
}

static void artifact_ref_error(const char *artifact_path, size_t output_bytes,
                               const char *message, char *out, size_t out_len) {
  char esc_path[PATH_MAX * 2];
  char esc_msg[RT_MED * 2];
  if (!out || out_len == 0) return;
  if (json_escape(artifact_path ? artifact_path : "", esc_path, sizeof(esc_path)) != 0)
    esc_path[0] = '\0';
  if (json_escape(message ? message : "action output error", esc_msg, sizeof(esc_msg)) != 0)
    esc_msg[0] = '\0';
  snprintf(out, out_len,
           "{\"message\":\"%s\",\"artifact\":\"%s\",\"output_bytes\":%zu}",
           esc_msg, esc_path, output_bytes);
}

static int copy_value_or_large(const JsonRef *value, char *out, size_t out_len) {
  size_t span;
  if (!value || !out || out_len == 0 || !value->start || !value->end ||
      value->end < value->start)
    return -1;
  span = (size_t)(value->end - value->start);
  if (span + 1U > out_len) return 1;
  return json_ref_value_copy(value, out, out_len) == 0 ? 0 : -1;
}

/* Salvage the managed-operation contract fields (status, handle, text)
 * from a raw output that's larger than the caller's result buffer. If
 * all three exist and the minimal JSON built from them fits in
 * `result_out`, populate it and return 0. On any failure return -1 so
 * the caller falls through to the current loud-failure path.
 *
 * The point is: an author who returned {status, handle, text,
 * giant_debug_dump} shouldn't get a silent-stall. The contract fields
 * are tiny; salvage them so the operation driver can complete the
 * turn. The debug bloat is still lost — a downside the author sees in
 * the artifact on disk. */
static int try_salvage_managed_op(const char *output_json, const JsonRef *root,
                                  char *result_out, size_t result_len) {
  JsonRef result_ref;
  char status[RT_SMALL] = "";
  char handle[RT_SMALL] = "";
  char text[RT_MED * 4] = "";
  char etext[RT_MED * 8];
  int n;
  (void)output_json;
  if (json_ref_object_get_object(root, "result", &result_ref) != 0) return -1;
  (void)json_ref_object_get_string(&result_ref, "status", status, sizeof(status));
  (void)json_ref_object_get_string(&result_ref, "handle", handle, sizeof(handle));
  (void)json_ref_object_get_string(&result_ref, "text", text, sizeof(text));
  if (!status[0] || !handle[0]) return -1;
  if (json_escape(text, etext, sizeof(etext)) != 0) etext[0] = '\0';
  n = snprintf(result_out, result_len,
               "{\"status\":\"%s\",\"handle\":\"%s\",\"text\":\"%s\","
               "\"_salvaged\":true}",
               status, handle, etext);
  return (n < 0 || (size_t)n >= result_len) ? -1 : 0;
}

int rt_action_output_extract(const char *output_json, const char *artifact_path,
                            char *result_out, size_t result_len,
                            char *error_out, size_t error_len,
                            int is_managed_op) {
  JsonRef root, value;
  size_t output_bytes = output_json ? strlen(output_json) : 0U;
  int rc;

  if (result_out && result_len) snprintf(result_out, result_len, "{}");
  if (error_out && error_len) snprintf(error_out, error_len, "null");
  if (!output_json || !result_out || result_len == 0 || !error_out || error_len == 0)
    return -1;

  if (json_ref_first_object(output_json, &root) != 0) {
    artifact_ref_obj(artifact_path, output_bytes, "invalid_action_output",
                     result_out, result_len);
    artifact_ref_error(artifact_path, output_bytes, "invalid action output",
                       error_out, error_len);
    return -1;
  }

  if (json_ref_object_get(&root, "result", &value) == 0) {
    if (value.type == JSON_REF_OBJECT) {
      rc = copy_value_or_large(&value, result_out, result_len);
      if (rc == 1) {
        /* Result JSON exceeds the caller's buffer (RT_LARGE = 4096).
         *
         * For managed-op actions, first try to salvage the contract
         * fields (status, handle, text) — the operation driver only
         * needs those to complete the turn, and losing them to a
         * silent tombstone stalled the conversation. If salvage
         * succeeds we return 0 (action_succeeded with a minimal but
         * contract-valid result); the debug bloat is still lost on
         * disk but the LLM finishes its two-turn flow. See
         * docs/concepts/actions.md.
         *
         * For non-managed-op actions, or when salvage can't produce
         * the contract fields, fall through to the loud failure: the
         * author needs to know their result was refused so they can
         * shrink it or move big payloads to disk (see web_fetch
         * pattern in docs/concepts/actions.md). */
        char esc_path[PATH_MAX * 2];
        if (is_managed_op &&
            try_salvage_managed_op(output_json, &root, result_out, result_len) == 0) {
          (void)rt_narrate("action result salvaged (managed-op contract fields): "
                           "%zu bytes exceeds %zu — full output at %s",
                           output_bytes, result_len,
                           artifact_path ? artifact_path : "?");
          /* Fall through to error handling below — we still want to
           * parse the "error" field if present, treat as success. */
          goto after_result;
        }
        if (json_escape(artifact_path ? artifact_path : "", esc_path,
                        sizeof(esc_path)) != 0)
          esc_path[0] = '\0';
        snprintf(error_out, error_len,
                 "{\"message\":\"result_too_large: %zu bytes exceeds max %zu — "
                 "action must return result JSON <= %zu bytes; "
                 "write large payloads to disk and return a pointer "
                 "(see docs/concepts/actions.md)\","
                 "\"artifact\":\"%s\",\"output_bytes\":%zu}",
                 output_bytes, result_len, result_len, esc_path, output_bytes);
        snprintf(result_out, result_len, "{}");
        (void)rt_narrate("action result refused: %zu bytes exceeds %zu "
                         "(artifact %s)",
                         output_bytes, result_len,
                         artifact_path ? artifact_path : "?");
        return -1;
      } else if (rc != 0) {
        artifact_ref_obj(artifact_path, output_bytes, "result_invalid",
                         result_out, result_len);
      }
    } else if (!json_ref_is_null(&value)) {
      artifact_ref_obj(artifact_path, output_bytes, "result_not_object",
                       result_out, result_len);
    }
  }

after_result:
  if (json_ref_object_get(&root, "error", &value) == 0) {
    rc = copy_value_or_large(&value, error_out, error_len);
    if (rc == 1)
      artifact_ref_error(artifact_path, output_bytes, "action error too large",
                         error_out, error_len);
    else if (rc != 0)
      artifact_ref_error(artifact_path, output_bytes, "invalid action error",
                         error_out, error_len);
  }

  return 0;
}
