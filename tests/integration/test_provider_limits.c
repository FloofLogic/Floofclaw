#include "test_support.h"

#include "../../runtime/llm/llm.h"
#include "../../runtime/llm/provider_limit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *capture_env_value(const char *name) {
  const char *value = getenv(name);
  if (!value) return NULL;
  return strdup(value);
}

static void restore_env_value(const char *name, char *value) {
  if (!name) return;
  if (value) {
    (void)setenv(name, value, 1);
    free(value);
  } else {
    (void)unsetenv(name);
  }
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

static int write_registry_fixture(const char *path, const char *providers_json) {
  char text[4096];
  if (snprintf(text, sizeof(text),
               "{\n"
               "  \"version\": 1,\n"
               "  \"providers\": [\n"
               "%s\n"
               "  ]\n"
               "}\n",
               providers_json) >= (int)sizeof(text)) return -1;
  return test_write_file(path, text);
}

int provider_blocked_at_zero_rpm_writes_local_limit_artifacts(void) {
  char *saved_registry = capture_env_value("FCLAW_LLM_REGISTRY");
  LlmProfile profile;
  char response[LLM_RESP_MAX];
  char *meta = NULL;
  char *raw = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= write_registry_fixture("workspace/test_registry_zero_rpm.json",
                               "    {\n"
                               "      \"id\": \"zero_rpm_provider\",\n"
                               "      \"kind\": \"openai_compat\",\n"
                               "      \"url\": \"http://127.0.0.1:1/v1/chat/completions\",\n"
                               "      \"limits\": { \"rpm\": 0, \"daily\": 500 }\n"
                               "    }");
  (void)setenv("FCLAW_LLM_REGISTRY", "workspace/test_registry_zero_rpm.json", 1);

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.id, sizeof(profile.id), "zero_rpm_profile");
  snprintf(profile.provider, sizeof(profile.provider), "zero_rpm_provider");
  snprintf(profile.model, sizeof(profile.model), "offline-model");

  rc |= test_mkdir_p("workspace/runs/run_001");
  rc |= expect(llm_call("workspace/runs/run_001", "hello", "tests-agent", &profile,
                        "{\"message\":\"hello\"}", response, sizeof(response)) != 0,
               "llm_call fails fast when rpm is zero");
  rc |= expect_file_exists("workspace/runs/run_001/provider_calls/001_request.json");
  rc |= expect_file_exists("workspace/runs/run_001/provider_calls/001_meta.json");
  rc |= expect_file_exists("workspace/runs/run_001/provider_calls/001_raw_response.json");
  rc |= expect_file_not_exists("workspace/runs/run_001/provider_calls/001_response.json");
  rc |= expect_file_not_exists("workspace/runs/run_001/provider_calls/001_raw_request.json");
  rc |= test_read_file("workspace/runs/run_001/provider_calls/001_meta.json", &meta);
  rc |= test_read_file("workspace/runs/run_001/provider_calls/001_raw_response.json", &raw);
  rc |= expect_substr(meta, "\"local_limit_blocked\":true", "meta records local limit block");
  rc |= expect_substr(meta, "\"local_limit_reason\":\"rpm\"", "meta records rpm block reason");
  rc |= expect_substr(raw, "\"error\":\"provider_local_limit\"", "raw response records local limit error");
  rc |= expect_substr(raw, "\"reason\":\"rpm\"", "raw response records rpm reason");

  free(raw);
  free(meta);
  restore_env_value("FCLAW_LLM_REGISTRY", saved_registry);
  return rc;
}

int provider_daily_cap_is_shared_across_profiles(void) {
  char *saved_registry = capture_env_value("FCLAW_LLM_REGISTRY");
  char *saved_retries = capture_env_value("FCLAW_LLM_MAX_RETRIES");
  LlmProfile first;
  LlmProfile second;
  char response[LLM_RESP_MAX];
  char *meta = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= write_registry_fixture("workspace/test_registry_daily_shared.json",
                               "    {\n"
                               "      \"id\": \"daily_shared_provider\",\n"
                               "      \"kind\": \"openai_compat\",\n"
                               "      \"url\": \"http://127.0.0.1:1/v1/chat/completions\",\n"
                               "      \"limits\": { \"rpm\": 15, \"daily\": 1 }\n"
                               "    }");
  (void)setenv("FCLAW_LLM_REGISTRY", "workspace/test_registry_daily_shared.json", 1);
  (void)setenv("FCLAW_LLM_MAX_RETRIES", "0", 1);

  memset(&first, 0, sizeof(first));
  snprintf(first.id, sizeof(first.id), "profile_a");
  snprintf(first.provider, sizeof(first.provider), "daily_shared_provider");
  snprintf(first.model, sizeof(first.model), "offline-model-a");

  memset(&second, 0, sizeof(second));
  snprintf(second.id, sizeof(second.id), "profile_b");
  snprintf(second.provider, sizeof(second.provider), "daily_shared_provider");
  snprintf(second.model, sizeof(second.model), "offline-model-b");

  rc |= test_mkdir_p("workspace/runs/run_001");
  rc |= test_mkdir_p("workspace/runs/run_002");
  rc |= expect(llm_call("workspace/runs/run_001", "hello", "tests-agent", &first,
                        "{\"message\":\"hello\"}", response, sizeof(response)) != 0,
               "first call consumes daily budget even when transport fails");
  rc |= expect(llm_call("workspace/runs/run_002", "hello", "tests-agent", &second,
                        "{\"message\":\"hello again\"}", response, sizeof(response)) != 0,
               "second profile is blocked by shared provider daily budget");
  rc |= test_read_file("workspace/runs/run_002/provider_calls/001_meta.json", &meta);
  rc |= expect_substr(meta, "\"local_limit_blocked\":true", "daily block writes blocked flag");
  rc |= expect_substr(meta, "\"local_limit_reason\":\"daily\"", "daily block shared across profiles");

  free(meta);
  restore_env_value("FCLAW_LLM_MAX_RETRIES", saved_retries);
  restore_env_value("FCLAW_LLM_REGISTRY", saved_registry);
  return rc;
}

int provider_daily_budget_reopens_on_next_local_day(void) {
  LlmProvider provider;
  LlmUsage usage;
  char raw[256];
  char *state = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "daily_reset_provider");
  provider.rpm_limit = -1;
  provider.daily_limit = 1;

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs",
                                             local_midday(2026, 5, 19),
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "day one request is allowed");
  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs",
                                             local_midday(2026, 5, 20),
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "next local day reopens daily budget");
  rc |= test_read_file("workspace/logs/provider_limits/daily_reset_provider.json", &state);
  rc |= expect_substr(state, "\"day_count\":1", "state file resets daily count on new day");

  free(state);
  return rc;
}
