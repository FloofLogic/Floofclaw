#include "runtime.h"

#include "secure/auth.h"
#include "secure/auth_targets.h"
#include "secure/secret_store.h"
#include "support/cli_output.h"
#include "support/color.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUTH_CLI_BUF 8192

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw auth set [-a|-h] <endpoint> <value>\n"
          "  fclaw auth set-stdin [-a|-h] <endpoint>\n"
          "  fclaw auth get [-a|-h] <endpoint>\n"
          "  fclaw auth delete [-a|-h] <endpoint>\n"
          "  fclaw auth header [-a|-h] <endpoint>\n");
}

static int do_set(const char *endpoint, const char *value, int human) {
  char key[256];
  if (!auth_target_is_secret_name(endpoint)) {
    fprintf(stderr, "auth: invalid endpoint name: %s\n", endpoint);
    return 1;
  }
  if (auth_target_build_secret_key(endpoint, key, sizeof(key)) != 0) return 1;
  if (secret_store_set(key, value) != 0) {
    fprintf(stderr, "auth: failed to store secret for %s\n", endpoint);
    return 1;
  }
  if (human)
    printf(COLOR_GREEN_BRIGHT "stored secret" COLOR_RESET " for %s\n", endpoint);
  else {
    fputs("{\"ok\":true,\"command\":\"auth.set\",\"endpoint\":", stdout);
    fc_cli_print_json_string(stdout, endpoint);
    puts("}");
  }
  return 0;
}

static int do_set_stdin(const char *endpoint, int human) {
  char buf[AUTH_CLI_BUF];
  size_t used = 0;
  int rc;
  while (used + 1 < sizeof(buf)) {
    ssize_t n = read(0, buf + used, sizeof(buf) - used - 1);
    if (n <= 0) break;
    used += (size_t)n;
  }
  buf[used] = '\0';
  while (used > 0 && (buf[used - 1] == '\n' || buf[used - 1] == '\r')) buf[--used] = '\0';
  if (used == 0) {
    fprintf(stderr, "auth: failed to read secret from stdin\n");
    return 1;
  }
  rc = do_set(endpoint, buf, human);
  secret_store_zero(buf, sizeof(buf));
  return rc;
}

static int do_get(const char *endpoint, int human) {
  char value[AUTH_CLI_BUF];
  int rc = 1;
  if (auth_resolve_static_secret(endpoint, value, sizeof(value)) == 0) {
    if (human)
      printf(COLOR_CYAN "%s:" COLOR_RESET " %s\n", endpoint, value);
    else {
      fputs("{\"endpoint\":", stdout); fc_cli_print_json_string(stdout, endpoint);
      fputs(",\"value\":", stdout); fc_cli_print_json_string(stdout, value);
      puts("}");
    }
    rc = 0;
  } else {
    fprintf(stderr, "auth: no secret stored for %s\n", endpoint);
  }
  secret_store_zero(value, sizeof(value));
  return rc;
}

static int do_delete(const char *endpoint, int human) {
  char key[256];
  if (!auth_target_is_secret_name(endpoint)) {
    fprintf(stderr, "auth: invalid endpoint name: %s\n", endpoint);
    return 1;
  }
  if (auth_target_build_secret_key(endpoint, key, sizeof(key)) != 0) return 1;
  if (secret_store_delete(key) != 0) {
    fprintf(stderr, "auth: failed to delete secret for %s\n", endpoint);
    return 1;
  }
  if (human)
    printf(COLOR_GREEN_BRIGHT "deleted secret" COLOR_RESET " for %s\n", endpoint);
  else {
    fputs("{\"ok\":true,\"command\":\"auth.delete\",\"endpoint\":", stdout);
    fc_cli_print_json_string(stdout, endpoint);
    puts("}");
  }
  return 0;
}

static int do_header(const char *endpoint, int human) {
  char header[AUTH_CLI_BUF];
  int rc = 1;
  if (auth_build_header_with_env(endpoint, auth_default_env_name(endpoint),
                                 header, sizeof(header)) == 0) {
    if (header[0]) {
      if (human)
        printf(COLOR_CYAN "%s header:" COLOR_RESET " %s\n", endpoint, header);
      else {
        fputs("{\"endpoint\":", stdout); fc_cli_print_json_string(stdout, endpoint);
        fputs(",\"header\":", stdout); fc_cli_print_json_string(stdout, header);
        puts("}");
      }
    }
    rc = 0;
  } else {
    fprintf(stderr, "auth: cannot build header for %s\n", endpoint);
  }
  secret_store_zero(header, sizeof(header));
  return rc;
}

int rt_auth_main(int argc, char **argv) {
  FcCliOutputMode mode;
  int first_operand = 1;
  fc_cli_output_mode_init(&mode);
  if (argc < 1) {
    usage();
    fc_cli_print_json_error("auth", "missing_subcommand", "missing auth subcommand");
    return 1;
  }
  while (first_operand < argc) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[first_operand], "fclaw auth");
    if (mode_rc < 0) {
      fc_cli_print_json_error("auth", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return 1;
    }
    if (mode_rc == 0) break;
    first_operand++;
  }
  if (strcmp(argv[0], "set") == 0) {
    if (argc - first_operand != 2) { usage(); return 1; }
    return do_set(argv[first_operand], argv[first_operand + 1], mode.human);
  }
  if (strcmp(argv[0], "set-stdin") == 0) {
    if (argc - first_operand != 1) { usage(); return 1; }
    return do_set_stdin(argv[first_operand], mode.human);
  }
  if (strcmp(argv[0], "get") == 0) {
    if (argc - first_operand != 1) { usage(); return 1; }
    return do_get(argv[first_operand], mode.human);
  }
  if (strcmp(argv[0], "delete") == 0) {
    if (argc - first_operand != 1) { usage(); return 1; }
    return do_delete(argv[first_operand], mode.human);
  }
  if (strcmp(argv[0], "header") == 0) {
    if (argc - first_operand != 1) { usage(); return 1; }
    return do_header(argv[first_operand], mode.human);
  }
  usage();
  return 1;
}
