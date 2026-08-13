#ifndef FCLAW_SUPPORT_CLI_OUTPUT_H
#define FCLAW_SUPPORT_CLI_OUTPUT_H

#include <stdio.h>

typedef struct {
  int human;
  int seen;
} FcCliOutputMode;

void fc_cli_output_mode_init(FcCliOutputMode *mode);

/* Return 1 when arg is -a or -h, 0 when it is not an output-mode flag, and
 * -1 when both modes were requested. The default is compact agent mode. */
int fc_cli_output_mode_arg(FcCliOutputMode *mode, const char *arg,
                           const char *command);

void fc_cli_print_json_string(FILE *out, const char *text);
void fc_cli_print_json_string_n(FILE *out, const char *text, size_t len);
void fc_cli_print_json_error(const char *command, const char *code,
                             const char *message);

#endif
