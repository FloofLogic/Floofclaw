#include "provider_limit.h"
#include "pricing.h"

#include "../support/fsutil.h"
#include "../support/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LIMIT_PATH_MAX 1024

typedef struct {
  char provider_id[LLM_ID_MAX];
  char day_key[16];
  long long day_count;
  long long day_usd_reserved_micros;
  long long minute_key;
  long long minute_count;
} ProviderLimitState;

static const char *default_logs_root(const char *logs_root) {
  return (logs_root && *logs_root) ? logs_root : "workspace/logs";
}

static int build_limit_paths(const char *logs_root, const char *provider_id,
                             char *dir_out, size_t dir_out_len,
                             char *path_out, size_t path_out_len) {
  char file_name[LLM_ID_MAX + 8];
  if (!logs_root || !provider_id || !dir_out || !path_out) return -1;
  if (snprintf(dir_out, dir_out_len, "%s/provider_limits", logs_root) >= (int)dir_out_len) return -1;
  if (snprintf(file_name, sizeof(file_name), "%s.json", provider_id) >= (int)sizeof(file_name)) return -1;
  return fs_join(path_out, path_out_len, dir_out, file_name);
}

static int current_day_key(time_t now, char *out, size_t out_len) {
  struct tm local_tm;
  if (!out || out_len == 0) return -1;
  if (localtime_r(&now, &local_tm) == NULL) return -1;
  return strftime(out, out_len, "%Y-%m-%d", &local_tm) > 0 ? 0 : -1;
}

static void state_init(ProviderLimitState *state, const LlmProvider *provider) {
  if (!state || !provider) return;
  memset(state, 0, sizeof(*state));
  snprintf(state->provider_id, sizeof(state->provider_id), "%s", provider->id);
}

/* Windows only roll forward. A clock stepped backwards across midnight (or
 * a minute boundary) keeps counting against the later window it already
 * opened, so a backwards step can never grant a second allowance. */
static void state_roll_windows(ProviderLimitState *state, time_t now) {
  char day_key[16];
  long long minute_key;
  if (!state) return;
  if (current_day_key(now, day_key, sizeof(day_key)) != 0) day_key[0] = '\0';
  minute_key = (long long)(now / 60);
  if (day_key[0] == '\0' || strcmp(state->day_key, day_key) < 0) {
    snprintf(state->day_key, sizeof(state->day_key), "%s", day_key);
    state->day_count = 0;
    state->day_usd_reserved_micros = 0;
  }
  if (state->minute_key < minute_key) {
    state->minute_key = minute_key;
    state->minute_count = 0;
  }
}

static int parse_state_text(const char *text, ProviderLimitState *state) {
  JsonRef root;
  long long value = 0;
  if (!text || !*text || !state) return 0;
  if (json_ref_first_object(text, &root) != 0) return -1;
  (void)json_ref_object_get_string(&root, "provider_id", state->provider_id, sizeof(state->provider_id));
  (void)json_ref_object_get_string(&root, "day_key", state->day_key, sizeof(state->day_key));
  if (json_ref_object_get_long(&root, "day_count", &value) == 0) state->day_count = value;
  if (json_ref_object_get_long(&root, "day_usd_reserved_micros", &value) == 0)
    state->day_usd_reserved_micros = value;
  if (json_ref_object_get_long(&root, "minute_key", &value) == 0) state->minute_key = value;
  if (json_ref_object_get_long(&root, "minute_count", &value) == 0) state->minute_count = value;
  return 0;
}

/* The state file is replaced, never rewritten in place: a child killed
 * between a truncate and its write used to leave an empty file (read back
 * as a fresh day, so the whole day's reservation vanished) or a partial one
 * (unparsable, so every later call failed preflight until an operator
 * deleted it). fs_write_text_atomic writes a sibling temp file, fsyncs it,
 * and renames it over the path, so the file is always the previous state
 * or the new one. The lock lives in a separate file (see open_lock_file)
 * because a lock held on the replaced inode would not exclude a waiter
 * that opened the path before the rename. */
