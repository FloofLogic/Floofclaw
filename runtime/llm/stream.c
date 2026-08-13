#include "stream.h"

#include "llm.h"
#include "../support/json.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STREAM_STACK_MAX 32
#define STREAM_KEY_MAX   128
#define STREAM_DELTA_MAX 512

enum {
  STREAM_ROLE_OTHER = 0,
  STREAM_ROLE_ROOT,
  STREAM_ROLE_CALLS,
  STREAM_ROLE_CALL,
  STREAM_ROLE_ARGS
};

typedef struct {
  char kind; /* '{' or '[' */
  int role;
  int state;
  int call_index;
  int call_is_message;
  char key[STREAM_KEY_MAX];
} StreamCtx;

struct LlmMessageStream {
  LlmTextProgressSink sink;
  void *sink_user;
  char raw[LLM_RESP_MAX];
  size_t raw_len;
  char decoded[LLM_RESP_MAX];
  size_t emitted;
  int disabled;
};

static int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int append_utf8(unsigned cp, char *out, size_t out_cap, size_t *used) {
  unsigned char bytes[4];
  size_t n;
  if (cp <= 0x7fU) {
    bytes[0] = (unsigned char)cp; n = 1;
  } else if (cp <= 0x7ffU) {
    bytes[0] = (unsigned char)(0xc0U | (cp >> 6));
    bytes[1] = (unsigned char)(0x80U | (cp & 0x3fU)); n = 2;
  } else if (cp <= 0xffffU) {
    if (cp >= 0xd800U && cp <= 0xdfffU) return -1;
    bytes[0] = (unsigned char)(0xe0U | (cp >> 12));
    bytes[1] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
    bytes[2] = (unsigned char)(0x80U | (cp & 0x3fU)); n = 3;
  } else if (cp <= 0x10ffffU) {
    bytes[0] = (unsigned char)(0xf0U | (cp >> 18));
    bytes[1] = (unsigned char)(0x80U | ((cp >> 12) & 0x3fU));
    bytes[2] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
    bytes[3] = (unsigned char)(0x80U | (cp & 0x3fU)); n = 4;
  } else {
    return -1;
  }
  if (*used + n + 1 > out_cap) return -1;
  memcpy(out + *used, bytes, n);
  *used += n;
  out[*used] = '\0';
  return 0;
}

static size_t utf8_width(unsigned char c) {
  if (c < 0x80U) return 1;
  if ((c & 0xe0U) == 0xc0U) return 2;
  if ((c & 0xf0U) == 0xe0U) return 3;
  if ((c & 0xf8U) == 0xf0U) return 4;
  return 0;
}

/* Decode one JSON string. Incomplete input is successful with complete=0
 * and only the safely decoded prefix in out. */
static int scan_string(const char *src, size_t src_len, size_t *pos,
                       char *out, size_t out_cap, size_t *out_used,
                       int *complete) {
  size_t i, used = 0;
  if (!src || !pos || *pos >= src_len || src[*pos] != '"' ||
      !out || out_cap == 0 || !out_used || !complete)
    return -1;
  i = *pos + 1;
  out[0] = '\0';
  *complete = 0;
  while (i < src_len) {
    unsigned char c = (unsigned char)src[i];
    if (c == '"') {
      *pos = i + 1;
      *out_used = used;
      *complete = 1;
      return 0;
    }
    if (c == '\\') {
      unsigned cp = 0;
      int hv;
      if (i + 1 >= src_len) break;
      c = (unsigned char)src[i + 1];
      if (c == '"' || c == '\\' || c == '/') {
        if (used + 2 > out_cap) return -1;
        out[used++] = (char)c; out[used] = '\0'; i += 2; continue;
      }
      if (c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') {
        static const char decoded[] = {'\b','\f','\n','\r','\t'};
        const char *marks = "bfnrt";
        const char *hit = strchr(marks, (int)c);
        if (!hit || used + 2 > out_cap) return -1;
        out[used++] = decoded[hit - marks]; out[used] = '\0';
        i += 2; continue;
      }
      if (c != 'u') return -1;
      if (i + 6 > src_len) break;
      for (size_t h = 0; h < 4; ++h) {
        hv = hex_value((unsigned char)src[i + 2 + h]);
        if (hv < 0) return -1;
        cp = (cp << 4) | (unsigned)hv;
      }
      i += 6;
      if (cp >= 0xd800U && cp <= 0xdbffU) {
        unsigned low = 0;
        if (i + 6 > src_len) break;
        if (src[i] != '\\' || src[i + 1] != 'u') return -1;
        for (size_t h = 0; h < 4; ++h) {
          hv = hex_value((unsigned char)src[i + 2 + h]);
          if (hv < 0) return -1;
          low = (low << 4) | (unsigned)hv;
        }
        if (low < 0xdc00U || low > 0xdfffU) return -1;
        cp = 0x10000U + ((cp - 0xd800U) << 10) + (low - 0xdc00U);
        i += 6;
      }
      if (append_utf8(cp, out, out_cap, &used) != 0) return -1;
      continue;
    }
    if (c < 0x20U) return -1;
    {
      size_t width = utf8_width(c);
      if (width == 0) return -1;
      if (i + width > src_len) break;
      for (size_t u = 1; u < width; ++u)
        if (((unsigned char)src[i + u] & 0xc0U) != 0x80U) return -1;
      if (used + width + 1 > out_cap) return -1;
      memcpy(out + used, src + i, width);
      used += width;
      out[used] = '\0';
      i += width;
    }
  }
  *pos = i;
  *out_used = used;
  return 0;
}

