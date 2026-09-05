#include "llm.h"

#include "../support/fsutil.h"
#include "../support/json.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

const char *llm_budget_outcome_name(LlmBudgetOutcome outcome) {
  switch (outcome) {
    case LLM_BUDGET_KEPT: return "kept";
    case LLM_BUDGET_SETTLED: return "settled";
    case LLM_BUDGET_RELEASED: return "released";
    case LLM_BUDGET_NONE: break;
  }
  return "none";
}

void llm_extract_usage(const char *provider_kind, const char *response_json,
                       LlmUsage *usage_out) {
  JsonRef root, usage, details;
  long long prompt_tokens = 0;
  long long completion_tokens = 0;
  long long total_tokens = 0;
  long long input_tokens = 0;
  long long output_tokens = 0;
  long long thought_tokens = 0;
  long long cache_creation_tokens = 0;
  if (!provider_kind || !usage_out || !response_json) return;
  if (json_ref_first_object(response_json, &root) != 0) return;
  if (strcmp(provider_kind, "gemini_key") == 0) {
    if (json_ref_object_get_object(&root, "usageMetadata", &usage) != 0) return;
    (void)json_ref_object_get_long(&usage, "promptTokenCount", &input_tokens);
    (void)json_ref_object_get_long(&usage, "candidatesTokenCount", &output_tokens);
    (void)json_ref_object_get_long(&usage, "totalTokenCount", &total_tokens);
    /* Gemini bills thinking as output but reports it beside candidates. */
    if (json_ref_object_get_long(&usage, "thoughtsTokenCount",
                                 &thought_tokens) == 0 && thought_tokens > 0)
      output_tokens += thought_tokens;
    usage_out->input_tokens = input_tokens;
    usage_out->output_tokens = output_tokens;
    usage_out->total_tokens = total_tokens ? total_tokens : input_tokens + output_tokens;
    if (json_ref_object_get_long(&usage, "cachedContentTokenCount",
                                 &usage_out->provider_cached_input_tokens) == 0)
      usage_out->provider_cached_input_tokens_reported = 1;
    return;
  }
  if (json_ref_object_get_object(&root, "usage", &usage) != 0) return;
  if (strcmp(provider_kind, "openai_chat") == 0 ||
      strcmp(provider_kind, "openai_compat") == 0) {
    (void)json_ref_object_get_long(&usage, "prompt_tokens", &prompt_tokens);
    (void)json_ref_object_get_long(&usage, "completion_tokens", &completion_tokens);
    (void)json_ref_object_get_long(&usage, "total_tokens", &total_tokens);
    usage_out->input_tokens = prompt_tokens;
    usage_out->output_tokens = completion_tokens;
    usage_out->total_tokens = total_tokens ? total_tokens : prompt_tokens + completion_tokens;
    if (json_ref_object_get_object(&usage, "prompt_tokens_details", &details) == 0 &&
        json_ref_object_get_long(&details, "cached_tokens",
                                 &usage_out->provider_cached_input_tokens) == 0)
      usage_out->provider_cached_input_tokens_reported = 1;
  } else if (strcmp(provider_kind, "openai_responses") == 0) {
    (void)json_ref_object_get_long(&usage, "input_tokens", &input_tokens);
    (void)json_ref_object_get_long(&usage, "output_tokens", &output_tokens);
    usage_out->input_tokens = input_tokens;
    usage_out->output_tokens = output_tokens;
    usage_out->total_tokens = input_tokens + output_tokens;
    if (json_ref_object_get_object(&usage, "input_tokens_details", &details) == 0 &&
        json_ref_object_get_long(&details, "cached_tokens",
                                 &usage_out->provider_cached_input_tokens) == 0)
      usage_out->provider_cached_input_tokens_reported = 1;
  } else {
    /* Anthropic's input_tokens is the uncached remainder only; the whole
     * prompt is that plus both cache counters. Fold them in so the ledger's
     * input_tokens means the same thing on every provider. */
    (void)json_ref_object_get_long(&usage, "input_tokens", &input_tokens);
    (void)json_ref_object_get_long(&usage, "output_tokens", &output_tokens);
    if (json_ref_object_get_long(&usage, "cache_read_input_tokens",
                                 &usage_out->provider_cached_input_tokens) == 0) {
      usage_out->provider_cached_input_tokens_reported = 1;
      if (usage_out->provider_cached_input_tokens > 0)
        input_tokens += usage_out->provider_cached_input_tokens;
    }
    if (json_ref_object_get_long(&usage, "cache_creation_input_tokens",
                                 &cache_creation_tokens) == 0) {
      usage_out->provider_cache_creation_input_tokens = cache_creation_tokens;
      usage_out->provider_cache_creation_input_tokens_reported = 1;
      if (cache_creation_tokens > 0) input_tokens += cache_creation_tokens;
    }
    usage_out->input_tokens = input_tokens;
    usage_out->output_tokens = output_tokens;
    usage_out->total_tokens = input_tokens + output_tokens;
  }
}

