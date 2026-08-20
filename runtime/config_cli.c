#include "runtime.h"
#include "support/cli_output.h"
#include "support/heap_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fclaw config — the one owner of config/floofclaw_config.json edits.
 *
 * get/set/enable/disable splice value spans in the raw text (JsonRef keeps
 * byte spans into the file), so every byte an edit does not target — key
 * order, indentation, _comment entries — survives verbatim. Nothing here
 * reserializes the document.
 *
 *   get <key>            print the value at a dotted path
 *   set <key> <value>    replace it (or add the leaf to an existing parent)
 *   enable <action-id>   remove the id from force_disable
 *   disable <action-id>  add the id to force_disable
 *
 * A <value> that parses as a complete JSON value is spliced verbatim
 * (numbers, booleans, arrays); anything else becomes a JSON string. */

#define CONFIG_CLI_PATH "config/floofclaw_config.json"

static void config_usage(void) {
  fprintf(stderr,
          "usage: fclaw config get [-a|-h] <key>\n"
          "       fclaw config set [-a|-h] <key> <value>\n"
          "       fclaw config enable [-a|-h] <action-id>\n"
          "       fclaw config disable [-a|-h] <action-id>\n"
          "keys are dotted paths: bot_name, gateway.port, "
          "channels.discord.enabled\n");
}

/* Walk a dotted path. Returns 1 with *val set when the leaf exists, 0 with
 * *parent set when only the leaf is missing, -1 when an ancestor is not
 * there (the error message owns naming the fix). */
static int config_resolve(const char *text, const char *key,
                          JsonRef *parent, JsonRef *val,
                          char *leaf, size_t leaf_len) {
  JsonRef obj;
  char segment[RT_MED];
  const char *p = key;
  if (json_ref_first_object(text, &obj) != 0) return -1;
  for (;;) {
    const char *dot = strchr(p, '.');
    size_t seg_len = dot ? (size_t)(dot - p) : strlen(p);
    if (seg_len == 0 || seg_len >= sizeof(segment)) return -1;
    memcpy(segment, p, seg_len);
    segment[seg_len] = '\0';
    if (!dot) {
      JsonRef found;
      if (snprintf(leaf, leaf_len, "%s", segment) >= (int)leaf_len) return -1;
      *parent = obj;
      if (json_ref_object_get(&obj, segment, &found) != 0) return 0;
      *val = found;
      return 1;
    }
    if (json_ref_object_get_object(&obj, segment, &obj) != 0) return -1;
    p = dot + 1;
  }
}

static int config_object_is_empty(const JsonRef *obj) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef v;
  return json_ref_object_iter(obj, &cursor, key, sizeof(key), &v) != 1;
}

/* Replace [from, to) in text with repl and write the file atomically. */
static int config_splice(const char *text, const char *from, const char *to,
                         const char *repl) {
  size_t head = (size_t)(from - text);
  size_t tail_len = strlen(to);
  size_t repl_len = strlen(repl);
  char *next = (char *)fc_xmalloc(head + repl_len + tail_len + 1);
  int rc;
  if (!next) return -1;
  memcpy(next, text, head);
  memcpy(next + head, repl, repl_len);
  memcpy(next + head + repl_len, to, tail_len + 1);
  rc = fs_write_text_atomic(CONFIG_CLI_PATH, next);
  fc_xfree(next);
  return rc;
}

static int config_read(char **text) {
  if (fs_read_text(CONFIG_CLI_PATH, text, FS_READ_TEXT_DEFAULT_CAP) != 0 ||
      !*text) {
    fprintf(stderr, "fclaw config: %s is unreadable; fix: run from the "
                    "floofclaw directory\n", CONFIG_CLI_PATH);
    return -1;
  }
  return 0;
}

static void config_print_value(const FcCliOutputMode *mode, const JsonRef *v) {
  if (mode->human && v->type == JSON_REF_STRING) {
    char plain[RT_LARGE];
    if (json_ref_string_copy(v, plain, sizeof(plain)) == 0) {
      printf("%s\n", plain);
      return;
    }
  }
  printf("%.*s\n", (int)(v->end - v->start), v->start);
}

