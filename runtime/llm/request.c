#include "request.h"

#include "../bus/media_manifest.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(LLM_MEDIA_MAX_COUNT == FC_MEDIA_MAX_ITEMS,
               "LLM and ingress media count limits must agree");

#define GEMINI_INLINE_REQUEST_MAX_BYTES 20000000U

static int profile_max_output_tokens(const LlmProfile *profile) {
  return profile && profile->max_output_tokens > 0
             ? profile->max_output_tokens
             : LLM_DEFAULT_MAX_OUTPUT_TOKENS;
}

static int gemini_25_effort_budget(const char *effort) {
  if (!effort) return -1;
  if (strcmp(effort, "low") == 0) return 512;
  if (strcmp(effort, "medium") == 0) return 1024;
  if (strcmp(effort, "high") == 0) return 4096;
  return -1;
}

/* Gemini 2.5 Flash and Flash-Lite accept thinkingBudget 0 (thinking off);
 * 2.5 Pro rejects it, and newer models use thinkingLevel. A profile with no
 * effort keeps the pre-0.30.0 default for these two models -- no thinking --
 * because the provider default for 2.5 Flash is dynamic thinking, which
 * made every Truly turn 3-5x slower and billed thinking tokens the ledger
 * cannot price. */
static int is_gemini_25_flash(const char *model) {
  return model && strncmp(model, "gemini-2.5-flash", 16U) == 0;
}

static int is_gemini_25(const char *model) {
  return model && strncmp(model, "gemini-2.5-", 11U) == 0;
}

int llm_build_gemini_body(const LlmProfile *profile,
                          const char *escaped_prompt,
                          char *body, size_t body_len) {
  char thinking[LLM_ID_MAX + 64];
  int max_output_tokens;
  int n;

  if (!profile || !escaped_prompt || !body || body_len == 0) return -1;

  max_output_tokens = profile_max_output_tokens(profile);
  if (max_output_tokens < 1 || max_output_tokens > LLM_MAX_OUTPUT_TOKENS)
    return -1;
  if (profile->effort[0] && is_gemini_25(profile->model)) {
    int budget = gemini_25_effort_budget(profile->effort);
    if (budget < 0) return -1;
    n = snprintf(thinking, sizeof(thinking),
                 ",\"thinkingConfig\":{\"thinkingBudget\":%d}", budget);
  } else if (profile->effort[0]) {
    n = snprintf(thinking, sizeof(thinking),
                 ",\"thinkingConfig\":{\"thinkingLevel\":\"%s\"}",
                 profile->effort);
  } else if (is_gemini_25_flash(profile->model)) {
    n = snprintf(thinking, sizeof(thinking),
                 ",\"thinkingConfig\":{\"thinkingBudget\":0}");
  } else {
    thinking[0] = '\0';
    n = 0;
  }
  if (n < 0 || n >= (int)sizeof(thinking)) return -1;

  return snprintf(body, body_len,
                  "{"
                  "\"contents\":[{"
                  "\"role\":\"user\","
                  "\"parts\":[{\"text\":\"%s\"}]"
                  "}],"
                  "\"generationConfig\":{"
                  "\"temperature\":0,"
                  "\"responseMimeType\":\"application/json\","
                  "\"maxOutputTokens\":%d%s"
                  "}"
                  "}",
                  escaped_prompt, max_output_tokens, thinking) <
         (int)body_len ? 0 : -1;
}

typedef struct {
  char *data;
  size_t cap;
  size_t pos;
  int failed;
} BodyWriter;

static int request_error(char *err, size_t err_len, const char *format, ...) {
  va_list ap;
  if (err && err_len > 0U) {
    va_start(ap, format);
    (void)vsnprintf(err, err_len, format, ap);
    va_end(ap);
  }
  return -1;
}

static int writer_bytes(BodyWriter *w, const char *data, size_t len) {
  if (!w || (!data && len != 0U) || w->failed) return -1;
  if (len > SIZE_MAX - w->pos) {
    w->failed = 1;
    return -1;
  }
  if (w->data) {
    if (w->pos + len >= w->cap) {
      w->failed = 1;
      return -1;
    }
    if (len > 0U) memcpy(w->data + w->pos, data, len);
  }
  w->pos += len;
  return 0;
}

