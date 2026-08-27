#include "runtime.h"
#include "support/heap_guard.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  RtActionRegistry *reg;
  char err[RT_LARGE];
  int failed;
  RtActionVisitor visitor;
  void *visitor_user;
} ActionScan;

static int is_safe_id(const char *s) {
  if (!s || !*s) return 0;
  for (; *s; ++s)
    if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-')) return 0;
  return 1;
}

/* Declared config names become environment variables verbatim, so they
 * are restricted to the conventional shell-safe uppercase form. */
static int is_safe_env_name(const char *s) {
  if (!s || !*s || isdigit((unsigned char)*s)) return 0;
  for (; *s; ++s)
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'))
      return 0;
  return 1;
}

static int is_safe_config_key(const char *s) {
  if (!s || !*s || isdigit((unsigned char)*s)) return 0;
  for (; *s; ++s)
    if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
  return 1;
}

/* A manifest is shipped code, shared across deployments and readable by
 * anyone with the repo. Refuse to let a credential be declared as config
 * there -- the secret namespace exists for that. */
static int name_looks_like_secret(const char *s) {
  static const char *const marks[] = {
    "SECRET", "TOKEN", "PASSWORD", "PASSWD", "CREDENTIAL",
    "PRIVATE_KEY", "APIKEY", "API_KEY", "CLIENT_SECRET", "REFRESH"
  };
  size_t i;
  if (!s) return 0;
  for (i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i)
    if (strstr(s, marks[i])) return 1;
  return 0;
}

/* Resolve an action's declared config from the DEPLOYMENT's config file
 * (config/floofclaw_config.json -> actions.<id>.<NAME>), writing
 * "NAME=VALUE" strings into out[]. Returns the count, or -1 when a
 * required name has no value -- the manifest declares, the deployment
 * supplies, and a missing required setting is a loud failure rather than
 * an action that starts and misbehaves. */
static int action_config_value(const RtActionDef *def, int index,
                               char *value, size_t value_len) {
  char *text = NULL;
  JsonRef root, actions, mine, configured;
  const char *key;
  int found = 0;
  if (!def || index < 0 || index >= def->config_count ||
      !value || value_len == 0)
    return -1;
  value[0] = '\0';
  key = def->config_keys[index][0]
            ? def->config_keys[index]
            : def->config_names[index];
  if (fs_read_text("config/floofclaw_config.json", &text,
                   FS_READ_TEXT_DEFAULT_CAP) == 0 && text &&
      json_ref_first_object(text, &root) == 0 &&
      json_ref_object_get_object(&root, "actions", &actions) == 0 &&
      json_ref_object_get_object(&actions, def->id, &mine) == 0 &&
      json_ref_object_get(&mine, key, &configured) == 0) {
    if (configured.type != JSON_REF_STRING ||
        json_ref_object_get_string(&mine, key, value, value_len) != 0 ||
        !value[0])
      found = -1;
    else
      found = 1;
  }
  free(text);
  return found;
}

int rt_action_resolve_config(const RtActionDef *def, void *out_buf,
                             size_t entry_len) {
  char (*out)[RT_MED] = (char (*)[RT_MED])out_buf;
  int n = 0, i;
  if (!def || !out || entry_len == 0) return -1;
  if (def->config_count == 0) return 0;
  for (i = 0; i < def->config_count; ++i) {
    char value[RT_MED] = "";
    int found = action_config_value(def, i, value, sizeof(value));
    if (found < 0) return -1;
    if (found == 0) {
      if (def->config_required[i]) return -1;
      continue;
    }
    if (snprintf(out[n], entry_len, "%s=%s", def->config_names[i], value) >=
        (int)entry_len) {
      return -1;
    }
    n++;
  }
  return n;
}

const char *rt_action_cached_config(const RtActionRegistry *reg,
                                    const char *action_id,
                                    const char *name) {
  if (!reg || !action_id || !name) return NULL;
  for (size_t i = 0; i < reg->cached_config_count; ++i) {
    const RtActionCachedConfig *cached = &reg->cached_configs[i];
    if (strcmp(cached->action_id, action_id) == 0 &&
        strcmp(cached->key, name) == 0)
      return cached->value;
  }
  return NULL;
}

static int is_safe_rel_path(const char *s) {
  if (!s || !*s || s[0] == '/' || strstr(s, "..")) return 0;
  for (; *s; ++s)
    if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-' || *s == '/' || *s == '.')) return 0;
  return 1;
}

