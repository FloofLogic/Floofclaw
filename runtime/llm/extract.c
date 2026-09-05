#include "extract.h"

#include "llm.h"
#include "../support/json.h"

#include <string.h>

/* Anthropic Messages: the first {"type":"text"} block in content[]. A
 * thinking, redacted_thinking, or tool_use block ahead of it is skipped;
 * a body with no text block at all is a refusal or a tool turn and fails
 * loudly rather than handing back an empty string. */
int llm_extract_anthropic_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, content_arr;
  size_t n;
  if (!response_json || !out || out_len == 0) return -1;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "content", &content_arr) != 0) return -1;
  n = json_ref_array_size(&content_arr);
  for (size_t i = 0; i < n; ++i) {
    JsonRef block;
    char type[LLM_ID_MAX] = "";
    if (json_ref_array_get(&content_arr, i, &block) != 0) continue;
    if (block.type != JSON_REF_OBJECT) continue;
    (void)json_ref_object_get_string(&block, "type", type, sizeof(type));
    if (strcmp(type, "text") != 0) continue;
    if (json_ref_object_get_string(&block, "text", out, out_len) == 0) return 0;
    return -1;
  }
  return -1;
}

/* OpenAI-compatible Chat: choices[0].message.content. */
int llm_extract_openai_compat_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, choices_arr, first, message;
  if (!response_json || !out || out_len == 0) return -1;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "choices", &choices_arr) != 0) return -1;
  if (json_ref_array_get(&choices_arr, 0, &first) != 0) return -1;
  if (json_ref_object_get_object(&first, "message", &message) != 0) return -1;
  return json_ref_object_get_string(&message, "content", out, out_len);
}

/* OpenAI Responses: the first output[] item of type "message", its first
 * content entry that carries text. Reasoning items precede it. */
int llm_extract_openai_responses_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, output_arr;
  size_t n;
  if (!response_json || !out || out_len == 0) return -1;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "output", &output_arr) != 0) return -1;
  n = json_ref_array_size(&output_arr);
  for (size_t i = 0; i < n; ++i) {
    JsonRef item, content_arr;
    char type[LLM_ID_MAX] = "";
    if (json_ref_array_get(&output_arr, i, &item) != 0) continue;
    (void)json_ref_object_get_string(&item, "type", type, sizeof(type));
    if (strcmp(type, "message") != 0) continue;
    if (json_ref_object_get_array(&item, "content", &content_arr) != 0) continue;
    if (json_ref_array_size(&content_arr) > 0) {
      JsonRef content;
      if (json_ref_array_get(&content_arr, 0, &content) == 0 &&
          json_ref_object_get_string(&content, "text", out, out_len) == 0) {
        return 0;
      }
    }
  }
  return -1;
}

/* Gemini: candidates[0].content.parts[], first part that is not a thought.
 * A thinking model returns {"thought":true,...} parts ahead of the answer
 * when thoughts are included; the streaming decoder feeds one SSE event at
 * a time through here, so an event carrying only a thought part yields -1
 * and is treated as metadata. */
int llm_extract_gemini_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, candidates_arr, candidate, content, parts;
  size_t n;
  if (!response_json || !out || out_len == 0) return -1;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "candidates", &candidates_arr) != 0) return -1;
  if (json_ref_array_get(&candidates_arr, 0, &candidate) != 0) return -1;
  if (json_ref_object_get_object(&candidate, "content", &content) != 0) return -1;
  if (json_ref_object_get_array(&content, "parts", &parts) != 0) return -1;
  n = json_ref_array_size(&parts);
  for (size_t i = 0; i < n; ++i) {
    JsonRef part;
    int thought = 0;
    if (json_ref_array_get(&parts, i, &part) != 0) continue;
    if (part.type != JSON_REF_OBJECT) continue;
    if (json_ref_object_get_bool(&part, "thought", &thought) == 0 && thought) continue;
    if (json_ref_object_get_string(&part, "text", out, out_len) == 0) return 0;
  }
  return -1;
}

void llm_extract_stop_reason(const char *provider_kind, const char *response_json,
                             char *out, size_t out_len) {
  JsonRef root, arr, first;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!provider_kind || !response_json) return;
  if (json_ref_first_object(response_json, &root) != 0) return;
  if (strcmp(provider_kind, "openai_chat") == 0 ||
      strcmp(provider_kind, "openai_compat") == 0) {
    if (json_ref_object_get_array(&root, "choices", &arr) == 0 &&
        json_ref_array_get(&arr, 0, &first) == 0)
      (void)json_ref_object_get_string(&first, "finish_reason", out, out_len);
  } else if (strcmp(provider_kind, "openai_responses") == 0) {
    (void)json_ref_object_get_string(&root, "status", out, out_len);
  } else if (strcmp(provider_kind, "gemini_key") == 0) {
    if (json_ref_object_get_array(&root, "candidates", &arr) == 0 &&
        json_ref_array_get(&arr, 0, &first) == 0)
      (void)json_ref_object_get_string(&first, "finishReason", out, out_len);
  } else {
    (void)json_ref_object_get_string(&root, "stop_reason", out, out_len);
  }
}
