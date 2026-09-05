#include "test_support.h"

#include "../../runtime/llm/llm.h"
#include "../../runtime/llm/pricing.h"
#include "../../runtime/llm/provider_limit.h"
#include "../../runtime/secure/auth.h"
#include "../../runtime/secure/action_secret_broker.h"
#include "../../runtime/secure/secret_store.h"

#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void restore_env_value(const char *name, char *value) {
  if (!name) return;
  if (value) {
    (void)setenv(name, value, 1);
    free(value);
  } else {
    (void)unsetenv(name);
  }
}

static char *capture_env_value(const char *name) {
  const char *value = getenv(name);
  if (!value) return NULL;
  return strdup(value);
}

static time_t local_midday(int year, int month, int day) {
  struct tm tmv;
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = year - 1900;
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = 12;
  tmv.tm_isdst = -1;
  return mktime(&tmv);
}

int provider_registry_parses_openrouter_auth_env_and_limits(void) {
  LlmProvider provider;
  int rc = 0;
  rc |= expect(llm_load_provider("openrouter_chat", &provider) == 0,
               "openrouter_chat provider loads");
  rc |= expect(strcmp(provider.kind, "openai_compat") == 0,
               "openrouter chat uses openai_compat");
  rc |= expect(strcmp(provider.auth_endpoint, "openrouter_key") == 0,
               "openrouter auth endpoint parsed");
  rc |= expect(strcmp(provider.auth_env, "OPENROUTER_API_KEY") == 0,
               "openrouter auth env parsed");
  rc |= expect(provider.rpm_limit == 15, "openrouter rpm limit parsed");
  rc |= expect(provider.daily_limit == 500, "openrouter daily limit parsed");
  rc |= expect(llm_load_provider("gemini_key", &provider) == 0,
               "gemini provider loads");
  rc |= expect(strcmp(provider.kind, "gemini_key") == 0,
               "gemini provider kind parsed");
  rc |= expect(strcmp(provider.auth_env, "GEMINI_API_KEY") == 0,
               "gemini auth env parsed");
  rc |= expect(provider.rpm_limit == 15, "gemini rpm limit parsed");
  rc |= expect(provider.daily_limit == 500, "gemini daily limit parsed");
  rc |= expect(provider.daily_usd_limit < 0.0,
               "providers without a dollar guard remain unlimited");
  rc |= expect(llm_load_provider("openai_key", &provider) == 0,
               "official OpenAI provider loads");
  rc |= expect(strcmp(provider.kind, "openai_chat") == 0,
               "official OpenAI chat has its current wire kind");
  return rc;
}