static int writer_text(BodyWriter *w, const char *text) {
  return writer_bytes(w, text ? text : "", strlen(text ? text : ""));
}

static int writer_int(BodyWriter *w, int value) {
  char text[32];
  int n = snprintf(text, sizeof(text), "%d", value);
  return n > 0 && n < (int)sizeof(text)
             ? writer_bytes(w, text, (size_t)n)
             : -1;
}

/* Keep byte-for-byte parity with runtime/support/json.c:json_escape(). */
static int writer_json_escaped(BodyWriter *w, const char *text) {
  static const char hex[] = "0123456789abcdef";
  const unsigned char *p = (const unsigned char *)(text ? text : "");
  for (; *p; ++p) {
    const char *escaped = NULL;
    char control[6];
    switch (*p) {
      case '"':  escaped = "\\\""; break;
      case '\\': escaped = "\\\\"; break;
      case '\b': escaped = "\\b"; break;
      case '\f': escaped = "\\f"; break;
      case '\n': escaped = "\\n"; break;
      case '\r': escaped = "\\r"; break;
      case '\t': escaped = "\\t"; break;
      default: break;
    }
    if (escaped) {
      if (writer_text(w, escaped) != 0) return -1;
    } else if (*p < 0x20U) {
      control[0] = '\\';
      control[1] = 'u';
      control[2] = '0';
      control[3] = '0';
      control[4] = hex[*p >> 4];
      control[5] = hex[*p & 0x0fU];
      if (writer_bytes(w, control, sizeof(control)) != 0) return -1;
    } else if (writer_bytes(w, (const char *)p, 1U) != 0) {
      return -1;
    }
  }
  return 0;
}

static int writer_json_string(BodyWriter *w, const char *text) {
  return writer_text(w, "\"") == 0 &&
         writer_json_escaped(w, text) == 0 &&
         writer_text(w, "\"") == 0 ? 0 : -1;
}

static int base64_size(uint64_t bytes, size_t *out) {
  uint64_t groups;
  if (!out || bytes > (uint64_t)SIZE_MAX) return -1;
  if (bytes > UINT64_MAX - 2U) return -1;
  groups = (bytes + 2U) / 3U;
  if (groups > (uint64_t)SIZE_MAX / 4U) return -1;
  *out = (size_t)groups * 4U;
  return 0;
}

static int open_verified_media(const LlmMedia *media) {
  struct stat st;
  int flags = O_RDONLY;
  int fd;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  fd = open(media->path, flags);
  if (fd < 0) return -1;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size != media->size_bytes) {
    close(fd);
    return -1;
  }
  return fd;
}

