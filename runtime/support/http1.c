#include "http1.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Bounds. A response head larger than this is refused rather than scanned
 * forever; real APIs stay far below it. */
#define HTTP1_HEAD_MAX 16384
#define HTTP1_CHUNK_LINE_MAX 64

static void fail(FcHttp1Response *out, const char *reason) {
  if (!out) return;
  snprintf(out->error, sizeof(out->error), "%s", reason ? reason : "malformed");
}

/* Find the end of the response head. Returns the offset just past the
 * blank line, or 0 when the head is not complete yet. */
static size_t head_end(const char *buf, size_t len) {
  for (size_t i = 0; i + 3 < len; ++i)
    if (buf[i] == '\r' && buf[i + 1] == '\n' &&
        buf[i + 2] == '\r' && buf[i + 3] == '\n')
      return i + 4;
  return 0;
}

static int ci_equal(const char *a, size_t a_len, const char *b) {
  size_t i;
  for (i = 0; i < a_len && b[i]; ++i)
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
  return i == a_len && b[i] == '\0';
}

/* Walk the header block, handing each name/value span to the caller. */
typedef int (*HeaderFn)(const char *name, size_t name_len,
                        const char *value, size_t value_len, void *user);

static int for_each_header(const char *head, size_t head_len,
                           HeaderFn fn, void *user) {
  const char *p = head;
  const char *end = head + head_len;
  const char *line_end;
  /* Skip the status line. */
  line_end = memchr(p, '\n', (size_t)(end - p));
  if (!line_end) return -1;
  p = line_end + 1;
  while (p < end) {
    const char *colon;
    const char *value;
    size_t value_len;
    line_end = memchr(p, '\n', (size_t)(end - p));
    if (!line_end) break;
    if (line_end == p || (line_end == p + 1 && p[0] == '\r')) break; /* blank */
    colon = memchr(p, ':', (size_t)(line_end - p));
    if (colon) {
      value = colon + 1;
      value_len = (size_t)(line_end - value);
      while (value_len > 0 && (*value == ' ' || *value == '\t')) { value++; value_len--; }
      while (value_len > 0 &&
             (value[value_len - 1] == '\r' || value[value_len - 1] == ' ' ||
              value[value_len - 1] == '\t'))
        value_len--;
      if (fn(p, (size_t)(colon - p), value, value_len, user) != 0) return 0;
    }
    p = line_end + 1;
  }
  return 0;
}

typedef struct {
  const char *want;
  char *out;
  size_t out_cap;
  int found;
} HeaderLookup;

static int lookup_header(const char *name, size_t name_len,
                         const char *value, size_t value_len, void *user) {
  HeaderLookup *l = (HeaderLookup *)user;
  if (!ci_equal(name, name_len, l->want)) return 0;
  if (value_len + 1U > l->out_cap) return 1; /* stop: does not fit */
  memcpy(l->out, value, value_len);
  l->out[value_len] = '\0';
  l->found = 1;
  return 1;
}

int fc_http1_header(const char *buf, size_t len, const char *name,
                    char *out, size_t out_cap) {
  HeaderLookup l;
  size_t head_len;
  if (!buf || !name || !out || out_cap == 0) return -1;
  out[0] = '\0';
  head_len = head_end(buf, len);
  if (head_len == 0) return -1;
  l.want = name; l.out = out; l.out_cap = out_cap; l.found = 0;
  if (for_each_header(buf, head_len, lookup_header, &l) != 0) return -1;
  return l.found ? 0 : -1;
}

typedef struct {
  long long content_length;   /* -1 when absent */
  int chunked;
  int close_requested;
  int bad;
} HeadInfo;