static int config_get_main(const FcCliOutputMode *mode, const char *key) {
  char *text = NULL;
  JsonRef parent, val;
  char leaf[RT_MED];
  int found;
  if (config_read(&text) != 0) return 1;
  found = config_resolve(text, key, &parent, &val, leaf, sizeof(leaf));
  if (found != 1) {
    fprintf(stderr, "fclaw config get: no value at %s\n", key);
    if (!mode->human)
      fc_cli_print_json_error("config.get", "key_not_found", key);
    free(text);
    return 1;
  }
  config_print_value(mode, &val);
  free(text);
  return 0;
}

/* A raw argument becomes JSON: verbatim when it already parses as one
 * complete JSON value, a JSON string otherwise. */
static int config_encode_value(const char *raw, char *out, size_t out_len) {
  JsonRef probe;
  char escaped[RT_LARGE];
  if (json_ref_from_text(raw, &probe) == 0)
    return snprintf(out, out_len, "%s", raw) >= (int)out_len ? -1 : 0;
  if (json_escape(raw, escaped, sizeof(escaped)) != 0) return -1;
  return snprintf(out, out_len, "\"%s\"", escaped) >= (int)out_len ? -1 : 0;
}

static void config_ack(const FcCliOutputMode *mode, const char *verb,
                       const char *key, const char *detail) {
  if (mode->human) {
    printf("config: %s %s%s%s\n", verb, key,
           detail && *detail ? " " : "", detail ? detail : "");
  } else {
    char ekey[RT_MED], edetail[RT_MED];
    (void)json_escape(key, ekey, sizeof(ekey));
    (void)json_escape(detail ? detail : "", edetail, sizeof(edetail));
    printf("{\"type\":\"config.%s\",\"key\":\"%s\",\"detail\":\"%s\"}\n",
           verb, ekey, edetail);
  }
}

static int config_set_main(const FcCliOutputMode *mode, const char *key,
                           const char *raw) {
  char *text = NULL;
  JsonRef parent, val;
  char leaf[RT_MED];
  char value[RT_LARGE];
  int found, rc = 1;
  if (config_read(&text) != 0) return 1;
  if (config_encode_value(raw, value, sizeof(value)) != 0) {
    fprintf(stderr, "fclaw config set: value is too large\n");
    free(text);
    return 1;
  }
  found = config_resolve(text, key, &parent, &val, leaf, sizeof(leaf));
  if (found < 0) {
    fprintf(stderr,
            "fclaw config set: no parent object for %s; fix: set keys "
            "under objects that exist in %s\n", key, CONFIG_CLI_PATH);
    if (!mode->human)
      fc_cli_print_json_error("config.set", "parent_missing", key);
    free(text);
    return 1;
  }
  if (found == 1) {
    rc = config_splice(text, val.start, val.end, value);
  } else if (config_object_is_empty(&parent)) {
    char repl[RT_LARGE + RT_MED];
    if (snprintf(repl, sizeof(repl), "{ \"%s\": %s }", leaf, value) <
        (int)sizeof(repl))
      rc = config_splice(text, parent.start, parent.end, repl);
  } else {
    char insert[RT_LARGE + RT_MED];
    if (snprintf(insert, sizeof(insert), "\n    \"%s\": %s,", leaf, value) <
        (int)sizeof(insert))
      rc = config_splice(text, parent.start + 1, parent.start + 1, insert);
  }
  free(text);
  if (rc != 0) {
    fprintf(stderr, "fclaw config set: could not write %s\n",
            CONFIG_CLI_PATH);
    return 1;
  }
  config_ack(mode, "set", key, raw);
  return 0;
}

/* force_disable membership edits. Both directions are idempotent facts:
 * enabling an id that is not listed, or disabling one that already is,
 * succeeds and says so. */