int provider_pricing_and_daily_usd_budget_are_conservative(void) {
  char *saved_pricing = capture_env_value("FCLAW_PRICING");
  char *saved_registry = capture_env_value("FCLAW_LLM_REGISTRY");
  LlmPricingCatalog catalog;
  LlmProvider provider;
  LlmProfile profile;
  LlmRequest request = {"hello", NULL, 0U};
  LlmUsage usage;
  char raw[512];
  char *state = NULL;
  double usd = 0.0;
  long long micros = 0;
  time_t day1 = local_midday(2026, 5, 19);
  time_t day2 = local_midday(2026, 5, 20);
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(test_write_file(
                   "workspace/pricing.json",
                   "{\"models\":[{\"provider\":\"priced_provider\","
                   "\"model\":\"priced-model\",\"input\":0.1,"
                   "\"cached_input\":0.01,\"output\":0.2}]}\n") == 0,
               "write pricing fixture");
  rc |= expect(test_write_file(
                   "workspace/registry.json",
                   "{\"providers\":[{\"id\":\"priced_provider\","
                   "\"kind\":\"openai_compat\",\"url\":\"http://127.0.0.1:1\","
                   "\"limits\":{\"daily_usd\":0.00015}}]}\n") == 0,
               "write daily USD provider fixture");
  (void)setenv("FCLAW_PRICING", "workspace/pricing.json", 1);
  (void)setenv("FCLAW_LLM_REGISTRY", "workspace/registry.json", 1);

  rc |= expect(llm_load_provider("priced_provider", &provider) == 0,
               "provider with daily USD cap loads");
  rc |= expect(provider.daily_usd_limit == 0.00015,
               "daily USD cap preserves its decimal value");
  rc |= expect(llm_pricing_catalog_load(&catalog) == 0,
               "bounded pricing catalog loads");
  rc |= expect(llm_pricing_usage_usd(&catalog, "priced_provider",
                                     "priced-model", 100, 10, 40, 1, 0, 0,
                                     &usd) == 0,
               "usage pricing finds exact provider and model");
  rc |= expect(usd > 0.00000839 && usd < 0.00000841,
               "cached and uncached tokens receive separate prices");
  rc |= expect(llm_pricing_usage_usd(&catalog, "priced_provider",
                                     "priced-model", 100, 10, 40, 1, 20, 1,
                                     &usd) == 0,
               "usage pricing accepts reported cache-creation tokens");
  rc |= expect(usd > 0.00000839 && usd < 0.00000841,
               "a row without cache_creation_input prices creation at the input rate");
  rc |= expect(llm_pricing_max_cost_micros(&catalog, "priced_provider",
                                           "priced-model", 100, 10,
                                           &micros) == 0 && micros == 12,
               "pre-dispatch price rounds upward in micro-USD");

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.provider, sizeof(profile.provider), "%s", provider.id);
  snprintf(profile.model, sizeof(profile.model), "%s", "priced-model");
  profile.max_output_tokens = 10;
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day1,
                   &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "first conservative request reservation fits the daily cap");
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day1 + 1,
                   &usage, raw, sizeof(raw)) ==
                   LLM_PROVIDER_LIMIT_BLOCKED_USD,
               "second request is blocked before exceeding the daily cap");
  rc |= expect(strcmp(usage.local_limit_reason, "daily_usd") == 0,
               "cost block reason is durable artifact metadata");
  rc |= expect_substr(raw, "\"error\":\"provider_budget_exceeded\"",
                      "cost block is loud and machine-readable");
  rc |= test_read_file(
      "workspace/logs/provider_limits/priced_provider.json", &state);
  rc |= expect_substr(state, "\"day_usd_reserved_micros\":105",
                      "state keeps the crash-safe maximum reservation");
  free(state);
  state = NULL;
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day2,
                   &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "the next local day resets the dollar budget");

  snprintf(profile.model, sizeof(profile.model), "%s", "unpriced-model");
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day2 + 1,
                   &usage, raw, sizeof(raw)) ==
                   LLM_PROVIDER_LIMIT_BLOCKED_USD,
               "an active cost cap fails closed for an unpriced model");
  rc |= expect_substr(raw, "\"reason\":\"daily_usd_unpriced\"",
                      "unpriced-model rejection names the fix boundary");

  (void)setenv("FCLAW_PRICING", "workspace/no-such-pricing.json", 1);
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day2 + 2,
                   &usage, raw, sizeof(raw)) ==
                   LLM_PROVIDER_LIMIT_BLOCKED_USD,
               "a missing pricing table fails closed under a cost cap");
  rc |= expect_substr(raw, "\"reason\":\"daily_usd_pricing_unavailable\"",
                      "a missing table is named apart from an unlisted model");

  restore_env_value("FCLAW_LLM_REGISTRY", saved_registry);
  restore_env_value("FCLAW_PRICING", saved_pricing);
  return rc;
}

