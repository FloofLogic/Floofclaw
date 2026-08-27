#ifndef FCLAW_LLM_PRICING_H
#define FCLAW_LLM_PRICING_H

#include "llm.h"

#include <stddef.h>

#define LLM_PRICING_MAX_MODELS 64

typedef struct {
  char provider[LLM_ID_MAX];
  char model[LLM_NAME_MAX];
  double input_per_million;
  double cached_input_per_million;
  double output_per_million;
} LlmPrice;

typedef struct {
  LlmPrice rows[LLM_PRICING_MAX_MODELS];
  size_t count;
} LlmPricingCatalog;

/* Load the checked-in pricing assumptions. FCLAW_PRICING may point tests or
 * an installation at an explicitly maintained replacement table. */
int llm_pricing_catalog_load(LlmPricingCatalog *out);

/* Exact provider/model lookup. Returns 0 when priced, 1 when unlisted, and
 * -1 for invalid arguments. */
int llm_pricing_lookup(const LlmPricingCatalog *catalog,
                       const char *provider, const char *model,
                       LlmPrice *out);

/* Estimate billed USD from provider-reported tokens. Cached input is charged
 * at its separate rate only when the provider actually reported it. */
int llm_pricing_usage_usd(const LlmPricingCatalog *catalog,
                          const char *provider, const char *model,
                          long long input_tokens, long long output_tokens,
                          long long cached_input_tokens,
                          int cached_input_reported, double *usd_out);

/* Conservative pre-dispatch reservation in millionths of a dollar. Since
 * rates are USD per million tokens, token_count * rate is already micro-USD.
 * The result rounds upward so the local cap never rounds a request down. */
int llm_pricing_max_cost_micros(const LlmPricingCatalog *catalog,
                                const char *provider, const char *model,
                                long long input_token_ceiling,
                                long long output_token_ceiling,
                                long long *micros_out);

#endif
