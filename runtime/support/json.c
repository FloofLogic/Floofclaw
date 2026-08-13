#include "json.h"
#include "heap_guard.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
  while (p && *p && isspace((unsigned char)*p)) ++p;
  return p;
}

static int parse_json_string_end(const char *start, const char *limit, const char **end_ptr) {
  const char *p;
  if (!start || !end_ptr || !limit) return -1;
  if (*start != '"') return -1;
  p = start + 1;
  while (p < limit) {
    const char ch = *p;
    if (ch == '"') { *end_ptr = p + 1; return 0; }
    if (ch == '\\') {
      ++p;
      if (p >= limit) return -1;
      if (*p == 'u') {
        ++p;
        for (int i = 0; i < 4; ++i) {
          if (p + i >= limit || !isxdigit((unsigned char)p[i])) return -1;
        }
        p += 4;
      } else {
        if (*p != '"' && *p != '\\' && *p != '/' && *p != 'b' && *p != 'f' &&
            *p != 'n' && *p != 'r' && *p != 't') return -1;
        ++p;
      }
      continue;
    }
    if ((unsigned char)ch < 0x20U) return -1;
    ++p;
  }
  return -1;
}

static int parse_json_number_end(const char *start, const char *limit, const char **end_ptr) {
  const char *p;
  if (!start || !end_ptr || !limit) return -1;
  p = start;
  if (p >= limit) return -1;
  if (*p == '-') { ++p; if (p >= limit) return -1; }
  if (!isdigit((unsigned char)*p)) return -1;
  while (p < limit && isdigit((unsigned char)*p)) ++p;
  if (p < limit && *p == '.') {
    const char *frac = p + 1;
    if (frac >= limit || !isdigit((unsigned char)*frac)) return -1;
    p = frac;
    while (p < limit && isdigit((unsigned char)*p)) ++p;
  }
  if (p < limit && (*p == 'e' || *p == 'E')) {
    ++p;
    if (p >= limit) return -1;
    if (*p == '+' || *p == '-') { ++p; if (p >= limit) return -1; }
    if (!isdigit((unsigned char)*p)) return -1;
    while (p < limit && isdigit((unsigned char)*p)) ++p;
  }
  *end_ptr = p;
  return 0;
}

static int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

static int parse_hex4(const char *p, const char *limit, unsigned *out) {
  unsigned value = 0;
  if (!p || !out || p + 4 > limit) return -1;
  for (int i = 0; i < 4; ++i) {
    int hv = hex_value(p[i]);
    if (hv < 0) return -1;
    value = (value << 4U) | (unsigned)hv;
  }
  *out = value;
  return 0;
}