int provider_budget_settles_reported_usage_and_releases_undispatched(void) {
  char *saved_pricing = capture_env_value("FCLAW_PRICING");
  LlmProvider provider;
  LlmProfile profile;
  LlmRequest request = {"hello", NULL, 0U};
  LlmUsage usage;
  LlmUsageRecord record;
  char raw[512];
  char *state = NULL;
  time_t day1 = local_midday(2026, 9, 2);
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(test_write_file(
                   "workspace/pricing.json",
                   "{\"models\":[{\"provider\":\"settle_provider\","
                   "\"model\":\"settle-model\",\"input\":0.1,"
                   "\"cached_input\":0.01,\"cache_creation_input\":0.125,"
                   "\"output\":0.2}]}\n") == 0,
               "write settlement pricing fixture");
  (void)setenv("FCLAW_PRICING", "workspace/pricing.json", 1);
  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "settle_provider");
  provider.rpm_limit = -1;
  provider.daily_limit = -1;
  provider.daily_usd_limit = 1.0;
  memset(&profile, 0, sizeof(profile));
  snprintf(profile.provider, sizeof(profile.provider), "%s", provider.id);
  snprintf(profile.model, sizeof(profile.model), "settle-model");
  profile.max_output_tokens = 10;

  /* Reported usage replaces the ceiling with the priced actual cost. */
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day1,
                   &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "reservation under a dollar cap is granted");
  rc |= expect(usage.budget_outcome == LLM_BUDGET_KEPT &&
                   usage.budget_reserved_micros == 105,
               "the reservation records its ceiling as kept");
  usage.request_dispatched = 1;
  usage.input_tokens = 100;
  usage.output_tokens = 10;
  usage.provider_cached_input_tokens = 40;
  usage.provider_cached_input_tokens_reported = 1;
  usage.provider_cache_creation_input_tokens = 20;
  usage.provider_cache_creation_input_tokens_reported = 1;
  rc |= expect(llm_provider_limit_settle_request_at(
                   &provider, &profile, "workspace/logs", day1 + 1,
                   &usage) == 0,
               "settlement with reported usage succeeds");
  rc |= expect(usage.budget_outcome == LLM_BUDGET_SETTLED &&
                   usage.budget_settled_micros == 9,
               "settlement prices uncached, cached, created, and output tokens");
  rc |= test_read_file(
      "workspace/logs/provider_limits/settle_provider.json", &state);
  rc |= expect_substr(state, "\"day_usd_reserved_micros\":9",
                      "the ledger holds the actual cost, not the ceiling");
  free(state);
  state = NULL;

  /* A request that never reached the provider returns its reservation. */
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day1 + 2,
                   &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "second reservation is granted");
  rc |= expect(llm_provider_limit_settle_request_at(
                   &provider, &profile, "workspace/logs", day1 + 3,
                   &usage) == 0,
               "settlement of an undispatched request succeeds");
  rc |= expect(usage.budget_outcome == LLM_BUDGET_RELEASED &&
                   usage.budget_settled_micros == 0,
               "an undispatched request is released");
  rc |= test_read_file(
      "workspace/logs/provider_limits/settle_provider.json", &state);
  rc |= expect_substr(state, "\"day_usd_reserved_micros\":9",
                      "the released reservation leaves the ledger unchanged");
  free(state);
  state = NULL;

  /* An ambiguous failure (dispatched, nothing reported) keeps its ceiling. */
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day1 + 4,
                   &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "third reservation is granted");
  usage.request_dispatched = 1;
  rc |= expect(llm_provider_limit_settle_request_at(
                   &provider, &profile, "workspace/logs", day1 + 5,
                   &usage) == 0,
               "settlement of an ambiguous failure returns cleanly");
  rc |= expect(usage.budget_outcome == LLM_BUDGET_KEPT,
               "an ambiguous failure keeps its reservation");
  rc |= test_read_file(
      "workspace/logs/provider_limits/settle_provider.json", &state);
  rc |= expect_substr(state, "\"day_usd_reserved_micros\":114",
                      "the kept ceiling stays charged to the day");
  free(state);
  state = NULL;

  /* No reservation, nothing to settle. */
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_settle_request_at(
                   &provider, &profile, "workspace/logs", day1 + 6,
                   &usage) == 0 && usage.budget_outcome == LLM_BUDGET_NONE,
               "a call without a reservation is a settlement no-op");

  /* The ledger field the settlement prices is optional on older records. */
  rc |= expect(llm_usage_record_parse(
                   "{\"ts\":\"2026-09-02T00:00:00Z\",\"floop\":\"hello\","
                   "\"run_id\":\"run_001\",\"call_seq\":1,\"agent\":\"chat\","
                   "\"profile\":\"p\",\"provider\":\"settle_provider\","
                   "\"model\":\"settle-model\",\"input_chars\":5,"
                   "\"input_media_count\":0,\"input_media_bytes\":0,"
                   "\"output_chars\":2,\"input_tokens\":100,"
                   "\"output_tokens\":10,\"total_tokens\":110,"
                   "\"provider_cached_input_tokens\":40,"
                   "\"provider_cache_creation_input_tokens\":20,"
                   "\"duration_ms\":1}",
                   &record) == 0 &&
                   record.provider_cache_creation_input_tokens_reported == 1 &&
                   record.provider_cache_creation_input_tokens == 20,
               "a record with cache-creation tokens parses and reports them");
  rc |= expect(llm_usage_record_parse(
                   "{\"ts\":\"2026-08-08T00:00:00Z\",\"floop\":\"hello\","
                   "\"run_id\":\"run_001\",\"call_seq\":1,\"agent\":\"chat\","
                   "\"profile\":\"p\",\"provider\":\"settle_provider\","
                   "\"model\":\"settle-model\",\"input_chars\":5,"
                   "\"input_media_count\":0,\"input_media_bytes\":0,"
                   "\"output_chars\":2,\"input_tokens\":100,"
                   "\"output_tokens\":10,\"total_tokens\":110,"
                   "\"provider_cached_input_tokens\":null,"
                   "\"duration_ms\":1}",
                   &record) == 0 &&
                   record.provider_cache_creation_input_tokens_reported == 0,
               "a pre-0.31.0 record without the field still parses");

  restore_env_value("FCLAW_PRICING", saved_pricing);
  return rc;
}

int provider_limit_windows_never_roll_backward(void) {
  LlmProvider provider;
  LlmUsage usage;
  char raw[256];
  time_t day1 = local_midday(2026, 9, 1);
  time_t day2 = local_midday(2026, 9, 2);
  time_t day3 = local_midday(2026, 9, 3);
  int rc = 0;

  rc |= test_reset_workspace();
  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "unit_backward_day");
  provider.rpm_limit = -1;
  provider.daily_limit = 1;

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day2,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "the first request of the day is allowed");
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day1,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_BLOCKED_DAILY,
               "a clock stepped back a day keeps counting against the open day");
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day3,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "the next real day still reopens the allowance");

  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "unit_backward_minute");
  provider.rpm_limit = 1;
  provider.daily_limit = -1;
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day2,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "the first request of the minute is allowed");
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day2 - 120,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_BLOCKED_RPM,
               "a clock stepped back two minutes does not reopen the rpm window");
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day2 + 60,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "the next real minute reopens the rpm window");
  return rc;
}

