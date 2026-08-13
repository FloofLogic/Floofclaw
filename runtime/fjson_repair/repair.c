#include "repair.h"
#include "../support/json.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#define FJR_STACK_MAX 128
#define FJR_OBJECT_OPEN  ((char)123)
#define FJR_OBJECT_CLOSE ((char)125)
#define FJR_ARRAY_OPEN   ((char)91)
#define FJR_ARRAY_CLOSE  ((char)93)
typedef enum {
  FJR_CTX_OBJECT_KEY_OR_END = 1,
  FJR_CTX_OBJECT_COLON,
  FJR_CTX_OBJECT_VALUE,
  FJR_CTX_OBJECT_COMMA_OR_END,
  FJR_CTX_ARRAY_VALUE_OR_END,
  FJR_CTX_ARRAY_COMMA_OR_END
} FjrCtx;
typedef struct {
  char opener;
  FjrCtx ctx;
} FjrStackItem;
typedef struct {
  char *out;
  size_t cap;
  size_t pos;
  FjrReport *report;
} FjrWriter;
#define FJR_MARK(r, field) do { if ((r) && !(r)->field) { (r)->field = 1; (r)->repairs++; } } while (0)
static const char *skip_ws_span(const char *p, const char *end) {
  while (p < end && isspace((unsigned char)*p)) p++;
  return p;
}
static const char *trim_end_ws(const char *start, const char *end) {
  (void)start;
  while (end > start && isspace((unsigned char)end[-1])) end--;
  return end;
}
static int append_char(FjrWriter *w, char ch) {
  if (!w || !w->out || w->cap == 0) return -1;
  if (w->pos + 1 >= w->cap) return -1;
  w->out[w->pos++] = ch;
  w->out[w->pos] = '\0';
  return 0;
}
static int append_text(FjrWriter *w, const char *s) {
  size_t n;
  if (!s) return -1;
  n = strlen(s);
  if (!w || !w->out || w->pos + n >= w->cap) return -1;
  memcpy(w->out + w->pos, s, n);
  w->pos += n;
  w->out[w->pos] = '\0';
  return 0;
}
static int is_ident_start(char ch) {
  return isalpha((unsigned char)ch) || ch == '_' || ch == '$';
}
static int is_ident_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.' || ch == '$';
}
static int is_simple_json_escape(char ch) {
  return ch == '"' || ch == '\\' || ch == '/' || ch == 'b' ||
         ch == 'f' || ch == 'n' || ch == 'r' || ch == 't';
}
static int is_value_expected(const FjrStackItem *stack, size_t sp) {
  if (sp == 0) return 1;
  switch (stack[sp - 1].ctx) {
  case FJR_CTX_OBJECT_VALUE:
  case FJR_CTX_ARRAY_VALUE_OR_END:
    return 1;
  default:
    return 0;
  }
}
static int is_key_expected(const FjrStackItem *stack, size_t sp) {
  return sp > 0 && stack[sp - 1].opener == FJR_OBJECT_OPEN && stack[sp - 1].ctx == FJR_CTX_OBJECT_KEY_OR_END;
}
static void mark_value_done(FjrStackItem *stack, size_t sp) {
  if (sp == 0) return;
  if (stack[sp - 1].ctx == FJR_CTX_OBJECT_VALUE) stack[sp - 1].ctx = FJR_CTX_OBJECT_COMMA_OR_END;
  else if (stack[sp - 1].ctx == FJR_CTX_ARRAY_VALUE_OR_END) stack[sp - 1].ctx = FJR_CTX_ARRAY_COMMA_OR_END;
}
static int push_container(FjrStackItem *stack, size_t *sp, char opener) {
  if (!stack || !sp || *sp >= FJR_STACK_MAX) return -1;
  stack[*sp].opener = opener;
  stack[*sp].ctx = opener == FJR_OBJECT_OPEN ? FJR_CTX_OBJECT_KEY_OR_END : FJR_CTX_ARRAY_VALUE_OR_END;
  (*sp)++;
  return 0;
}
static int append_json_string(FjrWriter *w, const char *p, const char *end,
                              char quote, const char **after_out) {
  int closed = 0;
  if (append_char(w, '"') != 0) return -1;
  p++;
  while (p < end) {
    unsigned char ch = (unsigned char)*p;
    if (*p == quote) { closed = 1; p++; break; }
    if (quote == '"' && *p == '\\') {
      if (p + 1 >= end) {
        FJR_MARK(w->report, closed_string);
        if (append_text(w, "\\\\") != 0) return -1;
        p++;
        continue;
      }
      if (is_simple_json_escape(p[1])) {
        if (append_char(w, *p++) != 0) return -1;
        if (append_char(w, *p++) != 0) return -1;
        continue;
      }
      if (p[1] == 'u' && p + 5 < end &&
          isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) &&
          isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5])) {
        for (int i = 0; i < 6; ++i) {
          if (append_char(w, *p++) != 0) return -1;
        }
        continue;
      }
      FJR_MARK(w->report, closed_string);
      if (append_text(w, "\\\\") != 0) return -1;
      p++;
      continue;
    }
    if (quote == '\'' && *p == '\\' && p + 1 < end && p[1] == '\'') {
      if (append_char(w, '\'') != 0) return -1;
      p += 2;
      continue;
    }
    if (quote == '\'' && (*p == '"' || *p == '\\')) {
      if (append_char(w, '\\') != 0 || append_char(w, *p++) != 0) return -1;
      continue;
    }
    if (*p == '\n') {
      FJR_MARK(w->report, closed_string);
      if (append_text(w, "\\n") != 0) return -1;
      p++;
      continue;
    }
    if (*p == '\t') {
      FJR_MARK(w->report, closed_string);
      if (append_text(w, "\\t") != 0) return -1;
      p++;
      continue;
    }
    if (ch < 0x20U) {
      char esc[7];
      FJR_MARK(w->report, closed_string);
      snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)ch);
      if (append_text(w, esc) != 0) return -1;
      p++;
      continue;
    }
    if (append_char(w, *p++) != 0) return -1;
  }
  if (!closed) {
    FJR_MARK(w->report, closed_string);
  }
  if (append_char(w, '"') != 0) return -1;
  *after_out = p;
  if (quote == '\'') FJR_MARK(w->report, converted_single_quotes);
  return 0;
}
static int append_bare_key(FjrWriter *w, const char *p, const char *end, const char **after_out) {
  const char *q = p;
  while (q < end && is_ident_char(*q)) q++;
  if (skip_ws_span(q, end) >= end || *skip_ws_span(q, end) != ':') return -1;
  if (!w || w->pos + (size_t)(q - p) + 2 >= w->cap) return FJR_ERR_OUTPUT_TOO_SMALL;
  if (append_char(w, '"') != 0) return -1;
  while (p < q) {
    if (*p == '"' || *p == '\\') {
      if (append_char(w, '\\') != 0) return -1;
    }
    if (append_char(w, *p++) != 0) return -1;
  }
  if (append_char(w, '"') != 0) return -1;
  *after_out = q;
  FJR_MARK(w->report, added_missing_quote);
  return 0;
}
static int token_boundary(const char *p, const char *end) {
  return p >= end || isspace((unsigned char)*p) || *p == ',' || *p == FJR_OBJECT_CLOSE || *p == FJR_ARRAY_CLOSE;
}
static int token_can_start_value(char ch) {
  return ch == FJR_OBJECT_OPEN || ch == FJR_ARRAY_OPEN || ch == '"' || ch == '\'' ||
         ch == '-' || isdigit((unsigned char)ch) || is_ident_start(ch);
}
static int is_bare_value_char(char ch) {
  return ch != ',' && ch != FJR_OBJECT_CLOSE && ch != FJR_ARRAY_CLOSE &&
         !isspace((unsigned char)ch);
}
static int looks_like_bare_key(const char *p, const char *end) {
  const char *q = p;
  if (!is_ident_start(*q)) return 0;
  while (q < end && is_ident_char(*q)) q++;
  q = skip_ws_span(q, end);
  return q < end && *q == ':';
}
static int append_literal_or_number(FjrWriter *w, const char *p, const char *end, const char **after_out) {
  const char *q = p;
  const char *replacement = NULL;
  size_t n = 0;
  if ((size_t)(end - p) >= 4 && strncmp(p, "True", 4) == 0 && token_boundary(p + 4, end)) {
    replacement = "true"; n = 4;
  } else if ((size_t)(end - p) >= 5 && strncmp(p, "False", 5) == 0 && token_boundary(p + 5, end)) {
    replacement = "false"; n = 5;
  } else if ((size_t)(end - p) >= 4 && strncmp(p, "None", 4) == 0 && token_boundary(p + 4, end)) {
    replacement = "null"; n = 4;
  }
  if (replacement) {
    if (append_text(w, replacement) != 0) return -1;
    *after_out = p + n;
    FJR_MARK(w->report, replaced_invalid_literal);
    return 0;
  }
  /* Bound each literal match by the remaining span BEFORE strncmp:
   * the input is a length-delimited buffer (not guaranteed NUL-
   * terminated), so an unchecked strncmp near `end` reads out of
   * bounds. Mirrors the guarded True/False/None matches above. */
  if ((size_t)(end - p) >= 4 && strncmp(p, "true", 4) == 0 && token_boundary(p + 4, end)) q = p + 4;
  else if ((size_t)(end - p) >= 5 && strncmp(p, "false", 5) == 0 && token_boundary(p + 5, end)) q = p + 5;
  else if ((size_t)(end - p) >= 4 && strncmp(p, "null", 4) == 0 && token_boundary(p + 4, end)) q = p + 4;
  else if (*p == '-' || isdigit((unsigned char)*p)) {
    q++;
    while (q < end && (isdigit((unsigned char)*q) || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-')) q++;
  } else if (is_ident_start(*p) || is_bare_value_char(*p)) {
    if (append_char(w, '"') != 0) return -1;
    while (q < end && is_bare_value_char(*q)) {
      if (*q == '"' || *q == '\\') {
        if (append_char(w, '\\') != 0) return -1;
      }
      if (append_char(w, *q++) != 0) return -1;
    }
    if (append_char(w, '"') != 0) return -1;
    *after_out = q;
    FJR_MARK(w->report, added_missing_quote);
    return 0;
  } else {
    return -1;
  }
  if (w->pos + (size_t)(q - p) >= w->cap) return -1;
  memcpy(w->out + w->pos, p, (size_t)(q - p));
  w->pos += (size_t)(q - p);
  w->out[w->pos] = '\0';
  *after_out = q;
  return 0;
}
static int can_close_top(FjrStackItem item, char closer) {
  if ((item.opener == FJR_OBJECT_OPEN && closer != FJR_OBJECT_CLOSE) || (item.opener == FJR_ARRAY_OPEN && closer != FJR_ARRAY_CLOSE)) return 0;
  if (item.opener == FJR_OBJECT_OPEN) {
    return item.ctx == FJR_CTX_OBJECT_KEY_OR_END || item.ctx == FJR_CTX_OBJECT_COMMA_OR_END;
  }
  return item.ctx == FJR_CTX_ARRAY_VALUE_OR_END || item.ctx == FJR_CTX_ARRAY_COMMA_OR_END;
}
static char expected_closer(FjrStackItem item) {
  return item.opener == FJR_OBJECT_OPEN ? FJR_OBJECT_CLOSE : FJR_ARRAY_CLOSE;
}
static int is_any_closer(char ch) {
  return ch == FJR_OBJECT_CLOSE || ch == FJR_ARRAY_CLOSE || ch == ')' || ch == '>';
}
/* A comma consumed from input is emitted immediately; when the input
 * then ends (or an unclosed container is auto-closed), that comma is
 * left dangling right before the closer — e.g. `{"name":"work",}` —
 * which is not valid JSON. Any comma sitting at the end of the output
 * buffer at close time can only be a structural separator (string
 * values are always closed with `"` before a structural comma), so it
 * is safe to retract before appending a closer. */
static void retract_trailing_comma(FjrWriter *w) {
  size_t p = w->pos;
  while (p > 0 && isspace((unsigned char)w->out[p - 1])) p--;
  if (p > 0 && w->out[p - 1] == ',') {
    w->pos = p - 1;
    w->out[w->pos] = '\0';
    FJR_MARK(w->report, removed_trailing_comma);
  }
}
static int close_remaining(FjrStackItem *stack, size_t *sp, FjrWriter *w) {
  while (*sp > 0) {
    char closer = expected_closer(stack[*sp - 1]);
    if (!can_close_top(stack[*sp - 1], closer)) return FJR_ERR_UNREPAIRABLE;
    retract_trailing_comma(w);
    if (append_char(w, closer) != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
    if (closer == FJR_OBJECT_CLOSE) FJR_MARK(w->report, closed_object);
    else FJR_MARK(w->report, closed_array);
    (*sp)--;
    if (*sp > 0) mark_value_done(stack, *sp);
  }
  return 0;
}
static int maybe_insert_missing_comma(FjrStackItem *stack, size_t sp,
                                      FjrWriter *w, const char *p,
                                      const char *end, char ch) {
  if (sp == 0) return 0;
  if (stack[sp - 1].ctx == FJR_CTX_OBJECT_COMMA_OR_END &&
      ch != ',' && ch != FJR_OBJECT_CLOSE &&
      (ch == '"' || ch == '\'' || looks_like_bare_key(p, end))) {
    if (append_char(w, ',') != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
    stack[sp - 1].ctx = FJR_CTX_OBJECT_KEY_OR_END;
    FJR_MARK(w->report, added_missing_comma);
    return 1;
  }
  if (stack[sp - 1].ctx == FJR_CTX_ARRAY_COMMA_OR_END &&
      ch != ',' && ch != FJR_ARRAY_CLOSE && token_can_start_value(ch)) {
    if (append_char(w, ',') != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
    stack[sp - 1].ctx = FJR_CTX_ARRAY_VALUE_OR_END;
    FJR_MARK(w->report, added_missing_comma);
    return 1;
  }
  return 0;
}
static int close_top(FjrStackItem *stack, size_t *sp, FjrWriter *w,
                     char actual, const char *base, const char *p) {
  char want;
  if (!stack || !sp || !w || *sp == 0) {
    FJR_MARK(w->report, trimmed_trailing_junk);
    return 1;
  }
  want = expected_closer(stack[*sp - 1]);
  if (!can_close_top(stack[*sp - 1], want)) {
    w->report->error_offset = (size_t)(p - base);
    return FJR_ERR_UNREPAIRABLE;
  }
  if (append_char(w, want) != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
  if (actual != want) FJR_MARK(w->report, substituted_wrong_closer);
  (*sp)--;
  if (*sp > 0) mark_value_done(stack, *sp);
  return 0;
}
static int repair_candidate(const char *base, const char *start, const char *end,
                            FjrWriter *w, const char **consumed_out) {
  FjrStackItem stack[FJR_STACK_MAX];
  size_t sp = 0;
  const char *p = start;
  int root_started = 0;
  int rc;
  while (p < end) {
    char ch = *p;
    if (isspace((unsigned char)ch)) {
      if (append_char(w, ch) != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
      p++;
      continue;
    }
    if (ch == '/' && p + 1 < end && p[1] == '/') {
      p += 2;
      while (p < end && *p != '\n') p++;
      FJR_MARK(w->report, stripped_comments);
      continue;
    }
    if (ch == '/' && p + 1 < end && p[1] == '*') {
      p += 2;
      while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
      FJR_MARK(w->report, stripped_comments);
      if (p + 1 >= end) { p = end; continue; }
      p += 2;
      continue;
    }
    {
      int comma_rc = maybe_insert_missing_comma(stack, sp, w, p, end, ch);
      if (comma_rc == FJR_ERR_OUTPUT_TOO_SMALL) return FJR_ERR_OUTPUT_TOO_SMALL;
      if (comma_rc > 0) continue;
    }
    if (ch == FJR_OBJECT_OPEN || ch == FJR_ARRAY_OPEN) {
      if (root_started && !is_value_expected(stack, sp)) { w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE; }
      if (append_char(w, ch) != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
      if (push_container(stack, &sp, ch) != 0) { w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE; }
      root_started = 1;
      p++;
      continue;
    }
    if (is_any_closer(ch)) {
      int close_rc = close_top(stack, &sp, w, ch, base, p);
      if (close_rc == 1) break;
      if (close_rc != 0) return close_rc;
      p++;
      if (sp == 0) break;
      continue;
    }
    if (ch == ',') {
      const char *nn = skip_ws_span(p + 1, end);
      if (nn < end && (*nn == FJR_OBJECT_CLOSE || *nn == FJR_ARRAY_CLOSE)) {
        FJR_MARK(w->report, removed_trailing_comma);
        p++;
        continue;
      }
      if (sp == 0) { w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE; }
      if (stack[sp - 1].ctx == FJR_CTX_OBJECT_COMMA_OR_END) stack[sp - 1].ctx = FJR_CTX_OBJECT_KEY_OR_END;
      else if (stack[sp - 1].ctx == FJR_CTX_ARRAY_COMMA_OR_END) stack[sp - 1].ctx = FJR_CTX_ARRAY_VALUE_OR_END;
      else { w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE; }
      if (append_char(w, ch) != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
      p++;
      continue;
    }
    if (ch == ':') {
      if (sp == 0 || stack[sp - 1].ctx != FJR_CTX_OBJECT_COLON) { w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE; }
      stack[sp - 1].ctx = FJR_CTX_OBJECT_VALUE;
      if (append_char(w, ch) != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
      p++;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      const char *after = NULL;
      if (ch == '\'' && !(is_key_expected(stack, sp) || is_value_expected(stack, sp))) {
        w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE;
      }
      if (append_json_string(w, p, end, ch, &after) != 0) return FJR_ERR_OUTPUT_TOO_SMALL;
      if (is_key_expected(stack, sp)) stack[sp - 1].ctx = FJR_CTX_OBJECT_COLON;
      else mark_value_done(stack, sp);
      p = after;
      continue;
    }
    if (is_key_expected(stack, sp) && is_ident_start(ch)) {
      const char *after = NULL;
      int key_rc = append_bare_key(w, p, end, &after);
      if (key_rc == FJR_ERR_OUTPUT_TOO_SMALL) return FJR_ERR_OUTPUT_TOO_SMALL;
      if (key_rc != 0) { w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE; }
      stack[sp - 1].ctx = FJR_CTX_OBJECT_COLON;
      p = after;
      continue;
    }
    if (is_value_expected(stack, sp)) {
      const char *after = NULL;
      if (append_literal_or_number(w, p, end, &after) != 0) { w->report->error_offset = (size_t)(p - base); return FJR_ERR_UNREPAIRABLE; }
      mark_value_done(stack, sp);
      p = after;
      continue;
    }
    if (sp > 0 &&
        (stack[sp - 1].ctx == FJR_CTX_OBJECT_COMMA_OR_END ||
         stack[sp - 1].ctx == FJR_CTX_ARRAY_COMMA_OR_END)) {
      int close_rc = close_remaining(stack, &sp, w);
      if (close_rc != 0) return close_rc;
      FJR_MARK(w->report, trimmed_trailing_junk);
      break;
    }
    w->report->error_offset = (size_t)(p - base);
    return FJR_ERR_UNREPAIRABLE;
  }
  if (!root_started) return FJR_ERR_UNREPAIRABLE;
  rc = close_remaining(stack, &sp, w);
  if (rc != 0) { w->report->error_offset = (size_t)(end - base); return rc; }
  if (consumed_out) *consumed_out = p;
  return FJR_OK_REPAIRED;
}
static int has_later_candidate(const char *p, const char *end) {
  while (p < end) {
    if (*p == FJR_OBJECT_OPEN || *p == FJR_ARRAY_OPEN) return 1;
    p++;
  }
  return 0;
}
int fjson_repair(const char *in, size_t in_len,
                 char *out, size_t out_cap,
                 size_t *out_len,
                 struct fjson_repair_report *report) {
  FjrReport local_report;
  const char *base = in;
  const char *begin;
  const char *end;
  const char *trimmed_begin;
  const char *trimmed_end;
  const char *start;
  const char *consumed = NULL;
  FjrWriter writer;
  int rc;
  if (report) memset(report, 0, sizeof(*report));
  memset(&local_report, 0, sizeof(local_report));
  if (!report) report = &local_report;
  if (out_len) *out_len = 0;
  if (!in || !out || out_cap == 0) return FJR_ERR_UNREPAIRABLE;
  out[0] = '\0';
  if (in_len + 1 <= out_cap) {
    JsonRef strict;
    memcpy(out, in, in_len);
    out[in_len] = '\0';
    if (json_ref_from_text(out, &strict) == 0) {
      if (out_len) *out_len = in_len;
      report->candidate_offset = 0;
      report->candidate_len = in_len;
      return FJR_OK_UNCHANGED;
    }
  }
  begin = in;
  end = in + in_len;
  trimmed_begin = skip_ws_span(begin, end);
  trimmed_end = trim_end_ws(trimmed_begin, end);
  begin = trimmed_begin;
  end = trimmed_end;
  if ((size_t)(end - begin) >= 3 && begin[0] == '`' && begin[1] == '`' && begin[2] == '`') {
    const char *nl = memchr(begin, '\n', (size_t)(end - begin));
    const char *last = NULL;
    const char *p = begin + 3;
    while (p + 2 < end) {
      if (p[0] == '`' && p[1] == '`' && p[2] == '`') last = p;
      p++;
    }
    if (!nl) return FJR_ERR_UNREPAIRABLE;
    begin = nl + 1;
    if (last && last > begin) end = trim_end_ws(begin, last);
    FJR_MARK(report, stripped_fence);
  }
  start = begin;
  while (start < end && *start != FJR_OBJECT_OPEN && *start != FJR_ARRAY_OPEN) start++;
  if (start >= end) return FJR_ERR_UNREPAIRABLE;
  if (skip_ws_span(begin, start) < start) FJR_MARK(report, stripped_prose);
  writer.out = out;
  writer.cap = out_cap;
  writer.pos = 0;
  writer.report = report;
  rc = repair_candidate(base, start, end, &writer, &consumed);
  if (rc != FJR_OK_REPAIRED) return rc;
  if (!consumed) consumed = end;
  if (has_later_candidate(consumed, end)) return FJR_ERR_AMBIGUOUS;
  if (skip_ws_span(consumed, end) < end) FJR_MARK(report, trimmed_trailing_junk);
  /* Contract backstop: a success return MUST mean the output is valid
   * JSON. Repair handles the common structural cases (missing/trailing
   * commas, unquoted keys/values, fences, python literals), but hostile
   * input can still drive an emit path to produce invalid output — e.g.
   * a raw control character preserved inside a string. Rather than
   * enumerate every such path, verify the result and honestly downgrade
   * to unrepairable if it does not strictly parse. This guarantees the
   * invariant the caller relies on: repaired output always re-parses. */
  {
    JsonRef verify;
    if (json_ref_from_text(out, &verify) != 0) return FJR_ERR_UNREPAIRABLE;
  }
  report->candidate_offset = (size_t)(start - base);
  report->candidate_len = (size_t)(consumed - start);
  if (out_len) *out_len = writer.pos;
  return report->repairs ? FJR_OK_REPAIRED : FJR_OK_UNCHANGED;
}