static int write_state_atomic(const char *path, const ProviderLimitState *state) {
  char json[512];
  if (!path || !state) return -1;
  if (snprintf(json, sizeof(json),
               "{"
               "\"provider_id\":\"%s\","
               "\"day_key\":\"%s\","
               "\"day_count\":%lld,"
               "\"day_usd_reserved_micros\":%lld,"
               "\"minute_key\":%lld,"
               "\"minute_count\":%lld"
               "}\n",
               state->provider_id,
               state->day_key,
               state->day_count,
               state->day_usd_reserved_micros,
               state->minute_key,
               state->minute_count) >= (int)sizeof(json)) return -1;
  return fs_write_text_atomic(path, json);
}

static void set_block_reason(LlmUsage *usage_out, const char *reason) {
  if (!usage_out || !reason) return;
  snprintf(usage_out->local_limit_reason, sizeof(usage_out->local_limit_reason), "%s", reason);
}

static void build_block_json(const LlmProvider *provider, const char *reason,
                             char *out, size_t out_len) {
  char provider_id[(LLM_ID_MAX * 2)];
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!provider || !reason) return;
  if (json_escape(provider->id, provider_id, sizeof(provider_id)) != 0) provider_id[0] = '\0';
  (void)snprintf(out, out_len,
                 "{"
                 "\"error\":\"provider_local_limit\","
                 "\"reason\":\"%s\","
                 "\"provider\":\"%s\","
                 "\"rpm_limit\":%d,"
                 "\"daily_limit\":%d"
                 "}",
                 reason, provider_id, provider->rpm_limit, provider->daily_limit);
}

static void build_budget_block_json(const LlmProvider *provider,
                                    const char *reason,
                                    long long reserved_micros,
                                    long long request_micros,
                                    char *out, size_t out_len) {
  char provider_id[(LLM_ID_MAX * 2)];
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!provider || !reason) return;
  if (json_escape(provider->id, provider_id, sizeof(provider_id)) != 0)
    provider_id[0] = '\0';
  (void)snprintf(out, out_len,
                 "{"
                 "\"error\":\"provider_budget_exceeded\","
                 "\"reason\":\"%s\","
                 "\"provider\":\"%s\","
                 "\"daily_usd_limit\":%.6f,"
                 "\"reserved_usd\":%.6f,"
                 "\"request_max_usd\":%.6f"
                 "}",
                 reason, provider_id, provider->daily_usd_limit,
                 (double)reserved_micros / 1000000.0,
                 (double)request_micros / 1000000.0);
}

/* Exclusive lock for one provider's reservation critical section. The
 * lock file is never replaced, so every process that opens the path locks
 * the same inode; the state file beside it is what gets renamed over. */
