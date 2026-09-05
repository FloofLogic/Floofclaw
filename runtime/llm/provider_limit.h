#ifndef FCLAW_LLM_PROVIDER_LIMIT_H
#define FCLAW_LLM_PROVIDER_LIMIT_H

#include "llm.h"

#include <stddef.h>
#include <time.h>

typedef enum {
  LLM_PROVIDER_LIMIT_OK = 0,
  LLM_PROVIDER_LIMIT_BLOCKED_RPM = 1,
  LLM_PROVIDER_LIMIT_BLOCKED_DAILY = 2,
  LLM_PROVIDER_LIMIT_BLOCKED_USD = 3
} LlmProviderLimitResult;

int llm_provider_limit_reserve(const LlmProvider *provider, const char *logs_root,
                               LlmUsage *usage_out,
                               char *raw_response_out, size_t raw_response_out_len);

int llm_provider_limit_reserve_at(const LlmProvider *provider, const char *logs_root,
                                  time_t now, LlmUsage *usage_out,
                                  char *raw_response_out, size_t raw_response_out_len);

/* Reserve count limits plus the configured daily USD ceiling before any
 * outside-world dispatch. Text input is conservatively bounded at one token
 * per byte plus serializer overhead; media is rejected while a cost cap is
 * active because provider-specific media billing is not token-complete. */
int llm_provider_limit_reserve_request(
    const LlmProvider *provider, const LlmProfile *profile,
    const LlmRequest *request, const char *logs_root, LlmUsage *usage_out,
    char *raw_response_out, size_t raw_response_out_len);

int llm_provider_limit_reserve_request_at(
    const LlmProvider *provider, const LlmProfile *profile,
    const LlmRequest *request, const char *logs_root, time_t now,
    LlmUsage *usage_out, char *raw_response_out,
    size_t raw_response_out_len);

/* Close out the dollar reservation a request made, once its outcome is
 * known. Reported usage replaces the reservation with the priced actual
 * cost; a request that provably never reached the provider (see
 * LlmUsage.request_dispatched) returns its reservation; anything else
 * keeps it. The outcome and amounts are recorded in usage. Returns 0 when
 * the ledger reflects the decision, -1 when it could not be updated (the
 * reservation then stands). */
int llm_provider_limit_settle_request(const LlmProvider *provider,
                                      const LlmProfile *profile,
                                      const char *logs_root,
                                      LlmUsage *usage);

int llm_provider_limit_settle_request_at(const LlmProvider *provider,
                                         const LlmProfile *profile,
                                         const char *logs_root, time_t now,
                                         LlmUsage *usage);

#endif
