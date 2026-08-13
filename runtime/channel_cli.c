#include "runtime.h"
#include "channel/channel.h"
#include "support/cli_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw -i [-a|-h] [--floop <name>]\n"
          "  fclaw channel cli [-a|-h] [--floop <name>]\n"
          "  fclaw channel ws  [-a|-h] [--floop <name>]\n"
          "  fclaw channel irc [-a|-h] [--floop <name>]\n");
}

/* Strict: accepts only `--floop <name>`. Unknown arguments, a missing
 * value, or a flag-shaped value fail loudly — a typo must never silently
 * run under the default or live floop. */
static const char *parse_floop(int argc, char **argv, int *ok,
                               FcCliOutputMode *mode) {
  const char *floop = NULL;
  if (ok) *ok = 1;
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(mode, argv[i], "fclaw channel");
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
  return floop;
}

int rt_chat_interactive(int argc, char **argv) {
  int ok = 1;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  const char *floop = parse_floop(argc, argv, &ok, &mode);
  return ok ? channel_run_cli_interactive(floop, mode.human) : 1;
}

int rt_channel_main(int argc, char **argv) {
  int ok = 1;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  if (argc < 1) { usage(); return 1; }
  if (strcmp(argv[0], "cli") == 0) { const char *f = parse_floop(argc - 1, argv + 1, &ok, &mode); return ok ? channel_run_cli_interactive(f, mode.human) : 1; }
  if (strcmp(argv[0], "ws") == 0)  { const char *f = parse_floop(argc - 1, argv + 1, &ok, &mode); return ok ? channel_run_ws(f, mode.human) : 1; }
  if (strcmp(argv[0], "irc") == 0) { const char *f = parse_floop(argc - 1, argv + 1, &ok, &mode); return ok ? channel_run_irc(f, mode.human) : 1; }
  usage();
  return 1;
}