static int utf8_write(unsigned codepoint, char *out, size_t out_len, size_t *used) {
  if (!out || !used || *used >= out_len) return -1;
  if (codepoint <= 0x7FU) {
    if (*used + 1U >= out_len) return -1;
    out[(*used)++] = (char)codepoint;
    return 0;
  }
  if (codepoint <= 0x7FFU) {
    if (*used + 2U >= out_len) return -1;
    out[(*used)++] = (char)(0xC0U | (codepoint >> 6U));
    out[(*used)++] = (char)(0x80U | (codepoint & 0x3FU));
    return 0;
  }
  if (codepoint <= 0xFFFFU) {
    if (*used + 3U >= out_len) return -1;
    out[(*used)++] = (char)(0xE0U | (codepoint >> 12U));
    out[(*used)++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[(*used)++] = (char)(0x80U | (codepoint & 0x3FU));
    return 0;
  }
  if (codepoint <= 0x10FFFFU) {
    if (*used + 4U >= out_len) return -1;
    out[(*used)++] = (char)(0xF0U | (codepoint >> 18U));
    out[(*used)++] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
    out[(*used)++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[(*used)++] = (char)(0x80U | (codepoint & 0x3FU));
    return 0;
  }
  return -1;
}

static int parse_json_value_end(const char *start, const char *limit, const char **end_ptr) {
  if (!start || !end_ptr || !limit || start >= limit) return -1;
  switch (*start) {
  case '"':
    return parse_json_string_end(start, limit, end_ptr);
  case '{': {
    const char *p = start + 1;
    p = skip_ws(p);
    if (p > limit) return -1;
    if (p == limit) return -1;
    if (*p == '}') { *end_ptr = p + 1; return 0; }
    while (1) {
      const char *key_end = NULL;
      const char *value_end = NULL;
      if (parse_json_string_end(p, limit, &key_end) != 0) return -1;
      p = skip_ws(key_end);
      if (p >= limit || *p != ':') return -1;
      ++p;
      p = skip_ws(p);
      if (parse_json_value_end(p, limit, &value_end) != 0) return -1;
      p = skip_ws(value_end);
      if (p >= limit) return -1;
      if (*p == ',') { ++p; p = skip_ws(p); continue; }
      if (*p == '}') { *end_ptr = p + 1; return 0; }
      return -1;
    }
  }
  case '[': {
    const char *p = start + 1;
    p = skip_ws(p);
    if (p > limit) return -1;
    if (p == limit) return -1;
    if (*p == ']') { *end_ptr = p + 1; return 0; }
    while (1) {
      const char *value_end = NULL;
      if (parse_json_value_end(p, limit, &value_end) != 0) return -1;
      p = skip_ws(value_end);
      if (p >= limit) return -1;
      if (*p == ',') { ++p; p = skip_ws(p); continue; }
      if (*p == ']') { *end_ptr = p + 1; return 0; }
      return -1;
    }
  }
  case 't':
    if ((size_t)(limit - start) >= 4 && strncmp(start, "true", 4) == 0) {
      *end_ptr = start + 4; return 0;
    }
    return -1;
  case 'f':
    if ((size_t)(limit - start) >= 5 && strncmp(start, "false", 5) == 0) {
      *end_ptr = start + 5; return 0;
    }
    return -1;
  case 'n':
    if ((size_t)(limit - start) >= 4 && strncmp(start, "null", 4) == 0) {
      *end_ptr = start + 4; return 0;
    }
    return -1;
  default:
    if (isdigit((unsigned char)*start) || *start == '-')
      return parse_json_number_end(start, limit, end_ptr);
    return -1;
  }
}

static JsonRefType json_ref_infer_type(const char *p) {
  if (!p) return JSON_REF_INVALID;
  switch (*p) {
  case '{': return JSON_REF_OBJECT;
  case '[': return JSON_REF_ARRAY;
  case '"': return JSON_REF_STRING;
  case 't':
  case 'f': return JSON_REF_BOOL;
  case 'n': return JSON_REF_NULL;
  default:
    if (isdigit((unsigned char)*p) || *p == '-') return JSON_REF_NUMBER;
    return JSON_REF_INVALID;
  }
}

static int json_ref_init_span(const char *json_root, const char *start, const char *end, JsonRef *out) {
  if (!json_root || !start || !end || !out || end < start) return -1;
  out->json = json_root;
  out->start = start;
  out->end = end;
  out->type = json_ref_infer_type(start);
  return out->type == JSON_REF_INVALID ? -1 : 0;
}

int json_ref_from_text(const char *json, JsonRef *out) {
  const char *start;
  const char *end;
  size_t json_len;
  if (!json || !out) return -1;
  start = skip_ws(json);
  if (!start || !*start) return -1;
  json_len = strlen(json);
  if (parse_json_value_end(start, json + json_len, &end) != 0) return -1;
  if (end && *skip_ws(end) != '\0') return -1;
  return json_ref_init_span(json, start, end, out);
}

int json_ref_top_object(const char *text, JsonRef *out) {
  if (!text || !out) return -1;
  if (json_ref_from_text(text, out) != 0 || out->type != JSON_REF_OBJECT) return -1;
  return 0;
}

int json_ref_first_object(const char *text, JsonRef *out) {
  size_t text_len;
  size_t offset;
  if (!text || !out) return -1;
  text_len = strlen(text);
  for (offset = 0U; offset < text_len; ++offset) {
    const char *start = text + offset;
    const char *end = NULL;
    if (*start != '{') continue;
    if (parse_json_value_end(start, text + text_len, &end) != 0) continue;
    return json_ref_init_span(text, start, end, out);
  }
  return -1;
}

int json_ref_object_get(const JsonRef *obj, const char *key, JsonRef *out) {
  const char *p;
  if (!obj || !key || !out || !obj->start || !obj->end || obj->type != JSON_REF_OBJECT) return -1;
  p = obj->start + 1;
  if (p > obj->end) return -1;
  p = skip_ws(p);
  if (p > obj->end) return -1;
  if (p == obj->end - 1U && *p == '}') return -1;
  while (1) {
    const char *key_end = NULL;
    const char *value_start = NULL;
    const char *value_end = NULL;
    size_t key_len;
    char key_decoded[256];
    if (parse_json_string_end(p, obj->end, &key_end) != 0) return -1;
    {
      JsonRef key_ref = {obj->json, p, key_end, JSON_REF_STRING};
      if (json_ref_string_copy(&key_ref, key_decoded, sizeof(key_decoded)) != 0) return -1;
    }
    p = skip_ws(key_end);
    if (!p || p >= obj->end || *p != ':') return -1;
    ++p;
    p = skip_ws(p);
    if (parse_json_value_end(p, obj->end, &value_end) != 0) return -1;
    value_start = p;
    key_len = strlen(key);
    if (strlen(key_decoded) == key_len && strncmp(key_decoded, key, key_len) == 0) {
      out->json = obj->json;
      out->start = value_start;
      out->end = value_end;
      out->type = json_ref_infer_type(value_start);
      return 0;
    }
    p = skip_ws(value_end);
    if (!p || p >= obj->end) return -1;
    if (*p == ',') { ++p; p = skip_ws(p); continue; }
    if (*p == '}') return -1;
    return -1;
  }
}

int json_ref_object_iter(const JsonRef *obj, size_t *cursor,
                         char *key_out, size_t key_out_len,
                         JsonRef *value_out) {
  const char *p;
  size_t pos = 0;
  if (!obj || !cursor || !key_out || !key_out_len || !value_out ||
      !obj->start || !obj->end || obj->type != JSON_REF_OBJECT) return -1;
  p = obj->start + 1;
  p = skip_ws(p);
  if (p >= obj->end || *p == '}') return 0;
  while (1) {
    const char *key_end = NULL;
    const char *value_end = NULL;
    if (parse_json_string_end(p, obj->end, &key_end) != 0) return -1;
    if (pos == *cursor) {
      JsonRef key_ref = {obj->json, p, key_end, JSON_REF_STRING};
      if (json_ref_string_copy(&key_ref, key_out, key_out_len) != 0) return -1;
      p = skip_ws(key_end);
      if (!p || p >= obj->end || *p != ':') return -1;
      p = skip_ws(p + 1);
      if (parse_json_value_end(p, obj->end, &value_end) != 0) return -1;
      value_out->json = obj->json;
      value_out->start = p;
      value_out->end = value_end;
      value_out->type = json_ref_infer_type(p);
      *cursor = pos + 1;
      return 1;
    }
    p = skip_ws(key_end);
    if (!p || p >= obj->end || *p != ':') return -1;
    p = skip_ws(p + 1);
    if (parse_json_value_end(p, obj->end, &value_end) != 0) return -1;
    p = skip_ws(value_end);
    if (!p || p >= obj->end) return 0;
    if (*p == '}') return 0;
    if (*p == ',') p = skip_ws(p + 1);
    pos++;
  }
}

int json_ref_array_first(const JsonRef *arr, JsonRef *out) {
  if (!arr || !out || arr->type != JSON_REF_ARRAY || json_ref_array_size(arr) == 0U) return -1;
  return json_ref_array_get(arr, 0U, out);
}

int json_ref_object_get_object(const JsonRef *obj, const char *key, JsonRef *out) {
  if (!obj || !key || !out || json_ref_object_get(obj, key, out) != 0 || out->type != JSON_REF_OBJECT) return -1;
  return 0;
}

int json_ref_object_get_array(const JsonRef *obj, const char *key, JsonRef *out) {
  if (!obj || !key || !out || json_ref_object_get(obj, key, out) != 0 || out->type != JSON_REF_ARRAY) return -1;
  return 0;
}

int json_ref_object_get_string(const JsonRef *obj, const char *key, char *out, size_t out_len) {
  JsonRef value;
  if (!obj || !key || !out || out_len == 0U) return -1;
  if (json_ref_object_get(obj, key, &value) != 0) return -1;
  return json_ref_string_copy(&value, out, out_len);
}

int json_ref_object_get_long(const JsonRef *obj, const char *key, long long *out) {
  JsonRef value;
  if (!obj || !key || !out) return -1;
  if (json_ref_object_get(obj, key, &value) != 0) return -1;
  return json_ref_get_long(&value, out);
}

int json_ref_object_get_double(const JsonRef *obj, const char *key, double *out) {
  JsonRef value;
  if (!obj || !key || !out) return -1;
  if (json_ref_object_get(obj, key, &value) != 0) return -1;
  return json_ref_get_double(&value, out);
}

int json_ref_object_get_bool(const JsonRef *obj, const char *key, int *out) {
  JsonRef value;
  if (!obj || !key || !out) return -1;
  if (json_ref_object_get(obj, key, &value) != 0) return -1;
  return json_ref_get_bool(&value, out);
}

int json_ref_array_get(const JsonRef *arr, size_t index, JsonRef *out) {
  const char *p;
  size_t i;
  if (!arr || !out || !arr->start || !arr->end || arr->type != JSON_REF_ARRAY) return -1;
  p = arr->start + 1;
  p = skip_ws(p);
  if (!p || p >= arr->end) return -1;
  if (*p == ']') return -1;
  for (i = 0; i <= index; ++i) {
    const char *value_end = NULL;
    if (parse_json_value_end(p, arr->end, &value_end) != 0) return -1;
    if (i == index) {
      out->json = arr->json;
      out->start = p;
      out->end = value_end;
      out->type = json_ref_infer_type(p);
      return 0;
    }
    p = skip_ws(value_end);
    if (!p || p >= arr->end) return -1;
    if (*p != ',') return -1;
    ++p;
    p = skip_ws(p);
  }
  return -1;
}

size_t json_ref_array_size(const JsonRef *arr) {
  const char *p;
  size_t size = 0;
  if (!arr || arr->type != JSON_REF_ARRAY) return 0;
  p = arr->start + 1;
  p = skip_ws(p);
  if (!p || p >= arr->end) return 0;
  if (*p == ']') return 0;
  while (1) {
    const char *value_end = NULL;
    if (parse_json_value_end(p, arr->end, &value_end) != 0) return 0;
    ++size;
    p = skip_ws(value_end);
    if (!p || p >= arr->end) return 0;
    if (*p == ',') { ++p; p = skip_ws(p); continue; }
    if (*p == ']') return size;
    return 0;
  }
}

int json_ref_string_copy(const JsonRef *v, char *out, size_t out_len) {
  const char *p;
  size_t used = 0;
  if (!v || !out || out_len == 0 || v->type != JSON_REF_STRING ||
      !v->start || !v->end || *v->start != '"' || v->start >= v->end) return -1;
  p = v->start + 1;
  while (p < v->end) {
    char ch = *p;
    if (ch == '"') {
      if (used >= out_len) return -1;
      out[used] = '\0';
      return 0;
    }
    if (ch == '\\') {
      ++p;
      if (p >= v->end) return -1;
      char decoded = '\0';
      if (*p == '"' || *p == '\\' || *p == '/') decoded = *p;
      else if (*p == 'b') decoded = '\b';
      else if (*p == 'f') decoded = '\f';
      else if (*p == 'n') decoded = '\n';
      else if (*p == 'r') decoded = '\r';
      else if (*p == 't') decoded = '\t';
      else if (*p == 'u') {
        unsigned hi = 0;
        unsigned codepoint;
        ++p;
        if (parse_hex4(p, v->end, &hi) != 0) return -1;
        p += 4;
        if (hi >= 0xD800U && hi <= 0xDBFFU) {
          unsigned lo = 0;
          if (p + 2 >= v->end || p[0] != '\\' || p[1] != 'u') return -1;
          p += 2;
          if (parse_hex4(p, v->end, &lo) != 0 ||
              lo < 0xDC00U || lo > 0xDFFFU) return -1;
          p += 4;
          codepoint = 0x10000U + (((hi - 0xD800U) << 10U) | (lo - 0xDC00U));
        } else if (hi >= 0xDC00U && hi <= 0xDFFFU) {
          return -1;
        } else {
          codepoint = hi;
        }
        if (utf8_write(codepoint, out, out_len, &used) != 0) return -1;
        continue;
      } else {
        return -1;
      }
      if (used + 1U >= out_len) return -1;
      out[used++] = decoded;
      ++p;
      continue;
    }
    if (used + 1U >= out_len) return -1;
    if ((unsigned char)ch < 0x20U) return -1;
    out[used++] = ch;
    ++p;
  }
  return -1;
}

int json_ref_value_copy(const JsonRef *v, char *out, size_t out_len) {
  size_t span;
  if (!v || !out || !out_len || !v->start || !v->end || v->end < v->start) return -1;
  span = (size_t)(v->end - v->start);
  if (span + 1U > out_len) return -1;
  if (span > 0) memcpy(out, v->start, span);
  out[span] = '\0';
  return 0;
}

char *json_ref_string_dup(const JsonRef *v) {
  size_t cap;
  char *out;
  if (!v || !v->start || !v->end || v->end < v->start) return NULL;
  cap = (size_t)(v->end - v->start) + 1U;
  out = (char *)fc_xmalloc(cap);
  if (!out) return NULL;
  if (json_ref_string_copy(v, out, cap) != 0) {
    fc_xfree(out);
    return NULL;
  }
  return out;
}

char *json_ref_value_dup(const JsonRef *v) {
  size_t span;
  char *out;
  if (!v || !v->start || !v->end || v->end < v->start) return NULL;
  span = (size_t)(v->end - v->start);
  out = (char *)fc_xmalloc(span + 1U);
  if (!out) return NULL;
  if (json_ref_value_copy(v, out, span + 1U) != 0) {
    fc_xfree(out);
    return NULL;
  }
  return out;
}

char *json_ref_value_compact_dup(const JsonRef *v) {
  char *raw;
  char *compact;
  size_t cap;
  if (!v || !v->start || !v->end || v->end < v->start) return NULL;
  cap = (size_t)(v->end - v->start) + 1U;
  raw = json_ref_value_dup(v);
  if (!raw) return NULL;
  compact = (char *)fc_xmalloc(cap);
  if (!compact) {
    fc_xfree(raw);
    return NULL;
  }
  if (json_compact(raw, compact, cap) != 0) {
    fc_xfree(raw);
    fc_xfree(compact);
    return NULL;
  }
  fc_xfree(raw);
  return compact;
}

int json_ref_get_long(const JsonRef *v, long long *out) {
  const char *value_end = NULL;
  char *scan = NULL;
  if (!v || !out || v->type != JSON_REF_NUMBER || !v->start || !v->end) return -1;
  if (parse_json_number_end(v->start, v->end, &value_end) != 0 || value_end != v->end) return -1;
  errno = 0;
  *out = strtoll(v->start, &scan, 10);
  if (scan != v->end || errno != 0) return -1;
  return 0;
}

int json_ref_get_double(const JsonRef *v, double *out) {
  const char *value_end = NULL;
  char *scan = NULL;
  if (!v || !out || v->type != JSON_REF_NUMBER || !v->start || !v->end) return -1;
  if (parse_json_number_end(v->start, v->end, &value_end) != 0 || value_end != v->end) return -1;
  errno = 0;
  *out = strtod(v->start, &scan);
  if (scan != v->end || errno != 0) return -1;
  return 0;
}

int json_ref_get_bool(const JsonRef *v, int *out) {
  if (!v || !out || !v->start || !v->end || v->type != JSON_REF_BOOL) return -1;
  if ((size_t)(v->end - v->start) == 4U && strncmp(v->start, "true", 4) == 0) { *out = 1; return 0; }
  if ((size_t)(v->end - v->start) == 5U && strncmp(v->start, "false", 5) == 0) { *out = 0; return 0; }
  return -1;
}

int json_ref_is_null(const JsonRef *v) {
  return v != NULL && v->type == JSON_REF_NULL;
}

int json_escape(const char *src, char *out, size_t out_len) {
  size_t used = 0;
  if (!out || out_len == 0) return -1;
  for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; ++p) {
    const char *escaped = NULL;
    char buf[8];
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
    /* Every failure path terminates at `used`, which always sits on a
     * complete-escape boundary: truncation yields a shorter but still
     * valid escaped string, never a dangling backslash or stale bytes.
     * Callers that must not truncate check the return code. */
    if (escaped) {
      size_t add = strlen(escaped);
      if (used + add + 1U > out_len) { out[used] = '\0'; return -1; }
      memcpy(out + used, escaped, add);
      used += add;
    } else if (*p < 0x20U) {
      int n = snprintf(buf, sizeof(buf), "\\u%04x", *p);
      if (n < 0 || (size_t)n + 1U > out_len - used) { out[used] = '\0'; return -1; }
      memcpy(out + used, buf, (size_t)n);
      used += (size_t)n;
    } else {
      if (used + 2U > out_len) { out[used] = '\0'; return -1; }
      out[used++] = (char)*p;
    }
  }
  out[used] = '\0';
  return 0;
}

int json_compact(const char *src, char *out, size_t out_len) {
  size_t used = 0;
  int in_string = 0;
  if (!src || !out || out_len == 0) return -1;
  for (const char *p = src; *p; ++p) {
    const char ch = *p;
    if (in_string) {
      if (ch == '\\') {
        if (used + 2U >= out_len || *(p + 1) == '\0') return -1;
        out[used++] = *p;
        out[used++] = *(++p);
        continue;
      }
      if (ch == '"') in_string = 0;
      if (used + 1U >= out_len) return -1;
      out[used++] = ch;
      continue;
    }
    if (ch == '"') {
      in_string = 1;
      if (used + 1U >= out_len) return -1;
      out[used++] = ch;
      continue;
    }
    if (!isspace((unsigned char)ch)) {
      if (used + 1U >= out_len) return -1;
      out[used++] = ch;
    }
  }
  if (in_string) return -1;
  out[used] = '\0';
  return 0;
}
