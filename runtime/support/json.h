#ifndef FCLAW_SUPPORT_JSON_H
#define FCLAW_SUPPORT_JSON_H

#include <stddef.h>

typedef enum {
  JSON_REF_INVALID = 0,
  JSON_REF_OBJECT,
  JSON_REF_ARRAY,
  JSON_REF_STRING,
  JSON_REF_NUMBER,
  JSON_REF_BOOL,
  JSON_REF_NULL
} JsonRefType;

typedef struct {
  const char *json;
  const char *start;
  const char *end;
  JsonRefType type;
} JsonRef;

int json_escape(const char *src, char *out, size_t out_len);
int json_compact(const char *src, char *out, size_t out_len);

int json_ref_from_text(const char *json, JsonRef *out);
int json_ref_top_object(const char *text, JsonRef *out);
int json_ref_first_object(const char *text, JsonRef *out);
int json_ref_object_get(const JsonRef *obj, const char *key, JsonRef *out);
int json_ref_array_get(const JsonRef *arr, size_t index, JsonRef *out);
size_t json_ref_array_size(const JsonRef *arr);
int json_ref_array_first(const JsonRef *arr, JsonRef *out);
int json_ref_object_get_object(const JsonRef *obj, const char *key, JsonRef *out);
int json_ref_object_get_array(const JsonRef *obj, const char *key, JsonRef *out);
int json_ref_object_get_string(const JsonRef *obj, const char *key, char *out, size_t out_len);
int json_ref_object_get_long(const JsonRef *obj, const char *key, long long *out);
int json_ref_object_get_double(const JsonRef *obj, const char *key, double *out);
int json_ref_object_get_bool(const JsonRef *obj, const char *key, int *out);

int json_ref_object_iter(const JsonRef *obj, size_t *cursor,
                         char *key_out, size_t key_out_len,
                         JsonRef *value_out);

int json_ref_string_copy(const JsonRef *v, char *out, size_t out_len);
int json_ref_value_copy(const JsonRef *v, char *out, size_t out_len);
char *json_ref_string_dup(const JsonRef *v);
char *json_ref_value_dup(const JsonRef *v);
char *json_ref_value_compact_dup(const JsonRef *v);

int json_ref_get_long(const JsonRef *v, long long *out);
int json_ref_get_double(const JsonRef *v, double *out);
int json_ref_get_bool(const JsonRef *v, int *out);
int json_ref_is_null(const JsonRef *v);

#endif