int provider_media_reservation_uses_the_table_allowance(void) {
  char *saved_pricing = capture_env_value("FCLAW_PRICING");
  LlmProvider provider;
  LlmProfile profile;
  LlmMedia media[2];
  LlmRequest request = {"hello", media, 2U};
  LlmUsage usage;
  char raw[512];
  char *state = NULL;
  time_t day1 = local_midday(2026, 9, 2);
  int rc = 0;

  rc |= test_reset_workspace();
  memset(media, 0, sizeof(media));
  snprintf(media[0].media_type, sizeof(media[0].media_type), "image/png");
  media[0].size_bytes = 4096;
  snprintf(media[1].media_type, sizeof(media[1].media_type), "image/jpeg");
  media[1].size_bytes = 8192;
  rc |= expect(test_write_file(
                   "workspace/pricing_media.json",
                   "{\"media_input_tokens_per_item\":100,"
                   "\"models\":[{\"provider\":\"media_provider\","
                   "\"model\":\"media-model\",\"input\":0.1,"
                   "\"cached_input\":0.01,\"output\":0.2}]}\n") == 0,
               "write pricing fixture with a media allowance");
  rc |= expect(test_write_file(
                   "workspace/pricing_no_media.json",
                   "{\"models\":[{\"provider\":\"media_provider\","
                   "\"model\":\"media-model\",\"input\":0.1,"
                   "\"cached_input\":0.01,\"output\":0.2}]}\n") == 0,
               "write pricing fixture without a media allowance");
  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "media_provider");
  provider.rpm_limit = -1;
  provider.daily_limit = -1;
  provider.daily_usd_limit = 1.0;
  memset(&profile, 0, sizeof(profile));
  snprintf(profile.provider, sizeof(profile.provider), "%s", provider.id);
  snprintf(profile.model, sizeof(profile.model), "media-model");
  profile.max_output_tokens = 10;

  (void)setenv("FCLAW_PRICING", "workspace/pricing_media.json", 1);
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day1,
                   &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "media under a cost cap is admitted when the table bounds it");
  rc |= expect(usage.budget_reserved_micros == 125,
               "each media item reserves the table's per-item allowance");
  rc |= test_read_file(
      "workspace/logs/provider_limits/media_provider.json", &state);
  rc |= expect_substr(state, "\"day_usd_reserved_micros\":125",
                      "the media allowance is charged to the day ledger");
  free(state);

  (void)setenv("FCLAW_PRICING", "workspace/pricing_no_media.json", 1);
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_request_at(
                   &provider, &profile, &request, "workspace/logs", day1 + 1,
                   &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_BLOCKED_USD,
               "media fails closed when the table declines to bound it");
  rc |= expect_substr(raw, "\"reason\":\"daily_usd_unpriced_media\"",
                      "unbounded media keeps its own rejection reason");

  restore_env_value("FCLAW_PRICING", saved_pricing);
  return rc;
}

int model_profile_output_tokens_default_override_and_bound(void) {
  char *saved = capture_env_value("FCLAW_MODEL_PROFILES");
  LlmProfile profile;
  char err[256];
  const char *path = "workspace/test-model-output-profiles.json";
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(test_write_file(
                   path,
                   "{\"profiles\":["
                   "{\"id\":\"default\",\"provider\":\"mock\",\"model\":\"m\"},"
                   "{\"id\":\"override\",\"provider\":\"mock\",\"model\":\"m\","
                   "\"max_output_tokens\":8192},"
                   "{\"id\":\"too_large\",\"provider\":\"mock\",\"model\":\"m\","
                   "\"max_output_tokens\":16385}]}\n") == 0,
               "write model-profile output-token fixture");
  (void)setenv("FCLAW_MODEL_PROFILES", path, 1);
  rc |= expect(llm_load_profile("default", &profile) == 0,
               "profile without output cap loads");
  rc |= expect(profile.max_output_tokens == LLM_DEFAULT_MAX_OUTPUT_TOKENS,
               "profile output cap defaults to 4096");
  rc |= expect(llm_load_profile("override", &profile) == 0,
               "explicit bounded output cap loads");
  rc |= expect(profile.max_output_tokens == 8192,
               "explicit output cap is retained");
  rc |= expect(llm_load_profile("too_large", &profile) != 0,
               "profile beyond the response bound is rejected");
  rc |= expect(llm_load_profile_with_error("too_large", &profile, err,
                                           sizeof(err)) != 0,
               "detailed profile loader rejects the same bound");
  rc |= expect_substr(err, "max_output_tokens",
                      "profile failure names max_output_tokens");
  rc |= expect_substr(err, "between 1 and 16384",
                      "profile failure names the accepted bound");
  restore_env_value("FCLAW_MODEL_PROFILES", saved);
  return rc;
}

