#ifndef FCLAW_LLM_PROVIDER_LIMIT_H
#define FCLAW_LLM_PROVIDER_LIMIT_H

#include "llm.h"

#include <stddef.h>
#include <time.h>

typedef enum {
  LLM_PROVIDER_LIMIT_OK = 0,
  LLM_PROVIDER_LIMIT_BLOCKED_RPM = 1,
  LLM_PROVIDER_LIMIT_BLOCKED_DAILY = 2
} LlmProviderLimitResult;

int llm_provider_limit_reserve(const LlmProvider *provider, const char *logs_root,
                               LlmUsage *usage_out,
                               char *raw_response_out, size_t raw_response_out_len);

int llm_provider_limit_reserve_at(const LlmProvider *provider, const char *logs_root,
                                  time_t now, LlmUsage *usage_out,
                                  char *raw_response_out, size_t raw_response_out_len);

#endif