static int scan_head(const char *name, size_t name_len,
                     const char *value, size_t value_len, void *user) {
  HeadInfo *h = (HeadInfo *)user;
  if (ci_equal(name, name_len, "content-length")) {
    long long n = 0;
    size_t i;
    if (value_len == 0) { h->bad = 1; return 1; }
    for (i = 0; i < value_len; ++i) {
      if (value[i] < '0' || value[i] > '9') { h->bad = 1; return 1; }
      n = n * 10 + (value[i] - '0');
      if (n > (long long)HTTP1_HEAD_MAX * 1024) { h->bad = 1; return 1; }
    }
    h->content_length = n;
  } else if (ci_equal(name, name_len, "transfer-encoding")) {
    /* Only the identity and chunked codings are supported; anything else
     * would be decoded wrong, so refuse rather than guess. */
    if (value_len >= 7 && ci_equal(value + value_len - 7, 7, "chunked"))
      h->chunked = 1;
    else
      h->bad = 1;
  } else if (ci_equal(name, name_len, "connection")) {
    if (value_len == 5 && ci_equal(value, 5, "close")) h->close_requested = 1;
  }
  return 0;
}

/* Decode a chunked body starting at `p`. Returns 1 when the terminating
 * zero chunk was seen, 0 when more bytes are needed, -1 on malformed input
 * or overflow. */
static int decode_chunked(const char *p, const char *end,
                          char *body_out, size_t body_cap,
                          size_t *body_len, size_t *consumed,
                          FcHttp1Response *out) {
  const char *start = p;
  size_t total = 0;
  for (;;) {
    const char *line_end = memchr(p, '\n', (size_t)(end - p));
    size_t size = 0;
    size_t digits = 0;
    if (!line_end) return 0;
    if ((size_t)(line_end - p) > HTTP1_CHUNK_LINE_MAX) {
      fail(out, "chunk size line too long");
      return -1;
    }
    for (const char *d = p; d < line_end && *d != ';' && *d != '\r'; ++d) {
      int v;
      if (*d >= '0' && *d <= '9') v = *d - '0';
      else if (*d >= 'a' && *d <= 'f') v = 10 + (*d - 'a');
      else if (*d >= 'A' && *d <= 'F') v = 10 + (*d - 'A');
      else { fail(out, "malformed chunk size"); return -1; }
      size = size * 16U + (size_t)v;
      digits++;
      if (size > body_cap + 1U) { fail(out, "chunked body exceeds cap"); return -1; }
    }
    if (digits == 0) { fail(out, "empty chunk size"); return -1; }
    p = line_end + 1;
    if (size == 0) {
      /* Trailer section, then the final CRLF. */
      for (;;) {
        const char *t = memchr(p, '\n', (size_t)(end - p));
        if (!t) return 0;
        if (t == p || (t == p + 1 && p[0] == '\r')) { p = t + 1; break; }
        p = t + 1;
      }
      *body_len = total;
      *consumed = (size_t)(p - start);
      return 1;
    }
    if ((size_t)(end - p) < size + 2U) return 0; /* chunk + CRLF not here yet */
    if (total + size > body_cap) { fail(out, "chunked body exceeds cap"); return -1; }
    memcpy(body_out + total, p, size);
    total += size;
    p += size;
    if (p + 1 >= end || p[0] != '\r' || p[1] != '\n') {
      if ((size_t)(end - p) < 2U) return 0;
      fail(out, "missing chunk terminator");
      return -1;
    }
    p += 2;
  }
}

