#include "runtime.h"
#include "channel/channel.h"
#include "support/cli_output.h"
#include "support/color.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <pthread.h>
#elif __linux__
#include <sys/prctl.h>
#endif

static char g_exe_path[PATH_MAX] = "./bin/fclaw";

static void resolve_exe_path(const char *argv0) {
  char buf[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    char resolved[PATH_MAX];
    if (realpath(buf, resolved)) {
      snprintf(g_exe_path, sizeof(g_exe_path), "%s", resolved);
      return;
    }
  }
#elif __linux__
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    snprintf(g_exe_path, sizeof(g_exe_path), "%s", buf);
    return;
  }
#endif
  if (argv0 && argv0[0]) {
    char resolved[PATH_MAX];
    if (realpath(argv0, resolved)) {
      snprintf(g_exe_path, sizeof(g_exe_path), "%s", resolved);
      return;
    }
    snprintf(g_exe_path, sizeof(g_exe_path), "%s", argv0);
  }
}

const char *rt_exe_path(void) { return g_exe_path; }

static int label_char(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int gateway_label(char *out, size_t out_len) {
  char *text = NULL;
  JsonRef root;
  char bot[RT_SMALL];
  size_t pos = 0;
  if (!out || out_len < 8) return -1;
  out[0] = '\0';
  if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  if (json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_string(&root, "bot_name", bot, sizeof(bot)) != 0 ||
      !bot[0]) {
    free(text);
    return -1;
  }
  free(text);
  pos = (size_t)snprintf(out, out_len, "fclaw_");
  for (const char *p = bot; *p && pos + 1 < out_len; ++p)
    out[pos++] = (char)(label_char((unsigned char)*p) ? *p : '_');
  out[pos] = '\0';
  return pos > strlen("fclaw_") ? 0 : -1;
}

static void maybe_label_gateway_process(int argc, char **argv) {
  char label[RT_SMALL];
  size_t arg0_len, label_len;
  if (argc < 3 || strcmp(argv[1], "gateway") != 0 ||
      (strcmp(argv[2], "start") != 0 && strcmp(argv[2], "run") != 0) ||
      gateway_label(label, sizeof(label)) != 0)
    return;
#ifdef __APPLE__
  (void)pthread_setname_np(label);
#elif __linux__
  (void)prctl(PR_SET_NAME, label, 0, 0, 0);
#endif
  arg0_len = strlen(argv[0]);
  label_len = strlen(label);
  if (label_len <= arg0_len) {
    memset(argv[0], 0, arg0_len);
    memcpy(argv[0], label, label_len);
  }
}

static void usage(int human) {
  if (!human) {
    fputs("{\"ok\":false,\"command\":\"fclaw\",\"error\":{"
          "\"code\":\"usage\",\"message\":\"a command is required\"},"
          "\"commands\":[\"run\",\"replay\",\"view\",\"usage\","
          "\"cacheview\",\"auth\",\"bus\",\"affair\",\"gateway\","
          "\"channel\",\"clear\",\"actions\",\"adapters\",\"floops\","
          "\"action\",\"setup\",\"--version\"]}\n", stdout);
    return;
  }
  fprintf(stderr,
          COLOR_CYAN "usage:" COLOR_RESET "\n"
          "  fclaw run [-a|-h] [--floop <name>] [--text] TEXT\n"
          "  fclaw replay [-a|-h] workspace/runs/<run_id>/event_log.jsonl\n"
          "  fclaw view [-a|-h] [-f] [-d] <run_id|run_dir>\n"
          "  fclaw usage [-a|-h] [--agent <id>] [--profile <id>]\n"
          "  fclaw cacheview [-a|-h] --agent <id> [-n count] "
          "[--run <run_id> [--call <seq>] | --list]\n"
          "  fclaw auth set|set-stdin|get|delete|header [-a|-h] <endpoint> [...]\n"
          "  fclaw bus publish|log [-a|-h] [...]\n"
          "  fclaw affair create|list|pause|resume|close|review|schedule [-a|-h] [...]\n"
          "  fclaw gateway start|run|status|stop|reload [-a|-h] [...]\n"
          "  fclaw -i [-a|-h] [--floop <name>]\n"
          "  fclaw channel cli|ws|irc [-a|-h] [--floop <name>]\n"
          "  fclaw clear [-a|-h] [--yes]\n"
          "  fclaw actions|adapters|floops [-a|-h]\n"
          "  fclaw action list [-a|-h]\n"
          "  fclaw action exec [-a|-h] --name <action> --args '<json-object>'\n"
          "  fclaw action auth [-a|-h] --name <action> -- <action-owned arguments>\n"
          "  fclaw setup [-a|-h]\n"
          "  fclaw --version|-v [-a|-h]\n"
          "\n"
          "Output defaults to -a (compact JSON/JSONL). Use -h for colored human output.\n");
}

static int loop_flag_error(void) {
  fprintf(stderr, "unknown flag --loop. Did you mean --floop? Run `fclaw` with no arguments for usage.\n");
  return 1;
}

static const char *arg_value(int *i, int argc, char **argv) {
  if (*i + 1 >= argc) return NULL;
  (*i)++;
  return argv[*i];
}

int main(int argc, char **argv) {
  resolve_exe_path(argv[0]);
  maybe_label_gateway_process(argc, argv);
  if (argc < 2) {
    usage(0);
    return 1;
  }
  if ((strcmp(argv[1], "-a") == 0 || strcmp(argv[1], "-h") == 0) &&
      argc == 2) {
    usage(strcmp(argv[1], "-h") == 0);
    return 1;
  }
  if (strcmp(argv[1], "internal") == 0) {
    return rt_internal_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "run") == 0) {
    const char *loop = NULL;
    const char *text = NULL;
    char response[RT_LARGE];
    char run_id[RT_SMALL];
    FcCliOutputMode mode;
    fc_cli_output_mode_init(&mode);
    for (int i = 2; i < argc; ++i) {
      int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw run");
      if (mode_rc < 0) {
        fc_cli_print_json_error("run", "output_mode_conflict",
                                "choose exactly one output mode: -a or -h");
        return 1;
      } else if (mode_rc > 0) {
        continue;
      } else if (strcmp(argv[i], "--loop") == 0) {
        return loop_flag_error();
      } else if (strcmp(argv[i], "--floop") == 0) {
        loop = arg_value(&i, argc, argv);
        if (!loop || loop[0] == '-') {
          fprintf(stderr, "missing value for --floop; fix: --floop <name>\n");
          return 1;
        }
      } else if (strcmp(argv[i], "--text") == 0) {
        text = arg_value(&i, argc, argv);
        if (!text || text[0] == '-') {
          fprintf(stderr, "missing value for --text; fix: --text \"your message\"\n");
          return 1;
        }
      } else if (argv[i][0] != '-' && !text) {
        text = argv[i];
      } else {
        usage(mode.human);
        return 1;
      }
    }
    if (!text) {
      usage(mode.human);
      return 1;
    }
    /* Exit-status contract for `fclaw run`:
     *   0 -> run_done; result holds the reply
     *   2 -> run_failed; result holds the "Run failed: …" message
     *   1 -> no terminal delivery (timeout, lock failure, bus error) */
    {
      int rc = channel_synchronous("cli", loop, text, response, sizeof(response),
                                   run_id, sizeof(run_id));
      if (rc == CHANNEL_FLOOP_CONFLICT) return 1;
      if (rc == 1) {
        fprintf(stderr, "run failed\n");
        if (!mode.human)
          fc_cli_print_json_error("run", "no_terminal_delivery",
                                  "run failed before a terminal delivery");
        return 1;
      }
      if (mode.human) {
        printf(COLOR_CYAN "run_id:" COLOR_RESET " %s\n", run_id);
        printf(COLOR_GREEN_BRIGHT "result:" COLOR_RESET " %s\n",
               response[0] ? response : "(no message)");
      } else {
        fputs("{\"run_id\":", stdout);
        fc_cli_print_json_string(stdout, run_id);
        fputs(",\"status\":", stdout);
        fc_cli_print_json_string(stdout, rc == 0 ? "done" : "failed");
        fputs(",\"result\":", stdout);
        fc_cli_print_json_string(stdout, response[0] ? response : "");
        fputs("}\n", stdout);
      }
      return rc;
    }
  }
  if (strcmp(argv[1], "view") == 0) {
    return rt_view_main(argc - 2, argv + 2) == 0 ? 0 : 1;
  }
  if (strcmp(argv[1], "usage") == 0) {
    return rt_usage_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "cacheview") == 0) {
    return rt_cacheview_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "clear") == 0) {
    return rt_clear_main(argc - 2, argv + 2);
  }
  /* "tools" is the former name of "actions"; kept working, no longer listed. */
  if (strcmp(argv[1], "actions") == 0 || strcmp(argv[1], "tools") == 0) {
    return rt_tools_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "adapters") == 0) {
    return rt_adapters_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "floops") == 0) {
    return rt_floops_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "action") == 0) {
    return rt_action_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "setup") == 0) {
    return rt_setup_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
    return rt_version_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "auth") == 0) {
    return rt_auth_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "bus") == 0) {
    return rt_bus_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "affair") == 0) {
    return rt_affair_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "gateway") == 0) {
    return rt_gateway_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "channel") == 0) {
    return rt_channel_main(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "-i") == 0) {
    return rt_chat_interactive(argc - 2, argv + 2);
  }
  if (strcmp(argv[1], "replay") == 0) {
    char *events = NULL;
    char state[RT_XL];
    const char *path = NULL;
    FcCliOutputMode mode;
    fc_cli_output_mode_init(&mode);
    for (int i = 2; i < argc; ++i) {
      int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw replay");
      if (mode_rc < 0) {
        fc_cli_print_json_error("replay", "output_mode_conflict",
                                "choose exactly one output mode: -a or -h");
        return 1;
      }
      if (mode_rc > 0) continue;
      if (!path && argv[i][0] != '-') path = argv[i];
      else {
        usage(mode.human);
        return 1;
      }
    }
    if (!path) {
      usage(mode.human);
      return 1;
    }
    if (fs_read_text(path, &events, FS_READ_TEXT_DEFAULT_CAP) != 0) {
      fprintf(stderr, "failed to read event log\n");
      if (!mode.human)
        fc_cli_print_json_error("replay", "event_log_unreadable",
                                "failed to read event log");
      return 1;
    }
    if (rt_reduce_event_log_text(events, state, sizeof(state)) != 0) {
      free(events);
      fprintf(stderr, "failed to replay event log\n");
      if (!mode.human)
        fc_cli_print_json_error("replay", "replay_failed",
                                "failed to replay event log");
      return 1;
    }
    free(events);
    if (mode.human)
      printf(COLOR_CYAN "replayed state" COLOR_RESET "\n%s", state);
    else {
      char *compact = (char *)malloc(RT_XL);
      if (!compact || json_compact(state, compact, RT_XL) != 0) {
        free(compact);
        fc_cli_print_json_error("replay", "state_compaction_failed",
                                "failed to compact replayed state");
        return 1;
      }
      printf("%s\n", compact);
      free(compact);
    }
    return 0;
  }
  usage(0);
  return 1;
}
