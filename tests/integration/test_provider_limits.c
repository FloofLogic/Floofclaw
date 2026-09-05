#include "test_support.h"

#include "../../runtime/llm/llm.h"
#include "../../runtime/llm/provider_limit.h"
#include "../../runtime/support/fsutil.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static int fd_write_all(int fd, const void *bytes, size_t len) {
  const unsigned char *p = (const unsigned char *)bytes;
  size_t off = 0U;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

/* One loopback HTTP exchange: accept, read the request, answer with `body`
 * as application/json, exit. The port is chosen by the kernel. */
static int spawn_one_json_response_server(const char *body, pid_t *pid_out,
                                          unsigned *port_out) {
  struct sockaddr_in address;
  socklen_t address_len = sizeof(address);
  int listener;
  pid_t pid;
  if (!body || !pid_out || !port_out) return -1;
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(listener, 1) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_len) != 0) {
    close(listener);
    return -1;
  }
  *port_out = (unsigned)ntohs(address.sin_port);
  pid = fork();
  if (pid < 0) {
    close(listener);
    return -1;
  }
  if (pid == 0) {
    char request[8192];
    char header[512];
    int client;
    int header_len;
    alarm(10);
    client = accept(listener, NULL, NULL);
    if (client < 0) _exit(2);
    (void)read(client, request, sizeof(request));
    header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        strlen(body));
    if (header_len < 0 || (size_t)header_len >= sizeof(header) ||
        fd_write_all(client, header, (size_t)header_len) != 0 ||
        fd_write_all(client, body, strlen(body)) != 0) {
      close(client);
      close(listener);
      _exit(3);
    }
    close(client);
    close(listener);
    _exit(0);
  }
  close(listener);
  *pid_out = pid;
  return 0;
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

/* The pre-dispatch reservation is a ceiling. Once the provider answers, the
 * ledger holds what the reported usage actually costs, so a $5 cap is no
 * longer spent by ~40 conservative ceilings. */
int provider_daily_usd_reservation_settles_after_a_loopback_response(void) {
  char *saved_registry = capture_env_value("FCLAW_LLM_REGISTRY");
  char *saved_pricing = capture_env_value("FCLAW_PRICING");
  char *saved_retries = capture_env_value("FCLAW_LLM_MAX_RETRIES");
  static const char body[] =
      "{\"choices\":[{\"message\":{\"content\":\"{\\\"ok\\\":true}\"}}],"
      "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":10,"
      "\"prompt_tokens_details\":{\"cached_tokens\":40}}}";
  LlmProfile profile;
  char response[LLM_RESP_MAX];
  char providers[1024];
  char *meta = NULL;
  char *state = NULL;
  pid_t server = -1;
  unsigned port = 0;
  int status = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(spawn_one_json_response_server(body, &server, &port) == 0,
               "loopback provider starts");
  snprintf(providers, sizeof(providers),
           "    {\n"
           "      \"id\": \"settle_provider\",\n"
           "      \"kind\": \"openai_compat\",\n"
           "      \"url\": \"http://127.0.0.1:%u/v1/chat/completions\",\n"
           "      \"limits\": { \"daily_usd\": 1.0 }\n"
           "    }",
           port);
  rc |= write_registry_fixture("workspace/test_registry_settle.json", providers);
  rc |= test_write_file(
      "workspace/test_pricing_settle.json",
      "{\"models\":[{\"provider\":\"settle_provider\","
      "\"model\":\"settle-model\",\"input\":0.1,"
      "\"cached_input\":0.01,\"output\":0.2}]}\n");
  (void)setenv("FCLAW_LLM_REGISTRY", "workspace/test_registry_settle.json", 1);
  (void)setenv("FCLAW_PRICING", "workspace/test_pricing_settle.json", 1);
  (void)setenv("FCLAW_LLM_MAX_RETRIES", "0", 1);

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.id, sizeof(profile.id), "settle_profile");
  snprintf(profile.provider, sizeof(profile.provider), "settle_provider");
  snprintf(profile.model, sizeof(profile.model), "settle-model");
  profile.max_output_tokens = 10;

  rc |= test_mkdir_p("workspace/runs/run_settle");
  rc |= expect(llm_call("workspace/runs/run_settle", "hello", "tests-agent",
                        &profile, "{\"message\":\"hello\"}", response,
                        sizeof(response)) == 0,
               "the capped call completes against the loopback provider");
  if (server > 0) (void)waitpid(server, &status, 0);
  rc |= test_read_file("workspace/runs/run_settle/provider_calls/001_meta.json",
                       &meta);
  rc |= expect_substr(meta, "\"budget_outcome\":\"settled\"",
                      "meta records that the reservation was settled");
  rc |= expect_substr(meta, "\"budget_reserved_usd\":0.000107",
                      "meta records the pre-dispatch ceiling");
  rc |= expect_substr(meta, "\"budget_settled_usd\":0.000009",
                      "meta records the priced actual cost");
  rc |= test_read_file("workspace/logs/provider_limits/settle_provider.json",
                       &state);
  rc |= expect_substr(state, "\"day_usd_reserved_micros\":9",
                      "the day ledger holds the actual cost after settlement");

  free(state);
  free(meta);
  restore_env_value("FCLAW_LLM_MAX_RETRIES", saved_retries);
  restore_env_value("FCLAW_PRICING", saved_pricing);
  restore_env_value("FCLAW_LLM_REGISTRY", saved_registry);
  return rc;
}

