#include "provider_limit.h"

#include "../support/fsutil.h"
#include "../support/json.h"

#include <errno.h>
#include <fcntl.h>
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

static int read_fd_text(int fd, char **out_text) {
  struct stat st;
  char *buf = NULL;
  ssize_t nread;
  if (!out_text) return -1;
  *out_text = NULL;
  if (fstat(fd, &st) != 0) return -1;
  if (st.st_size <= 0) {
    buf = (char *)malloc(1);
    if (!buf) return -1;
    buf[0] = '\0';
    *out_text = buf;
    return 0;
  }
  buf = (char *)malloc((size_t)st.st_size + 1U);
  if (!buf) return -1;
  if (lseek(fd, 0, SEEK_SET) < 0) { free(buf); return -1; }
  nread = read(fd, buf, (size_t)st.st_size);
  if (nread < 0 || nread != st.st_size) { free(buf); return -1; }
  buf[nread] = '\0';
  *out_text = buf;
  return 0;
}

static void state_init(ProviderLimitState *state, const LlmProvider *provider) {
  if (!state || !provider) return;
  memset(state, 0, sizeof(*state));
  snprintf(state->provider_id, sizeof(state->provider_id), "%s", provider->id);
}

static void state_roll_windows(ProviderLimitState *state, time_t now) {
  char day_key[16];
  long long minute_key;
  if (!state) return;
  if (current_day_key(now, day_key, sizeof(day_key)) != 0) day_key[0] = '\0';
  minute_key = (long long)(now / 60);
  if (day_key[0] == '\0' || strcmp(state->day_key, day_key) != 0) {
    snprintf(state->day_key, sizeof(state->day_key), "%s", day_key);
    state->day_count = 0;
  }
  if (state->minute_key != minute_key) {
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
  if (json_ref_object_get_long(&root, "minute_key", &value) == 0) state->minute_key = value;
  if (json_ref_object_get_long(&root, "minute_count", &value) == 0) state->minute_count = value;
  return 0;
}

static int write_state_fd(int fd, const ProviderLimitState *state) {
  char json[512];
  size_t len;
  if (!state) return -1;
  if (snprintf(json, sizeof(json),
               "{"
               "\"provider_id\":\"%s\","
               "\"day_key\":\"%s\","
               "\"day_count\":%lld,"
               "\"minute_key\":%lld,"
               "\"minute_count\":%lld"
               "}\n",
               state->provider_id,
               state->day_key,
               state->day_count,
               state->minute_key,
               state->minute_count) >= (int)sizeof(json)) return -1;
  len = strlen(json);
  if (ftruncate(fd, 0) != 0) return -1;
  if (lseek(fd, 0, SEEK_SET) < 0) return -1;
  if (write(fd, json, len) != (ssize_t)len) return -1;
  return fsync(fd);
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

static int open_state_file(const char *path) {
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

int llm_provider_limit_reserve_at(const LlmProvider *provider, const char *logs_root,
                                  time_t now, LlmUsage *usage_out,
                                  char *raw_response_out, size_t raw_response_out_len) {
  ProviderLimitState state;
  char dir_path[LIMIT_PATH_MAX];
  char state_path[LIMIT_PATH_MAX];
  char *text = NULL;
  int fd = -1;
  int rc = LLM_PROVIDER_LIMIT_OK;
  const char *root = default_logs_root(logs_root);

  if (!provider || !provider->id[0]) return -1;
  if (usage_out) usage_out->local_limit_reason[0] = '\0';
  if (raw_response_out && raw_response_out_len > 0) raw_response_out[0] = '\0';

  rc = limit_disabled(provider, usage_out, raw_response_out, raw_response_out_len);
  if (rc != LLM_PROVIDER_LIMIT_OK) return rc;
  if (provider->rpm_limit < 0 && provider->daily_limit < 0) return LLM_PROVIDER_LIMIT_OK;

  if (build_limit_paths(root, provider->id, dir_path, sizeof(dir_path),
                        state_path, sizeof(state_path)) != 0) return -1;
  if (fs_mkdir_p(dir_path) != 0) return -1;

  fd = open_state_file(state_path);
  if (fd < 0) return -1;

  state_init(&state, provider);
  if (read_fd_text(fd, &text) != 0) goto fail;
  if (parse_state_text(text, &state) != 0) goto fail;
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

  state.day_count++;
  state.minute_count++;
  if (write_state_fd(fd, &state) != 0) goto fail;
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

int llm_provider_limit_reserve(const LlmProvider *provider, const char *logs_root,
                               LlmUsage *usage_out,
                               char *raw_response_out, size_t raw_response_out_len) {
  return llm_provider_limit_reserve_at(provider, logs_root, time(NULL),
                                       usage_out, raw_response_out, raw_response_out_len);
}
