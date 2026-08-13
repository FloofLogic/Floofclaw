#include "../../runtime/support/json.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FUZZ_INPUT_CAP (64U * 1024U)

static char g_input[FUZZ_INPUT_CAP + 1U];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  JsonRef root;
  if (!data || size > FUZZ_INPUT_CAP) return 0;
  memcpy(g_input, data, size);
  g_input[size] = '\0';

  if (json_ref_first_object(g_input, &root) == 0) {
    size_t cursor = 0;
    char key[256];
    JsonRef value;
    char text[2048];
    long long integer;
    double decimal;
    int boolean;

    (void)json_ref_object_get(&root, "type", &value);
    (void)json_ref_object_get_string(&root, "type", text, sizeof(text));
    (void)json_ref_object_get_long(&root, "value", &integer);
    (void)json_ref_object_get_double(&root, "value", &decimal);
    (void)json_ref_object_get_bool(&root, "value", &boolean);
    (void)json_ref_object_get_object(&root, "payload", &value);
    (void)json_ref_object_get_array(&root, "calls", &value);

    while (json_ref_object_iter(&root, &cursor, key, sizeof(key), &value) == 1) {
      (void)json_ref_value_copy(&value, text, sizeof(text));
      if (value.type == JSON_REF_STRING)
        (void)json_ref_string_copy(&value, text, sizeof(text));
      if (value.type == JSON_REF_ARRAY) {
        size_t count = json_ref_array_size(&value);
        size_t limit = count < 16U ? count : 16U;
        for (size_t i = 0; i < limit; ++i) {
          JsonRef item;
          if (json_ref_array_get(&value, i, &item) == 0)
            (void)json_ref_value_copy(&item, text, sizeof(text));
        }
      }
    }
  }
  return 0;
}
