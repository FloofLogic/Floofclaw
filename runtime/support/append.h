#ifndef FCLAW_SUPPORT_APPEND_H
#define FCLAW_SUPPORT_APPEND_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "heap_guard.h"

static inline int rt_append_text(char *out, size_t out_len, size_t *pos,
                                 const char *text) {
  size_t n = strlen(text ? text : "");
  if (!out || !pos || *pos + n >= out_len) return -1;
  memcpy(out + *pos, text ? text : "", n);
  *pos += n;
  out[*pos] = '\0';
  return 0;
}

static inline int rt_dyn_append_text(char **out, size_t *out_len, size_t *pos,
                                     const char *text) {
  const char *body = text ? text : "";
  size_t n = strlen(body);
  size_t new_len;
  char *grown;
  if (!out || !out_len || !pos) return -1;
  if (!*out || *out_len <= *pos + n + 1U) {
    new_len = *out_len ? *out_len : 256U;
    while (new_len <= *pos + n + 1U) {
      if (new_len > (size_t)-1 / 2U) return -1;
      new_len *= 2U;
    }
    grown = *out ? (char *)fc_xrealloc(*out, new_len)
                 : (char *)fc_xmalloc(new_len);
    if (!grown) return -1;
    if (!*out) grown[0] = '\0';
    *out = grown;
    *out_len = new_len;
  }
  memcpy(*out + *pos, body, n);
  *pos += n;
  (*out)[*pos] = '\0';
  return 0;
}

/* require_complete preserves the caller's old snprintf policy: checked
 * callers reject encoding errors/truncation before append; unchecked callers
 * append the buffer exactly as they did before this consolidation. */
static inline int rt_append_event_format(RtContext *ctx, const char *type,
                                         const char *source, char *payload,
                                         size_t payload_len,
                                         int require_complete,
                                         const char *format, ...) {
  va_list ap;
  int n;
  if (!payload || payload_len == 0 || !format) return -1;
  va_start(ap, format);
  n = vsnprintf(payload, payload_len, format, ap);
  va_end(ap);
  if (require_complete && (n < 0 || (size_t)n >= payload_len)) return -1;
  return rt_append_event(ctx, type, source, payload);
}

#endif
