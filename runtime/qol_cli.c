#include "runtime.h"
#include "version.h"
#include "gateway/adapter.h"
#include "support/cli_output.h"
#include "support/color.h"
#include "support/heap_guard.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rt_clear_main(int argc, char **argv) {
  int yes = 0;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw clear");
    if (mode_rc < 0) {
      fc_cli_print_json_error("clear", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return 1;
    }
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) yes = 1;
    else {
      fprintf(stderr, "usage: fclaw clear [-a|-h] [--yes]\n");
      if (!mode.human)
        fc_cli_print_json_error("clear", "invalid_arguments",
                                "usage: fclaw clear [-a|-h] [--yes]");
      return 1;
    }
  }
  if (!yes) {
    fprintf(stderr,
            "fclaw clear deletes workspace/runs, workspace/memory,\n"
            "workspace/media, workspace/bus, workspace/logs, .fclaw/ — "
            "re-run with --yes to confirm.\n");
    if (!mode.human)
      fc_cli_print_json_error("clear", "confirmation_required",
                              "re-run with --yes to confirm workspace deletion");
    return 1;
  }
  /* system() is fine here — this is the CLI path, not the reactor. */
  (void)system("rm -rf workspace/runs workspace/memory workspace/media "
               "workspace/bus workspace/logs .fclaw");
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "workspace cleared" COLOR_RESET "\n");
  else
    printf("{\"ok\":true,\"command\":\"clear\",\"cleared\":true}\n");
  return 0;
}

typedef struct {
  int human;
  int first;
} ActionListOutput;

static int print_registered_action(const char *id, const char *area_id,
                                  const char *dir, const char *manifest_path,
                                  const char *script_path, void *user) {
  char *text = NULL;
  int outside = 0;
  ActionListOutput *output = (ActionListOutput *)user;
  (void)dir; (void)script_path;
  if (fs_read_text(manifest_path, &text, FS_READ_TEXT_DEFAULT_CAP) == 0 && text) {
    JsonRef root;
    if (json_ref_first_object(text, &root) == 0)
      (void)json_ref_object_get_bool(&root, "outside_world", &outside);
    fc_xfree(text);
  }
  if (output && output->human) {
    printf(COLOR_GREEN_BRIGHT "%-20s" COLOR_RESET " %-12s %-12s %s\n",
           id, area_id, outside ? "yes" : "no", manifest_path);
  } else {
    if (output && !output->first) putchar(',');
    fputs("{\"id\":", stdout); fc_cli_print_json_string(stdout, id);
    fputs(",\"area\":", stdout); fc_cli_print_json_string(stdout, area_id);
    printf(",\"outside_world\":%s,\"manifest\":",
           outside ? "true" : "false");
    fc_cli_print_json_string(stdout, manifest_path);
    putchar('}');
    if (output) output->first = 0;
  }
  return 0;
}

int rt_tools_main(int argc, char **argv) {
  FcCliOutputMode mode;
  ActionListOutput output;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw actions");
    if (rc < 0) return 1;
    if (rc == 0) {
      fprintf(stderr, "usage: fclaw actions [-a|-h]\n");
      if (!mode.human)
        fc_cli_print_json_error("actions", "invalid_arguments",
                                "usage: fclaw actions [-a|-h]");
      return 1;
    }
  }
  output.human = mode.human;
  output.first = 1;
  if (mode.human)
    printf(COLOR_CYAN "%-20s %-12s %-12s %s" COLOR_RESET "\n",
           "action", "area", "outside?", "manifest");
  else
    putchar('[');
  {
    int rc = rt_action_foreach_registered(print_registered_action, &output);
    if (!mode.human) puts("]");
    return rc;
  }
}

/* channels.<name>.enabled in the deployment config. Absent or unreadable
 * config means every adapter is off, which is what a fresh clone wants. */
