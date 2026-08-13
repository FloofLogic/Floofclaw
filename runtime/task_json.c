#include "task_internal.h"

#include <stdlib.h>
#include <string.h>

int rt_task_json_status(const JsonRef *task, char *out, size_t out_len) {
  JsonRef state;
  if (!task || !out || out_len == 0) return -1;
  if (json_ref_object_get_object(task, "state", &state) == 0 &&
      json_ref_object_get_string(&state, "status", out, out_len) == 0)
    return 0;
  return json_ref_object_get_string(task, "state", out, out_len);
}

int rt_task_json_state_string(const JsonRef *task, const char *key,
                              char *out, size_t out_len) {
  JsonRef state;
  if (!task || !key || !out || out_len == 0) return -1;
  if (strcmp(key, "status") == 0)
    return rt_task_json_status(task, out, out_len);
  if (strcmp(key, "state") == 0) {
    if (json_ref_object_get_object(task, "state", &state) == 0) {
      if (json_ref_object_get_string(&state, "status", out, out_len) == 0 ||
          json_ref_object_get_string(&state, "state", out, out_len) == 0)
        return 0;
    }
    if (json_ref_object_get_string(task, "state", out, out_len) == 0)
      return 0;
    out[0] = '\0';
    return -1;
  }
  if (json_ref_object_get_object(task, "state", &state) == 0 &&
      json_ref_object_get_string(&state, key, out, out_len) == 0)
    return 0;
  if (json_ref_object_get_string(task, key, out, out_len) == 0) return 0;
  out[0] = '\0';
  return -1;
}

int rt_task_json_state_value(const JsonRef *task, const char *key,
                             JsonRef *out) {
  JsonRef state;
  if (!task || !key || !out) return -1;
  if (json_ref_object_get_object(task, "state", &state) == 0 &&
      json_ref_object_get(&state, key, out) == 0)
    return 0;
  return json_ref_object_get(task, key, out);
}

int rt_task_json_state_array(const JsonRef *task, const char *key,
                             JsonRef *out) {
  JsonRef state;
  if (!task || !key || !out) return -1;
  if (json_ref_object_get_object(task, "state", &state) == 0 &&
      json_ref_object_get_array(&state, key, out) == 0)
    return 0;
  return json_ref_object_get_array(task, key, out);
}

int rt_task_json_state_long(const JsonRef *task, const char *key,
                            long long *out) {
  JsonRef state;
  if (!task || !key || !out) return -1;
  if (json_ref_object_get_object(task, "state", &state) == 0 &&
      json_ref_object_get_long(&state, key, out) == 0)
    return 0;
  return json_ref_object_get_long(task, key, out);
}

int rt_task_json_append_pair(char *out, size_t out_len, size_t *pos,
                             const char *key, const JsonRef *value,
                             int first) {
  char escaped_key[RT_MED];
  char *raw = NULL;
  char *compact = NULL;
  int rc = -1;
  raw = (char *)malloc(RT_XL);
  compact = (char *)malloc(RT_XL);
  if (!raw || !compact) goto done;
  if (json_escape(key, escaped_key, sizeof(escaped_key)) != 0 ||
      json_ref_value_copy(value, raw, RT_XL) != 0 ||
      json_compact(raw, compact, RT_XL) != 0)
    goto done;
  if ((!first && rt_append_text(out, out_len, pos, ",") != 0) ||
      rt_append_text(out, out_len, pos, "\"") != 0 ||
      rt_append_text(out, out_len, pos, escaped_key) != 0 ||
      rt_append_text(out, out_len, pos, "\":") != 0 ||
      rt_append_text(out, out_len, pos, compact) != 0)
    goto done;
  rc = 0;
done:
  free(raw);
  free(compact);
  return rc;
}
