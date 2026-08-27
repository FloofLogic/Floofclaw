#include "test_support.h"

#include "../../runtime/llm/llm.h"
#include "../../runtime/llm/provider_limit.h"
#include "../../runtime/support/fsutil.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
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

int provider_daily_usd_zero_blocks_before_external_dispatch(void) {
  char *saved_registry = capture_env_value("FCLAW_LLM_REGISTRY");
  LlmProfile profile;
  char response[LLM_RESP_MAX];
  char *meta = NULL;
  char *raw = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= write_registry_fixture(
      "workspace/test_registry_zero_usd.json",
      "    {\n"
      "      \"id\": \"zero_usd_provider\",\n"
      "      \"kind\": \"openai_compat\",\n"
      "      \"url\": \"http://127.0.0.1:1/v1/chat/completions\",\n"
      "      \"limits\": { \"daily_usd\": 0 }\n"
      "    }");
  (void)setenv("FCLAW_LLM_REGISTRY",
               "workspace/test_registry_zero_usd.json", 1);

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.id, sizeof(profile.id), "zero_usd_profile");
  snprintf(profile.provider, sizeof(profile.provider), "zero_usd_provider");
  snprintf(profile.model, sizeof(profile.model), "offline-model");
  profile.max_output_tokens = 128;

  rc |= test_mkdir_p("workspace/runs/run_usd_zero");
  rc |= expect(llm_call("workspace/runs/run_usd_zero", "hello",
                        "tests-agent", &profile,
                        "{\"message\":\"hello\"}", response,
                        sizeof(response)) != 0,
               "zero dollar cap fails before the loopback transport");
  rc |= expect_file_not_exists(
      "workspace/runs/run_usd_zero/provider_calls/001_raw_request.json");
  rc |= test_read_file(
      "workspace/runs/run_usd_zero/provider_calls/001_meta.json", &meta);
  rc |= test_read_file(
      "workspace/runs/run_usd_zero/provider_calls/001_raw_response.json", &raw);
  rc |= expect_substr(meta, "\"local_limit_blocked\":true",
                      "USD cap writes the standard local block flag");
  rc |= expect_substr(meta, "\"local_limit_reason\":\"daily_usd\"",
                      "USD cap reason survives in provider metadata");
  rc |= expect_substr(raw,
                      "\"error\":\"provider_budget_exceeded\"",
                      "raw response names the provider budget error");
  rc |= expect_substr(raw, "\"reason\":\"daily_usd\"",
                      "raw response names the daily USD boundary");

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

/* The daily state used to be rewritten in place (ftruncate, write, fsync):
 * a child killed between the truncate and the write left an empty file that
 * read back as a fresh day, and a partial write left a file no later call
 * could parse. Now the state is replaced by rename under a separate lock
 * file, so the path always holds either the previous state or the new one,
 * and a writer that died mid-write leaves only an ignorable temp file. */
int provider_limit_state_is_replaced_atomically_under_a_lock_file(void) {
  static const char *dir = "workspace/logs/provider_limits";
  static const char *state_path = "workspace/logs/provider_limits/atomic_provider.json";
  LlmProvider provider;
  LlmUsage usage;
  char raw[256];
  char *state = NULL;
  struct stat first, second;
  DIR *listing = NULL;
  struct dirent *entry;
  int entries = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  memset(&provider, 0, sizeof(provider));
  snprintf(provider.id, sizeof(provider.id), "atomic_provider");
  provider.rpm_limit = -1;
  provider.daily_limit = 10;

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs",
                                             local_midday(2026, 8, 25),
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "first reservation is allowed");
  rc |= expect(stat(state_path, &first) == 0 && first.st_size > 0,
               "first reservation leaves a complete state file");
  rc |= expect(fs_file_exists("workspace/logs/provider_limits/atomic_provider.json.lock"),
               "the lock lives beside the state, in a file that is never replaced");

  /* A writer that died between its temp write and its rename. */
  rc |= test_write_file("workspace/logs/provider_limits/atomic_provider.json.tmp.999.1",
                        "{\"provider_id\":\"atomic_provider\",\"day_c");

  memset(&usage, 0, sizeof(usage));
  rc |= expect(llm_provider_limit_reserve_at(&provider, "workspace/logs",
                                             local_midday(2026, 8, 25),
                                             &usage, raw, sizeof(raw)) == LLM_PROVIDER_LIMIT_OK,
               "a torn temp file from a dead writer does not brick the provider");
  rc |= expect(stat(state_path, &second) == 0 && second.st_size > 0 &&
                   second.st_ino != first.st_ino,
               "the state file is replaced by rename, never truncated in place");
  rc |= test_read_file(state_path, &state);
  rc |= expect_substr(state, "\"day_count\":2", "the replaced state carries both reservations");

  listing = opendir(dir);
  rc |= expect(listing != NULL, "provider_limits directory lists");
  while (listing && (entry = readdir(listing)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    entries++;
  }
  if (listing) closedir(listing);
  rc |= expect(entries == 3,
               "only the state, its lock, and the planted torn temp remain (no leaked temp files)");

  free(state);
  return rc;
}