int provider_usage_extracts_reported_cache_tokens(void) {
  struct Fixture {
    const char *kind;
    const char *json;
    long long cached;
  } fixtures[] = {
    {"gemini_key",
     "{\"usageMetadata\":{\"promptTokenCount\":100,"
     "\"candidatesTokenCount\":5,\"thoughtsTokenCount\":7,"
     "\"cachedContentTokenCount\":80}}",
     80},
    {"openai_compat",
     "{\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":5,"
     "\"prompt_tokens_details\":{\"cached_tokens\":70}}}",
     70},
    {"openai_chat",
     "{\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":5,"
     "\"prompt_tokens_details\":{\"cached_tokens\":65}}}",
     65},
    {"openai_responses",
     "{\"usage\":{\"input_tokens\":100,\"output_tokens\":5,"
     "\"input_tokens_details\":{\"cached_tokens\":60}}}",
     60},
    {"http",
     "{\"usage\":{\"input_tokens\":100,\"output_tokens\":5,"
     "\"cache_read_input_tokens\":50,\"cache_creation_input_tokens\":20}}",
     50},
  };
  int rc = 0;
  for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
    LlmUsage usage;
    memset(&usage, 0, sizeof(usage));
    llm_extract_usage(fixtures[i].kind, fixtures[i].json, &usage);
    rc |= expect(usage.provider_cached_input_tokens_reported == 1,
                 "provider cache counter marked reported");
    rc |= expect(usage.provider_cached_input_tokens == fixtures[i].cached,
                 "provider cache counter preserved exactly");
  }
  {
    /* Anthropic reports the uncached remainder as input_tokens; the ledger
     * carries the whole prompt so pricing never mistakes the remainder for
     * the total (200 uncached + 5,000 cached used to price as 60 uncached). */
    LlmUsage usage;
    memset(&usage, 0, sizeof(usage));
    llm_extract_usage("http", fixtures[4].json, &usage);
    rc |= expect(usage.input_tokens == 170 && usage.total_tokens == 175,
                 "anthropic input_tokens is the whole prompt, cache included");
    rc |= expect(usage.provider_cache_creation_input_tokens_reported == 1 &&
                     usage.provider_cache_creation_input_tokens == 20,
                 "anthropic cache-creation tokens are reported");
  }
  {
    LlmUsage usage;
    memset(&usage, 0, sizeof(usage));
    llm_extract_usage("gemini_key", fixtures[0].json, &usage);
    rc |= expect(usage.output_tokens == 12,
                 "gemini thought tokens are billed output and counted as such");
    rc |= expect(usage.provider_cache_creation_input_tokens_reported == 0,
                 "providers that never report cache creation stay unreported");
  }
  {
    LlmUsage usage;
    memset(&usage, 0, sizeof(usage));
    llm_extract_usage("openai_responses",
                      "{\"usage\":{\"input_tokens\":10,"
                      "\"output_tokens\":2}}", &usage);
    rc |= expect(usage.provider_cached_input_tokens_reported == 0,
                 "missing provider cache counter remains unavailable");
  }
  return rc;
}

int usage_record_parser_requires_complete_current_contract(void) {
  static const char valid[] =
      "{\"ts\":\"2026-08-08T00:00:00Z\",\"floop\":\"hello\","
      "\"run_id\":\"run_001\",\"call_seq\":1,\"agent\":\"chat\","
      "\"profile\":\"mock\",\"provider\":\"mock\",\"model\":\"mock-v1\","
      "\"input_chars\":10,\"input_media_count\":0,\"input_media_bytes\":0,"
      "\"output_chars\":5,\"input_tokens\":3,\"output_tokens\":2,"
      "\"total_tokens\":5,\"provider_cached_input_tokens\":null,"
      "\"duration_ms\":1}";
  static const char missing_floop[] =
      "{\"ts\":\"2026-08-08T00:00:00Z\",\"run_id\":\"run_001\","
      "\"call_seq\":1,\"agent\":\"chat\",\"profile\":\"mock\","
      "\"provider\":\"mock\",\"model\":\"mock-v1\",\"input_chars\":10,"
      "\"input_media_count\":0,\"input_media_bytes\":0,\"output_chars\":5,"
      "\"input_tokens\":3,\"output_tokens\":2,\"total_tokens\":5,"
      "\"provider_cached_input_tokens\":null,\"duration_ms\":1}";
  static const char missing_cache_field[] =
      "{\"ts\":\"2026-08-08T00:00:00Z\",\"floop\":\"hello\","
      "\"run_id\":\"run_001\",\"call_seq\":1,\"agent\":\"chat\","
      "\"profile\":\"mock\",\"provider\":\"mock\",\"model\":\"mock-v1\","
      "\"input_chars\":10,\"input_media_count\":0,\"input_media_bytes\":0,"
      "\"output_chars\":5,\"input_tokens\":3,\"output_tokens\":2,"
      "\"total_tokens\":5,\"duration_ms\":1}";
  LlmUsageRecord record;
  int rc = 0;
  rc |= expect(llm_usage_record_parse(valid, &record) == 0,
               "complete current usage record parses");
  rc |= expect(record.provider_cached_input_tokens_reported == 0,
               "current null cache evidence remains explicitly unavailable");
  rc |= expect(llm_usage_record_parse(missing_floop, &record) != 0,
               "current usage record requires floop identity");
  rc |= expect(llm_usage_record_parse(missing_cache_field, &record) != 0,
               "current usage record requires explicit cache evidence");
  return rc;
}