static int is_dir(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

const RtActionDef *rt_action_registry_find(const RtActionRegistry *reg, const char *action_id) {
  if (!reg || !action_id) return NULL;
  for (size_t i = 0; i < reg->count; ++i)
    if (strcmp(reg->defs[i].id, action_id) == 0) return &reg->defs[i];
  return NULL;
}

int rt_agent_allows_action(const RtAgentMeta *meta, const RtActionDef *def) {
  uint64_t bit;
  if (!meta || !def || def->runtime_id >= RT_MAX_ACTIONS) return 0;
  if (!meta->has_action_allowlist) return 1;
  bit = UINT64_C(1) << def->runtime_id;
  return (meta->action_allow_mask & bit) != 0;
}

static int copy_ref_compact(const JsonRef *v, char *out, size_t out_len) {
  char raw[RT_ACTION_SCHEMA_MAX];
  if (json_ref_value_copy(v, raw, sizeof(raw)) != 0) return -1;
  return json_compact(raw, out, out_len);
}

static int parse_exec(const JsonRef *root, RtActionDef *def, char *err, size_t err_len) {
  JsonRef arr, e0, e1;
  if (json_ref_object_get_array(root, "exec", &arr) != 0 || json_ref_array_size(&arr) < 2 ||
      json_ref_array_get(&arr, 0, &e0) != 0 || json_ref_array_get(&arr, 1, &e1) != 0 ||
      json_ref_string_copy(&e0, def->exec_argv0, sizeof(def->exec_argv0)) != 0 ||
      json_ref_string_copy(&e1, def->exec_arg, sizeof(def->exec_arg)) != 0) {
    snprintf(err, err_len, "%s: exec must be an array with command and argument", def->manifest_path);
    return -1;
  }
  if (!is_safe_id(def->exec_argv0) || !def->exec_arg[0]) {
    snprintf(err, err_len, "%s: unsafe exec", def->manifest_path);
    return -1;
  }
  if (strcmp(def->exec_argv0, "runtime") == 0) {
    def->exec_kind = RT_ACTION_EXEC_RUNTIME;
    if (!rt_action_registry_runtime_function_known(def->exec_arg)) {
      snprintf(err, err_len, "%s: unknown runtime action function %s",
               def->manifest_path, def->exec_arg);
      return -1;
    }
    return 0;
  }
  def->exec_kind = RT_ACTION_EXEC_SUBPROCESS;
  if (!is_safe_rel_path(def->exec_arg)) {
    snprintf(err, err_len, "%s: unsafe subprocess exec arg", def->manifest_path);
    return -1;
  }
  return 0;
}

static int parse_operator_exec(const JsonRef *root, RtActionDef *def,
                               char *err, size_t err_len) {
  JsonRef arr, e0, e1;
  if (json_ref_object_get_array(root, "operator_exec", &arr) != 0)
    return 0;
  if (json_ref_array_size(&arr) < 2 ||
      json_ref_array_get(&arr, 0, &e0) != 0 ||
      json_ref_array_get(&arr, 1, &e1) != 0 ||
      json_ref_string_copy(&e0, def->operator_argv0,
                           sizeof(def->operator_argv0)) != 0 ||
      json_ref_string_copy(&e1, def->operator_arg,
                           sizeof(def->operator_arg)) != 0 ||
      !is_safe_id(def->operator_argv0) ||
      !is_safe_rel_path(def->operator_arg)) {
    snprintf(err, err_len,
             "%s: operator_exec must contain a safe command and relative executable",
             def->manifest_path);
    return -1;
  }
  if (strcmp(def->operator_argv0, "runtime") == 0) {
    snprintf(err, err_len,
             "%s: operator_exec must be action-owned subprocess code",
             def->manifest_path);
    return -1;
  }
  def->has_operator_exec = 1;
  return 0;
}

static int load_action_contract(const char *area_id, const char *dir,
                               const char *manifest, RtActionDef *def,
                               char *err, size_t err_len) {
  char *text = NULL;
  JsonRef root, schema;
  long long timeout = 0;
  int outside = 0;
  memset(def, 0, sizeof(*def));
  snprintf(def->area_id, sizeof(def->area_id), "%s", area_id);
  snprintf(def->dir, sizeof(def->dir), "%s", dir);
  snprintf(def->manifest_path, sizeof(def->manifest_path), "%s", manifest);
  if (fs_read_text(manifest, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
    snprintf(err, err_len, "could not read %s", manifest);
    return -1;
  }
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_string(&root, "id", def->id, sizeof(def->id)) != 0 ||
      !is_safe_id(def->id) ||
      json_ref_object_get_string(&root, "description", def->description, sizeof(def->description)) != 0 ||
      !def->description[0] ||
      json_ref_object_get_bool(&root, "outside_world", &outside) != 0 ||
      json_ref_object_get_object(&root, "args_schema", &schema) != 0 ||
      copy_ref_compact(&schema, def->args_schema, sizeof(def->args_schema)) != 0 ||
      json_ref_object_get_long(&root, "timeout_ms", &timeout) != 0 ||
      timeout < 0 || timeout > 3600000) {
    snprintf(err, err_len, "%s: invalid action contract", manifest);
    fc_xfree(text);
    return -1;
  }
  def->outside_world = outside;
  def->timeout_ms = (int)timeout;
  (void)json_ref_object_get_bool(&root, "private_credentials",
                                 &def->private_credentials);
  {
    /* "config": [ {"name":"IMESSAGE_SSH_HOST","required":true}, ... ]
     * Names only -- values come from the deployment's config file. An
     * entry that looks like a credential is refused here so secrets can
     * never drift into a shipped, world-readable manifest. */
    JsonRef cfg;
    if (json_ref_object_get_array(&root, "config", &cfg) == 0) {
      size_t n = json_ref_array_size(&cfg);
      if (n > RT_ACTION_CONFIG_MAX) {
        snprintf(err, err_len, "%s: config declares %zu entries, max %d",
                 manifest, n, RT_ACTION_CONFIG_MAX);
        fc_xfree(text);
        return -1;
      }
      for (size_t i = 0; i < n; ++i) {
        JsonRef entry;
        char name[RT_SMALL] = "";
        char key[RT_SMALL] = "";
        char kind[RT_SMALL] = "";
        int required = 0;
        if (json_ref_array_get(&cfg, i, &entry) != 0 ||
            entry.type != JSON_REF_OBJECT ||
            json_ref_object_get_string(&entry, "name", name, sizeof(name)) != 0 ||
            !is_safe_env_name(name)) {
          snprintf(err, err_len,
                   "%s: config entry %zu needs a name of [A-Z0-9_]", manifest, i);
          fc_xfree(text);
          return -1;
        }
        if (name_looks_like_secret(name)) {
          snprintf(err, err_len,
                   "%s: config name %s looks like a credential; store it as "
                   "action:%s:<name> instead", manifest, name, def->id);
          fc_xfree(text);
          return -1;
        }
        (void)json_ref_object_get_bool(&entry, "required", &required);
        if (json_ref_object_get_string(&entry, "key", key,
                                       sizeof(key)) != 0)
          snprintf(key, sizeof(key), "%s", name);
        if (!is_safe_config_key(key)) {
          snprintf(err, err_len,
                   "%s: config entry %zu has an unsafe deployment key",
                   manifest, i);
          fc_xfree(text);
          return -1;
        }
        if (json_ref_object_get_string(&entry, "kind", kind,
                                       sizeof(kind)) == 0) {
          if (strcmp(kind, "existing_directory") == 0)
            def->config_kinds[def->config_count] =
                RT_ACTION_CONFIG_EXISTING_DIRECTORY;
          else if (strcmp(kind, "string") == 0)
            def->config_kinds[def->config_count] = RT_ACTION_CONFIG_STRING;
          else if (strcmp(kind, "permission_mode_ceiling") == 0)
            def->config_kinds[def->config_count] =
                RT_ACTION_CONFIG_PERMISSION_MODE_CEILING;
          else {
            snprintf(err, err_len, "%s: config entry %zu has unknown kind %s",
                     manifest, i, kind);
            fc_xfree(text);
            return -1;
          }
        }
        snprintf(def->config_names[def->config_count],
                 sizeof(def->config_names[0]), "%s", name);
        snprintf(def->config_keys[def->config_count],
                 sizeof(def->config_keys[0]), "%s", key);
        if (!kind[0] && strcmp(key, name) != 0) {
          snprintf(err, err_len,
                   "%s: config key overrides require a typed config entry",
                   manifest);
          fc_xfree(text);
          return -1;
        }
        def->config_required[def->config_count] = required;
        def->config_count++;
      }
    }
  }
  {
    char contract[RT_SMALL] = "";
    if (json_ref_object_get_string(&root, "contract", contract, sizeof(contract)) == 0 &&
        contract[0]) {
      if (strcmp(contract, "managed_operation") != 0 &&
          strcmp(contract, "work_terminal") != 0) {
        snprintf(err, err_len, "%s: unknown contract %s", manifest, contract);
        fc_xfree(text);
        return -1;
      }
      if (strcmp(contract, "managed_operation") == 0)
        def->managed_operation = 1;
      else
        def->work_terminal = 1;
    }
  }
  {
    JsonRef poll_arg;
    long long default_timeout = 0, max_timeout = 0;
    if (json_ref_object_get(&root, "poll_handle_arg", &poll_arg) == 0) {
      snprintf(err, err_len,
               "%s: poll_handle_arg is no longer supported; managed "
               "operations complete through the bus completion contract "
               "(declare default_timeout_ms instead)",
               manifest);
      fc_xfree(text);
      return -1;
    }
    (void)json_ref_object_get_long(&root, "default_timeout_ms",
                                   &default_timeout);
    (void)json_ref_object_get_long(&root, "max_timeout_ms", &max_timeout);
    if (def->managed_operation && default_timeout <= 0) {
      snprintf(err, err_len,
               "%s: contract managed_operation requires default_timeout_ms "
               "(the action-owned completion deadline)",
               manifest);
      fc_xfree(text);
      return -1;
    }
    if (!def->managed_operation && (default_timeout > 0 || max_timeout > 0)) {
      snprintf(err, err_len,
               "%s: default_timeout_ms/max_timeout_ms require contract "
               "managed_operation",
               manifest);
      fc_xfree(text);
      return -1;
    }
    if (max_timeout > 0 && max_timeout < default_timeout) {
      snprintf(err, err_len,
               "%s: max_timeout_ms must be >= default_timeout_ms",
               manifest);
      fc_xfree(text);
      return -1;
    }
    def->default_timeout_ms = (int)default_timeout;
    def->max_timeout_ms =
        (int)(max_timeout > 0 ? max_timeout : default_timeout);
  }
  if (parse_exec(&root, def, err, err_len) != 0) {
    fc_xfree(text);
    return -1;
  }
  if (parse_operator_exec(&root, def, err, err_len) != 0) {
    fc_xfree(text);
    return -1;
  }
  if (def->has_operator_exec && !def->private_credentials) {
    snprintf(err, err_len,
             "%s: operator_exec requires private_credentials:true",
             manifest);
    fc_xfree(text);
    return -1;
  }
  if (def->work_terminal && def->exec_kind != RT_ACTION_EXEC_RUNTIME) {
    snprintf(err, err_len,
             "%s: work_terminal contract requires a registered runtime intrinsic",
             manifest);
    fc_xfree(text);
    return -1;
  }
  fc_xfree(text);
  return 0;
}

static int append_action(ActionScan *scan, const RtActionDef *def) {
  RtActionDef cached;
  const RtActionDef *prior = rt_action_registry_find(scan->reg, def->id);
  if (prior) {
    /* action.json's id is the action's identity; the directory is only
     * where it was found. Name both manifests so the collision is fixable
     * without a second scan. */
    snprintf(scan->err, sizeof(scan->err),
             "duplicate action id \"%s\": %s and %s; "
             "fix: change one action's id, or rename one directory with a "
             "leading underscore to unload it",
             def->id, prior->manifest_path, def->manifest_path);
    return -1;
  }
  if (scan->reg->count >= RT_MAX_ACTIONS) {
    snprintf(scan->err, sizeof(scan->err), "action registry full: cap=%d", RT_MAX_ACTIONS);
    return -1;
  }
  cached = *def;
  cached.runtime_id = (unsigned char)scan->reg->count;
  scan->reg->defs[scan->reg->count++] = cached;
  return 0;
}

static int cache_typed_config(RtActionRegistry *reg, RtActionDef *def,
                              char *err, size_t err_len) {
  if (!def) return 0;
  for (int i = 0; i < def->config_count; ++i) {
    char configured[PATH_MAX] = "";
    char canonical[PATH_MAX] = "";
    struct stat st;
    int found;
    RtActionCachedConfig *entry;
    RtActionConfigKind kind = (RtActionConfigKind)def->config_kinds[i];
    const char *key = def->config_keys[i][0]
                          ? def->config_keys[i]
                          : def->config_names[i];
    if (kind == RT_ACTION_CONFIG_PLAIN) continue;
    found = action_config_value(def, i, configured, sizeof(configured));
    if (found < 0) {
      if (kind == RT_ACTION_CONFIG_EXISTING_DIRECTORY)
        snprintf(err, err_len,
                 "%s: actions.%s.%s must name an existing directory; fix the deployment config",
                 def->manifest_path, def->id, key);
      else if (kind == RT_ACTION_CONFIG_PERMISSION_MODE_CEILING)
        snprintf(err, err_len,
                 "%s: actions.%s.%s must be safe or dangerous",
                 def->manifest_path, def->id, key);
      else
        snprintf(err, err_len,
                 "%s: actions.%s.%s must be a non-empty string",
                 def->manifest_path, def->id, key);
      return -1;
    }
    if (found == 0) {
      if (def->config_required[i]) {
        if (kind == RT_ACTION_CONFIG_EXISTING_DIRECTORY)
          snprintf(err, err_len,
                   "%s: actions.%s.%s is required and must name an existing directory",
                   def->manifest_path, def->id, key);
        else if (kind == RT_ACTION_CONFIG_PERMISSION_MODE_CEILING)
          snprintf(err, err_len,
                   "%s: actions.%s.%s is required and must be safe or dangerous",
                   def->manifest_path, def->id, key);
        else
          snprintf(err, err_len,
                   "%s: actions.%s.%s is required and must be a non-empty string",
                   def->manifest_path, def->id, key);
        return -1;
      }
      continue;
    }
    if (kind == RT_ACTION_CONFIG_EXISTING_DIRECTORY) {
      if (!realpath(configured, canonical) ||
          stat(canonical, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, err_len,
                 "%s: actions.%s.%s must name an existing directory; fix the deployment config",
                 def->manifest_path, def->id, key);
        return -1;
      }
    } else if (kind == RT_ACTION_CONFIG_PERMISSION_MODE_CEILING) {
      if (strcmp(configured, "safe") != 0 &&
          strcmp(configured, "dangerous") != 0) {
        snprintf(err, err_len,
                 "%s: actions.%s.%s must be safe or dangerous",
                 def->manifest_path, def->id, key);
        return -1;
      }
      snprintf(canonical, sizeof(canonical), "%s", configured);
    } else {
      snprintf(canonical, sizeof(canonical), "%s", configured);
    }
    if (!reg || reg->cached_config_count >= RT_ACTION_TYPED_CONFIG_MAX) {
      snprintf(err, err_len,
               "%s: typed action config cache is full (cap=%d)",
               def->manifest_path, RT_ACTION_TYPED_CONFIG_MAX);
      return -1;
    }
    entry = &reg->cached_configs[reg->cached_config_count++];
    snprintf(entry->action_id, sizeof(entry->action_id), "%s", def->id);
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    snprintf(entry->value, sizeof(entry->value), "%s", canonical);
  }
  return 0;
}

static int visit_action_dir(const char *area_id, const char *dir, ActionScan *scan) {
  char manifest[PATH_MAX];
  RtActionDef def;
  snprintf(manifest, sizeof(manifest), "%s/action.json", dir);
  if (!fs_file_exists(manifest)) return 0;
  if (load_action_contract(area_id, dir, manifest, &def, scan->err, sizeof(scan->err)) != 0)
    return -1;
  if (scan->visitor) {
    char script[PATH_MAX];
    if (def.exec_kind == RT_ACTION_EXEC_SUBPROCESS)
      snprintf(script, sizeof(script), "%s/%s", def.dir, def.exec_arg);
    else
      snprintf(script, sizeof(script), "runtime:%s", def.exec_arg);
    if (scan->visitor(def.id, def.area_id, def.dir, def.manifest_path, script,
                      scan->visitor_user) != 0) return -1;
  }
  return scan->reg ? append_action(scan, &def) : 0;
}

/* Scanner memory invariant: the scanner holds at most one sorted child list
 * per recursion depth, each sized to that directory's actual loadable child
 * count and freed before its depth returns. Names live inline in the entry, so
 * a list is a single allocation with no per-name strings to own or leak. Peak
 * heap for a scan is therefore the sum of the child counts along one
 * root-to-leaf path -- not the size of the tree -- bounded above by
 * RT_SCAN_CHILD_MAX * RT_SCAN_NAME_MAX per level and 7 levels of depth.
 *
 * Heap rather than stack because this recurses and the suite runs on a 1 MB
 * stack. RT_SCAN_CHILD_MAX is a runaway guard, not a budget: RT_MAX_ACTIONS is
 * 64, so a directory holding more than this many loadable children cannot
 * produce a loadable registry anyway. */
#define RT_SCAN_NAME_MAX  64
#define RT_SCAN_CHILD_MAX 256

typedef struct { char name[RT_SCAN_NAME_MAX]; } ScanName;


static int scan_name_cmp(const void *a, const void *b) {
  return strcmp(((const ScanName *)a)->name, ((const ScanName *)b)->name);
}

/* Names starting with '.' or '_' are skipped: '_' is how a directory stays in
 * the tree without being loaded (mv actions/github actions/_github), and it is
 * what keeps actions/common/web_read/_pluck out of the scan. */
static int scan_child_is_loadable(const char *name) {
  return name[0] != '.' && name[0] != '_' && is_safe_id(name);
}

/* Deterministic child enumeration. readdir order is filesystem-dependent, and
 * a duplicate-id collision must report the same two manifests on every
 * machine.
 *
 * Counts first and allocates exactly, so a directory holding two entries costs
 * two entries. The count and fill passes can disagree if the tree changes
 * underneath a live scan; filling stops at the counted size and the caller
 * uses the returned count, so neither direction overruns. */
static int read_sorted_children(const char *dir, ScanName **out, size_t *count,
                                ActionScan *scan) {
  DIR *d;
  struct dirent *e;
  ScanName *names;
  size_t want = 0, n = 0;
  *out = NULL;
  *count = 0;
  d = opendir(dir);
  if (!d) return 0;
  while ((e = readdir(d)) != NULL && want < RT_SCAN_CHILD_MAX) {
    if (!scan_child_is_loadable(e->d_name)) continue;
    /* Truncating here would rebuild a path that does not exist, and the
     * directory would vanish from the scan with no explanation. */
    if (strlen(e->d_name) >= RT_SCAN_NAME_MAX) {
      snprintf(scan->err, sizeof(scan->err),
               "%s/%s: directory name is longer than %d characters; "
               "fix: shorten it, or prefix it with _ to keep it unloaded",
               dir, e->d_name, RT_SCAN_NAME_MAX - 1);
      closedir(d);
      return -1;
    }
    want++;
  }
  if (want == 0) {
    closedir(d);
    return 0;
  }
  names = (ScanName *)fc_xmalloc(sizeof(*names) * want);
  if (!names) {
    snprintf(scan->err, sizeof(scan->err),
             "%s: could not allocate the directory listing", dir);
    closedir(d);
    return -1;
  }
  rewinddir(d);
  while ((e = readdir(d)) != NULL && n < want) {
    if (!scan_child_is_loadable(e->d_name)) continue;
    if (strlen(e->d_name) >= RT_SCAN_NAME_MAX) continue;
    snprintf(names[n].name, sizeof(names[n].name), "%s", e->d_name);
    n++;
  }
  closedir(d);
  qsort(names, n, sizeof(*names), scan_name_cmp);
  *out = names;
  *count = n;
  return 0;
}

static int scan_tree(const char *area_id, const char *dir, int depth, ActionScan *scan) {
  ScanName *children = NULL;
  size_t n = 0, i;
  int rc = 0;
  if (!dir || !scan || depth > 6) return 0;
  if (visit_action_dir(area_id, dir, scan) != 0) return -1;
  if (read_sorted_children(dir, &children, &n, scan) != 0) return -1;
  for (i = 0; i < n && rc == 0; ++i) {
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", dir, children[i].name);
    if (is_dir(child)) rc = scan_tree(area_id, child, depth + 1, scan);
  }
  fc_xfree(children);
  return rc;
}

/* Discovery is the directory tree. Every directory under actions/ holding an
 * action.json is an action, at any depth; there is no registration list. The
 * first path segment is kept as a display grouping only -- action.json's id
 * remains the action's sole identity. */
static int scan_actions_tree(ActionScan *scan) {
  ScanName *children = NULL;
  size_t n = 0, i;
  int rc = 0;
  if (!scan) return -1;
  if (read_sorted_children("actions", &children, &n, scan) != 0) return -1;
  for (i = 0; i < n && rc == 0; ++i) {
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "actions/%s", children[i].name);
    if (is_dir(root)) rc = scan_tree(children[i].name, root, 0, scan);
  }
  fc_xfree(children);
  return rc;
}

