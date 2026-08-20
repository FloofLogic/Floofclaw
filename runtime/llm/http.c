/* HTTP provider for runtime/llm/. Uses libcurl easy mode (blocking).
 *
 * This blocking code only ever runs inside an LLM agent-job
 * subprocess, never inline in the gateway reactor. The reactor sees a pid
 * + two pipes; this file's blocking is invisible to it.
 *
 * v1 supports Anthropic Messages, OpenAI-compatible Chat Completions,
 * OpenAI-compatible Responses, and Gemini generateContent. */

#include "llm.h"
#include "stream.h"

#include "../secure/auth.h"
#include "../support/fsutil.h"
#include "../support/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef FCLAW_HAVE_LIBCURL
#include <curl/curl.h>

typedef struct {
  char *buf;
  size_t cap;
  size_t used;
} HttpResponseBuf;

/* The persisted raw-response slot is LLM_RESP_MAX bytes. Refuse an
 * oversized body instead of allocating without bound and silently losing
 * the tail needed to normalize or audit the response. */
#define LLM_HTTP_RESPONSE_CAP (LLM_RESP_MAX - 1)

/* §5.E: transport/provider errors worth a bounded retry — timeouts,
 * connection drops, truncated streams, and the standard "try again"
 * HTTP statuses. A non-retryable 4xx (bad request, auth) or a clean
 * parse failure fails fast as before. This runs in the LLM child
 * process, not the reactor, so a blocking backoff sleep is fine
 * (Principle #3 is about reactor callbacks, not subprocesses). */
static int llm_http_retryable(CURLcode rc, long http_status) {
  if (rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_COULDNT_CONNECT ||
      rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
      rc == CURLE_GOT_NOTHING || rc == CURLE_PARTIAL_FILE)
    return 1;
  if (rc == CURLE_OK &&
      (http_status == 429 || http_status == 500 || http_status == 502 ||
       http_status == 503 || http_status == 504))
    return 1;
  return 0;
}

/* Retry budget vs the caller's budget.
 *
 * The agent job is SIGTERM'd at RT_AGENT_TIMEOUT_MS_DEFAULT. Everything
 * this layer does happens inside that, so the worst case here --
 * (1 + retries) attempts plus the backoff between them -- must fit, with
 * room left for the agent's own input/output work. It did not: a 90s
 * per-attempt cap with 3 retries is 363s against a 120s budget, so a call
 * that hung was aborted at 90s, waited 500ms, started its retry, and was
 * killed mid-flight at 120s. The retry could never finish, and the user
 * got a bare agent_timeout with nothing written.
 *
 * The cap is sized from measured behaviour, not guessed: across observed
 * gemini-3-flash-preview traffic the successful calls were p50 5.2s, p90
 * 19.3s, with nothing at all between 20s and 95s. The apparent 95-100s
 * "slow" calls were hangs already rescued by a retry. So a hang is a
 * hang -- cap the attempt well above the real tail and let the retry do
 * the work it was always meant to do. */
#define LLM_HTTP_ATTEMPT_TIMEOUT_S 25
#define LLM_HTTP_MAX_RETRIES_DEFAULT 3
/* 500 + 1000 + 2000, the backoff for three retries. */
#define LLM_HTTP_BACKOFF_TOTAL_MS 3500
/* Leave the agent room to read its input and write its artifacts. */
#define LLM_HTTP_AGENT_RESERVE_MS 10000

#define LLM_HTTP_WORST_CASE_MS(retries) \
  (((retries) + 1) * LLM_HTTP_ATTEMPT_TIMEOUT_S * 1000 + LLM_HTTP_BACKOFF_TOTAL_MS)

_Static_assert(LLM_HTTP_WORST_CASE_MS(LLM_HTTP_MAX_RETRIES_DEFAULT) +
                   LLM_HTTP_AGENT_RESERVE_MS <=
                       LLM_AGENT_JOB_BUDGET_MS,
               "llm http retry budget must fit inside the agent job budget "
               "(RT_AGENT_TIMEOUT_MS_DEFAULT); otherwise a retry is killed "
               "mid-flight and the caller sees only agent_timeout");