/* A connection refused never reached the provider, so the reservation is
 * returned; the call-count limits still charge the attempt. */
int provider_daily_usd_reservation_is_released_when_the_provider_is_unreachable(void) {
  char *saved_registry = capture_env_value("FCLAW_LLM_REGISTRY");
  char *saved_pricing = capture_env_value("FCLAW_PRICING");
  char *saved_retries = capture_env_value("FCLAW_LLM_MAX_RETRIES");
  LlmProfile profile;
  char response[LLM_RESP_MAX];
  char *meta = NULL;
  char *state = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= write_registry_fixture(
      "workspace/test_registry_release.json",
      "    {\n"
      "      \"id\": \"release_provider\",\n"
      "      \"kind\": \"openai_compat\",\n"
      "      \"url\": \"http://127.0.0.1:1/v1/chat/completions\",\n"
      "      \"limits\": { \"daily\": 5, \"daily_usd\": 1.0 }\n"
      "    }");
  rc |= test_write_file(
      "workspace/test_pricing_release.json",
      "{\"models\":[{\"provider\":\"release_provider\","
      "\"model\":\"release-model\",\"input\":0.1,"
      "\"cached_input\":0.01,\"output\":0.2}]}\n");
  (void)setenv("FCLAW_LLM_REGISTRY", "workspace/test_registry_release.json", 1);
  (void)setenv("FCLAW_PRICING", "workspace/test_pricing_release.json", 1);
  (void)setenv("FCLAW_LLM_MAX_RETRIES", "0", 1);

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.id, sizeof(profile.id), "release_profile");
  snprintf(profile.provider, sizeof(profile.provider), "release_provider");
  snprintf(profile.model, sizeof(profile.model), "release-model");
  profile.max_output_tokens = 10;

  rc |= test_mkdir_p("workspace/runs/run_release");
  rc |= expect(llm_call("workspace/runs/run_release", "hello", "tests-agent",
                        &profile, "{\"message\":\"hello\"}", response,
                        sizeof(response)) != 0,
               "the call fails against a refused connection");
  rc |= test_read_file("workspace/runs/run_release/provider_calls/001_meta.json",
                       &meta);
  rc |= expect_substr(meta, "\"budget_outcome\":\"released\"",
                      "meta records that the reservation was released");
  rc |= expect_substr(meta, "\"budget_settled_usd\":0.000000",
                      "a released reservation settles to nothing");
  rc |= test_read_file("workspace/logs/provider_limits/release_provider.json",
                       &state);
  rc |= expect_substr(state, "\"day_usd_reserved_micros\":0",
                      "the day ledger no longer holds the ceiling");
  rc |= expect_substr(state, "\"day_count\":1",
                      "the attempt still counts against the daily call limit");

  free(state);
  free(meta);
  restore_env_value("FCLAW_LLM_MAX_RETRIES", saved_retries);
  restore_env_value("FCLAW_PRICING", saved_pricing);
  restore_env_value("FCLAW_LLM_REGISTRY", saved_registry);
  return rc;
}