static int writer_base64_file(BodyWriter *w, const LlmMedia *media) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char input[12290];
  size_t encoded_len;
  size_t carry = 0U;
  size_t out_pos = 0U;
  uint64_t read_total = 0U;
  char *dst;
  int fd;

  if (!w || !media || base64_size(media->size_bytes, &encoded_len) != 0)
    return -1;
  if (!w->data) return writer_bytes(w, "", encoded_len);
  if (encoded_len > SIZE_MAX - w->pos || w->pos + encoded_len >= w->cap) {
    w->failed = 1;
    return -1;
  }
  dst = w->data + w->pos;
  fd = open_verified_media(media);
  if (fd < 0) {
    w->failed = 1;
    return -1;
  }

  for (;;) {
    ssize_t got;
    size_t available;
    size_t whole;
    do {
      got = read(fd, input + carry, sizeof(input) - carry);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
      close(fd);
      w->failed = 1;
      return -1;
    }
    if (got == 0) break;
    if ((uint64_t)got > UINT64_MAX - read_total) {
      close(fd);
      w->failed = 1;
      return -1;
    }
    read_total += (uint64_t)got;
    if (read_total > media->size_bytes) {
      close(fd);
      w->failed = 1;
      return -1;
    }
    available = carry + (size_t)got;
    whole = available - (available % 3U);
    for (size_t i = 0; i < whole; i += 3U) {
      unsigned value = ((unsigned)input[i] << 16) |
                       ((unsigned)input[i + 1U] << 8) |
                       (unsigned)input[i + 2U];
      dst[out_pos++] = alphabet[(value >> 18) & 0x3fU];
      dst[out_pos++] = alphabet[(value >> 12) & 0x3fU];
      dst[out_pos++] = alphabet[(value >> 6) & 0x3fU];
      dst[out_pos++] = alphabet[value & 0x3fU];
    }
    carry = available - whole;
    if (carry > 0U) memmove(input, input + whole, carry);
  }
  close(fd);
  if (read_total != media->size_bytes) {
    w->failed = 1;
    return -1;
  }
  if (carry == 1U) {
    unsigned value = (unsigned)input[0] << 16;
    dst[out_pos++] = alphabet[(value >> 18) & 0x3fU];
    dst[out_pos++] = alphabet[(value >> 12) & 0x3fU];
    dst[out_pos++] = '=';
    dst[out_pos++] = '=';
  } else if (carry == 2U) {
    unsigned value = ((unsigned)input[0] << 16) |
                     ((unsigned)input[1] << 8);
    dst[out_pos++] = alphabet[(value >> 18) & 0x3fU];
    dst[out_pos++] = alphabet[(value >> 12) & 0x3fU];
    dst[out_pos++] = alphabet[(value >> 6) & 0x3fU];
    dst[out_pos++] = '=';
  }
  if (out_pos != encoded_len) {
    w->failed = 1;
    return -1;
  }
  w->pos += encoded_len;
  return 0;
}

static int mime_token_char(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '!' || ch == '#' ||
         ch == '$' || ch == '&' || ch == '^' || ch == '_' ||
         ch == '.' || ch == '+' || ch == '-';
}

static int valid_media_type(const char *media_type) {
  const unsigned char *p = (const unsigned char *)media_type;
  int saw_slash = 0;
  int side_len = 0;
  if (!p || !*p) return 0;
  for (; *p; ++p) {
    if (*p == '/') {
      if (saw_slash || side_len == 0) return 0;
      saw_slash = 1;
      side_len = 0;
    } else {
      if (!mime_token_char(*p)) return 0;
      side_len++;
    }
  }
  return saw_slash && side_len > 0;
}

static int is_openai_image(const char *media_type) {
  return strcmp(media_type, "image/png") == 0 ||
         strcmp(media_type, "image/jpeg") == 0 ||
         strcmp(media_type, "image/webp") == 0 ||
         strcmp(media_type, "image/gif") == 0;
}

static int is_anthropic_image(const char *media_type) {
  return is_openai_image(media_type);
}

static int is_openai_document(const char *media_type) {
  return strcmp(media_type, "application/pdf") == 0 ||
         strcmp(media_type, "text/plain") == 0 ||
         strcmp(media_type, "text/markdown") == 0 ||
         strcmp(media_type, "text/csv") == 0 ||
         strcmp(media_type, "application/json") == 0 ||
         strcmp(media_type, "application/msword") == 0 ||
         strcmp(media_type,
                "application/vnd.openxmlformats-officedocument."
                "wordprocessingml.document") == 0 ||
         strcmp(media_type, "application/vnd.ms-excel") == 0 ||
         strcmp(media_type,
                "application/vnd.openxmlformats-officedocument."
                "spreadsheetml.sheet") == 0 ||
         strcmp(media_type, "application/vnd.ms-powerpoint") == 0 ||
         strcmp(media_type,
                "application/vnd.openxmlformats-officedocument."
                "presentationml.presentation") == 0;
}

static int is_gemini_media(const char *media_type) {
  return strncmp(media_type, "image/", 6U) == 0 ||
         strncmp(media_type, "audio/", 6U) == 0 ||
         strncmp(media_type, "video/", 6U) == 0 ||
         strcmp(media_type, "application/pdf") == 0;
}