int provider_limit_rolls_minute_and_day_windows(void) {
  LlmProvider provider;
  LlmUsage usage;
  char raw[256];
  time_t day1 = local_midday(2026, 5, 19);
  time_t day2 = local_midday(2026, 5, 20);
  int rc = 0;

  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "unit_limit_rollover");
  provider.rpm_limit = 2;
  provider.daily_limit = 3;

  rc |= test_reset_workspace();

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day1,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "first request allowed");
  rc |= expect(usage.local_limit_reason[0] == '\0', "first request has no local limit reason");

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day1 + 10,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "second request in minute allowed");

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day1 + 20,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_BLOCKED_RPM,
               "third request in minute blocked by rpm");
  rc |= expect(strcmp(usage.local_limit_reason, "rpm") == 0, "rpm block reason recorded");

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day1 + 70,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "next minute reopens rpm window");

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day1 + 130,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_BLOCKED_DAILY,
               "daily cap blocks after third granted request");
  rc |= expect(strcmp(usage.local_limit_reason, "daily") == 0, "daily block reason recorded");

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", day2,
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "new local day resets daily budget");
  return rc;
}

int provider_limit_zero_caps_disable_calls(void) {
  LlmProvider provider;
  LlmUsage usage;
  char raw[256];
  int rc = 0;

  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "unit_limit_zero");
  provider.rpm_limit = 0;
  provider.daily_limit = 500;
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", local_midday(2026, 5, 19),
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_BLOCKED_RPM,
               "rpm zero blocks immediately");
  rc |= expect_substr(raw, "\"reason\":\"rpm\"", "rpm zero block reason serialized");

  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "unit_limit_zero_daily");
  provider.rpm_limit = -1;
  provider.daily_limit = 0;
  memset(&usage, 0, sizeof(usage));
  raw[0] = '\0';
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs", local_midday(2026, 5, 19),
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_BLOCKED_DAILY,
               "daily zero blocks immediately");
  rc |= expect_substr(raw, "\"reason\":\"daily\"", "daily zero block reason serialized");
  return rc;
}

int auth_env_overrides_secret_store_and_falls_back(void) {
  char *saved_home = capture_env_value("FCLAW_HOME");
  char *saved_router = capture_env_value("OPENROUTER_API_KEY");
  char header[512];
  int rc = 0;

  rc |= test_remove_path("workspace/test-auth-home");
  rc |= test_mkdir_p("workspace/test-auth-home");
  (void)setenv("FCLAW_HOME", "workspace/test-auth-home", 1);
  rc |= expect(secret_store_set("endpoint:openrouter_key", "stored-router-token") == 0,
               "openrouter secret stored");

  (void)setenv("OPENROUTER_API_KEY", "env-router-token", 1);
  memset(header, 0, sizeof(header));
  rc |= expect(auth_build_header_with_env("openrouter_key", "OPENROUTER_API_KEY",
                                          header, sizeof(header)) == 0,
               "auth header resolves with env fallback");
  rc |= expect_substr(header, "env-router-token", "env token wins over secret store");

  (void)unsetenv("OPENROUTER_API_KEY");
  memset(header, 0, sizeof(header));
  rc |= expect(auth_build_header_with_env("openrouter_key", "OPENROUTER_API_KEY",
                                          header, sizeof(header)) == 0,
               "auth header falls back to secret store");
  rc |= expect_substr(header, "stored-router-token", "secret store used after env removal");

  restore_env_value("OPENROUTER_API_KEY", saved_router);
  restore_env_value("FCLAW_HOME", saved_home);
  return rc;
}

/* An action names a bare secret ("client_id"); the runtime supplies the
 * action:<id>: prefix from the id it dispatched. The action therefore
 * cannot reach another action's credentials, provider keys, or the whole
 * store -- the isolation is structural, not a check the action performs. */
static int collect_suffix(const char *suffix, void *user) {
  char *acc = (char *)user;
  strncat(acc, suffix, 200);
  strncat(acc, ",", 2);
  return 0;
}

