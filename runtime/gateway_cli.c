#include "runtime.h"
#include "gateway/gateway.h"
#include "support/cli_output.h"

#include <stdlib.h>
#include <string.h>

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw gateway start [-a|-h] [--floop <name>]\n"
          "  fclaw gateway run [-a|-h] [--floop <name>]\n"
          "  fclaw gateway status [-a|-h]\n"
          "  fclaw gateway stop [-a|-h]\n"
          "  fclaw gateway reload [-a|-h] [--floop <name>]\n");
}

/* Strict: accepts only `--floop <name>`. Unknown arguments, a missing
 * value, or a flag-shaped value fail loudly — a typo must never silently
 * start the gateway under the default floop. */
static const char *parse_floop(int argc, char **argv, int *ok,
                               FcCliOutputMode *mode) {
  const char *floop = NULL;
  if (ok) *ok = 1;
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(mode, argv[i], "fclaw gateway");
    if (mode_rc < 0) {
      if (ok) *ok = 0;
      return NULL;
    }
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--loop") == 0) {
      fprintf(stderr, "unknown flag --loop. Did you mean --floop? Run `fclaw` with no arguments for usage.\n");
      if (ok) *ok = 0;
      return NULL;
    }
    if (strcmp(argv[i], "--floop") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '-') {
        fprintf(stderr, "missing value for --floop; fix: --floop <name>\n");
        if (ok) *ok = 0;
        return NULL;
      }
      floop = argv[++i];
      continue;
    }
    fprintf(stderr, "unknown argument '%s'; fix: remove it or run `fclaw` with no arguments for usage\n", argv[i]);
    if (ok) *ok = 0;
    return NULL;
  }
  return floop ? floop : rt_default_loop();
}

static int parse_output_only(int argc, char **argv, const char *command,
                             FcCliOutputMode *mode) {
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(mode, argv[i], command);
    if (mode_rc < 0) return -1;
    if (mode_rc > 0) continue;
    fprintf(stderr, "%s: unknown argument '%s'; accepts only -a or -h\n",
            command, argv[i]);
    return -1;
  }
  return 0;
}

int rt_gateway_main(int argc, char **argv) {
  int ok = 1;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  if (argc < 1) {
    usage();
    fc_cli_print_json_error("gateway", "missing_subcommand",
                            "missing gateway subcommand");
    return 1;
  }
  if (strcmp(argv[0], "start") == 0) {
    const char *f = parse_floop(argc - 1, argv + 1, &ok, &mode);
    return ok ? gateway_start(f, mode.human) : 1;
  }
  if (strcmp(argv[0], "run") == 0) {
    const char *f = parse_floop(argc - 1, argv + 1, &ok, &mode);
    return ok ? gateway_run_foreground(f, mode.human) : 1;
  }
  if (strcmp(argv[0], "status") == 0) {
    ok = parse_output_only(argc - 1, argv + 1, "fclaw gateway status",
                           &mode) == 0;
    return ok ? gateway_status(mode.human) : 1;
  }
  if (strcmp(argv[0], "stop") == 0) {
    if (parse_output_only(argc - 1, argv + 1, "fclaw gateway stop",
                          &mode) != 0) return 1;
    return gateway_stop(mode.human);
  }
  if (strcmp(argv[0], "reload") == 0) {
    const char *f = parse_floop(argc - 1, argv + 1, &ok, &mode);
    return ok ? gateway_reload(f, mode.human) : 1;
  }
  usage();
  return 1;
}