static int channel_enabled(const char *name) {
  char *text = NULL;
  JsonRef root, channels, mine;
  int enabled = 0;
  if (fs_read_text("config/floofclaw_config.json", &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return 0;
  if (json_ref_first_object(text, &root) == 0 &&
      json_ref_object_get_object(&root, "channels", &channels) == 0 &&
      json_ref_object_get_object(&channels, name, &mine) == 0)
    (void)json_ref_object_get_bool(&mine, "enabled", &enabled);
  fc_xfree(text);
  return enabled;
}

int rt_adapters_main(int argc, char **argv) {
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw adapters");
    if (rc <= 0) {
      if (rc == 0) fprintf(stderr, "usage: fclaw adapters [-a|-h]\n");
      if (!mode.human)
        fc_cli_print_json_error("adapters", "invalid_arguments",
                                "usage: fclaw adapters [-a|-h]");
      return 1;
    }
  }
  if (mode.human)
    printf(COLOR_CYAN "%-16s %-10s %s" COLOR_RESET "\n",
           "adapter", "enabled?", "config key");
  else
    putchar('[');
  for (size_t i = 0; i < fc_adapter_count; ++i) {
    const char *name = fc_adapters[i]->name;
    int enabled = channel_enabled(name);
    if (mode.human)
      printf(COLOR_GREEN_BRIGHT "%-16s" COLOR_RESET " %-10s channels.%s\n",
             name, enabled ? "yes" : "no", name);
    else {
      if (i) putchar(',');
      fputs("{\"id\":", stdout); fc_cli_print_json_string(stdout, name);
      printf(",\"enabled\":%s,\"config_key\":", enabled ? "true" : "false");
      {
        char key[RT_MED];
        snprintf(key, sizeof(key), "channels.%s", name);
        fc_cli_print_json_string(stdout, key);
      }
      putchar('}');
    }
  }
  if (!mode.human) puts("]");
  else if (fc_adapter_count == 0)
    printf("(none — add a directory under adapters/ and rebuild)\n");
  return 0;
}

#define RT_FLOOP_LIST_MAX 128

typedef struct { char name[RT_SMALL]; } FloopName;

static int floop_name_cmp(const void *a, const void *b) {
  return strcmp(((const FloopName *)a)->name, ((const FloopName *)b)->name);
}

int rt_floops_main(int argc, char **argv) {
  DIR *d;
  struct dirent *e;
  FloopName names[RT_FLOOP_LIST_MAX];
  size_t count = 0, i;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int argi = 0; argi < argc; ++argi) {
    int rc = fc_cli_output_mode_arg(&mode, argv[argi], "fclaw floops");
    if (rc <= 0) {
      if (rc == 0) fprintf(stderr, "usage: fclaw floops [-a|-h]\n");
      if (!mode.human)
        fc_cli_print_json_error("floops", "invalid_arguments",
                                "usage: fclaw floops [-a|-h]");
      return 1;
    }
  }
  if (mode.human)
    printf(COLOR_CYAN "%-20s %s" COLOR_RESET "\n", "floop", "loop");
  d = opendir("floops");
  if (!d) {
    fprintf(stderr, "floops/ is missing\n");
    return 1;
  }
  /* Reuse the resolver's notion of a floop -- a directory holding loop.json --
   * so this listing can never disagree with what --floop accepts. Sorted for
   * the same reason the action scan is: readdir order is not stable across
   * machines, and a listing people compare should be. */
  while ((e = readdir(d)) != NULL && count < RT_FLOOP_LIST_MAX) {
    char path[512];
    if (e->d_name[0] == '.') continue;
    if (snprintf(path, sizeof(path), "floops/%s/loop.json", e->d_name) >=
        (int)sizeof(path) || !fs_file_exists(path))
      continue;
    snprintf(names[count].name, sizeof(names[0].name), "%s", e->d_name);
    count++;
  }
  closedir(d);
  qsort(names, count, sizeof(names[0]), floop_name_cmp);
  if (!mode.human) putchar('[');
  for (i = 0; i < count; ++i) {
    if (mode.human)
      printf(COLOR_GREEN_BRIGHT "%-20s" COLOR_RESET " floops/%s/loop.json\n",
             names[i].name, names[i].name);
    else {
      if (i) putchar(',');
      fputs("{\"id\":", stdout); fc_cli_print_json_string(stdout, names[i].name);
      fputs(",\"loop\":", stdout);
      {
        char path[512];
        snprintf(path, sizeof(path), "floops/%s/loop.json", names[i].name);
        fc_cli_print_json_string(stdout, path);
      }
      putchar('}');
    }
  }
  if (!mode.human) puts("]");
  else if (!count) printf("(none — copy a floop directory into floops/)\n");
  return 0;
}