int action_secret_namespace_cannot_reach_outside_itself(void) {
  char *saved_home = capture_env_value("FCLAW_HOME");
  char seen[512];
  int rc = 0;

  rc |= test_remove_path("workspace/test-secret-ns");
  rc |= test_mkdir_p("workspace/test-secret-ns");
  (void)setenv("FCLAW_HOME", "workspace/test-secret-ns", 1);

  rc |= expect(secret_store_set("endpoint:action:gcal:client_id", "mine") == 0,
               "gcal secret stored");
  rc |= expect(secret_store_set("endpoint:action:other:client_id", "theirs") == 0,
               "other action secret stored");
  rc |= expect(secret_store_set("endpoint:gemini_key", "provider") == 0,
               "provider key stored");

  seen[0] = '\0';
  rc |= expect(secret_store_list_prefix("endpoint:action:gcal:",
                                        collect_suffix, seen) == 0,
               "gcal namespace enumerates");
  rc |= expect_substr(seen, "client_id", "gcal sees its own bare name");
  rc |= expect(strstr(seen, "action:") == NULL,
               "the namespace prefix is never handed to the action");

  /* The store is keyed by one flat namespace, so the only way to reach a
   * sibling is to name it -- which the action never gets to do. */
  seen[0] = '\0';
  rc |= expect(secret_store_list_prefix("endpoint:action:other:",
                                        collect_suffix, seen) == 0,
               "sibling namespace is a separate enumeration");
  rc |= expect_no_substr(seen, "gemini", "provider keys are not in an action namespace");

  /* A traversal or sweep attempt is rejected by the same charset check
   * that guards whole keys, so no prefix can widen past its namespace. */
  seen[0] = '\0';
  rc |= expect(secret_store_list_prefix("endpoint:action:../", collect_suffix, seen) != 0,
               "path traversal in a prefix is refused");
  rc |= expect(secret_store_list_prefix("", collect_suffix, seen) != 0,
               "empty prefix cannot sweep the whole store");

  restore_env_value("FCLAW_HOME", saved_home);
  return rc;
}

static int broker_exchange(ActionSecretBroker *broker, int peer,
                           const char *request, char *response,
                           size_t response_len) {
  size_t request_len = strlen(request), written = 0, used = 0;
  while (written < request_len) {
    ssize_t n = write(peer, request + written, request_len - written);
    if (n <= 0) return -1;
    written += (size_t)n;
  }
  if (action_secret_broker_on_events(broker, POLLIN) != 0) return -1;
  while (used + 1U < response_len) {
    ssize_t n = read(peer, response + used, response_len - used - 1U);
    if (n <= 0) return -1;
    used += (size_t)n;
    response[used] = '\0';
    if (strchr(response, '\n')) break;
  }
  return 0;
}

int action_secret_broker_bounds_namespace_and_lifecycle(void) {
  char *saved_home = capture_env_value("FCLAW_HOME");
  ActionSecretBroker broker;
  int fds[2] = {-1, -1};
  char response[512];
  int rc = 0;

  memset(&broker, 0, sizeof(broker));
  broker.fd = -1;
  rc |= test_remove_path("workspace/test-secret-broker");
  rc |= test_mkdir_p("workspace/test-secret-broker");
  (void)setenv("FCLAW_HOME", "workspace/test-secret-broker", 1);
  rc |= expect(secret_store_set("endpoint:action:other:token", "sibling") == 0,
               "sibling broker fixture stored");
  rc |= expect(action_secret_broker_pair(fds) == 0,
               "private broker socket pair created");
  if (fds[0] < 0 || fds[1] < 0) goto done;
  rc |= expect(action_secret_broker_init(&broker, fds[0], "gcal") == 0,
               "broker stamps trusted action identity");
  fds[0] = -1;

  memset(response, 0, sizeof(response));
  rc |= expect(broker_exchange(&broker, fds[1], "SET token 4\nmine",
                               response, sizeof(response)) == 0,
               "broker accepts bounded private write");
  rc |= expect(strcmp(response, "OK\n") == 0,
               "broker write returns sanitized acknowledgement");
  memset(response, 0, sizeof(response));
  rc |= expect(broker_exchange(&broker, fds[1], "GET token\n",
                               response, sizeof(response)) == 0,
               "broker reads its stamped namespace");
  rc |= expect_substr(response, "VALUE 4\nmine",
                      "broker returns exact private value on private fd");

  memset(response, 0, sizeof(response));
  rc |= expect(broker_exchange(&broker, fds[1], "GET ../token\n",
                               response, sizeof(response)) == 0,
               "broker handles traversal request");
  rc |= expect(strcmp(response, "ERROR BAD_NAME\n") == 0,
               "broker refuses namespace traversal without data");
  memset(response, 0, sizeof(response));
  rc |= expect(broker_exchange(&broker, fds[1], "SET huge 8193\n",
                               response, sizeof(response)) == 0,
               "broker handles oversized value declaration");
  rc |= expect(strcmp(response, "ERROR BAD_REQUEST\n") == 0,
               "broker enforces value cap with sanitized error");

  memset(response, 0, sizeof(response));
  rc |= expect(broker_exchange(&broker, fds[1], "LIST\n",
                               response, sizeof(response)) == 0,
               "broker lists scoped suffixes after a rejected request");
  rc |= expect_substr(response, "token\n", "broker lists own suffix");
  rc |= expect_no_substr(response, "sibling",
                         "broker never lists sibling action values");
  rc |= expect_no_substr(response, "other",
                         "broker never exposes sibling action names");

  close(fds[1]);
  fds[1] = -1;
  (void)action_secret_broker_on_events(&broker, POLLIN | POLLHUP);
  rc |= expect(broker.closed && broker.fd < 0,
               "broker clears and closes when child endpoint closes");

done:
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
  if (broker.fd >= 0) action_secret_broker_close(&broker);
  restore_env_value("FCLAW_HOME", saved_home);
  return rc;
}