static int open_lock_file(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0644);
  if (fd < 0) return -1;
  if (flock(fd, LOCK_EX) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int limit_disabled(const LlmProvider *provider, LlmUsage *usage_out,
                          char *raw_response_out, size_t raw_response_out_len) {
  if (!provider) return -1;
  if (provider->rpm_limit == 0) {
    set_block_reason(usage_out, "rpm");
    build_block_json(provider, "rpm", raw_response_out, raw_response_out_len);
    return LLM_PROVIDER_LIMIT_BLOCKED_RPM;
  }
  if (provider->daily_limit == 0) {
    set_block_reason(usage_out, "daily");
    build_block_json(provider, "daily", raw_response_out, raw_response_out_len);
    return LLM_PROVIDER_LIMIT_BLOCKED_DAILY;
  }
  return LLM_PROVIDER_LIMIT_OK;
}

static long long daily_usd_limit_micros(const LlmProvider *provider) {
  double micros;
  if (!provider || provider->daily_usd_limit < 0.0) return -1;
  micros = provider->daily_usd_limit * 1000000.0;
  if (micros <= 0.0) return 0;
  if (micros > (double)LLONG_MAX) return LLONG_MAX;
  return (long long)micros;
}

static int reserve_at(const LlmProvider *provider, const char *logs_root,
                      time_t now, int enforce_usd,
                      long long request_usd_micros, LlmUsage *usage_out,
                      char *raw_response_out,
                      size_t raw_response_out_len) {
  ProviderLimitState state;
  char dir_path[LIMIT_PATH_MAX];
  char state_path[LIMIT_PATH_MAX];
  char lock_path[LIMIT_PATH_MAX];
  char *text = NULL;
  int fd = -1;
  int rc = LLM_PROVIDER_LIMIT_OK;
  long long usd_limit_micros = daily_usd_limit_micros(provider);
  const char *root = default_logs_root(logs_root);

  if (!provider || !provider->id[0]) return -1;
  if (usage_out) usage_out->local_limit_reason[0] = '\0';
  if (raw_response_out && raw_response_out_len > 0) raw_response_out[0] = '\0';

  rc = limit_disabled(provider, usage_out, raw_response_out,
                      raw_response_out_len);
  if (rc != LLM_PROVIDER_LIMIT_OK) return rc;
  if (enforce_usd && usd_limit_micros == 0) {
    set_block_reason(usage_out, "daily_usd");
    build_budget_block_json(provider, "daily_usd", 0,
                            request_usd_micros, raw_response_out,
                            raw_response_out_len);
    return LLM_PROVIDER_LIMIT_BLOCKED_USD;
  }
  if (provider->rpm_limit < 0 && provider->daily_limit < 0 &&
      (!enforce_usd || usd_limit_micros < 0))
    return LLM_PROVIDER_LIMIT_OK;

  if (build_limit_paths(root, provider->id, dir_path, sizeof(dir_path),
                        state_path, sizeof(state_path)) != 0) return -1;
  if (fs_mkdir_p(dir_path) != 0) return -1;
  if (snprintf(lock_path, sizeof(lock_path), "%s.lock", state_path) >=
      (int)sizeof(lock_path)) return -1;

  fd = open_lock_file(lock_path);
  if (fd < 0) return -1;

  state_init(&state, provider);
  /* A missing state file is a fresh provider; any other read failure is
   * fail-closed, like an unparsable file. */
  if (fs_read_text(state_path, &text, 0) != 0 && errno != ENOENT) goto fail;
  if (parse_state_text(text, &state) != 0) goto fail;
  if (strcmp(state.provider_id, provider->id) != 0 || state.day_count < 0 ||
      state.day_usd_reserved_micros < 0 || state.minute_count < 0)
    goto fail;
  state_roll_windows(&state, now);

  if (provider->daily_limit >= 0 && state.day_count >= provider->daily_limit) {
    set_block_reason(usage_out, "daily");
    build_block_json(provider, "daily", raw_response_out, raw_response_out_len);
    rc = LLM_PROVIDER_LIMIT_BLOCKED_DAILY;
    goto done;
  }
  if (provider->rpm_limit >= 0 && state.minute_count >= provider->rpm_limit) {
    set_block_reason(usage_out, "rpm");
    build_block_json(provider, "rpm", raw_response_out, raw_response_out_len);
    rc = LLM_PROVIDER_LIMIT_BLOCKED_RPM;
    goto done;
  }
  if (enforce_usd && usd_limit_micros >= 0 &&
      (request_usd_micros > usd_limit_micros ||
       state.day_usd_reserved_micros >
           usd_limit_micros - request_usd_micros)) {
    set_block_reason(usage_out, "daily_usd");
    build_budget_block_json(provider, "daily_usd",
                            state.day_usd_reserved_micros,
                            request_usd_micros, raw_response_out,
                            raw_response_out_len);
    rc = LLM_PROVIDER_LIMIT_BLOCKED_USD;
    goto done;
  }

  state.day_count++;
  state.minute_count++;
  if (enforce_usd) state.day_usd_reserved_micros += request_usd_micros;
  if (write_state_atomic(state_path, &state) != 0) goto fail;
  if (enforce_usd && usage_out) {
    usage_out->budget_outcome = LLM_BUDGET_KEPT;
    usage_out->budget_reserved_micros = request_usd_micros;
    snprintf(usage_out->budget_day_key, sizeof(usage_out->budget_day_key),
             "%s", state.day_key);
  }
  rc = LLM_PROVIDER_LIMIT_OK;
  goto done;

fail:
  rc = -1;

done:
  free(text);
  (void)flock(fd, LOCK_UN);
  close(fd);
  return rc;
}

int llm_provider_limit_reserve_at(const LlmProvider *provider, const char *logs_root,
                                  time_t now, LlmUsage *usage_out,
                                  char *raw_response_out, size_t raw_response_out_len) {
  return reserve_at(provider, logs_root, now, 0, 0, usage_out,
                    raw_response_out, raw_response_out_len);
}

int llm_provider_limit_reserve(const LlmProvider *provider, const char *logs_root,
                               LlmUsage *usage_out,
                               char *raw_response_out, size_t raw_response_out_len) {
  return llm_provider_limit_reserve_at(provider, logs_root, time(NULL),
                                       usage_out, raw_response_out, raw_response_out_len);
}

int llm_provider_limit_reserve_request_at(
    const LlmProvider *provider, const LlmProfile *profile,
    const LlmRequest *request, const char *logs_root, time_t now,
    LlmUsage *usage_out, char *raw_response_out,
    size_t raw_response_out_len) {
  LlmPricingCatalog catalog;
  long long input_token_ceiling;
  long long output_token_ceiling;
  long long request_micros = 0;
  size_t text_len;
  int price_rc;
  if (!provider || !profile || !request || !request->text) return -1;
  if (provider->daily_usd_limit < 0.0)
    return llm_provider_limit_reserve_at(provider, logs_root, now, usage_out,
                                         raw_response_out,
                                         raw_response_out_len);
  if (provider->daily_usd_limit == 0.0)
    return reserve_at(provider, logs_root, now, 1, 0, usage_out,
                      raw_response_out, raw_response_out_len);
  if (llm_pricing_catalog_load(&catalog) != 0) {
    /* Not the same thing as an unlisted model: the table itself is missing
     * or malformed, and every capped call fails until it is fixed. */
    fprintf(stderr,
            "llm: pricing table %s is missing or malformed; daily_usd cap on "
            "%s rejects every call until it loads\n",
            llm_pricing_path(), provider->id);
    set_block_reason(usage_out, "daily_usd_pricing_unavailable");
    build_budget_block_json(provider, "daily_usd_pricing_unavailable", 0, 0,
                            raw_response_out, raw_response_out_len);
    return LLM_PROVIDER_LIMIT_BLOCKED_USD;
  }
  if (request->media_count > 0U && catalog.media_input_tokens_per_item <= 0) {
    set_block_reason(usage_out, "daily_usd_unpriced_media");
    build_budget_block_json(provider, "daily_usd_unpriced_media", 0, 0,
                            raw_response_out, raw_response_out_len);
    return LLM_PROVIDER_LIMIT_BLOCKED_USD;
  }
  text_len = strlen(request->text);
  if (text_len > (size_t)(LLONG_MAX - 1024)) return -1;
  /* One token per input byte plus a fixed serializer allowance deliberately
   * over-reserves ordinary text, and each media item takes the table's
   * per-item allowance. A hard local ceiling values safety over perfect
   * utilization: a crash leaves this reservation intact, and a completed
   * call settles it to the provider's reported usage. */
  input_token_ceiling = (long long)text_len + 1024;
  if (request->media_count > 0U) {
    long long media_tokens =
        (long long)request->media_count * catalog.media_input_tokens_per_item;
    if (media_tokens < 0 || media_tokens > LLONG_MAX - input_token_ceiling)
      return -1;
    input_token_ceiling += media_tokens;
  }
  output_token_ceiling = profile->max_output_tokens > 0
                             ? profile->max_output_tokens
                             : LLM_DEFAULT_MAX_OUTPUT_TOKENS;
  price_rc = llm_pricing_max_cost_micros(
      &catalog, provider->id, profile->model, input_token_ceiling,
      output_token_ceiling, &request_micros);
  if (price_rc != 0) {
    set_block_reason(usage_out, "daily_usd_unpriced");
    build_budget_block_json(provider, "daily_usd_unpriced", 0, 0,
                            raw_response_out, raw_response_out_len);
    return LLM_PROVIDER_LIMIT_BLOCKED_USD;
  }
  return reserve_at(provider, logs_root, now, 1, request_micros, usage_out,
                    raw_response_out, raw_response_out_len);
}

/* Replace the reservation in the day ledger with `actual_micros`. If the day
 * rolled between reservation and settlement, the reservation already left
 * with the old day, so only the actual cost is charged to the new one. */
static int adjust_reserved(const LlmProvider *provider, const char *logs_root,
                           time_t now, const char *reserved_day_key,
                           long long reserved_micros, long long actual_micros) {
  ProviderLimitState state;
  char dir_path[LIMIT_PATH_MAX];
  char state_path[LIMIT_PATH_MAX];
  char lock_path[LIMIT_PATH_MAX];
  char *text = NULL;
  int fd = -1;
  int rc = -1;
  const char *root = default_logs_root(logs_root);

  if (!provider || !provider->id[0] || reserved_micros < 0 ||
      actual_micros < 0)
    return -1;
  if (build_limit_paths(root, provider->id, dir_path, sizeof(dir_path),
                        state_path, sizeof(state_path)) != 0) return -1;
  if (snprintf(lock_path, sizeof(lock_path), "%s.lock", state_path) >=
      (int)sizeof(lock_path)) return -1;
  fd = open_lock_file(lock_path);
  if (fd < 0) return -1;

  state_init(&state, provider);
  if (fs_read_text(state_path, &text, 0) != 0 && errno != ENOENT) goto done;
  if (parse_state_text(text, &state) != 0) goto done;
  if (strcmp(state.provider_id, provider->id) != 0 || state.day_count < 0 ||
      state.day_usd_reserved_micros < 0 || state.minute_count < 0)
    goto done;
  state_roll_windows(&state, now);
  if (reserved_day_key && strcmp(state.day_key, reserved_day_key) == 0) {
    state.day_usd_reserved_micros -= reserved_micros;
    if (state.day_usd_reserved_micros < 0) state.day_usd_reserved_micros = 0;
  }
  if (actual_micros > LLONG_MAX - state.day_usd_reserved_micros)
    state.day_usd_reserved_micros = LLONG_MAX;
  else
    state.day_usd_reserved_micros += actual_micros;
  if (write_state_atomic(state_path, &state) != 0) goto done;
  rc = 0;

done:
  free(text);
  (void)flock(fd, LOCK_UN);
  close(fd);
  return rc;
}

int llm_provider_limit_settle_request_at(const LlmProvider *provider,
                                         const LlmProfile *profile,
                                         const char *logs_root, time_t now,
                                         LlmUsage *usage) {
  LlmPricingCatalog catalog;
  long long actual_micros = 0;
  int usage_reported;
  if (!provider || !profile || !usage) return -1;
  if (usage->budget_outcome != LLM_BUDGET_KEPT) return 0;
  usage_reported = usage->input_tokens > 0 || usage->output_tokens > 0;
  if (!usage_reported && usage->request_dispatched) return 0;
  if (usage_reported) {
    if (llm_pricing_catalog_load(&catalog) != 0 ||
        llm_pricing_usage_micros(
            &catalog, provider->id, profile->model, usage->input_tokens,
            usage->output_tokens, usage->provider_cached_input_tokens,
            usage->provider_cached_input_tokens_reported,
            usage->provider_cache_creation_input_tokens,
            usage->provider_cache_creation_input_tokens_reported,
            &actual_micros) != 0)
      return -1;
  }
  if (adjust_reserved(provider, logs_root, now, usage->budget_day_key,
                      usage->budget_reserved_micros, actual_micros) != 0)
    return -1;
  usage->budget_settled_micros = actual_micros;
  usage->budget_outcome =
      usage_reported ? LLM_BUDGET_SETTLED : LLM_BUDGET_RELEASED;
  return 0;
}

int llm_provider_limit_settle_request(const LlmProvider *provider,
                                      const LlmProfile *profile,
                                      const char *logs_root,
                                      LlmUsage *usage) {
  return llm_provider_limit_settle_request_at(provider, profile, logs_root,
                                              time(NULL), usage);
}

int llm_provider_limit_reserve_request(
    const LlmProvider *provider, const LlmProfile *profile,
    const LlmRequest *request, const char *logs_root, LlmUsage *usage_out,
    char *raw_response_out, size_t raw_response_out_len) {
  return llm_provider_limit_reserve_request_at(
      provider, profile, request, logs_root, time(NULL), usage_out,
      raw_response_out, raw_response_out_len);
}
