#include "runtime.h"
#include "floop.h"
#include "support/fsutil.h"
#include "support/json.h"

#include <stdlib.h>
#include <string.h>

/* Which floop an install runs by default is install policy, not a
 * compile-time constant. `default_floop` in config/floofclaw_config.json
 * overrides the built-in name; read once per process. */
const char *rt_default_loop(void) {
  static char cached[RT_SMALL];
  static int loaded;
  if (!loaded) {
    char *text = NULL;
    JsonRef root;
    loaded = 1;
    cached[0] = '\0';
    if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) == 0 && text) {
      if (json_ref_first_object(text, &root) != 0 ||
          json_ref_object_get_string(&root, "default_floop", cached,
                                     sizeof(cached)) != 0)
        cached[0] = '\0';
      free(text);
    }
  }
  return cached[0] ? cached : RT_DEFAULT_LOOP;
}

static int copy_step_target(const JsonRef *step, const char *type,
                            char *out, size_t out_len) {
  const char *key;
  if (strcmp(type, "agent") == 0) key = "agent";
  else if (strcmp(type, "builtin") == 0) key = "builtin";
  else if (strcmp(type, "action") == 0) key = "action";
  else return -1;
  return json_ref_object_get_string(step, key, out, out_len);
}

int rt_profile_load(const char *name_or_path, RtProfile *out, char *error, size_t error_len) {
  char path[PATH_MAX];
  char *text = NULL;
  JsonRef root, steps;
  long long version_v;
  size_t step_count;
  size_t i;
  if (!name_or_path || !out) return -1;
  memset(out, 0, sizeof(*out));
  if (strchr(name_or_path, '/')) {
    snprintf(path, sizeof(path), "%s", name_or_path);
  } else if (rt_floop_loop_path(name_or_path, path, sizeof(path)) != 0) {
    char available[RT_LARGE] = "";
    (void)rt_floop_list_available(available, sizeof(available));
    if (error && error_len)
      snprintf(error, error_len,
               "default_floop '%s' not found — available: %s; fix: pass "
               "--floop <name> or set default_floop in config/floofclaw_config.json",
               name_or_path, available[0] ? available : "(none)");
    return -1;
  }
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0) {
    if (error && error_len)
      snprintf(error, error_len, "floop file '%s' is unreadable; fix: restore it or pass --floop <name>", path);
    return -1;
  }
  if (json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_long(&root, "version", &version_v) != 0 ||
      json_ref_object_get_string(&root, "name", out->name, sizeof(out->name)) != 0 ||
      json_ref_object_get_array(&root, "steps", &steps) != 0) {
    free(text);
    if (error && error_len)
      snprintf(error, error_len, "floop file '%s' is malformed; fix: validate its version, name, and steps JSON", path);
    return -1;
  }
  out->version = (int)version_v;
  (void)json_ref_object_get_string(&root, "description", out->description, sizeof(out->description));
  {
    int b = 0;
    long long v = 0;
    if (json_ref_object_get_bool(&root, "one_pass", &b) == 0) out->one_pass = b ? 1 : 0;
    b = 0;
    if (json_ref_object_get_bool(&root, "serialize_contexts", &b) == 0)
      out->serialize_contexts = b ? 1 : 0;
    if (json_ref_object_get_long(&root, "retry_attempts", &v) == 0 && v > 0)
      out->retry_attempts = v > 8 ? 8 : (int)v;
    v = 0;
    if (json_ref_object_get_long(&root, "operation_timeout_ms", &v) == 0 && v > 0)
      out->operation_timeout_ms = (int)v;
  }
  step_count = json_ref_array_size(&steps);
  if (step_count == 0) {
    free(text);
    if (error && error_len) snprintf(error, error_len, "floop '%s' has no steps; fix: add at least one step to %s", out->name, path);
    return -1;
  }
  if (step_count > RT_MAX_STEPS) step_count = RT_MAX_STEPS;
  for (i = 0; i < step_count; ++i) {
    JsonRef step;
    RtStep *s = &out->steps[out->step_count];
    if (json_ref_array_get(&steps, i, &step) != 0 || step.type != JSON_REF_OBJECT) {
      free(text);
      if (error && error_len)
        snprintf(error, error_len,
                 "floop step %zu in %s is malformed; fix: make it a JSON object",
                 i, path);
      return -1;
    }
    if (json_ref_object_get_string(&step, "id", s->id, sizeof(s->id)) != 0 ||
        json_ref_object_get_string(&step, "type", s->type, sizeof(s->type)) != 0) {
      free(text);
      if (error && error_len)
        snprintf(error, error_len,
                 "floop step %zu in %s is missing id/type; fix: add string id and type fields",
                 i, path);
      return -1;
    }
    if (copy_step_target(&step, s->type, s->target, sizeof(s->target)) != 0) {
      free(text);
      if (error && error_len)
        snprintf(error, error_len,
                 "floop step '%s' in %s has an invalid type or target; fix: use agent, builtin, or action with its matching target field",
                 s->id, path);
      return -1;
    }
    if (json_ref_object_get_string(&step, "gate", s->gate, sizeof(s->gate)) != 0)
      snprintf(s->gate, sizeof(s->gate), "always");
    {
      int nc = 0;
      s->non_critical = json_ref_object_get_bool(&step, "non_critical", &nc) == 0 && nc ? 1 : 0;
    }
    out->step_count++;
  }
  free(text);
  return 0;
}
