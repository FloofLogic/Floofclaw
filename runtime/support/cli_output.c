#include "cli_output.h"

#include <string.h>

void fc_cli_output_mode_init(FcCliOutputMode *mode) {
  if (!mode) return;
  mode->human = 0;
  mode->seen = 0;
}

int fc_cli_output_mode_arg(FcCliOutputMode *mode, const char *arg,
                           const char *command) {
  int human;
  if (!mode || !arg) return 0;
  if (strcmp(arg, "-a") == 0) human = 0;
  else if (strcmp(arg, "-h") == 0) human = 1;
  else return 0;
  if (mode->seen && mode->human != human) {
    fprintf(stderr, "%s: choose exactly one output mode: -a or -h\n",
            command ? command : "fclaw");
    return -1;
  }
  mode->human = human;
  mode->seen = 1;
  return 1;
}

void fc_cli_print_json_string_n(FILE *out, const char *text, size_t len) {
  if (!out) return;
  fputc('"', out);
  for (size_t i = 0; text && i < len; ++i) {
    unsigned char c = (unsigned char)text[i];
    switch (c) {
      case '"': fputs("\\\"", out); break;
      case '\\': fputs("\\\\", out); break;
      case '\b': fputs("\\b", out); break;
      case '\f': fputs("\\f", out); break;
      case '\n': fputs("\\n", out); break;
      case '\r': fputs("\\r", out); break;
      case '\t': fputs("\\t", out); break;
      default:
        if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc((int)c, out);
    }
  }
  fputc('"', out);
}

void fc_cli_print_json_string(FILE *out, const char *text) {
  fc_cli_print_json_string_n(out, text ? text : "",
                             strlen(text ? text : ""));
}

void fc_cli_print_json_error(const char *command, const char *code,
                             const char *message) {
  fputs("{\"ok\":false,\"command\":", stdout);
  fc_cli_print_json_string(stdout, command ? command : "fclaw");
  fputs(",\"error\":{\"code\":", stdout);
  fc_cli_print_json_string(stdout, code ? code : "command_failed");
  fputs(",\"message\":", stdout);
  fc_cli_print_json_string(stdout, message ? message : "command failed");
  fputs("}}\n", stdout);
}