_Static_assert(LLM_MEDIA_DOWNLOAD_BUDGET_MS +
                   LLM_HTTP_WORST_CASE_MS(LLM_HTTP_MEDIA_RETRY_CAP) +
                   LLM_HTTP_AGENT_RESERVE_MS <=
                       LLM_AGENT_JOB_BUDGET_MS,
               "media download and provider retry budgets must fit inside "
               "the owning agent job");

/* Worst case for a given attempt timeout, mirroring
 * LLM_HTTP_WORST_CASE_MS but usable with the env-overridden timeout. */
static int llm_http_worst_case_ms(int retries, int attempt_timeout_s) {
  return (retries + 1) * attempt_timeout_s * 1000 + LLM_HTTP_BACKOFF_TOTAL_MS;
}

/* The measured 25s cap fits hosted providers; a local model (LM Studio,
 * vLLM, ollama) legitimately needs longer per attempt on modest
 * hardware. FCLAW_LLM_ATTEMPT_TIMEOUT_S raises it; the clamps below
 * shed retries first and then the timeout itself so no override can
 * reintroduce the killed-mid-retry defect the constants exist to
 * prevent. */
static int llm_http_requested_attempt_timeout_s(void) {
  int t = LLM_HTTP_ATTEMPT_TIMEOUT_S;
  const char *e = getenv("FCLAW_LLM_ATTEMPT_TIMEOUT_S");
  if (e && *e) {
    int requested = atoi(e);
    if (requested >= 5 && requested <= 600) t = requested;
  }
  return t;
}

int llm_http_effective_max_retries(void) {
  int t = llm_http_requested_attempt_timeout_s();
  int v = LLM_HTTP_MAX_RETRIES_DEFAULT;
  const char *e = getenv("FCLAW_LLM_MAX_RETRIES");
  if (e && *e) {
    int requested = atoi(e);
    if (requested >= 0 && requested <= 10) v = requested;
  }
  /* An override must not be able to reintroduce the defect. Clamp until
   * the worst case fits the agent budget again. */
  while (v > 0 &&
         llm_http_worst_case_ms(v, t) + LLM_HTTP_AGENT_RESERVE_MS >
             LLM_AGENT_JOB_BUDGET_MS)
    v--;
  return v;
}

int llm_http_effective_attempt_timeout_s(void) {
  int t = llm_http_requested_attempt_timeout_s();
  if (llm_http_worst_case_ms(0, t) + LLM_HTTP_AGENT_RESERVE_MS >
      LLM_AGENT_JOB_BUDGET_MS)
    t = (LLM_AGENT_JOB_BUDGET_MS - LLM_HTTP_AGENT_RESERVE_MS -
         LLM_HTTP_BACKOFF_TOTAL_MS) / 1000;
  return t;
}

static int append_http_header(struct curl_slist **headers,
                              const char *value) {
  struct curl_slist *appended;
  if (!headers || !value) return -1;
  appended = curl_slist_append(*headers, value);
  if (!appended) return -1;
  *headers = appended;
  return 0;
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  HttpResponseBuf *r = (HttpResponseBuf *)userdata;
  size_t want = size * nmemb;
  if (want > LLM_HTTP_RESPONSE_CAP ||
      r->used > LLM_HTTP_RESPONSE_CAP - want)
    return 0;
  if (r->used + want + 1 > r->cap) {
    size_t newcap = r->cap ? r->cap * 2 : 8192;
    while (newcap < r->used + want + 1) newcap *= 2;
    char *nb = (char *)realloc(r->buf, newcap);
    if (!nb) return 0;
    r->buf = nb;
    r->cap = newcap;
  }
  memcpy(r->buf + r->used, ptr, want);
  r->used += want;
  r->buf[r->used] = '\0';
  return want;
}

