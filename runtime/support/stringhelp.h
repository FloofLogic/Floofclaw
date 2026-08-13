#ifndef FCLAW_SUPPORT_STRINGHELP_H
#define FCLAW_SUPPORT_STRINGHELP_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *s;
  size_t n;
} str_t;

static inline str_t str_from_c(const char *s) {
  str_t r = {s, s ? strlen(s) : 0};
  return r;
}

static inline int str_eq(str_t a, str_t b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.s, b.s, a.n) == 0);
}

static inline int str_eqc(str_t a, const char *b) {
  return str_eq(a, str_from_c(b));
}

static inline int str_starts(str_t s, str_t pre) {
  return s.n >= pre.n && (pre.n == 0 || memcmp(s.s, pre.s, pre.n) == 0);
}

static inline int str_ends(str_t s, str_t suf) {
  return s.n >= suf.n &&
         (suf.n == 0 || memcmp(s.s + s.n - suf.n, suf.s, suf.n) == 0);
}

static inline int str_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' ||
         c == '\r' || c == '\v' || c == '\f';
}

static inline str_t str_trim_left(str_t s) {
  size_t i = 0;
  while (i < s.n && str_space(s.s[i])) i++;
  return (str_t){s.s + i, s.n - i};
}

static inline str_t str_trim_right(str_t s) {
  while (s.n && str_space(s.s[s.n - 1])) s.n--;
  return s;
}

static inline str_t str_trim(str_t s) {
  return str_trim_right(str_trim_left(s));
}

/* Offset-tracked formatted append. */
static inline int str_appendf(char *buf, size_t len, size_t *used,
                              const char *fmt, ...) {
  va_list ap;
  int n;
  if (!buf || !used || !fmt || *used >= len) return -1;
  va_start(ap, fmt);
  n = vsnprintf(buf + *used, len - *used, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= len - *used) return -1;
  *used += (size_t)n;
  return 0;
}

#endif