static int config_toggle_main(const FcCliOutputMode *mode, const char *id,
                              int disable) {
  char *text = NULL;
  JsonRef root, arr;
  JsonRef elems[RT_MAX_ACTIONS];
  size_t n = 0, i, match = (size_t)-1;
  const char *verb = disable ? "disable" : "enable";
  int rc = 1;
  if (config_read(&text) != 0) return 1;
  if (json_ref_first_object(text, &root) != 0) {
    fprintf(stderr, "fclaw config %s: %s is not a JSON object\n", verb,
            CONFIG_CLI_PATH);
    free(text);
    return 1;
  }
  if (json_ref_object_get_array(&root, "force_disable", &arr) != 0) {
    if (!disable) {
      config_ack(mode, verb, id, "(no force_disable list; already enabled)");
      free(text);
      return 0;
    }
    /* No list yet: add one at the top of the root object. */
    {
      char insert[RT_MED * 2];
      if (snprintf(insert, sizeof(insert),
                   "\n  \"force_disable\": [\"%s\"],", id) <
          (int)sizeof(insert))
        rc = config_splice(text, root.start + 1, root.start + 1, insert);
    }
    free(text);
    if (rc != 0) return 1;
    config_ack(mode, verb, id, "(added to force_disable)");
    return 0;
  }
  for (i = 0; i < json_ref_array_size(&arr) && n < RT_MAX_ACTIONS; ++i) {
    char entry[RT_SMALL];
    if (json_ref_array_get(&arr, i, &elems[n]) != 0) continue;
    if (json_ref_string_copy(&elems[n], entry, sizeof(entry)) == 0 &&
        strcmp(entry, id) == 0)
      match = n;
    n++;
  }
  if (disable) {
    if (match != (size_t)-1) {
      config_ack(mode, verb, id, "(already in force_disable)");
      free(text);
      return 0;
    }
    if (n == 0) {
      char repl[RT_MED];
      if (snprintf(repl, sizeof(repl), "[\"%s\"]", id) < (int)sizeof(repl))
        rc = config_splice(text, arr.start, arr.end, repl);
    } else {
      char insert[RT_MED];
      if (snprintf(insert, sizeof(insert), "\n    \"%s\",", id) <
          (int)sizeof(insert))
        rc = config_splice(text, arr.start + 1, arr.start + 1, insert);
    }
    free(text);
    if (rc != 0) return 1;
    config_ack(mode, verb, id, "(added to force_disable)");
    return 0;
  }
  if (match == (size_t)-1) {
    config_ack(mode, verb, id, "(not in force_disable; already enabled)");
    free(text);
    return 0;
  }
  if (n == 1) {
    rc = config_splice(text, arr.start, arr.end, "[]");
  } else if (match < n - 1) {
    rc = config_splice(text, elems[match].start, elems[match + 1].start, "");
  } else {
    rc = config_splice(text, elems[match - 1].end, elems[match].end, "");
  }
  free(text);
  if (rc != 0) return 1;
  config_ack(mode, verb, id, "(removed from force_disable)");
  return 0;
}

int rt_config_main(int argc, char **argv) {
  FcCliOutputMode mode;
  const char *verb = NULL;
  const char *args[2] = {NULL, NULL};
  size_t nargs = 0;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw config");
    if (rc < 0) {
      config_usage();
      return 2;
    }
    if (rc > 0) continue;
    if (!verb) verb = argv[i];
    else if (nargs < 2) args[nargs++] = argv[i];
    else { config_usage(); return 2; }
  }
  if (!verb) { config_usage(); return 2; }
  if (strcmp(verb, "get") == 0 && nargs == 1)
    return config_get_main(&mode, args[0]);
  if (strcmp(verb, "set") == 0 && nargs == 2)
    return config_set_main(&mode, args[0], args[1]);
  if (strcmp(verb, "enable") == 0 && nargs == 1)
    return config_toggle_main(&mode, args[0], 0);
  if (strcmp(verb, "disable") == 0 && nargs == 1)
    return config_toggle_main(&mode, args[0], 1);
  config_usage();
  return 2;
}