int secret_store_atomic_replacement_survives_concurrent_writers(void) {
  char *saved_home = capture_env_value("FCLAW_HOME");
  char value_a[4097], value_b[4097], got[8192];
  pid_t children[2] = {-1, -1};
  int rc = 0;

  memset(value_a, 'A', sizeof(value_a) - 1U); value_a[sizeof(value_a) - 1U] = '\0';
  memset(value_b, 'B', sizeof(value_b) - 1U); value_b[sizeof(value_b) - 1U] = '\0';
  rc |= test_remove_path("workspace/test-secret-atomic");
  rc |= test_mkdir_p("workspace/test-secret-atomic");
  (void)setenv("FCLAW_HOME", "workspace/test-secret-atomic", 1);
  for (int child = 0; child < 2; ++child) {
    children[child] = fork();
    if (children[child] == 0) {
      const char *value = child == 0 ? value_a : value_b;
      for (int i = 0; i < 40; ++i)
        if (secret_store_set("endpoint:action:gcal:refresh_token", value) != 0)
          _exit(1);
      _exit(0);
    }
    rc |= expect(children[child] > 0, "concurrent secret writer forked");
  }
  for (int child = 0; child < 2; ++child) {
    int status = 0;
    if (children[child] <= 0) continue;
    rc |= expect(waitpid(children[child], &status, 0) == children[child] &&
                     WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "concurrent atomic secret writer completed");
  }
  memset(got, 0, sizeof(got));
  rc |= expect(secret_store_get("endpoint:action:gcal:refresh_token",
                                got, sizeof(got)) == 0,
               "concurrently replaced secret remains readable");
  rc |= expect(strcmp(got, value_a) == 0 || strcmp(got, value_b) == 0,
               "concurrent replacement never exposes a partial token");
  {
    DIR *dir = opendir("workspace/test-secret-atomic/secrets");
    struct dirent *ent;
    int temp_found = 0;
    if (dir) {
      while ((ent = readdir(dir)) != NULL)
        if (strstr(ent->d_name, ".tmp.") != NULL) temp_found = 1;
      closedir(dir);
    }
    rc |= expect(!temp_found, "atomic replacement leaves no temporary file");
  }
  secret_store_zero(got, sizeof(got));
  restore_env_value("FCLAW_HOME", saved_home);
  return rc;
}


int llm_normalize_reports_model_output_too_large(void) {
  char *big = NULL;
  char normalized[512];
  size_t payload_len = (size_t)LLM_RESP_MAX + 1024U;
  int rc = 0;
  big = (char *)malloc(payload_len + 32U);
  rc |= expect(big != NULL, "allocates oversized model output");
  if (!big) return rc;
  {
    size_t prefix_len = strlen("{\"message\":\"");
    memcpy(big, "{\"message\":\"", prefix_len);
    memset(big + prefix_len, 'A', payload_len);
    memcpy(big + prefix_len + payload_len, "\"}", 3U);
  }
  rc |= expect(llm_normalize_response_text(big, normalized, sizeof(normalized), NULL)
               == LLM_ERR_MODEL_OUTPUT_TOO_LARGE,
               "oversized model output returns model_output_too_large");
  free(big);
  return rc;
}

int llm_normalize_trims_tool_noop_suffix_without_corruption(void) {
  const char *raw = "{\"calls\":[{\"name\":\"write_file\",\"args\":{\"path\":\"x\",\"content\":\"<html>狐 café</html>\"}}]} {\"tool\":null}";
  char normalized[2048];
  int rc = 0;
  rc |= expect(llm_normalize_response_text(raw, normalized, sizeof(normalized), NULL) == 0,
               "valid object plus tool noop suffix normalizes");
  rc |= expect_substr(normalized, "\"content\":\"<html>狐 café</html>\"",
                      "normalized output preserves unicode and html content");
  rc |= expect_no_substr(normalized, "{\"tool\":null}",
                         "tool noop suffix is trimmed from normalized output");
  return rc;
}
