#ifndef FCLAW_LLM_H
#define FCLAW_LLM_H

#include <stddef.h>
#include <stdint.h>

#include "../fjson_repair/repair.h"

#define LLM_ID_MAX     64
#define LLM_NAME_MAX   128
#define LLM_PATH_MAX   1024
#define LLM_RESP_MAX   65536
#define LLM_PROMPT_MAX 65536

#define LLM_ERR_MODEL_OUTPUT_TOO_LARGE  -2

#define LLM_MEDIA_ID_MAX       128
#define LLM_MEDIA_FILENAME_MAX 256
#define LLM_MEDIA_TYPE_MAX     128
#define LLM_MEDIA_MAX_COUNT    10U
/* Media preprocessing and provider retries share one 120s agent job. Keep
 * their independent caps explicit so the child cannot start work whose worst
 * case is guaranteed to be killed by its owner. */
#define LLM_AGENT_JOB_BUDGET_MS      120000
#define LLM_MEDIA_DOWNLOAD_BUDGET_MS 30000U
#define LLM_HTTP_MEDIA_RETRY_CAP     1

/* Provider-neutral media input. The descriptor array is allocated only for a
 * media-bearing invocation; bytes stay in verified run-local regular files
 * and are serialized by the provider boundary. */
typedef struct {
  char id[LLM_MEDIA_ID_MAX];
  char filename[LLM_MEDIA_FILENAME_MAX];
  char media_type[LLM_MEDIA_TYPE_MAX];
  char path[LLM_PATH_MAX];
  uint64_t size_bytes;
} LlmMedia;

typedef struct {
  const char *text;
  const LlmMedia *media;
  size_t media_count;
} LlmRequest;

typedef struct {
  char id[LLM_ID_MAX];
  char provider[LLM_ID_MAX];
  char model[LLM_NAME_MAX];
  char effort[LLM_ID_MAX];   /* optional; currently mapped by Gemini to
                              * thinkingConfig.thinkingLevel. Other current
                              * transports load but do not send it. */
} LlmProfile;

typedef struct {
  char id[LLM_ID_MAX];
  char kind[LLM_ID_MAX];           /* "mock" | "http" | "openai_compat" | "openai_responses" | "gemini_key" */
  char url[LLM_PATH_MAX];          /* http/openai_compat/openai_responses/gemini_key only */
  char auth_endpoint[LLM_ID_MAX];  /* optional http/openai_compat/openai_responses/gemini_key auth route */
  char auth_env[LLM_ID_MAX];       /* optional env-var fallback for auth */
  int rpm_limit;                   /* <= 0 blocks immediately, < 0 means unlimited */
  int daily_limit;                 /* <= 0 blocks immediately, < 0 means unlimited */
} LlmProvider;

typedef struct {
  long long input_chars;
  size_t input_media_count;
  uint64_t input_media_bytes;
  long long output_chars;
  long long duration_ms;
  long long input_tokens;
  long long output_tokens;
  long long total_tokens;
  long long provider_cached_input_tokens;
  int provider_cached_input_tokens_reported;
  int json_repair_checked;
  FjrStatus json_repair_status;
  FjrReport json_repair_report;
  size_t json_repair_raw_len;
  size_t json_repair_repaired_len;
  char local_limit_reason[LLM_ID_MAX];
} LlmUsage;

typedef struct {
  char ts[64];
  char floop[LLM_NAME_MAX];
  char run_id[LLM_ID_MAX];
  char agent_id[LLM_ID_MAX];
  char profile_id[LLM_ID_MAX];
  char provider_id[LLM_ID_MAX];
  char model[LLM_NAME_MAX];
  long long call_seq;
  long long input_chars;
  long long input_media_count;
  long long input_media_bytes;
  long long output_chars;
  long long input_tokens;
  long long output_tokens;
  long long total_tokens;
  long long provider_cached_input_tokens;
  int provider_cached_input_tokens_reported;
  long long duration_ms;
} LlmUsageRecord;

/* Load profile or provider definitions from disk. Default config paths are
 * config/model_profiles.json and config/llm_registry.json; both are
 * overridable via the FCLAW_MODEL_PROFILES / FCLAW_LLM_REGISTRY env vars. */
int llm_load_profile(const char *profile_id, LlmProfile *out);
int llm_load_provider(const char *provider_id, LlmProvider *out);

/* Resolve a profile through its provider and authentication preconditions.
 * Used by startup so the runtime does not duplicate LLM configuration rules. */
int llm_validate_profile_startup(const char *profile_id,
                                 char *err, size_t err_len);

/* Mock provider: returns the contents of LLM_MOCK_RESPONSE_PATH (a file) or
 * LLM_MOCK_RESPONSE (literal JSON) as the response text. */
int llm_mock_call(const char *prompt, char *response_out, size_t response_out_len);

/* HTTP providers. Blocking
 * libcurl easy mode — only ever runs inside an agent-job
 * subprocess, never inline in the gateway reactor. Returns 0 on success
 * with response_out filled with one JSON object; -1 on failure. */
int llm_http_call(const LlmProfile *profile, const LlmProvider *provider,
                  const char *request_body, size_t request_body_len,
                  /* -1 uses the configured cap; nonnegative values lower it. */
                  int retry_cap,
                  char *response_out, size_t response_out_len,
                  char *raw_response_out, size_t raw_response_out_len,
                  LlmUsage *usage_out);

/* Parse provider usage metadata into the normalized per-call counters. Raw
 * responses remain the wire evidence saved beside the normalized metadata. */
void llm_extract_usage(const char *provider_kind, const char *response_json,
                       LlmUsage *usage_out);

/* Parse the current usage-ledger contract. Structurally invalid records are
 * rejected without attempting to infer missing values. */
int llm_usage_record_parse(const char *json, LlmUsageRecord *out);

void llm_record_usage(const char *floop, const char *run_id, int call_seq,
                      const char *agent_id, const LlmProfile *profile,
                      const LlmUsage *usage);

/* Normalize raw model text into one compact JSON object. Agent-specific output
 * contracts are enforced later before anything reaches truth. */
int llm_normalize_response_text(const char *model_text,
                                char *out, size_t out_len,
                                LlmUsage *usage_out);

/* Orchestrate one call:
 *   - dispatch via provider
 *   - save outbound (request) and inbound (response) artifacts under run_dir/provider_calls/
 *   - append a usage record to workspace/logs/llm_usage.jsonl
 * Returns 0 and fills response_out on success. */
int llm_call(const char *run_dir, const char *floop, const char *agent_id,
             const LlmProfile *profile, const char *prompt,
             char *response_out, size_t response_out_len);

/* Media-aware form used by the runtime child entrypoint. */
int llm_call_request(const char *run_dir, const char *floop,
                     const char *agent_id, const LlmProfile *profile,
                     const LlmRequest *request, char *response_out,
                     size_t response_out_len);


/* Retries actually used per provider call, after clamping any
 * FCLAW_LLM_MAX_RETRIES override so that
 * (1 + retries) attempts + backoff still fits inside the agent job
 * budget. An override that would not fit is reduced, never honored:
 * a retry the caller will SIGTERM mid-flight is worse than no retry. */
int llm_http_effective_max_retries(void);

#endif