int llm_usage_record_parse(const char *json, LlmUsageRecord *out) {
  JsonRef root, cached, created;
  if (!json || !out) return -1;
  memset(out, 0, sizeof(*out));
  if (json_ref_top_object(json, &root) != 0 ||
      json_ref_object_get_string(&root, "ts", out->ts,
                                 sizeof(out->ts)) != 0 ||
      json_ref_object_get_string(&root, "floop", out->floop,
                                 sizeof(out->floop)) != 0 ||
      json_ref_object_get_string(&root, "run_id", out->run_id,
                                 sizeof(out->run_id)) != 0 ||
      json_ref_object_get_long(&root, "call_seq", &out->call_seq) != 0 ||
      json_ref_object_get_string(&root, "agent", out->agent_id,
                                 sizeof(out->agent_id)) != 0 ||
      json_ref_object_get_string(&root, "profile", out->profile_id,
                                 sizeof(out->profile_id)) != 0 ||
      json_ref_object_get_string(&root, "provider", out->provider_id,
                                 sizeof(out->provider_id)) != 0 ||
      json_ref_object_get_string(&root, "model", out->model,
                                 sizeof(out->model)) != 0 ||
      json_ref_object_get_long(&root, "input_chars", &out->input_chars) != 0 ||
      json_ref_object_get_long(&root, "input_media_count",
                               &out->input_media_count) != 0 ||
      json_ref_object_get_long(&root, "input_media_bytes",
                               &out->input_media_bytes) != 0 ||
      json_ref_object_get_long(&root, "output_chars", &out->output_chars) != 0 ||
      json_ref_object_get_long(&root, "input_tokens", &out->input_tokens) != 0 ||
      json_ref_object_get_long(&root, "output_tokens", &out->output_tokens) != 0 ||
      json_ref_object_get_long(&root, "total_tokens", &out->total_tokens) != 0 ||
      json_ref_object_get(&root, "provider_cached_input_tokens", &cached) != 0 ||
      json_ref_object_get_long(&root, "duration_ms", &out->duration_ms) != 0)
    return -1;
  if (!out->ts[0] || !out->floop[0] || !out->run_id[0] ||
      !out->agent_id[0] || !out->profile_id[0] || !out->provider_id[0] ||
      !out->model[0] || out->call_seq < 1 || out->input_chars < 0 ||
      out->input_media_count < 0 ||
      out->input_media_count > LLM_MEDIA_MAX_COUNT ||
      out->input_media_bytes < 0 || out->output_chars < 0 ||
      out->input_tokens < 0 || out->output_tokens < 0 ||
      out->total_tokens < 0 || out->duration_ms < 0)
    return -1;
  if (!json_ref_is_null(&cached)) {
    if (json_ref_get_long(&cached, &out->provider_cached_input_tokens) != 0 ||
        out->provider_cached_input_tokens < 0)
      return -1;
    out->provider_cached_input_tokens_reported = 1;
  }
  /* Added in 0.31.0; records written before it simply never reported one. */
  if (json_ref_object_get(&root, "provider_cache_creation_input_tokens",
                          &created) == 0 &&
      !json_ref_is_null(&created)) {
    if (json_ref_get_long(&created,
                          &out->provider_cache_creation_input_tokens) != 0 ||
        out->provider_cache_creation_input_tokens < 0)
      return -1;
    out->provider_cache_creation_input_tokens_reported = 1;
  }
  return 0;
}

void llm_record_usage(const char *floop, const char *run_id, int call_seq,
                      const char *agent_id, const LlmProfile *profile,
                      const LlmUsage *usage) {
  char ts[64];
  char line[1024];
  char cached_json[64];
  char created_json[64];
  time_t now;
  struct tm tmv;
  if (!floop || !*floop || !run_id || !*run_id || call_seq < 1 ||
      !agent_id || !*agent_id || !profile || !profile->id[0] ||
      !profile->provider[0] || !profile->model[0] || !usage)
    return;
  now = time(NULL);
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  if (usage->provider_cached_input_tokens_reported)
    snprintf(cached_json, sizeof(cached_json), "%lld",
             usage->provider_cached_input_tokens);
  else
    snprintf(cached_json, sizeof(cached_json), "null");
  if (usage->provider_cache_creation_input_tokens_reported)
    snprintf(created_json, sizeof(created_json), "%lld",
             usage->provider_cache_creation_input_tokens);
  else
    snprintf(created_json, sizeof(created_json), "null");
  snprintf(line, sizeof(line),
           "{\"ts\":\"%s\",\"floop\":\"%s\",\"run_id\":\"%s\",\"call_seq\":%d,"
           "\"agent\":\"%s\",\"profile\":\"%s\",\"provider\":\"%s\","
           "\"model\":\"%s\",\"input_chars\":%lld,"
           "\"input_media_count\":%zu,\"input_media_bytes\":%llu,"
           "\"output_chars\":%lld,"
           "\"input_tokens\":%lld,\"output_tokens\":%lld,\"total_tokens\":%lld,"
           "\"provider_cached_input_tokens\":%s,"
           "\"provider_cache_creation_input_tokens\":%s,"
           "\"duration_ms\":%lld}\n",
           ts, floop, run_id, call_seq,
           agent_id, profile->id, profile->provider, profile->model,
           usage->input_chars, usage->input_media_count,
           (unsigned long long)usage->input_media_bytes,
           usage->output_chars,
           usage->input_tokens, usage->output_tokens, usage->total_tokens,
           cached_json, created_json,
           usage->duration_ms);
  (void)fs_mkdir_p("workspace/logs");
  (void)fs_append_text("workspace/logs/llm_usage.jsonl", line);
}
