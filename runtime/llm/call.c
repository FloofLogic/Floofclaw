#include "llm.h"
#include "provider_limit.h"
#include "request.h"

#include "../support/fsutil.h"
#include "../support/json.h"
#include "../support/timing.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int next_provider_seq(const char *dir) {
  /* Cheap: just read the directory and count entries. */
  char *(unused);
  (void)unused;
  /* Use stat-loop: try seq=1..999, stop when none exists. */
  int seq = 1;
  for (; seq < 999; ++seq) {
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/%03d_request.json", dir, seq);
    if (!fs_file_exists(probe)) return seq;
  }
  return seq;
}

static int write_artifact_bytes(const char *path, const char *data, size_t len) {
  int flags = O_WRONLY | O_CREAT | O_TRUNC;
  int fd;
  size_t off = 0U;
  if (!path || (!data && len != 0U)) return -1;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  fd = open(path, flags, 0600);
  if (fd < 0) return -1;
  if (fchmod(fd, 0600) != 0) {
    int saved_errno = errno;
    (void)close(fd);
    (void)unlink(path);
    errno = saved_errno;
    return -1;
  }
  while (off < len) {
    ssize_t wrote;
    do {
      wrote = write(fd, data + off, len - off);
    } while (wrote < 0 && errno == EINTR);
    if (wrote <= 0) {
      int saved_errno = wrote < 0 ? errno : EIO;
      (void)close(fd);
      (void)unlink(path);
      errno = saved_errno;
      return -1;
    }
    off += (size_t)wrote;
  }
  if (close(fd) != 0) {
    int saved_errno = errno;
    (void)unlink(path);
    errno = saved_errno;
    return -1;
  }
  return 0;
}

static int request_media_belongs_to_run(const char *run_dir,
                                        const LlmRequest *request,
                                        char *err, size_t err_len) {
  char prefix[PATH_MAX];
  struct stat dir_stat;
  size_t prefix_len;
  if (!request || request->media_count == 0U) return 0;
  if (snprintf(prefix, sizeof(prefix), "%s/media/", run_dir) >=
          (int)sizeof(prefix)) {
    if (err) snprintf(err, err_len, "run media path exceeds boundary");
    return -1;
  }
  prefix_len = strlen(prefix);
  prefix[prefix_len - 1U] = '\0';
  if (lstat(prefix, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode) ||
      dir_stat.st_uid != getuid()) {
    if (err) snprintf(err, err_len, "run media directory is unsafe");
    return -1;
  }
  prefix[prefix_len - 1U] = '/';
  for (size_t i = 0; i < request->media_count; ++i) {
    const char *leaf;
    if (strncmp(request->media[i].path, prefix, prefix_len) != 0) {
      if (err)
        snprintf(err, err_len,
                 "media item %zu path is outside the run media directory", i);
      return -1;
    }
    leaf = request->media[i].path + prefix_len;
    if (!*leaf || strchr(leaf, '/') || strcmp(leaf, ".") == 0 ||
        strcmp(leaf, "..") == 0) {
      if (err) snprintf(err, err_len, "media item %zu path is invalid", i);
      return -1;
    }
  }
  return 0;
}

static void append_repair_name(char *out, size_t out_len, size_t *pos,
                               const char *name, int *first) {
  int n;
  if (!out || !pos || !name || !first || *pos >= out_len) return;
  n = snprintf(out + *pos, out_len - *pos, "%s\"%s\"", *first ? "" : ",", name);
  if (n > 0 && (size_t)n < out_len - *pos) {
    *pos += (size_t)n;
    *first = 0;
  }
}