/* Extract content[0].text from an Anthropic Messages response. */
static int extract_anthropic_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, content_arr, first;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "content", &content_arr) != 0) return -1;
  if (json_ref_array_get(&content_arr, 0, &first) != 0) return -1;
  return json_ref_object_get_string(&first, "text", out, out_len);
}

/* Extract choices[0].message.content from an OpenAI-compatible Chat response. */
static int extract_openai_compat_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, choices_arr, first, message;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "choices", &choices_arr) != 0) return -1;
  if (json_ref_array_get(&choices_arr, 0, &first) != 0) return -1;
  if (json_ref_object_get_object(&first, "message", &message) != 0) return -1;
  return json_ref_object_get_string(&message, "content", out, out_len);
}

/* Extract output[0].content[0].text from an OpenAI Responses response. */
static int extract_openai_responses_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, output_arr;
  size_t n;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "output", &output_arr) != 0) return -1;
  n = json_ref_array_size(&output_arr);
  for (size_t i = 0; i < n; ++i) {
    JsonRef item, content_arr;
    char type[LLM_ID_MAX] = "";
    if (json_ref_array_get(&output_arr, i, &item) != 0) continue;
    (void)json_ref_object_get_string(&item, "type", type, sizeof(type));
    if (strcmp(type, "message") != 0) continue;
    if (json_ref_object_get_array(&item, "content", &content_arr) != 0) continue;
    if (json_ref_array_size(&content_arr) > 0) {
      JsonRef content;
      if (json_ref_array_get(&content_arr, 0, &content) == 0 &&
          json_ref_object_get_string(&content, "text", out, out_len) == 0) {
        return 0;
      }
    }
  }
  return -1;
}

/* Extract candidates[0].content.parts[0].text from a Gemini response. */
static int extract_gemini_text(const char *response_json, char *out, size_t out_len) {
  JsonRef root, candidates_arr, candidate, content, parts, part;
  if (json_ref_first_object(response_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "candidates", &candidates_arr) != 0) return -1;
  if (json_ref_array_get(&candidates_arr, 0, &candidate) != 0) return -1;
  if (json_ref_object_get_object(&candidate, "content", &content) != 0) return -1;
  if (json_ref_object_get_array(&content, "parts", &parts) != 0) return -1;
  if (json_ref_array_get(&parts, 0, &part) != 0) return -1;
  return json_ref_object_get_string(&part, "text", out, out_len);
}

static int build_request_url(const LlmProfile *profile, const LlmProvider *provider,
                             int streaming,
                             char *url_out, size_t url_out_len) {
  if (!profile || !provider || !url_out || url_out_len == 0) return -1;
  if (strcmp(provider->kind, "gemini_key") == 0) {
    return snprintf(url_out, url_out_len, "%s/models/%s:%s%s",
                    provider->url, profile->model,
                    streaming ? "streamGenerateContent" : "generateContent",
                    streaming ? "?alt=sse" : "") < (int)url_out_len ? 0 : -1;
  }
  return snprintf(url_out, url_out_len, "%s", provider->url) < (int)url_out_len ? 0 : -1;
}

static void copy_raw(const char *src, char *out, size_t out_len) {
  if (!out || out_len == 0) return;
  if (!src) src = "";
  snprintf(out, out_len, "%s", src);
}

typedef struct {
  HttpResponseBuf raw;
  char line[LLM_RESP_MAX];
  size_t line_len;
  char model_text[LLM_RESP_MAX];
  size_t model_len;
  LlmMessageStream *decoder;
  LlmUsage *usage;
  int failed;
} GeminiStreamState;