static int provider_accepts_media(const char *kind, const char *media_type) {
  if (strcmp(kind, "gemini_key") == 0) return is_gemini_media(media_type);
  if (strcmp(kind, "http") == 0)
    return is_anthropic_image(media_type) ||
           strcmp(media_type, "application/pdf") == 0;
  if (strcmp(kind, "openai_responses") == 0)
    return is_openai_image(media_type) ||
           is_openai_document(media_type);
  if (strcmp(kind, "openai_chat") == 0 ||
      strcmp(kind, "openai_compat") == 0)
    return is_openai_image(media_type);
  return strcmp(kind, "mock") == 0;
}

static int has_terminated_field(const char *field, size_t cap) {
  return field && memchr(field, '\0', cap) != NULL;
}

int llm_request_validate(const char *provider_kind, const LlmRequest *request,
                         char *err, size_t err_len) {
  uint64_t total = 0U;
  if (err && err_len > 0U) err[0] = '\0';
  if (!provider_kind || !request || !request->text)
    return request_error(err, err_len, "invalid LLM request");
  if (request->media_count > FC_MEDIA_MAX_ITEMS)
    return request_error(err, err_len, "media count %zu exceeds limit %u",
                         request->media_count, FC_MEDIA_MAX_ITEMS);
  if (request->media_count > 0U && !request->media)
    return request_error(err, err_len, "media descriptor array is missing");
  if (strcmp(provider_kind, "mock") != 0 &&
      strcmp(provider_kind, "http") != 0 &&
      strcmp(provider_kind, "openai_chat") != 0 &&
      strcmp(provider_kind, "openai_compat") != 0 &&
      strcmp(provider_kind, "openai_responses") != 0 &&
      strcmp(provider_kind, "gemini_key") != 0)
    return request_error(err, err_len, "provider kind '%s' is unsupported",
                         provider_kind);

  for (size_t i = 0; i < request->media_count; ++i) {
    const LlmMedia *media = &request->media[i];
    struct stat st;
    if (!has_terminated_field(media->id, sizeof(media->id)) ||
        !has_terminated_field(media->filename, sizeof(media->filename)) ||
        !has_terminated_field(media->media_type, sizeof(media->media_type)) ||
        !has_terminated_field(media->path, sizeof(media->path)) ||
        !media->id[0] || !media->filename[0] || !media->path[0])
      return request_error(err, err_len, "media item %zu has invalid metadata", i);
    if (!valid_media_type(media->media_type))
      return request_error(err, err_len,
                           "media item %zu has invalid MIME type", i);
    if (media->size_bytes == 0U ||
        media->size_bytes > FC_MEDIA_MAX_ITEM_BYTES)
      return request_error(err, err_len,
                           "media item %zu size is outside the allowed range", i);
    if (UINT64_MAX - total < media->size_bytes ||
        total + media->size_bytes > FC_MEDIA_MAX_TOTAL_BYTES)
      return request_error(err, err_len, "media total exceeds limit");
    total += media->size_bytes;
    if (!provider_accepts_media(provider_kind, media->media_type))
      return request_error(err, err_len,
                           "provider kind '%s' does not support MIME type '%s'",
                           provider_kind, media->media_type);
    if (lstat(media->path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || (uint64_t)st.st_size != media->size_bytes)
      return request_error(err, err_len,
                           "media item %zu is not a verified regular file", i);
  }
  return 0;
}

static int write_data_uri(BodyWriter *w, const LlmMedia *media) {
  return writer_text(w, "data:") == 0 &&
         writer_json_escaped(w, media->media_type) == 0 &&
         writer_text(w, ";base64,") == 0 &&
         writer_base64_file(w, media) == 0 ? 0 : -1;
}

static int serialize_anthropic(const LlmProfile *profile,
                               const LlmRequest *request, BodyWriter *w) {
  if (writer_text(w, "{\"model\":\"") != 0 ||
      writer_text(w, profile->model) != 0 ||
      writer_text(w, "\",\"max_tokens\":") != 0 ||
      writer_int(w, profile_max_output_tokens(profile)) != 0)
    return -1;
  if (profile->effort[0] &&
      (writer_text(w, ",\"output_config\":{\"effort\":") != 0 ||
       writer_json_string(w, profile->effort) != 0 ||
       writer_text(w, "}") != 0))
    return -1;
  if (writer_text(w, ",\"messages\":[{\"role\":\"user\",\"content\":") != 0)
    return -1;
  if (request->media_count == 0U) {
    if (writer_json_string(w, request->text) != 0) return -1;
  } else {
    if (writer_text(w, "[{\"type\":\"text\",\"text\":") != 0 ||
        writer_json_string(w, request->text) != 0 ||
        writer_text(w, "}") != 0)
      return -1;
    for (size_t i = 0; i < request->media_count; ++i) {
      const LlmMedia *media = &request->media[i];
      const char *type = is_anthropic_image(media->media_type)
                             ? "image" : "document";
      if (writer_text(w, ",{\"type\":\"") != 0 ||
          writer_text(w, type) != 0 ||
          writer_text(w, "\",\"source\":{\"type\":\"base64\",\"media_type\":") != 0 ||
          writer_json_string(w, media->media_type) != 0 ||
          writer_text(w, ",\"data\":\"") != 0 ||
          writer_base64_file(w, media) != 0 ||
          writer_text(w, "\"}}") != 0)
        return -1;
    }
    if (writer_text(w, "]") != 0) return -1;
  }
  return writer_text(w, "}]}");
}

static int serialize_openai_compat(const LlmProfile *profile,
                                   const LlmRequest *request, BodyWriter *w) {
  if (writer_text(w, "{\"model\":\"") != 0 ||
      writer_text(w, profile->model) != 0 ||
      writer_text(w, "\",\"max_tokens\":") != 0 ||
      writer_int(w, profile_max_output_tokens(profile)) != 0)
    return -1;
  if (profile->effort[0] &&
      (writer_text(w, ",\"reasoning_effort\":") != 0 ||
       writer_json_string(w, profile->effort) != 0))
    return -1;
  if (writer_text(w, ",\"temperature\":0,\"stream\":false,"
                     "\"messages\":[{\"role\":\"user\",\"content\":") != 0)
    return -1;
  if (request->media_count == 0U) {
    if (writer_json_string(w, request->text) != 0) return -1;
  } else {
    if (writer_text(w, "[{\"type\":\"text\",\"text\":") != 0 ||
        writer_json_string(w, request->text) != 0 ||
        writer_text(w, "}") != 0)
      return -1;
    for (size_t i = 0; i < request->media_count; ++i) {
      if (writer_text(w, ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"") != 0 ||
          write_data_uri(w, &request->media[i]) != 0 ||
          writer_text(w, "\"}}") != 0)
        return -1;
    }
    if (writer_text(w, "]") != 0) return -1;
  }
  return writer_text(w, "}]}");
}

static int serialize_openai_chat(const LlmProfile *profile,
                                 const LlmRequest *request, BodyWriter *w) {
  if (writer_text(w, "{\"model\":\"") != 0 ||
      writer_text(w, profile->model) != 0 ||
      writer_text(w, "\",\"max_completion_tokens\":") != 0 ||
      writer_int(w, profile_max_output_tokens(profile)) != 0)
    return -1;
  if (profile->effort[0] &&
      (writer_text(w, ",\"reasoning_effort\":") != 0 ||
       writer_json_string(w, profile->effort) != 0))
    return -1;
  /* OpenAI reasoning models reject non-default sampling controls. Omitting
   * temperature works for both reasoning and non-reasoning chat models. */
  if (writer_text(w, ",\"stream\":false,"
                     "\"messages\":[{\"role\":\"user\",\"content\":") != 0)
    return -1;
  if (request->media_count == 0U) {
    if (writer_json_string(w, request->text) != 0) return -1;
  } else {
    if (writer_text(w, "[{\"type\":\"text\",\"text\":") != 0 ||
        writer_json_string(w, request->text) != 0 ||
        writer_text(w, "}") != 0)
      return -1;
    for (size_t i = 0; i < request->media_count; ++i) {
      if (writer_text(w, ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"") != 0 ||
          write_data_uri(w, &request->media[i]) != 0 ||
          writer_text(w, "\"}}") != 0)
        return -1;
    }
    if (writer_text(w, "]") != 0) return -1;
  }
  return writer_text(w, "}]}");
}

static int serialize_openai_responses(const LlmProfile *profile,
                                      const LlmRequest *request, BodyWriter *w) {
  if (writer_text(w, "{\"model\":\"") != 0 ||
      writer_text(w, profile->model) != 0)
    return -1;
  if (request->media_count == 0U) {
    if (writer_text(w, "\",\"input\":") != 0 ||
        writer_json_string(w, request->text) != 0)
      return -1;
  } else {
    if (writer_text(w, "\",\"input\":[{\"role\":\"user\",\"content\":["
                       "{\"type\":\"input_text\",\"text\":") != 0 ||
        writer_json_string(w, request->text) != 0 ||
        writer_text(w, "}") != 0)
      return -1;
    for (size_t i = 0; i < request->media_count; ++i) {
      const LlmMedia *media = &request->media[i];
      if (is_openai_image(media->media_type)) {
        if (writer_text(w, ",{\"type\":\"input_image\",\"image_url\":\"") != 0 ||
            write_data_uri(w, media) != 0 ||
            writer_text(w, "\"}") != 0)
          return -1;
      } else {
        if (writer_text(w, ",{\"type\":\"input_file\",\"filename\":") != 0 ||
            writer_json_string(w, media->filename) != 0 ||
            writer_text(w, ",\"file_data\":\"") != 0 ||
            write_data_uri(w, media) != 0 ||
            writer_text(w, "\"}") != 0)
          return -1;
      }
    }
    if (writer_text(w, "]}]") != 0) return -1;
  }
  /* Effort passes through unvalidated; the provider is the authority on
   * which values a model accepts, so e.g. "minimal" is not filtered here. */
  if (profile->effort[0]) {
    if (writer_text(w, ",\"reasoning\":{\"effort\":") != 0 ||
        writer_json_string(w, profile->effort) != 0 ||
        writer_text(w, "}") != 0)
      return -1;
  }
  if (writer_text(w, ",\"max_output_tokens\":") != 0 ||
      writer_int(w, profile_max_output_tokens(profile)) != 0)
    return -1;
  return writer_text(w, ",\"temperature\":0,\"store\":false,\"stream\":false}");
}

static int serialize_gemini(const LlmProfile *profile,
                            const LlmRequest *request, BodyWriter *w) {
  if (writer_text(w, "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":") != 0 ||
      writer_json_string(w, request->text) != 0 ||
      writer_text(w, "}") != 0)
    return -1;
  for (size_t i = 0; i < request->media_count; ++i) {
    const LlmMedia *media = &request->media[i];
    if (writer_text(w, ",{\"inline_data\":{\"mime_type\":") != 0 ||
        writer_json_string(w, media->media_type) != 0 ||
        writer_text(w, ",\"data\":\"") != 0 ||
        writer_base64_file(w, media) != 0 ||
        writer_text(w, "\"}}") != 0)
      return -1;
  }
  if (writer_text(w, "]}],\"generationConfig\":{\"temperature\":0,"
                     "\"responseMimeType\":\"application/json\","
                     "\"maxOutputTokens\":") != 0 ||
      writer_int(w, profile_max_output_tokens(profile)) != 0)
    return -1;
  if (profile->effort[0] && is_gemini_25(profile->model)) {
    int budget = gemini_25_effort_budget(profile->effort);
    if (budget < 0 ||
        writer_text(w, ",\"thinkingConfig\":{\"thinkingBudget\":") != 0 ||
        writer_int(w, budget) != 0 || writer_text(w, "}") != 0)
      return -1;
  } else if (profile->effort[0]) {
    if (writer_text(w, ",\"thinkingConfig\":{\"thinkingLevel\":") != 0 ||
        writer_json_string(w, profile->effort) != 0 ||
        writer_text(w, "}") != 0)
      return -1;
  } else if (is_gemini_25_flash(profile->model)) {
    if (writer_text(w, ",\"thinkingConfig\":{\"thinkingBudget\":0}") != 0)
      return -1;
  }
  return writer_text(w, "}}");
}

static int serialize_request(const LlmProfile *profile,
                             const char *provider_kind,
                             const LlmRequest *request,
                             BodyWriter *writer) {
  if (strcmp(provider_kind, "http") == 0)
    return serialize_anthropic(profile, request, writer);
  if (strcmp(provider_kind, "openai_chat") == 0)
    return serialize_openai_chat(profile, request, writer);
  if (strcmp(provider_kind, "openai_compat") == 0)
    return serialize_openai_compat(profile, request, writer);
  if (strcmp(provider_kind, "openai_responses") == 0)
    return serialize_openai_responses(profile, request, writer);
  if (strcmp(provider_kind, "gemini_key") == 0)
    return serialize_gemini(profile, request, writer);
  return -1;
}

int llm_request_body_build(const LlmProfile *profile,
                           const char *provider_kind,
                           const LlmRequest *request,
                           char *text_scratch, size_t text_scratch_len,
                           LlmRequestBody *out,
                           char *err, size_t err_len) {
  BodyWriter measure = {0};
  BodyWriter writer;
  char *body = NULL;
  int owned = 0;
  if (out) {
    out->data = NULL;
    out->len = 0U;
    out->owned = 0;
  }
  if (!profile || !provider_kind || !request || !out)
    return request_error(err, err_len, "invalid request body arguments");
  if (profile_max_output_tokens(profile) < 1 ||
      profile_max_output_tokens(profile) > LLM_MAX_OUTPUT_TOKENS)
    return request_error(err, err_len,
                         "max_output_tokens must be between 1 and %d",
                         LLM_MAX_OUTPUT_TOKENS);
  if (strcmp(provider_kind, "http") == 0 && profile->effort[0] &&
      strcmp(profile->effort, "low") != 0 &&
      strcmp(profile->effort, "medium") != 0 &&
      strcmp(profile->effort, "high") != 0 &&
      strcmp(profile->effort, "xhigh") != 0 &&
      strcmp(profile->effort, "max") != 0)
    return request_error(
        err, err_len,
        "Anthropic effort must be low, medium, high, xhigh, or max");
  if (llm_request_validate(provider_kind, request, err, err_len) != 0)
    return -1;
  if (strcmp(provider_kind, "mock") == 0)
    return request_error(err, err_len, "mock requests have no HTTP body");
  if (serialize_request(profile, provider_kind, request, &measure) != 0 ||
      measure.failed || measure.pos == SIZE_MAX)
    return request_error(err, err_len, "request body size overflow");
  if (request->media_count > 0U &&
      strcmp(provider_kind, "gemini_key") == 0 &&
      measure.pos >= GEMINI_INLINE_REQUEST_MAX_BYTES)
    return request_error(
        err, err_len,
        "Gemini inline media request is %zu bytes; the 20 MB transport "
        "limit requires a Files API upload",
        measure.pos);
  if (request->media_count == 0U) {
    if (!text_scratch || measure.pos + 1U > text_scratch_len)
      return request_error(err, err_len, "text request body exceeds boundary");
    body = text_scratch;
  } else {
    body = (char *)malloc(measure.pos + 1U);
    if (!body)
      return request_error(err, err_len, "request body allocation failed");
    owned = 1;
  }
  writer.data = body;
  writer.cap = measure.pos + 1U;
  writer.pos = 0U;
  writer.failed = 0;
  if (serialize_request(profile, provider_kind, request, &writer) != 0 ||
      writer.failed || writer.pos != measure.pos) {
    if (owned) free(body);
    return request_error(err, err_len,
                         "media file changed while building request body");
  }
  body[writer.pos] = '\0';
  out->data = body;
  out->len = writer.pos;
  out->owned = owned;
  if (err && err_len > 0U) err[0] = '\0';
  return 0;
}

void llm_request_body_free(LlmRequestBody *body) {
  if (!body) return;
  if (body->owned) free(body->data);
  body->data = NULL;
  body->len = 0U;
  body->owned = 0;
}
