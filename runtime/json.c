#include "runtime.h"

int rt_json_escape(const char *src, char *out, size_t out_len) {
  return json_escape(src, out, out_len);
}

int rt_json_get_string(const char *json, const char *key, char *out, size_t out_len) {
  JsonRef root;
  if (json_ref_first_object(json, &root) != 0) return -1;
  return json_ref_object_get_string(&root, key, out, out_len);
}

int rt_json_get_int(const char *json, const char *key, int *out) {
  JsonRef root;
  long long v;
  if (!out) return -1;
  if (json_ref_first_object(json, &root) != 0) return -1;
  if (json_ref_object_get_long(&root, key, &v) != 0) return -1;
  *out = (int)v;
  return 0;
}

int rt_json_get_array_raw(const char *json, const char *key, char *out, size_t out_len) {
  JsonRef root, val;
  if (json_ref_first_object(json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, key, &val) != 0) return -1;
  return json_ref_value_copy(&val, out, out_len);
}
