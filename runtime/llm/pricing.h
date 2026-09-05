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
  /* Optional per-row rate for provider-reported cache-creation tokens
   * (Anthropic's cache writes). A row without one prices those tokens at
   * the plain input rate. */
  double cache_creation_input_per_million;
  double output_per_million;
} LlmPrice;

typedef struct {
  LlmPrice rows[LLM_PRICING_MAX_MODELS];
  size_t count;
  /* Optional top-level pre-dispatch allowance per media item, in input
   * tokens. 0 means the table declines to bound media, so a request that
   * carries media fails closed while a dollar cap is set. */
  long long media_input_tokens_per_item;
} LlmPricingCatalog;

/* Path the catalog loads from: FCLAW_PRICING, else config/pricing.json. */
const char *llm_pricing_path(void);

/* Load the checked-in pricing assumptions. FCLAW_PRICING may point tests or
 * an installation at an explicitly maintained replacement table. */
int llm_pricing_catalog_load(LlmPricingCatalog *out);

/* Exact provider/model lookup. Returns 0 when priced, 1 when unlisted, and
 * -1 for invalid arguments. */
int llm_pricing_lookup(const LlmPricingCatalog *catalog,
                       const char *provider, const char *model,
                       LlmPrice *out);

/* Estimate billed USD from provider-reported tokens. input_tokens is the
 * whole prompt; cached and cache-creation tokens are the subsets of it that
 * carry their own rates, and each is applied only when the provider actually
 * reported it. */
int llm_pricing_usage_usd(const LlmPricingCatalog *catalog,
                          const char *provider, const char *model,
                          long long input_tokens, long long output_tokens,
                          long long cached_input_tokens,
                          int cached_input_reported,
                          long long cache_creation_input_tokens,
                          int cache_creation_input_reported,
                          double *usd_out);

/* The same estimate in micro-USD, rounded upward, for the local ledger. */
int llm_pricing_usage_micros(const LlmPricingCatalog *catalog,
                             const char *provider, const char *model,
                             long long input_tokens, long long output_tokens,
                             long long cached_input_tokens,
                             int cached_input_reported,
                             long long cache_creation_input_tokens,
                             int cache_creation_input_reported,
                             long long *micros_out);

/* Conservative pre-dispatch reservation in millionths of a dollar. Since
 * rates are USD per million tokens, token_count * rate is already micro-USD.
 * The result rounds upward so the local cap never rounds a request down. */
int llm_pricing_max_cost_micros(const LlmPricingCatalog *catalog,
                                const char *provider, const char *model,
                                long long input_token_ceiling,
                                long long output_token_ceiling,
                                long long *micros_out);

#endif
