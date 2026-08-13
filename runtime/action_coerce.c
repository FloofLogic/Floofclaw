#include "action_coerce.h"
#include "support/json.h"
#include "support/heap_guard.h"
#include "runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Is s a complete JSON number literal? Optionally reject a fractional
 * part (for integer schema). Empty / trailing-garbage strings fail. */
static int numeric_string(const char *s, int integer_only) {
  char *endp = NULL;
  double d;
  if (!s || !*s) return 0;
  errno = 0;
  d = strtod(s, &endp);
  (void)d;
  if (endp == s || *endp != '\0') return 0;
  if (integer_only && strchr(s, '.') != NULL) return 0;
  if (integer_only && (strchr(s, 'e') || strchr(s, 'E'))) return 0;
  return 1;
}

static int dyn_reserve(char **buf, size_t *cap, size_t need_len) {
  size_t new_cap;
  char *grown;
  if (!buf || !cap) return -1;
  if (*buf && *cap > need_len + 1U) return 0;
  new_cap = *cap ? *cap : 256U;
  while (new_cap <= need_len + 1U) {
    if (new_cap > (size_t)-1 / 2U) return -1;
    new_cap *= 2U;
  }
  grown = *buf ? (char *)fc_xrealloc(*buf, new_cap) : (char *)fc_xmalloc(new_cap);
  if (!grown) return -1;
  if (!*buf) grown[0] = '\0';
  *buf = grown;
  *cap = new_cap;
  return 0;
}

static int dyn_append(char **buf, size_t *cap, size_t *pos, const char *s) {
  size_t n = strlen(s ? s : "");
  if (!buf || !cap || !pos) return -1;
  if (dyn_reserve(buf, cap, *pos + n) != 0) return -1;
  memcpy(*buf + *pos, s ? s : "", n);
  *pos += n;
  (*buf)[*pos] = '\0';
  return 0;
}

/* Look up the declared primitive type of a top-level property. Writes
 * "string"/"integer"/"number"/"boolean"/"" (empty = not a primitive
 * we coerce, or property absent). */
static void schema_prop_type(const JsonRef *schema, const char *key,
                             char *type_out, size_t type_len) {
  JsonRef props, prop;
  type_out[0] = '\0';
  if (json_ref_object_get_object(schema, "properties", &props) != 0) return;
  if (json_ref_object_get_object(&props, key, &prop) != 0) return;
  (void)json_ref_object_get_string(&prop, "type", type_out, type_len);
}

int rt_action_coerce_args(const char *args_schema,
                         char *args_inout, size_t cap) {
  JsonRef schema, args, val;
  char *rebuilt = NULL;
  size_t rebuilt_cap = 0, pos = 0, cursor = 0;
  int changed = 0, first = 1;
  char key[RT_SMALL];
  int rc = 0;
  if (!args_schema || !args_inout) return -1;
  if (json_ref_from_text(args_schema, &schema) != 0 ||
      schema.type != JSON_REF_OBJECT)
    return 0; /* no usable schema -> leave args as-is */
  if (json_ref_from_text(args_inout, &args) != 0 ||
      args.type != JSON_REF_OBJECT)
    return 0; /* not an object -> validation will reject + report */

  if (dyn_append(&rebuilt, &rebuilt_cap, &pos, "{") != 0) return 0;
  while (json_ref_object_iter(&args, &cursor, key, sizeof(key), &val) == 1) {
    char ekey[RT_MED];
    char type[RT_SMALL];
    char *rawval = NULL;
    char *emitted = NULL;
    int did = 0;
    if (json_escape(key, ekey, sizeof(ekey)) != 0) goto fail;
    schema_prop_type(&schema, key, type, sizeof(type));

    /* Decide the coerced token for this value, if any. Default:
     * copy the value through verbatim. */
    if (type[0] && val.type == JSON_REF_STRING &&
        (strcmp(type, "integer") == 0 || strcmp(type, "number") == 0)) {
      char *sv = json_ref_string_dup(&val);
      if (sv && numeric_string(sv, strcmp(type, "integer") == 0)) {
        emitted = fc_xstrdup(sv); /* bare number */
        did = 1;
      }
      fc_xfree(sv);
    } else if (type[0] && strcmp(type, "string") == 0 &&
               (val.type == JSON_REF_NUMBER || val.type == JSON_REF_BOOL)) {
      char sv[RT_LARGE], *esv = NULL;
      size_t need;
      if (val.type == JSON_REF_BOOL) {
        int b = 0;
        (void)json_ref_get_bool(&val, &b);
        snprintf(sv, sizeof(sv), "%s", b ? "true" : "false");
      } else if ((rawval = json_ref_value_dup(&val)) != NULL) {
        snprintf(sv, sizeof(sv), "%s", rawval);
      } else {
        sv[0] = '\0';
      }
      need = strlen(sv) * 6U + 1U;
      esv = (char *)fc_xmalloc(need);
      if (esv && json_escape(sv, esv, need) == 0) {
        size_t emitted_len = strlen(esv) + 3U;
        emitted = (char *)fc_xmalloc(emitted_len);
        if (!emitted) {
          fc_xfree(esv);
          fc_xfree(rawval);
          goto fail;
        }
        snprintf(emitted, emitted_len, "\"%s\"", esv);
        did = 1;
      }
      fc_xfree(esv);
      fc_xfree(rawval);
      rawval = NULL;
    } else if (type[0] && strcmp(type, "boolean") == 0 &&
               val.type == JSON_REF_STRING) {
      char sv[RT_SMALL];
      if (json_ref_string_copy(&val, sv, sizeof(sv)) == 0 &&
          (strcmp(sv, "true") == 0 || strcmp(sv, "false") == 0)) {
        emitted = fc_xstrdup(sv);
        did = 1;
      }
    } else if (type[0] && strcmp(type, "boolean") == 0 &&
               val.type == JSON_REF_NUMBER) {
      long long n = 0;
      if (json_ref_get_long(&val, &n) == 0 && (n == 0 || n == 1)) {
        emitted = fc_xstrdup(n ? "true" : "false");
        did = 1;
      }
    }

    if (!did) {
      rawval = json_ref_value_dup(&val);
      if (!rawval) goto fail;
      emitted = rawval;
      rawval = NULL;
    } else {
      changed = 1;
    }

    if ((!first && dyn_append(&rebuilt, &rebuilt_cap, &pos, ",") != 0) ||
        dyn_append(&rebuilt, &rebuilt_cap, &pos, "\"") != 0 ||
        dyn_append(&rebuilt, &rebuilt_cap, &pos, ekey) != 0 ||
        dyn_append(&rebuilt, &rebuilt_cap, &pos, "\":") != 0 ||
        dyn_append(&rebuilt, &rebuilt_cap, &pos, emitted ? emitted : "") != 0) {
      fc_xfree(emitted);
      goto fail;
    }
    fc_xfree(emitted);
    first = 0;
  }
  if (dyn_append(&rebuilt, &rebuilt_cap, &pos, "}") != 0) goto fail;
  if (!changed) goto done;
  if (strlen(rebuilt) + 1 > cap) goto done; /* leave raw if caller cap is smaller */
  snprintf(args_inout, cap, "%s", rebuilt);
  rc = 1;
done:
  fc_xfree(rebuilt);
  return rc;
fail:
  fc_xfree(rebuilt);
  return 0;
}