static void skip_ws(const char *s, size_t n, size_t *pos) {
  while (*pos < n &&
         (s[*pos] == ' ' || s[*pos] == '\t' ||
          s[*pos] == '\r' || s[*pos] == '\n'))
    (*pos)++;
}

static int value_is_done_char(unsigned char c) {
  return c == ',' || c == '}' || c == ']' ||
         c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int mark_parent_value_done(StreamCtx *stack, int depth) {
  if (depth <= 0) return 0;
  stack[depth - 1].state =
      stack[depth - 1].kind == '{' ? 3 : 1;
  return 0;
}

static int emit_new_text(LlmMessageStream *s, size_t decoded_len) {
  size_t pos;
  if (decoded_len < s->emitted) {
    s->disabled = 1;
    return -1;
  }
  pos = s->emitted;
  while (pos < decoded_len) {
    size_t take = decoded_len - pos;
    if (take > STREAM_DELTA_MAX) take = STREAM_DELTA_MAX;
    while (take > 0 &&
           (((unsigned char)s->decoded[pos + take] & 0xc0U) == 0x80U))
      take--;
    if (take == 0) return -1;
    if (s->sink &&
        s->sink(s->decoded + pos, take, s->sink_user) < 0) {
      s->disabled = 1;
      return -1;
    }
    pos += take;
  }
  s->emitted = decoded_len;
  return 0;
}

/* Reparse the bounded prefix on each feed. This keeps incremental state
 * small and makes arbitrary provider frame boundaries equivalent. */
static int inspect_stream(LlmMessageStream *s) {
  StreamCtx stack[STREAM_STACK_MAX];
  int depth = 0;
  size_t pos = 0;
  memset(stack, 0, sizeof(stack));
  skip_ws(s->raw, s->raw_len, &pos);
  if (pos >= s->raw_len || s->raw[pos] != '{') return 0;
  stack[0].kind = '{';
  stack[0].role = STREAM_ROLE_ROOT;
  stack[0].state = 0;
  depth = 1;
  pos++;

  while (pos < s->raw_len && depth > 0) {
    StreamCtx *ctx = &stack[depth - 1];
    skip_ws(s->raw, s->raw_len, &pos);
    if (pos >= s->raw_len) return 0;
    if (ctx->kind == '{') {
      if (ctx->state == 0) {
        size_t used = 0;
        int complete = 0;
        if (s->raw[pos] == '}') { depth--; pos++; continue; }
        if (s->raw[pos] != '"') return 0;
        if (scan_string(s->raw, s->raw_len, &pos,
                        ctx->key, sizeof(ctx->key), &used,
                        &complete) != 0 || !complete)
          return 0;
        ctx->state = 1;
        continue;
      }
      if (ctx->state == 1) {
        if (s->raw[pos] != ':') return 0;
        ctx->state = 2; pos++; continue;
      }
      if (ctx->state == 3) {
        if (s->raw[pos] == ',') { ctx->state = 0; pos++; continue; }
        if (s->raw[pos] == '}') { depth--; pos++; continue; }
        return 0;
      }
    } else {
      if (ctx->state == 1) {
        if (s->raw[pos] == ',') { ctx->state = 0; pos++; continue; }
        if (s->raw[pos] == ']') { depth--; pos++; continue; }
        return 0;
      }
      if (ctx->state == 0 && s->raw[pos] == ']') {
        depth--; pos++; continue;
      }
    }

    /* Value position for object state=2 or array state=0. */
    if (s->raw[pos] == '{' || s->raw[pos] == '[') {
      int role = STREAM_ROLE_OTHER;
      int call_index = -1;
      char kind = s->raw[pos];
      if (depth >= STREAM_STACK_MAX) return -1;
      if (ctx->kind == '{' && ctx->role == STREAM_ROLE_ROOT &&
          strcmp(ctx->key, "calls") == 0 && kind == '[')
        role = STREAM_ROLE_CALLS;
      else if (ctx->kind == '[' && ctx->role == STREAM_ROLE_CALLS &&
               kind == '{')
        role = STREAM_ROLE_CALL;
      else if (ctx->kind == '{' && ctx->role == STREAM_ROLE_CALL &&
               strcmp(ctx->key, "args") == 0 && kind == '{') {
        role = STREAM_ROLE_ARGS;
        call_index = depth - 1;
      }
      mark_parent_value_done(stack, depth);
      memset(&stack[depth], 0, sizeof(stack[depth]));
      stack[depth].kind = kind;
      stack[depth].role = role;
      stack[depth].state = 0;
      stack[depth].call_index = call_index;
      depth++;
      pos++;
      continue;
    }
    if (s->raw[pos] == '"') {
      char small_value[STREAM_KEY_MAX];
      char *value = small_value;
      size_t value_cap = sizeof(small_value);
      size_t used = 0;
      int complete = 0;
      int target = 0;
      if (ctx->kind == '{' && ctx->role == STREAM_ROLE_ROOT &&
          strcmp(ctx->key, "message") == 0)
        target = 1;
      else if (ctx->kind == '{' && ctx->role == STREAM_ROLE_ARGS &&
               strcmp(ctx->key, "message") == 0 &&
               ctx->call_index >= 0 &&
               ctx->call_index < depth &&
               stack[ctx->call_index].call_is_message)
        target = 1;
      if (target) {
        value = s->decoded;
        value_cap = sizeof(s->decoded);
      }
      if (scan_string(s->raw, s->raw_len, &pos,
                      value, value_cap, &used, &complete) != 0)
        return 0;
      if (target) {
        return emit_new_text(s, used);
      }
      if (!complete) return 0;
      if (ctx->kind == '{' && ctx->role == STREAM_ROLE_CALL &&
          strcmp(ctx->key, "name") == 0)
        ctx->call_is_message = strcmp(value, "message") == 0;
      mark_parent_value_done(stack, depth);
      continue;
    }
    /* Primitive. Only accept it once a delimiter proves it complete. */
    {
      size_t start = pos;
      while (pos < s->raw_len &&
             !value_is_done_char((unsigned char)s->raw[pos]))
        pos++;
      if (pos == s->raw_len) return 0;
      if (pos == start) return 0;
      mark_parent_value_done(stack, depth);
    }
  }
  return 0;
}

LlmMessageStream *llm_message_stream_create(LlmTextProgressSink sink,
                                            void *user) {
  LlmMessageStream *s =
      (LlmMessageStream *)calloc(1, sizeof(LlmMessageStream));
  if (!s) return NULL;
  s->sink = sink;
  s->sink_user = user;
  return s;
}

void llm_message_stream_destroy(LlmMessageStream *stream) {
  if (!stream) return;
  memset(stream, 0, sizeof(*stream));
  free(stream);
}

int llm_message_stream_feed(LlmMessageStream *s,
                            const char *bytes, size_t len) {
  if (!s || (!bytes && len > 0) || s->disabled) return -1;
  if (len == 0) return 0;
  if (s->raw_len + len + 1 > sizeof(s->raw)) {
    s->disabled = 1;
    return -1;
  }
  memcpy(s->raw + s->raw_len, bytes, len);
  s->raw_len += len;
  s->raw[s->raw_len] = '\0';
  return inspect_stream(s);
}

size_t llm_message_stream_emitted(const LlmMessageStream *stream) {
  return stream ? stream->emitted : 0;
}

static int progress_fd(void) {
  const char *v = getenv("FCLAW_PROGRESS_FD");
  char *end = NULL;
  long fd;
  if (!v || !*v) return -1;
  errno = 0;
  fd = strtol(v, &end, 10);
  if (errno || !end || *end || fd < 3 || fd > INT_MAX) return -1;
  return (int)fd;
}

int llm_progress_fd_available(void) { return progress_fd() >= 0; }

int llm_progress_fd_sink(const char *text, size_t text_len, void *user) {
  char plain[STREAM_DELTA_MAX + 1];
  char escaped[(STREAM_DELTA_MAX * 6) + 1];
  char record[(STREAM_DELTA_MAX * 6) + 96];
  size_t used = 0;
  int fd = progress_fd();
  int n;
  (void)user;
  if (fd < 0) return 1;
  if (!text || text_len == 0 || text_len > STREAM_DELTA_MAX) return -1;
  memcpy(plain, text, text_len);
  plain[text_len] = '\0';
  if (json_escape(plain, escaped, sizeof(escaped)) != 0) return -1;
  n = snprintf(record, sizeof(record),
               "{\"kind\":\"assistant_text_delta\",\"delta\":\"%s\"}\n",
               escaped);
  if (n < 0 || (size_t)n >= sizeof(record)) return -1;
  while (used < (size_t)n) {
    ssize_t wrote = write(fd, record + used, (size_t)n - used);
    if (wrote > 0) { used += (size_t)wrote; continue; }
    if (wrote < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}