static int gemini_stream_process_line(GeminiStreamState *s) {
  char *payload;
  char delta[LLM_RESP_MAX];
  size_t len;
  if (!s) return -1;
  s->line[s->line_len] = '\0';
  while (s->line_len > 0 &&
         (s->line[s->line_len - 1] == '\r' ||
          s->line[s->line_len - 1] == '\n'))
    s->line[--s->line_len] = '\0';
  if (s->line_len == 0 || s->line[0] == ':') return 0;
  if (strncmp(s->line, "data:", 5) != 0) return 0;
  payload = s->line + 5;
  while (*payload == ' ' || *payload == '\t') payload++;
  if (!*payload || strcmp(payload, "[DONE]") == 0) return 0;
  if (s->usage) llm_extract_usage("gemini_key", payload, s->usage);
  if (extract_gemini_text(payload, delta, sizeof(delta)) != 0)
    return 0; /* usage-only or provider metadata event */
  len = strlen(delta);
  if (s->model_len + len + 1 > sizeof(s->model_text)) return -1;
  memcpy(s->model_text + s->model_len, delta, len);
  s->model_len += len;
  s->model_text[s->model_len] = '\0';
  if (s->decoder &&
      llm_message_stream_feed(s->decoder, delta, len) != 0)
    return -1;
  return 0;
}

static size_t gemini_stream_write_cb(void *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  GeminiStreamState *s = (GeminiStreamState *)userdata;
  const char *bytes = (const char *)ptr;
  size_t want = size * nmemb;
  size_t i = 0;
  if (!s || s->failed) return 0;
  if (want > LLM_HTTP_RESPONSE_CAP ||
      s->raw.used > LLM_HTTP_RESPONSE_CAP - want) {
    s->failed = 1;
    return 0;
  }
  if (s->raw.used + want + 1 > s->raw.cap) {
    size_t newcap = s->raw.cap ? s->raw.cap * 2 : 8192;
    char *nb;
    while (newcap < s->raw.used + want + 1) newcap *= 2;
    nb = (char *)realloc(s->raw.buf, newcap);
    if (!nb) { s->failed = 1; return 0; }
    s->raw.buf = nb;
    s->raw.cap = newcap;
  }
  memcpy(s->raw.buf + s->raw.used, bytes, want);
  s->raw.used += want;
  s->raw.buf[s->raw.used] = '\0';
  while (i < want) {
    const char *nl = memchr(bytes + i, '\n', want - i);
    size_t part = nl ? (size_t)(nl - (bytes + i)) : want - i;
    if (s->line_len + part + 1 > sizeof(s->line)) {
      s->failed = 1;
      return 0;
    }
    memcpy(s->line + s->line_len, bytes + i, part);
    s->line_len += part;
    if (!nl) break;
    if (gemini_stream_process_line(s) != 0) {
      s->failed = 1;
      return 0;
    }
    s->line_len = 0;
    i += part + 1;
  }
  return want;
}

static int gemini_stream_reset(GeminiStreamState *s) {
  if (!s) return -1;
  free(s->raw.buf);
  s->raw.buf = NULL;
  s->raw.cap = s->raw.used = 0;
  s->line_len = 0;
  s->model_len = 0;
  s->model_text[0] = '\0';
  s->failed = 0;
  if (s->decoder) llm_message_stream_destroy(s->decoder);
  s->decoder = llm_message_stream_create(llm_progress_fd_sink, NULL);
  if (!s->decoder) return -1;
  if (s->usage) {
    s->usage->input_tokens = 0;
    s->usage->output_tokens = 0;
    s->usage->total_tokens = 0;
    s->usage->provider_cached_input_tokens = 0;
    s->usage->provider_cached_input_tokens_reported = 0;
  }
  return 0;
}

static int gemini_stream_finish(GeminiStreamState *s) {
  if (!s || s->failed) return -1;
  if (s->line_len > 0) {
    if (gemini_stream_process_line(s) != 0) return -1;
    s->line_len = 0;
  }
  return s->model_len > 0 ? 0 : -1;
}