FcHttp1Status fc_http1_parse_response(const char *buf, size_t len,
                                      char *body_out, size_t body_cap,
                                      FcHttp1Response *out) {
  size_t head_len;
  HeadInfo info;
  const char *body;
  size_t avail;
  if (!buf || !out) return FC_HTTP1_BAD;
  memset(out, 0, sizeof(*out));
  out->keep_alive = 1;
  if (len > HTTP1_HEAD_MAX && head_end(buf, len) == 0) {
    fail(out, "response head exceeds bound");
    return FC_HTTP1_BAD;
  }
  head_len = head_end(buf, len);
  if (head_len == 0) return FC_HTTP1_NEED_MORE;

  /* Status line: HTTP/1.x SP code SP reason */
  if (len < 12 || strncmp(buf, "HTTP/1.", 7) != 0 ||
      (buf[7] != '0' && buf[7] != '1') || buf[8] != ' ') {
    fail(out, "not an HTTP/1.x status line");
    return FC_HTTP1_BAD;
  }
  if (buf[9] < '0' || buf[9] > '9' || buf[10] < '0' || buf[10] > '9' ||
      buf[11] < '0' || buf[11] > '9') {
    fail(out, "malformed status code");
    return FC_HTTP1_BAD;
  }
  out->status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');

  info.content_length = -1;
  info.chunked = 0;
  info.close_requested = 0;
  info.bad = 0;
  (void)for_each_header(buf, head_len, scan_head, &info);
  if (info.bad) {
    fail(out, "unsupported or malformed framing header");
    return FC_HTTP1_BAD;
  }
  if (buf[7] == '0' || info.close_requested) out->keep_alive = 0;

  body = buf + head_len;
  avail = len - head_len;

  /* Responses that carry no body by definition. */
  if (out->status == 204 || out->status == 304 ||
      (out->status >= 100 && out->status < 200)) {
    out->body_len = 0;
    out->consumed = head_len;
    if (body_out && body_cap > 0) body_out[0] = '\0';
    return FC_HTTP1_DONE;
  }

  if (info.chunked) {
    size_t body_len = 0, used = 0;
    int rc;
    if (!body_out || body_cap == 0) { fail(out, "no body buffer"); return FC_HTTP1_BAD; }
    rc = decode_chunked(body, body + avail, body_out, body_cap - 1U,
                        &body_len, &used, out);
    if (rc < 0) return FC_HTTP1_BAD;
    if (rc == 0) return FC_HTTP1_NEED_MORE;
    body_out[body_len] = '\0';
    out->body_len = body_len;
    out->consumed = head_len + used;
    return FC_HTTP1_DONE;
  }

  if (info.content_length >= 0) {
    size_t want = (size_t)info.content_length;
    if (!body_out || body_cap == 0) { fail(out, "no body buffer"); return FC_HTTP1_BAD; }
    if (want + 1U > body_cap) { fail(out, "body exceeds cap"); return FC_HTTP1_BAD; }
    if (avail < want) return FC_HTTP1_NEED_MORE;
    memcpy(body_out, body, want);
    body_out[want] = '\0';
    out->body_len = want;
    out->consumed = head_len + want;
    return FC_HTTP1_DONE;
  }

  /* No framing header: the body runs to connection close. The caller can
   * only know it is complete once the peer closes, so report NEED_MORE and
   * let the transport decide; it calls once more with the final buffer. */
  if (!body_out || body_cap == 0) { fail(out, "no body buffer"); return FC_HTTP1_BAD; }
  if (avail + 1U > body_cap) { fail(out, "body exceeds cap"); return FC_HTTP1_BAD; }
  memcpy(body_out, body, avail);
  body_out[avail] = '\0';
  out->body_len = avail;
  out->consumed = len;
  out->keep_alive = 0;
  return FC_HTTP1_NEED_MORE;
}

int fc_http1_build_request(char *out, size_t out_cap,
                           const char *method, const char *path,
                           const char *host, const char *user_agent,
                           const char *extra_headers,
                           const char *content_type, const char *body) {
  int n;
  char length_header[64] = "";
  char type_header[128] = "";
  if (!out || out_cap == 0 || !method || !path || !host) return -1;
  if (body) {
    if (snprintf(length_header, sizeof(length_header),
                 "Content-Length: %zu\r\n", strlen(body)) >=
        (int)sizeof(length_header))
      return -1;
    if (content_type &&
        snprintf(type_header, sizeof(type_header), "Content-Type: %s\r\n",
                 content_type) >= (int)sizeof(type_header))
      return -1;
  }
  n = snprintf(out, out_cap,
               "%s %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "User-Agent: %s\r\n"
               "Accept: */*\r\n"
               "%s%s%s\r\n%s",
               method, path, host,
               user_agent && *user_agent ? user_agent : "floofclaw",
               extra_headers ? extra_headers : "",
               type_header, length_header,
               body ? body : "");
  return (n < 0 || (size_t)n >= out_cap) ? -1 : 0;
}
