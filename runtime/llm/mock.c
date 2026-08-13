/* LLM mock provider — child-side. Knows nothing about test
 * sequencing; it reads exactly one fixture per call.
 *
 * The test harness sets LLM_MOCK_RESPONSE_PATHS (plural,
 * colon-separated) on the *parent* process. Before each LLM child
 * launch, the parent picks the next token and passes it down as
 * singular LLM_MOCK_RESPONSE_PATH. See agent_runner.c
 * (start_llm_agent_job + next_mock_fixture_path). The child only
 * sees the singular var.
 *
 * Resolution order:
 *   1. LLM_MOCK_RESPONSE_PATH  — read fixture file at this path
 *   2. LLM_MOCK_RESPONSE       — return this literal string
 *   3. default friendly local reply
 *
 * No counter, no static state, no sequence handling. The boundary
 * is correct: sequencing is a property of the caller, the child
 * services exactly one call. */

#include "llm.h"
#include "stream.h"

#include "../support/fsutil.h"
#include "../support/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_response_file(const char *path, char *response_out, size_t response_out_len) {
  char *text = NULL;
  size_t len;
  if (!path || !*path || !response_out || response_out_len == 0) return -1;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  len = strlen(text);
  if (len + 1 > response_out_len) {
    free(text);
    return -1;
  }
  memcpy(response_out, text, len + 1);
  free(text);
  return 0;
}

static int mock_extract_task_id(const char *prompt, char *out, size_t out_len) {
  const char *input;
  JsonRef root, tasks, active, task;
  if (!prompt || !out || out_len == 0) return -1;
  out[0] = '\0';
  input = strstr(prompt, "=== model input ===");
  input = input ? input : prompt;
  if (json_ref_first_object(input, &root) != 0 ||
      json_ref_object_get_object(&root, "tasks", &tasks) != 0 ||
      json_ref_object_get_array(&tasks, "active", &active) != 0)
    return -1;
  for (size_t i = 0; i < json_ref_array_size(&active); ++i) {
    char state[64] = "";
    JsonRef state_obj;
    if (json_ref_array_get(&active, i, &task) != 0 || task.type != JSON_REF_OBJECT)
      continue;
    if (json_ref_object_get_object(&task, "state", &state_obj) == 0)
      (void)json_ref_object_get_string(&state_obj, "status", state, sizeof(state));
    if (!state[0])
      (void)json_ref_object_get_string(&task, "state", state, sizeof(state));
    if (strcmp(state, "open") != 0 && strcmp(state, "working") != 0) continue;
    return json_ref_object_get_string(&task, "task_id", out, out_len);
  }
  return -1;
}

static int mock_replace_task_placeholder(const char *prompt,
                                         char *response_out,
                                         size_t response_out_len) {
  const char *token = "__TASK_ID__";
  const char *hit;
  char task_id[128];
  char tmp[8192];
  size_t token_len = strlen(token), used = 0;
  const char *p = response_out;
  if (!strstr(response_out, token)) return 0;
  if (mock_extract_task_id(prompt, task_id, sizeof(task_id)) != 0) return -1;
  tmp[0] = '\0';
  while ((hit = strstr(p, token)) != NULL) {
    size_t prefix = (size_t)(hit - p);
    size_t id_len = strlen(task_id);
    if (used + prefix + id_len + 1 > sizeof(tmp)) return -1;
    memcpy(tmp + used, p, prefix);
    used += prefix;
    memcpy(tmp + used, task_id, id_len);
    used += id_len;
    p = hit + token_len;
  }
  if (used + strlen(p) + 1 > sizeof(tmp) || used + strlen(p) + 1 > response_out_len)
    return -1;
  memcpy(tmp + used, p, strlen(p) + 1);
  snprintf(response_out, response_out_len, "%s", tmp);
  return 0;
}

static void mock_emit_progress(const char *response) {
  LlmMessageStream *stream;
  const char *chunk_env;
  const char *delay_env;
  const char *disabled;
  long chunk = 7;
  long delay_ms = 0;
  size_t len, pos = 0;
  disabled = getenv("FCLAW_DISABLE_LLM_STREAM");
  if (!response || !*response || !llm_progress_fd_available() ||
      (disabled && *disabled && strcmp(disabled, "0") != 0))
    return;
  chunk_env = getenv("FCLAW_MOCK_STREAM_CHUNK_BYTES");
  delay_env = getenv("FCLAW_MOCK_STREAM_DELAY_MS");
  if (chunk_env && *chunk_env) {
    long v = strtol(chunk_env, NULL, 10);
    if (v >= 1 && v <= 1024) chunk = v;
  }
  if (delay_env && *delay_env) {
    long v = strtol(delay_env, NULL, 10);
    if (v >= 0 && v <= 1000) delay_ms = v;
  }
  stream = llm_message_stream_create(llm_progress_fd_sink, NULL);
  if (!stream) return;
  len = strlen(response);
  while (pos < len) {
    size_t take = len - pos;
    if (take > (size_t)chunk) take = (size_t)chunk;
    if (llm_message_stream_feed(stream, response + pos, take) != 0) break;
    pos += take;
    if (delay_ms > 0)
      usleep((useconds_t)delay_ms * 1000U);
  }
  llm_message_stream_destroy(stream);
}

int llm_mock_call(const char *prompt, char *response_out, size_t response_out_len) {
  const char *path;
  const char *literal;
  (void)prompt;
  if (!response_out || response_out_len == 0) return -1;
  response_out[0] = '\0';
  path = getenv("LLM_MOCK_RESPONSE_PATH");
  if (path && *path) {
    if (read_response_file(path, response_out, response_out_len) != 0) return -1;
    if (mock_replace_task_placeholder(prompt, response_out, response_out_len) != 0)
      return -1;
    mock_emit_progress(response_out);
    return 0;
  }
  literal = getenv("LLM_MOCK_RESPONSE");
  if (literal && *literal) {
    if (snprintf(response_out, response_out_len, "%s", literal) >= (int)response_out_len) return -1;
    if (mock_replace_task_placeholder(prompt, response_out, response_out_len) != 0)
      return -1;
    mock_emit_progress(response_out);
    return 0;
  }
  /* Fresh-clone default: a deterministic reply proves the full local path
   * without credentials. Tests that need another shape supply a fixture. */
  if (snprintf(response_out, response_out_len,
               "{\"message\":\"Hello! FloofClaw is running locally "
               "with the built-in mock model.\"}") >= (int)response_out_len)
    return -1;
  mock_emit_progress(response_out);
  return 0;
}
