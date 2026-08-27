#include "pricing.h"

#include "../support/fsutil.h"
#include "../support/json.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *pricing_path(void) {
  const char *path = getenv("FCLAW_PRICING");
  return (path && *path) ? path : "config/pricing.json";
}

static int valid_rate(double value) {
  return value >= 0.0 && value <= 1000000.0;
}

int llm_pricing_catalog_load(LlmPricingCatalog *out) {
  JsonRef root, models;
  char *text = NULL;
  size_t count;
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (fs_read_text(pricing_path(), &text, FS_READ_TEXT_DEFAULT_CAP) != 0)
    return -1;
  if (json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_array(&root, "models", &models) != 0) {
    free(text);
    return -1;
  }
  count = json_ref_array_size(&models);
  if (count > LLM_PRICING_MAX_MODELS) {
    free(text);
    return -1;
  }
  for (size_t i = 0; i < count; ++i) {
    JsonRef row;
    LlmPrice *price = &out->rows[i];
    if (json_ref_array_get(&models, i, &row) != 0 ||
        json_ref_object_get_string(&row, "provider", price->provider,
                                   sizeof(price->provider)) != 0 ||
        json_ref_object_get_string(&row, "model", price->model,
                                   sizeof(price->model)) != 0 ||
        json_ref_object_get_double(&row, "input",
                                   &price->input_per_million) != 0 ||
        json_ref_object_get_double(&row, "cached_input",
                                   &price->cached_input_per_million) != 0 ||
        json_ref_object_get_double(&row, "output",
                                   &price->output_per_million) != 0 ||
        !price->provider[0] || !price->model[0] ||
        !valid_rate(price->input_per_million) ||
        !valid_rate(price->cached_input_per_million) ||
        !valid_rate(price->output_per_million)) {
      free(text);
      memset(out, 0, sizeof(*out));
      return -1;
    }
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(out->rows[j].provider, price->provider) == 0 &&
          strcmp(out->rows[j].model, price->model) == 0) {
        free(text);
        memset(out, 0, sizeof(*out));
        return -1;
      }
    }
  }
  out->count = count;
  free(text);
  return 0;
}

int llm_pricing_lookup(const LlmPricingCatalog *catalog,
                       const char *provider, const char *model,
                       LlmPrice *out) {
  if (!catalog || !provider || !*provider || !model || !*model || !out)
    return -1;
  for (size_t i = 0; i < catalog->count; ++i) {
    if (strcmp(catalog->rows[i].provider, provider) == 0 &&
        strcmp(catalog->rows[i].model, model) == 0) {
      *out = catalog->rows[i];
      return 0;
    }
  }
  return 1;
}

int llm_pricing_usage_usd(const LlmPricingCatalog *catalog,
                          const char *provider, const char *model,
                          long long input_tokens, long long output_tokens,
                          long long cached_input_tokens,
                          int cached_input_reported, double *usd_out) {
  LlmPrice price;
  long long cached = 0;
  long long uncached;
  int found;
  if (!usd_out || input_tokens < 0 || output_tokens < 0 ||
      cached_input_tokens < 0)
    return -1;
  *usd_out = 0.0;
  found = llm_pricing_lookup(catalog, provider, model, &price);
  if (found != 0) return found;
  if (cached_input_reported) {
    cached = cached_input_tokens > input_tokens
                 ? input_tokens
                 : cached_input_tokens;
  }
  uncached = input_tokens - cached;
  *usd_out = ((double)uncached * price.input_per_million +
              (double)cached * price.cached_input_per_million +
              (double)output_tokens * price.output_per_million) /
             1000000.0;
  return 0;
}

int llm_pricing_max_cost_micros(const LlmPricingCatalog *catalog,
                                const char *provider, const char *model,
                                long long input_token_ceiling,
                                long long output_token_ceiling,
                                long long *micros_out) {
  LlmPrice price;
  double micros;
  long long rounded;
  int found;
  if (!micros_out || input_token_ceiling < 0 || output_token_ceiling < 0)
    return -1;
  *micros_out = 0;
  found = llm_pricing_lookup(catalog, provider, model, &price);
  if (found != 0) return found;
  micros = (double)input_token_ceiling * price.input_per_million +
           (double)output_token_ceiling * price.output_per_million;
  if (micros < 0.0 || micros > (double)LLONG_MAX) return -1;
  rounded = (long long)micros;
  if ((double)rounded < micros) rounded++;
  *micros_out = rounded;
  return 0;
}