static void build_json_repair_meta(const LlmUsage *usage, char *out, size_t out_len) {
  size_t pos = 0;
  int first = 1;
  const FjrReport *r;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!usage || !usage->json_repair_checked) {
    snprintf(out, out_len, "{\"status\":\"not_checked\"}");
    return;
  }
  r = &usage->json_repair_report;
  pos += (size_t)snprintf(out + pos, out_len - pos,
                          "{\"diagnostic\":\"%s\",\"status\":\"%s\","
                          "\"raw_len\":%zu,\"repaired_len\":%zu,\"repairs\":[",
                          usage->json_repair_status == FJR_OK_REPAIRED ? "llm_json_repaired" : "llm_json_checked",
                          usage->json_repair_status == FJR_OK_REPAIRED ? "repaired" :
                          usage->json_repair_status == FJR_OK_UNCHANGED ? "unchanged" : "rejected",
                          usage->json_repair_raw_len,
                          usage->json_repair_repaired_len);
  if (pos >= out_len) { out[out_len - 1] = '\0'; return; }
  if (r->stripped_fence) append_repair_name(out, out_len, &pos, "stripped_fence", &first);
  if (r->stripped_prose) append_repair_name(out, out_len, &pos, "stripped_prose", &first);
  if (r->added_missing_quote) append_repair_name(out, out_len, &pos, "added_missing_quote", &first);
  if (r->added_missing_comma) append_repair_name(out, out_len, &pos, "added_missing_comma", &first);
  if (r->removed_trailing_comma) append_repair_name(out, out_len, &pos, "removed_trailing_comma", &first);
  if (r->closed_string) append_repair_name(out, out_len, &pos, "closed_string", &first);
  if (r->closed_object) append_repair_name(out, out_len, &pos, "closed_object", &first);
  if (r->closed_array) append_repair_name(out, out_len, &pos, "closed_array", &first);
  if (r->substituted_wrong_closer) append_repair_name(out, out_len, &pos, "substituted_wrong_closer", &first);
  if (r->converted_single_quotes) append_repair_name(out, out_len, &pos, "converted_single_quotes", &first);
  if (r->replaced_invalid_literal) append_repair_name(out, out_len, &pos, "replaced_invalid_literal", &first);
  if (r->trimmed_trailing_junk) append_repair_name(out, out_len, &pos, "trimmed_trailing_junk", &first);
  if (r->stripped_comments) append_repair_name(out, out_len, &pos, "stripped_comments", &first);
  (void)snprintf(out + pos, out_len - pos,
                 "],\"repair_count\":%d,\"candidate_offset\":%zu,"
                 "\"candidate_len\":%zu,\"error_offset\":%zu}",
                 r->repairs, r->candidate_offset, r->candidate_len, r->error_offset);
}

static void write_provider_meta(const char *path, const char *floop,
                                const char *agent_id,
                                const LlmProfile *profile, const LlmProvider *provider,
                                const LlmUsage *usage, int rc) {
  char floop_esc[LLM_NAME_MAX * 2];
  char agent_esc[LLM_ID_MAX * 2];
  char profile_esc[LLM_ID_MAX * 2];
  char provider_esc[LLM_ID_MAX * 2];
  char kind_esc[LLM_ID_MAX * 2];
  char model_esc[LLM_NAME_MAX * 2];
  char url_esc[LLM_PATH_MAX * 2];
  char local_limit_reason_esc[LLM_ID_MAX * 2];
  char local_limit_reason_json[(LLM_ID_MAX * 2) + 4];
  char provider_cached_input_tokens_json[64];
  char repair_meta[1024];
  char out[4096];
  if (!path || !floop || !*floop || !agent_id || !profile || !provider ||
      !usage)
    return;
  if (json_escape(floop, floop_esc, sizeof(floop_esc)) != 0)
    floop_esc[0] = '\0';
  if (json_escape(agent_id, agent_esc, sizeof(agent_esc)) != 0) agent_esc[0] = '\0';
  if (json_escape(profile->id, profile_esc, sizeof(profile_esc)) != 0) profile_esc[0] = '\0';
  if (json_escape(provider->id, provider_esc, sizeof(provider_esc)) != 0) provider_esc[0] = '\0';
  if (json_escape(provider->kind, kind_esc, sizeof(kind_esc)) != 0) kind_esc[0] = '\0';
  if (json_escape(profile->model, model_esc, sizeof(model_esc)) != 0) model_esc[0] = '\0';
  if (json_escape(provider->url, url_esc, sizeof(url_esc)) != 0) url_esc[0] = '\0';
  if (json_escape(usage->local_limit_reason, local_limit_reason_esc, sizeof(local_limit_reason_esc)) != 0)
    local_limit_reason_esc[0] = '\0';
  if (usage->local_limit_reason[0]) {
    snprintf(local_limit_reason_json, sizeof(local_limit_reason_json),
             "\"%s\"", local_limit_reason_esc);
  } else {
    snprintf(local_limit_reason_json, sizeof(local_limit_reason_json), "null");
  }
  if (usage->provider_cached_input_tokens_reported) {
    snprintf(provider_cached_input_tokens_json,
             sizeof(provider_cached_input_tokens_json), "%lld",
             usage->provider_cached_input_tokens);
  } else {
    snprintf(provider_cached_input_tokens_json,
             sizeof(provider_cached_input_tokens_json), "null");
  }
  build_json_repair_meta(usage, repair_meta, sizeof(repair_meta));
  snprintf(out, sizeof(out),
           "{"
           "\"floop\":\"%s\","
           "\"agent\":\"%s\","
           "\"profile\":\"%s\","
           "\"provider\":\"%s\","
           "\"provider_kind\":\"%s\","
           "\"model\":\"%s\","
           "\"url\":\"%s\","
           "\"ok\":%s,"
           "\"input_chars\":%lld,"
           "\"input_media_count\":%zu,"
           "\"input_media_bytes\":%llu,"
           "\"output_chars\":%lld,"
           "\"input_tokens\":%lld,"
           "\"output_tokens\":%lld,"
           "\"total_tokens\":%lld,"
           "\"provider_cached_input_tokens\":%s,"
           "\"duration_ms\":%lld,"
           "\"local_limit_blocked\":%s,"
           "\"local_limit_reason\":%s,"
           "\"json_repair\":%s"
           "}\n",
           floop_esc, agent_esc, profile_esc, provider_esc, kind_esc,
           model_esc, url_esc,
           rc == 0 ? "true" : "false",
           usage->input_chars, usage->input_media_count,
           (unsigned long long)usage->input_media_bytes,
           usage->output_chars,
           usage->input_tokens, usage->output_tokens, usage->total_tokens,
           provider_cached_input_tokens_json,
           usage->duration_ms,
           usage->local_limit_reason[0] ? "true" : "false",
           local_limit_reason_json,
           repair_meta);
  (void)fs_write_text(path, out);
}

