/* `fclaw mcp` — an operator entry point for the MCP generation scripts.
 *
 * This wraps, it does not implement. The protocol lives in
 * scripts/mcp_sync.sh and actions/mcp/_bridge.sh, and nothing here knows a
 * JSON-RPC method, a transport, or a tool. The binary gains a verb, not an
 * MCP client: no executor, no dlopen, no connection lifecycle.
 *
 * The verb exists because the CLI output contract is deployment-wide — every
 * public command answers to -a and -h — and a script invoked directly cannot
 * be discovered by `fclaw` with no arguments. Agent mode maps to the scripts'
 * --json; human mode is their operator text.
 */
#include "runtime.h"
#include "support/cli_output.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define RT_MCP_MAX_ARGS 16

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw mcp sync [-a|-h] [--dry-run]\n"
          "  fclaw mcp secret [-a|-h] <server> <ENV_NAME>   (value on stdin)\n"
          "\n"
          "sync reads config/mcp.json and regenerates actions/mcp/.\n"
          "Restart the gateway afterwards to pick up the catalog.\n");
}

/* The scripts, like the rest of the CLI, are addressed from the deployment
 * root. Say so plainly rather than exec'ing into a confusing failure. */
static int check_script(const char *path, int human) {
  if (access(path, X_OK) == 0) return 0;
  if (human) {
    fprintf(stderr, "mcp: %s is missing or not executable.\n", path);
    fprintf(stderr, "     Run fclaw from the deployment root.\n");
  } else {
    fc_cli_print_json_error("mcp", "script_missing",
                            "the mcp scripts were not found; run fclaw from "
                            "the deployment root");
  }
  return 1;
}

static int run_script(const char *path, char **args, int nargs, int human) {
  char *argv[RT_MCP_MAX_ARGS];
  int n = 0, i, status = 0;
  pid_t pid;
  if (check_script(path, human) != 0) return 1;
  argv[n++] = (char *)path;
  if (!human) argv[n++] = (char *)"--json";
  for (i = 0; i < nargs && n < RT_MCP_MAX_ARGS - 1; ++i) argv[n++] = args[i];
  argv[n] = NULL;

  pid = fork();
  if (pid < 0) {
    if (human) fprintf(stderr, "mcp: fork failed: %s\n", strerror(errno));
    else fc_cli_print_json_error("mcp", "fork_failed", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    execv(path, argv);
    _exit(127);
  }
  if (waitpid(pid, &status, 0) < 0) return 1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
}

int rt_mcp_main(int argc, char **argv) {
  FcCliOutputMode mode;
  char *rest[RT_MCP_MAX_ARGS];
  int nrest = 0, i;
  const char *verb;

  fc_cli_output_mode_init(&mode);
  if (argc < 1) { usage(); return 1; }
  verb = argv[0];

  for (i = 1; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw mcp");
    if (mode_rc < 0) {
      fc_cli_print_json_error("mcp", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return 1;
    }
    if (mode_rc > 0) continue;
    if (nrest >= RT_MCP_MAX_ARGS - 2) { usage(); return 1; }
    rest[nrest++] = argv[i];
  }

  if (strcmp(verb, "sync") == 0) {
    for (i = 0; i < nrest; ++i) {
      if (strcmp(rest[i], "--dry-run") != 0) {
        fprintf(stderr, "unknown argument '%s'; fix: remove it or run "
                        "`fclaw mcp` for usage\n", rest[i]);
        return 1;
      }
    }
    return run_script("scripts/mcp_sync.sh", rest, nrest, mode.human);
  }
  if (strcmp(verb, "secret") == 0) {
    if (nrest != 2) {
      fprintf(stderr, "mcp secret takes <server> and <ENV_NAME>, and reads "
                      "the value on stdin\n");
      return 1;
    }
    return run_script("scripts/mcp_secret.sh", rest, nrest, mode.human);
  }
  usage();
  return 1;
}
