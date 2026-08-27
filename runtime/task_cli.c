#include "runtime.h"

#include "support/cli_output.h"
#include "support/color.h"

#include <stdio.h>
#include <string.h>

static void task_usage(void) {
  fprintf(stderr, "usage: fclaw task cancel [-a|-h] <task_id>\n");
}

static int task_cancel_main(int argc, char **argv) {
  const char *task_id = NULL;
  char current_status[RT_SMALL] = "";
  FcCliOutputMode mode;
  int rc;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc =
        fc_cli_output_mode_arg(&mode, argv[i], "fclaw task cancel");
    if (mode_rc < 0) {
      if (!mode.human)
        fc_cli_print_json_error("task.cancel", "output_mode_conflict",
                                "choose exactly one output mode: -a or -h");
      return 2;
    }
    if (mode_rc > 0) continue;
    if (!task_id && argv[i][0] != '-') task_id = argv[i];
    else {
      task_usage();
      if (!mode.human)
        fc_cli_print_json_error(
            "task.cancel", "invalid_arguments",
            "usage: fclaw task cancel [-a|-h] <task_id>");
      return 2;
    }
  }
  if (!task_id) {
    task_usage();
    if (!mode.human)
      fc_cli_print_json_error(
          "task.cancel", "invalid_arguments",
          "usage: fclaw task cancel [-a|-h] <task_id>");
    return 2;
  }

  rc = rt_task_cancel_operator(task_id, current_status,
                               sizeof(current_status));
  if (rc == RT_TASK_CANCEL_NOT_FOUND) {
    if (mode.human)
      fprintf(stderr, COLOR_RED "task not found:" COLOR_RESET " %s\n",
              task_id);
    else
      fc_cli_print_json_error("task.cancel", "task_not_found",
                              "task does not exist in the active task store");
    return 1;
  }
  if (rc == RT_TASK_CANCEL_NOT_OPEN) {
    char message[RT_MED];
    snprintf(message, sizeof(message),
             "task is not open (current status: %s)",
             current_status[0] ? current_status : "unknown");
    if (mode.human)
      fprintf(stderr, COLOR_RED "task is not open:" COLOR_RESET " %s (%s)\n",
              task_id, current_status[0] ? current_status : "unknown");
    else
      fc_cli_print_json_error("task.cancel", "task_not_open", message);
    return 1;
  }
  if (rc != RT_TASK_CANCEL_OK) {
    if (mode.human)
      fprintf(stderr, COLOR_RED "task cancellation failed:" COLOR_RESET
                      " %s\n",
              task_id);
    else
      fc_cli_print_json_error("task.cancel", "task_cancel_failed",
                              "task cancellation could not be persisted");
    return 1;
  }

  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "%s" COLOR_RESET " -> canceled\n", task_id);
  else {
    fputs("{\"ok\":true,\"command\":\"task.cancel\",\"task_id\":", stdout);
    fc_cli_print_json_string(stdout, task_id);
    puts(",\"status\":\"canceled\"}");
  }
  return 0;
}

int rt_task_main(int argc, char **argv) {
  if (argc >= 1 && strcmp(argv[0], "cancel") == 0)
    return task_cancel_main(argc - 1, argv + 1);
  task_usage();
  fc_cli_print_json_error("task", "invalid_arguments",
                          "usage: fclaw task cancel [-a|-h] <task_id>");
  return 2;
}