/* The deployment's `force_disable` list masks registered actions without
 * touching floop files: allowlists keep declaring intent, the mask says
 * what this deployment currently runs. Entries are action ids (never
 * directory groupings, which are display-only). An id the registry does
 * not hold is inert — an absent action needs no disabling — so one config
 * stays portable across deployment branches. */
static void apply_force_disable(RtActionRegistry *reg) {
  char *text = NULL;
  JsonRef root, list, entry;
  if (fs_read_text("config/floofclaw_config.json", &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return;
  if (json_ref_first_object(text, &root) == 0 &&
      json_ref_object_get_array(&root, "force_disable", &list) == 0) {
    for (size_t i = 0; i < json_ref_array_size(&list); ++i) {
      char id[RT_SMALL];
      if (json_ref_array_get(&list, i, &entry) != 0 ||
          json_ref_string_copy(&entry, id, sizeof(id)) != 0)
        continue;
      for (size_t j = 0; j < reg->count; ++j) {
        if (strcmp(reg->defs[j].id, id) == 0) {
          reg->defs[j].force_disabled = 1;
          break;
        }
      }
    }
  }
  free(text);
}

int rt_action_registry_load(RtActionRegistry *reg, char *err, size_t err_len) {
  ActionScan scan;
  if (!reg) return -1;
  memset(reg, 0, sizeof(*reg));
  memset(&scan, 0, sizeof(scan));
  scan.reg = reg;
  if (scan_actions_tree(&scan) != 0 || scan.err[0]) {
    if (err) snprintf(err, err_len, "%s", scan.err[0] ? scan.err : "action registry load failed");
    return -1;
  }
  if (reg->count == 0) {
    if (err) snprintf(err, err_len, "action registry loaded zero enabled actions");
    return -1;
  }
  apply_force_disable(reg);
  for (size_t i = 0; i < reg->count; ++i) {
    if (cache_typed_config(reg, &reg->defs[i], err, err_len) != 0)
      return -1;
  }
  return 0;
}

static int add_error(char *errors, size_t errors_len, int *count,
                     const char *path, const char *code, const char *msg) {
  char epath[RT_MED], ecode[RT_SMALL], emsg[RT_MED], item[RT_LARGE];
  size_t used = strlen(errors);
  if (json_escape(path ? path : "$", epath, sizeof(epath)) != 0 ||
      json_escape(code ? code : "invalid", ecode, sizeof(ecode)) != 0 ||
      json_escape(msg ? msg : "Invalid value.", emsg, sizeof(emsg)) != 0) return -1;
  snprintf(item, sizeof(item), "%s{\"path\":\"%s\",\"code\":\"%s\",\"message\":\"%s\"}",
           *count ? "," : "", epath, ecode, emsg);
  if (used + strlen(item) + 2 >= errors_len) return -1;
  memcpy(errors + used, item, strlen(item) + 1);
  (*count)++;
  return 0;
}

static int number_is_integer(const JsonRef *v) {
  if (!v || v->type != JSON_REF_NUMBER) return 0;
  for (const char *p = v->start; p && p < v->end; ++p)
    if (*p == '.' || *p == 'e' || *p == 'E') return 0;
  return 1;
}

static int schema_type_matches(const char *type, const JsonRef *value) {
  if (!type || !*type) return 1;
  if (strcmp(type, "object") == 0) return value->type == JSON_REF_OBJECT;
  if (strcmp(type, "array") == 0) return value->type == JSON_REF_ARRAY;
  if (strcmp(type, "string") == 0) return value->type == JSON_REF_STRING;
  if (strcmp(type, "number") == 0) return value->type == JSON_REF_NUMBER;
  if (strcmp(type, "integer") == 0) return number_is_integer(value);
  if (strcmp(type, "boolean") == 0) return value->type == JSON_REF_BOOL;
  return 0;
}

static int validate_schema_ref(const JsonRef *schema, const JsonRef *value,
                               const char *path, char *errors,
                               size_t errors_len, int *count);

static int validate_enum(const JsonRef *schema, const JsonRef *value, const char *path,
                         char *errors, size_t errors_len, int *count) {
  JsonRef enums, ev;
  char got[RT_SMALL], candidate[RT_SMALL];
  if (json_ref_object_get_array(schema, "enum", &enums) != 0) return 0;
  if (value->type != JSON_REF_STRING ||
      json_ref_string_copy(value, got, sizeof(got)) != 0) {
    return add_error(errors, errors_len, count, path, "enum", "Value is not in enum.");
  }
  for (size_t i = 0; i < json_ref_array_size(&enums); ++i) {
    if (json_ref_array_get(&enums, i, &ev) != 0 || ev.type != JSON_REF_STRING) continue;
    if (json_ref_string_copy(&ev, candidate, sizeof(candidate)) == 0 &&
        strcmp(got, candidate) == 0) return 0;
  }
  return add_error(errors, errors_len, count, path, "enum", "Value is not in enum.");
}

static int validate_object_schema(const JsonRef *schema, const JsonRef *value,
                                  const char *path, char *errors,
                                  size_t errors_len, int *count) {
  JsonRef required, props, addl;
  if (json_ref_object_get_array(schema, "required", &required) == 0) {
    for (size_t i = 0; i < json_ref_array_size(&required); ++i) {
      JsonRef item, present;
      char key[RT_SMALL], child_path[RT_MED];
      if (json_ref_array_get(&required, i, &item) != 0 ||
          json_ref_string_copy(&item, key, sizeof(key)) != 0) continue;
      if (json_ref_object_get(value, key, &present) == 0) continue;
      snprintf(child_path, sizeof(child_path), "%s.%s", path, key);
      add_error(errors, errors_len, count, child_path, "required", "Missing required property.");
    }
  }
  if (json_ref_object_get_object(schema, "properties", &props) != 0) return 0;
  if (json_ref_object_get(schema, "additionalProperties", &addl) == 0 &&
      addl.type == JSON_REF_BOOL && strncmp(addl.start, "false", 5) == 0) {
    size_t cursor = 0;
    JsonRef val, prop_schema;
    char key[RT_SMALL], child_path[RT_MED];
    while (json_ref_object_iter(value, &cursor, key, sizeof(key), &val) == 1) {
      if (json_ref_object_get(&props, key, &prop_schema) == 0) continue;
      snprintf(child_path, sizeof(child_path), "%s.%s", path, key);
      add_error(errors, errors_len, count, child_path, "additionalProperties",
                "Unknown property.");
    }
  }
  {
    size_t cursor = 0;
    JsonRef prop_schema, val;
    char key[RT_SMALL], child_path[RT_MED];
    while (json_ref_object_iter(&props, &cursor, key, sizeof(key), &prop_schema) == 1) {
      if (json_ref_object_get(value, key, &val) != 0) continue;
      snprintf(child_path, sizeof(child_path), "%s.%s", path, key);
      validate_schema_ref(&prop_schema, &val, child_path, errors, errors_len, count);
    }
  }
  return 0;
}

static int validate_array_schema(const JsonRef *schema, const JsonRef *value,
                                 const char *path, char *errors,
                                 size_t errors_len, int *count) {
  JsonRef items, val;
  if (json_ref_object_get_object(schema, "items", &items) != 0) return 0;
  for (size_t i = 0; i < json_ref_array_size(value); ++i) {
    char child_path[RT_MED];
    if (json_ref_array_get(value, i, &val) != 0) continue;
    snprintf(child_path, sizeof(child_path), "%s[%zu]", path, i);
    validate_schema_ref(&items, &val, child_path, errors, errors_len, count);
  }
  return 0;
}

static int validate_schema_ref(const JsonRef *schema, const JsonRef *value,
                               const char *path, char *errors,
                               size_t errors_len, int *count) {
  char type[RT_SMALL] = "";
  if (!schema || schema->type != JSON_REF_OBJECT || !value) return -1;
  (void)json_ref_object_get_string(schema, "type", type, sizeof(type));
  if (!schema_type_matches(type, value)) {
    char msg[RT_MED];
    snprintf(msg, sizeof(msg), "Expected type %s.", type[0] ? type : "valid");
    add_error(errors, errors_len, count, path, "type", msg);
    return 0;
  }
  validate_enum(schema, value, path, errors, errors_len, count);
  if (strcmp(type, "object") == 0)
    validate_object_schema(schema, value, path, errors, errors_len, count);
  else if (strcmp(type, "array") == 0)
    validate_array_schema(schema, value, path, errors, errors_len, count);
  return 0;
}

int rt_action_validate_args(const RtActionDef *def, const char *args_json,
                           char *errors_json, size_t errors_len,
                           char *message, size_t message_len) {
  JsonRef schema, args;
  int count = 0;
  const char *src = (args_json && *args_json) ? args_json : "{}";
  if (!def || !errors_json || !errors_len) return -1;
  snprintf(errors_json, errors_len, "[");
  if (json_ref_from_text(def->args_schema, &schema) != 0 ||
      json_ref_from_text(src, &args) != 0 || args.type != JSON_REF_OBJECT) {
    add_error(errors_json, errors_len, &count, "$", "invalid_args", "Args must be an object.");
  } else {
    validate_schema_ref(&schema, &args, "$", errors_json, errors_len, &count);
  }
  strncat(errors_json, "]", errors_len - strlen(errors_json) - 1);
  if (message && message_len) {
    snprintf(message, message_len, "%s", count ? "Invalid args for action." : "");
  }
  return count ? -1 : 0;
}

static int render_available_action(const RtActionDef *def, int visible,
                                   char *out, size_t out_len, size_t *pos) {
  char id[RT_MED], desc[RT_LARGE], header[RT_LARGE];
  int n;
  if (!def || !out || !pos ||
      json_escape(def->id, id, sizeof(id)) != 0 ||
      json_escape(def->description, desc, sizeof(desc)) != 0)
    return -1;
  n = snprintf(header, sizeof(header),
               "%s\n  {\n    \"name\": \"%s\",\n"
               "    \"description\": \"%s\",\n    \"args\": ",
               visible ? "," : "", id, desc);
  if (n < 0 || (size_t)n >= sizeof(header) ||
      rt_append_text(out, out_len, pos, header) != 0 ||
      rt_append_text(out, out_len, pos, def->args_schema) != 0 ||
      rt_append_text(out, out_len, pos, "\n  }") != 0)
    return -1;
  return 0;
}

int rt_action_render_registry(const RtActionRegistry *reg,
                              char *out, size_t out_len) {
  size_t pos = 0;
  int visible = 0;
  if (!reg || !out || !out_len) return -1;
  out[0] = '\0';
  if (rt_append_text(out, out_len, &pos, "[") != 0) return -1;
  for (size_t i = 0; i < reg->count; ++i) {
    const RtActionDef *def = &reg->defs[i];
    if (def->force_disabled) {
      /* The operator listing states the whole registry, mask included --
       * only model-facing catalogs omit masked actions. */
      char id[RT_MED], desc[RT_LARGE], header[RT_LARGE];
      int n;
      if (json_escape(def->id, id, sizeof(id)) != 0 ||
          json_escape(def->description, desc, sizeof(desc)) != 0)
        return -1;
      n = snprintf(header, sizeof(header),
                   "%s\n  {\n    \"name\": \"%s\",\n"
                   "    \"force_disabled\": true,\n"
                   "    \"description\": \"%s\",\n    \"args\": ",
                   visible ? "," : "", id, desc);
      if (n < 0 || (size_t)n >= sizeof(header) ||
          rt_append_text(out, out_len, &pos, header) != 0 ||
          rt_append_text(out, out_len, &pos, def->args_schema) != 0 ||
          rt_append_text(out, out_len, &pos, "\n  }") != 0)
        return -1;
    } else if (render_available_action(def, visible, out, out_len, &pos) != 0) {
      return -1;
    }
    visible++;
  }
  return rt_append_text(out, out_len, &pos, visible ? "\n]" : "]");
}

int rt_action_render_available(const RtActionRegistry *reg, const RtAgentMeta *meta,
                              char *out, size_t out_len) {
  size_t pos = 0;
  int visible = 0;
  if (!reg || !out || !out_len) return -1;
  out[0] = '\0';
  if (rt_append_text(out, out_len, &pos, "[") != 0) return -1;
  if (meta && meta->has_action_allowlist) {
    /* agent.json order is product policy. Resolve each configured entry back
     * to authoritative manifests without string matching on the hot path.
     * all_actions expands in place and deliberately does not deduplicate
     * explicit entries; overlap is visible configuration, not renderer policy.
     * Force-disabled entries stay listed in the allowlist but never reach
     * the rendered catalog. */
    for (size_t i = 0; i < meta->action_count; ++i) {
      unsigned int id = meta->action_ids[i];
      if (id == RT_ACTION_PRESENT_ALL) {
        for (size_t j = 0; j < reg->count; ++j) {
          if (reg->defs[j].force_disabled) continue;
          if (render_available_action(&reg->defs[j], visible,
                                      out, out_len, &pos) != 0)
            return -1;
          visible++;
        }
      } else {
        const RtActionDef *def = id < reg->count ? &reg->defs[id] : NULL;
        if (!def) return -1;
        if (def->force_disabled) continue;
        if (render_available_action(def, visible, out, out_len, &pos) != 0)
          return -1;
        visible++;
      }
    }
  } else if (meta) {
    for (size_t i = 0; i < reg->count; ++i) {
      if (reg->defs[i].force_disabled) continue;
      if (render_available_action(&reg->defs[i], visible,
                                  out, out_len, &pos) != 0)
        return -1;
      visible++;
    }
  }
  return rt_append_text(out, out_len, &pos, visible ? "\n]" : "]");
}

int rt_action_render_definition(const RtActionDef *def,
                                char *out, size_t out_len) {
  size_t pos = 0;
  if (!def || !out || out_len == 0) return -1;
  out[0] = '\0';
  if (rt_append_text(out, out_len, &pos, "[") != 0 ||
      render_available_action(def, 0, out, out_len, &pos) != 0)
    return -1;
  return rt_append_text(out, out_len, &pos, "\n]");
}

int rt_action_render_allowed_ids(const RtActionRegistry *reg,
                                 const RtAgentMeta *meta,
                                 char *out, size_t out_len) {
  size_t pos = 0;
  int visible = 0;
  if (!reg || !meta || !out || out_len == 0) return -1;
  out[0] = '\0';
  if (rt_append_text(out, out_len, &pos, "[") != 0) return -1;
  for (size_t i = 0; i < reg->count; ++i) {
    char escaped[RT_MED], item[RT_MED + 4];
    const RtActionDef *def = &reg->defs[i];
    int n;
    if (def->force_disabled) continue;
    if (!rt_agent_allows_action(meta, def)) continue;
    if (json_escape(def->id, escaped, sizeof(escaped)) != 0) return -1;
    n = snprintf(item, sizeof(item), "%s\"%s\"", visible ? "," : "",
                 escaped);
    if (n < 0 || (size_t)n >= sizeof(item) ||
        rt_append_text(out, out_len, &pos, item) != 0)
      return -1;
    visible++;
  }
  return rt_append_text(out, out_len, &pos, "]");
}

int rt_action_foreach_registered(RtActionVisitor visitor, void *user) {
  RtActionRegistry reg;
  char err[RT_LARGE];
  if (!visitor) return -1;
  if (rt_action_registry_load(&reg, err, sizeof(err)) != 0) {
    /* A listing that silently shows nothing hides exactly the collision the
     * scan exists to report. */
    fprintf(stderr, "%s\n", err);
    return -1;
  }
  for (size_t i = 0; i < reg.count; ++i) {
    const RtActionDef *d = &reg.defs[i];
    char script[PATH_MAX];
    if (d->exec_kind == RT_ACTION_EXEC_SUBPROCESS)
      snprintf(script, sizeof(script), "%s/%s", d->dir, d->exec_arg);
    else
      snprintf(script, sizeof(script), "runtime:%s", d->exec_arg);
    if (visitor(d->id, d->area_id, d->dir, d->manifest_path, script, user) != 0) return -1;
  }
  return 0;
}