int llm_http_call(const LlmProfile *profile, const LlmProvider *provider,
                  const char *request_body, size_t request_body_len,
                  int retry_cap,
                  char *response_out, size_t response_out_len,
                  char *raw_response_out, size_t raw_response_out_len,
                  LlmUsage *usage_out) {
  CURL *curl;
  CURLcode rc;
  struct curl_slist *headers = NULL;
  char auth_header[8192];
  char request_url[LLM_PATH_MAX + LLM_NAME_MAX + 64];
  HttpResponseBuf resp = {NULL, 0, 0};
  GeminiStreamState *gemini_stream = NULL;
  long http_status = 0;
  int return_code = -1;
  int streaming = 0;

  if (!profile || !provider || !provider->url[0] || !request_body ||
      !response_out)
    return -1;
  response_out[0] = '\0';

  auth_header[0] = '\0';
  if (provider->auth_endpoint[0] &&
      auth_build_header_with_env(provider->auth_endpoint, provider->auth_env,
                                 auth_header, sizeof(auth_header)) != 0) {
    fprintf(stderr, "llm/http: failed to resolve auth for endpoint %s\n", provider->auth_endpoint);
    return -1;
  }
  streaming = strcmp(provider->kind, "gemini_key") == 0 &&
              llm_progress_fd_available() &&
              !(getenv("FCLAW_DISABLE_LLM_STREAM") &&
                strcmp(getenv("FCLAW_DISABLE_LLM_STREAM"), "0") != 0);
  if (build_request_url(profile, provider, streaming,
                        request_url, sizeof(request_url)) != 0) {
    fprintf(stderr, "llm/http: request URL too large\n");
    return -1;
  }
  return_code = -1;

  curl = curl_easy_init();
  if (!curl) return -1;
  if (streaming) {
    gemini_stream =
        (GeminiStreamState *)calloc(1, sizeof(GeminiStreamState));
    if (!gemini_stream) {
      curl_easy_cleanup(curl);
      return -1;
    }
    gemini_stream->usage = usage_out;
    if (gemini_stream_reset(gemini_stream) != 0) {
      free(gemini_stream);
      curl_easy_cleanup(curl);
      return -1;
    }
  }
  if (append_http_header(&headers, "Content-Type: application/json") != 0 ||
      (strcmp(provider->kind, "http") == 0 &&
       append_http_header(&headers, "anthropic-version: 2023-06-01") != 0) ||
      (auth_header[0] &&
       append_http_header(&headers, auth_header) != 0)) {
    fprintf(stderr, "llm/http: failed to allocate request headers\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (gemini_stream) {
      llm_message_stream_destroy(gemini_stream->decoder);
      free(gemini_stream->raw.buf);
      free(gemini_stream);
    }
    return -1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, request_url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   (curl_off_t)request_body_len);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   streaming ? gemini_stream_write_cb : write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,
                   streaming ? (void *)gemini_stream : (void *)&resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                   (long)llm_http_effective_attempt_timeout_s());

  {
    int max_retries = llm_http_effective_max_retries();
    int attempt = 0;
    if (retry_cap >= 0 && max_retries > retry_cap)
      max_retries = retry_cap;
    for (;;) {
      if (streaming) {
        if (attempt > 0 && gemini_stream_reset(gemini_stream) != 0) {
          rc = CURLE_OUT_OF_MEMORY;
          break;
        }
      } else {
        free(resp.buf);
        resp.buf = NULL;
        resp.used = resp.cap = 0;
      }
      http_status = 0;
      rc = curl_easy_perform(curl);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
      if (rc == CURLE_OK && http_status >= 200 && http_status < 300) break;
      if (attempt < max_retries && llm_http_retryable(rc, http_status) &&
          (!streaming ||
           llm_message_stream_emitted(gemini_stream->decoder) == 0)) {
        unsigned ms = 500u << attempt; /* 500, 1000, 2000, ... */
        fprintf(stderr,
                "llm/http: transient failure (curl=%s http=%ld), retry %d/%d in %ums\n",
                curl_easy_strerror(rc), http_status, attempt + 1, max_retries, ms);
        usleep((useconds_t)ms * 1000u);
        attempt++;
        continue;
      }
      break;
    }
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    fprintf(stderr, "llm/http: curl_easy_perform failed: %s\n", curl_easy_strerror(rc));
    free(resp.buf);
    if (gemini_stream) {
      llm_message_stream_destroy(gemini_stream->decoder);
      free(gemini_stream->raw.buf);
      free(gemini_stream);
    }
    return -1;
  }
  if (http_status < 200 || http_status >= 300) {
    const char *raw = streaming
        ? (gemini_stream->raw.buf ? gemini_stream->raw.buf : "")
        : (resp.buf ? resp.buf : "");
    fprintf(stderr, "llm/http: HTTP %ld response: %.500s\n", http_status, raw);
    copy_raw(raw, raw_response_out, raw_response_out_len);
    free(resp.buf);
    if (gemini_stream) {
      llm_message_stream_destroy(gemini_stream->decoder);
      free(gemini_stream->raw.buf);
      free(gemini_stream);
    }
    return -1;
  }
  {
    char model_text[LLM_RESP_MAX];
    const char *raw = streaming
        ? (gemini_stream->raw.buf ? gemini_stream->raw.buf : "")
        : (resp.buf ? resp.buf : "");
    copy_raw(raw, raw_response_out, raw_response_out_len);
    if (!streaming) llm_extract_usage(provider->kind, raw, usage_out);
    int extract_rc;
    if (streaming) {
      extract_rc = gemini_stream_finish(gemini_stream);
      if (extract_rc == 0)
        snprintf(model_text, sizeof(model_text), "%s",
                 gemini_stream->model_text);
    } else if (strcmp(provider->kind, "openai_compat") == 0) {
      extract_rc = extract_openai_compat_text(resp.buf ? resp.buf : "", model_text, sizeof(model_text));
    } else if (strcmp(provider->kind, "openai_responses") == 0) {
      extract_rc = extract_openai_responses_text(resp.buf ? resp.buf : "", model_text, sizeof(model_text));
    } else if (strcmp(provider->kind, "gemini_key") == 0) {
      extract_rc = extract_gemini_text(resp.buf ? resp.buf : "", model_text, sizeof(model_text));
    } else {
      extract_rc = extract_anthropic_text(resp.buf ? resp.buf : "", model_text, sizeof(model_text));
    }
    if (extract_rc != 0) {
      fprintf(stderr, "llm/http: could not extract model text from response\n");
      free(resp.buf);
      if (gemini_stream) {
        llm_message_stream_destroy(gemini_stream->decoder);
        free(gemini_stream->raw.buf);
        free(gemini_stream);
      }
      return -1;
    }
    return_code = llm_normalize_response_text(model_text, response_out, response_out_len, usage_out);
    if (return_code == LLM_ERR_MODEL_OUTPUT_TOO_LARGE)
      fprintf(stderr, "llm/normalize: model_output_too_large\n");
  }
  free(resp.buf);
  if (gemini_stream) {
    llm_message_stream_destroy(gemini_stream->decoder);
    free(gemini_stream->raw.buf);
    free(gemini_stream);
  }
  return return_code;
}

#else /* !FCLAW_HAVE_LIBCURL */

int llm_http_call(const LlmProfile *profile, const LlmProvider *provider,
                  const char *request_body, size_t request_body_len,
                  int retry_cap,
                  char *response_out, size_t response_out_len,
                  char *raw_response_out, size_t raw_response_out_len,
                  LlmUsage *usage_out) {
  (void)profile; (void)provider; (void)request_body; (void)request_body_len;
  (void)retry_cap;
  (void)raw_response_out; (void)raw_response_out_len;
  (void)usage_out;
  if (response_out && response_out_len > 0) response_out[0] = '\0';
  fprintf(stderr, "llm/http: libcurl not compiled in — rebuild with -DFCLAW_HAVE_LIBCURL -lcurl\n");
  return -1;
}

#endif