int llm_call_request(const char *run_dir, const char *floop,
                     const char *agent_id, const LlmProfile *profile,
                     const LlmRequest *request, char *response_out,
                     size_t response_out_len) {
  LlmProvider provider;
  LlmUsage usage;
  char calls_dir[PATH_MAX];
  char req_path[PATH_MAX];
  char resp_path[PATH_MAX];
  char raw_req_path[PATH_MAX];
  char raw_resp_path[PATH_MAX];
  char meta_path[PATH_MAX];
  char text_request_body[LLM_PROMPT_MAX + 1024];
  char raw_response[LLM_RESP_MAX];
  char request_err[512];
  const char *raw_request = NULL;
  size_t raw_request_len = 0U;
  LlmRequestBody request_body = {NULL, 0U, 0};
  int raw_request_saved = 0;
  const char *run_id;
  int seq;
  uint64_t t0;
  int rc;

  if (!run_dir || !floop || !*floop || !agent_id || !profile || !request ||
      !request->text || !response_out || response_out_len == 0)
    return -1;
  run_id = strrchr(run_dir, '/');
  run_id = run_id ? run_id + 1 : run_dir;
  if (!*run_id) return -1;
  response_out[0] = '\0';
  memset(&usage, 0, sizeof(usage));

  if (llm_load_provider(profile->provider, &provider) != 0) {
    fprintf(stderr, "llm: unknown provider %s\n", profile->provider);
    return -1;
  }

  snprintf(calls_dir, sizeof(calls_dir), "%s/provider_calls", run_dir);
  if (fs_mkdir_p(calls_dir) != 0) return -1;
  seq = next_provider_seq(calls_dir);
  snprintf(req_path, sizeof(req_path), "%s/%03d_request.json", calls_dir, seq);
  snprintf(resp_path, sizeof(resp_path), "%s/%03d_response.json", calls_dir, seq);
  snprintf(raw_req_path, sizeof(raw_req_path), "%s/%03d_raw_request.json", calls_dir, seq);
  snprintf(raw_resp_path, sizeof(raw_resp_path), "%s/%03d_raw_response.json", calls_dir, seq);
  snprintf(meta_path, sizeof(meta_path), "%s/%03d_meta.json", calls_dir, seq);
  raw_response[0] = '\0';

  /* Save raw outbound prompt. Secrets are never embedded in `prompt` itself —
   * the provider's auth header is built at send time elsewhere and never
   * written into this artifact. */
  (void)fs_write_text(req_path, request->text);

  t0 = timing_monotonic_ms();
  if (strcmp(provider.kind, "mock") == 0) {
    if (request->media_count > 0U) {
      fprintf(stderr,
              "llm: mock provider does not support media; "
              "use a loopback fake HTTP provider for media tests\n");
      rc = -1;
    } else {
      rc = llm_mock_call(request->text, response_out, response_out_len);
      raw_request = request->text;
      raw_request_len = strlen(request->text);
      snprintf(raw_response, sizeof(raw_response), "%s", response_out);
    }
  } else if (strcmp(provider.kind, "http") == 0 ||
             strcmp(provider.kind, "openai_compat") == 0 ||
             strcmp(provider.kind, "openai_responses") == 0 ||
             strcmp(provider.kind, "gemini_key") == 0) {
    int limit_rc;
    if (llm_request_validate(provider.kind, request,
                             request_err, sizeof(request_err)) != 0 ||
        request_media_belongs_to_run(run_dir, request,
                                     request_err, sizeof(request_err)) != 0) {
      rc = -1;
      fprintf(stderr, "llm: %s\n",
              request_err[0] ? request_err : "invalid media request");
    } else {
      limit_rc = llm_provider_limit_reserve(&provider, "workspace/logs", &usage,
                                            raw_response, sizeof(raw_response));
      if (limit_rc == LLM_PROVIDER_LIMIT_OK) {
        if (llm_request_body_build(profile, provider.kind, request,
                                   text_request_body, sizeof(text_request_body),
                                   &request_body,
                                   request_err, sizeof(request_err)) != 0) {
          rc = -1;
          fprintf(stderr, "llm: %s\n",
                  request_err[0] ? request_err : "request body build failed");
        } else {
          raw_request = request_body.data;
          raw_request_len = request_body.len;
          if (write_artifact_bytes(raw_req_path, raw_request,
                                   raw_request_len) != 0) {
            rc = -1;
            fprintf(stderr,
                    "llm: cannot preserve raw request artifact; "
                    "provider call was not sent\n");
          } else {
            raw_request_saved = 1;
            rc = llm_http_call(profile, &provider,
                               request_body.data, request_body.len,
                               request->media_count > 0U
                                   ? LLM_HTTP_MEDIA_RETRY_CAP : -1,
                               response_out, response_out_len,
                               raw_response, sizeof(raw_response),
                               &usage);
          }
        }
      } else if (limit_rc == LLM_PROVIDER_LIMIT_BLOCKED_RPM ||
                 limit_rc == LLM_PROVIDER_LIMIT_BLOCKED_DAILY) {
        rc = -1;
      } else {
        rc = -1;
        fprintf(stderr, "llm: provider limit preflight failed for %s\n", provider.id);
      }
    }
  } else {
    fprintf(stderr, "llm: provider kind '%s' not implemented yet\n", provider.kind);
    rc = -1;
  }
  {
    uint64_t finished = timing_monotonic_ms();
    usage.duration_ms = finished >= t0 ? (long long)(finished - t0) : 0;
  }
  usage.input_chars = (long long)strlen(request->text);
  usage.input_media_count = request->media_count;
  if (request->media && request->media_count <= LLM_MEDIA_MAX_COUNT) {
    for (size_t i = 0; i < request->media_count; ++i) {
      if (UINT64_MAX - usage.input_media_bytes <
          request->media[i].size_bytes) {
        usage.input_media_bytes = UINT64_MAX;
        break;
      }
      usage.input_media_bytes += request->media[i].size_bytes;
    }
  }
  usage.output_chars = (long long)strlen(response_out);

  if (rc == 0) (void)fs_write_text(resp_path, response_out);
  if (!raw_request_saved && raw_request && raw_request_len > 0U)
    (void)write_artifact_bytes(raw_req_path, raw_request, raw_request_len);
  if (raw_response[0]) (void)fs_write_text(raw_resp_path, raw_response);
  if (rc != 0 && usage.json_repair_checked)
    fprintf(stderr, "llm: json rejected by output contract\n");
  write_provider_meta(meta_path, floop, agent_id, profile, &provider, &usage,
                      rc);
  llm_record_usage(floop, run_id, seq, agent_id, profile, &usage);
  llm_request_body_free(&request_body);
  return rc;
}

int llm_call(const char *run_dir, const char *floop, const char *agent_id,
             const LlmProfile *profile, const char *prompt,
             char *response_out, size_t response_out_len) {
  LlmRequest request = {prompt, NULL, 0U};
  return llm_call_request(run_dir, floop, agent_id, profile, &request,
                          response_out, response_out_len);
}
