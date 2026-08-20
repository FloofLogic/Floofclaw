/* `fclaw operation ...` — the action-facing managed-operation helper.
 *
 *   complete — publish a completion claim for a detached managed
 *              operation through the ordinary bus: durable inbox
 *              envelope first, then the disposable wake knock. Works
 *              with the gateway stopped; duplicate invocations are safe
 *              (idempotent admission + the terminal gate absorb them).
 *
 * The helper accepts ONLY the operation id, the completion token, and a
 * bounded result. It never accepts caller-authored routing, correlation,
 * event ids, run ids, request ids, task revisions, or timestamps — the
 * runtime stamps all of those from its durable operation record at
 * intake. */

#include "runtime.h"

#include "support/cli_output.h"
#include "support/heap_guard.h"
#include "support/json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void operation_usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw operation complete [-a|-h] "
          "[--operation-id <id>] [--token <token>] "
          "(--result-file <json> | --status succeeded|failed [--text <text>])\n"
          "\n"
          "--operation-id and --token default to FCLAW_OPERATION_ID and\n"
          "FCLAW_COMPLETION_TOKEN from the trusted launch environment.\n");
}

static int enter_runtime_root(void) {
  const char *root = getenv("FCLAW_ACTION_ROOT");
  if (!root || !*root) return 0;
  if (chdir(root) == 0) return 0;
  fprintf(stderr, "fclaw operation: cannot enter runtime root %s: %s\n",
          root, strerror(errno));
  return -1;
}

static int operation_complete_main(int argc, char **argv) {
  const char *operation_id = getenv("FCLAW_OPERATION_ID");
  const char *token = getenv("FCLAW_COMPLETION_TOKEN");
  const char *result_file = NULL;
  const char *status = NULL;
  const char *text = "";
  char *file_text = NULL;
  char parsed_status[RT_SMALL] = "";
  char *parsed_text = NULL;
  FcCliOutputMode mode;
  int rc;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw operation complete");
    if (mode_rc < 0) {
      fc_cli_print_json_error("operation.complete", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return 2;
    }
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--operation-id") == 0 && i + 1 < argc) {
      operation_id = argv[++i];
    } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
      token = argv[++i];
    } else if (strcmp(argv[i], "--result-file") == 0 && i + 1 < argc) {
      result_file = argv[++i];
    } else if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) {
      status = argv[++i];
    } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
      text = argv[++i];
    } else {
      operation_usage();
      if (!mode.human)
        fc_cli_print_json_error("operation.complete", "invalid_arguments",
                                "invalid operation complete arguments");
      return 2;
    }
  }
  if (!operation_id || !*operation_id || !token || !*token) {
    operation_usage();
    if (!mode.human)
      fc_cli_print_json_error(
          "operation.complete", "missing_capability",
          "operation id and completion token are required (flags or "
          "FCLAW_OPERATION_ID/FCLAW_COMPLETION_TOKEN)");
    return 2;
  }
  if (result_file) {
    JsonRef root, value;
    if (fs_read_text(result_file, &file_text, FS_READ_TEXT_DEFAULT_CAP) != 0 ||
        !file_text || json_ref_first_object(file_text, &root) != 0) {
      fprintf(stderr, "fclaw operation complete: unreadable result file %s\n",
              result_file);
      if (!mode.human)
        fc_cli_print_json_error("operation.complete", "result_unreadable",
                                "result file is missing or not a JSON object");
      fc_xfree(file_text);
      return 1;
    }
    if (json_ref_object_get_string(&root, "status", parsed_status,
                                   sizeof(parsed_status)) != 0)
      parsed_status[0] = '\0';
    if (json_ref_object_get(&root, "text", &value) == 0 &&
        value.type == JSON_REF_STRING) {
      size_t cap = (size_t)(value.end - value.start) + 1U;
      parsed_text = (char *)fc_xmalloc(cap);
      if (parsed_text && json_ref_string_copy(&value, parsed_text, cap) != 0)
        parsed_text[cap - 1U] = '\0';
    }
    status = parsed_status;
    if (parsed_text) text = parsed_text;
  }
  if (!status ||
      (strcmp(status, "succeeded") != 0 && strcmp(status, "failed") != 0)) {
    fprintf(stderr,
            "fclaw operation complete: result status must be succeeded or "
            "failed\n");
    if (!mode.human)
      fc_cli_print_json_error("operation.complete", "invalid_status",
                              "result status must be succeeded or failed");
    fc_xfree(parsed_text);
    fc_xfree(file_text);
    return 1;
  }
  if (strlen(text) > RT_OPERATION_RESULT_TEXT_MAX) {
    fprintf(stderr,
            "fclaw operation complete: result text exceeds %d bytes; write "
            "large payloads to disk and return a bounded pointer\n",
            RT_OPERATION_RESULT_TEXT_MAX);
    if (!mode.human)
      fc_cli_print_json_error("operation.complete", "result_payload_too_large",
                              "result text exceeds the bounded result cap");
    fc_xfree(parsed_text);
    fc_xfree(file_text);
    return 1;
  }
  rc = rt_operation_publish_worker_claim(operation_id, token, status, text);
  fc_xfree(parsed_text);
  fc_xfree(file_text);
  if (rc != 0) {
    fprintf(stderr, "fclaw operation complete: claim publication failed\n");
    if (!mode.human)
      fc_cli_print_json_error("operation.complete", "publish_failed",
                              "completion claim could not be published");
    return 1;
  }
  if (mode.human) {
    printf("completion claim published for %s (%s)\n", operation_id, status);
  } else {
    fputs("{\"ok\":true,\"command\":\"operation.complete\","
          "\"operation_id\":", stdout);
    fc_cli_print_json_string(stdout, operation_id);
    fputs(",\"status\":", stdout);
    fc_cli_print_json_string(stdout, status);
    puts("}");
  }
  return 0;
}

int rt_operation_cli_main(int argc, char **argv) {
  if (enter_runtime_root() != 0) return 1;
  if (argc < 1) {
    operation_usage();
    return 2;
  }
  if (strcmp(argv[0], "complete") == 0)
    return operation_complete_main(argc - 1, argv + 1);
  operation_usage();
  return 2;
}