int rt_setup_main(int argc, char **argv) {
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw setup");
    if (rc <= 0) {
      if (rc == 0) fprintf(stderr, "usage: fclaw setup [-a|-h]\n");
      if (!mode.human)
        fc_cli_print_json_error("setup", "invalid_arguments",
                                "usage: fclaw setup [-a|-h]");
      return 1;
    }
  }
  if (!mode.human) {
    puts("{\"command\":\"setup\",\"wizard\":\"./scripts/onboard.sh\","
         "\"steps\":["
         "\"make test\","
         "\"./bin/fclaw run -h --text hello\","
         "\"fclaw auth set-stdin -h <provider-endpoint>\","
         "\"review config/floofclaw_config.json\","
         "\"./bin/fclaw gateway start -h\","
         "\"./bin/fclaw -i -h\"],"
         "\"paths\":{\"secrets\":\"./secrets/<endpoint>\","
         "\"workspace\":\"./workspace/\","
         "\"gateway_owner\":\".fclaw/run/gateway.{pid,loop}\"}}");
    return 0;
  }
  printf(
      COLOR_CYAN "fclaw setup" COLOR_RESET "\n\n"
      "Guided first run (identity, key, floop, channels, action enablement):\n"
      "  ./scripts/onboard.sh\n\n"
      "Quick first-run checklist:\n\n"
      "  1. Verify the runtime with the hermetic offline suite:\n"
      "       make test\n\n"
      "  2. Try the offline mock-backed one-shot (no key, gateway, or network):\n"
      "       ./bin/fclaw run -h --text \"hello\"\n"
      "\n"
      "  3. For a real provider, choose a profile and store its registry endpoint:\n"
      "       fclaw auth set-stdin -h gemini_key\n"
      "     (or the openai_key, openrouter_key, or anthropic_key endpoint you selected)\n\n"
      "  4. Review config/floofclaw_config.json. Fresh-clone channel entries\n"
      "     are disabled; configure their required fields before enabling one.\n\n"
      "  5. To run as a service:\n"
      "       ./bin/fclaw gateway start -h\n"
      "       ./bin/fclaw gateway status -h\n"
      "       ./bin/fclaw gateway stop -h\n\n"
      "  6. To talk on stdin:\n"
      "       ./bin/fclaw gateway start -h\n"
      "       ./bin/fclaw -i -h\n\n"
      "Storage (all in this directory — one floofclaw, one directory):\n"
      "  - secrets:   ./secrets/<endpoint>  (gitignored; set FCLAW_HOME to relocate)\n"
      "  - workspace: ./workspace/  (runs, memory, bus, logs)\n"
      "  - .fclaw/run/gateway.{pid,loop} when the gateway is running\n");
  return 0;
}

int rt_version_main(int argc, char **argv) {
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw version");
    if (rc <= 0) {
      if (rc == 0) fprintf(stderr, "usage: fclaw --version|-v [-a|-h]\n");
      if (!mode.human)
        fc_cli_print_json_error("version", "invalid_arguments",
                                "usage: fclaw --version|-v [-a|-h]");
      return 1;
    }
  }
  if (mode.human)
    printf(COLOR_CYAN "fclaw" COLOR_RESET " %s\n", FCLAW_VERSION);
  else {
    fputs("{\"name\":\"fclaw\",\"version\":", stdout);
    fc_cli_print_json_string(stdout, FCLAW_VERSION);
    puts("}");
  }
  return 0;
}
