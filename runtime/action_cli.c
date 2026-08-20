#include "runtime.h"

#include "bus/bus.h"
#include "support/fsutil.h"
#include "support/json.h"
#include "support/heap_guard.h"
#include "support/cli_output.h"
#include "support/color.h"
#include "support/timing.h"
#include "secure/action_secret_broker.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define ACTION_CLI_CHANNEL "action_cli"
#define ACTION_CLI_SOURCE  "action_cli"
#define ACTION_CLI_TIMEOUT_MS 180000LL
#define ACTION_CLI_POLL_US 50000

static void action_usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw action list [-a|-h]\n"
          "  fclaw action exec [-a|-h] --name <action> --args '<json-object>'\n"
          "  fclaw action auth [-a|-h] --name <action> -- <action-owned arguments>\n");
}

static int enter_runtime_root(void) {
  const char *root = getenv("FCLAW_ACTION_ROOT");
  if (!root || !*root) return 0;
  if (chdir(root) == 0) return 0;
  fprintf(stderr, "fclaw action: cannot enter runtime root %s: %s\n",
          root, strerror(errno));
  return -1;
}

static int action_list_main(int argc, char **argv) {
  RtActionRegistry *registry;
  char *catalog;
  char *compact = NULL;
  char err[RT_LARGE] = "";
  int rc = 1;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw action list");
    if (mode_rc <= 0) {
      action_usage();
      if (!mode.human)
        fc_cli_print_json_error("action.list", "invalid_arguments",
                                "usage: fclaw action list [-a|-h]");
      return 2;
    }
  }
  registry = (RtActionRegistry *)fc_xcalloc(1U, sizeof(*registry));
  catalog = (char *)fc_xmalloc(RT_XL);
  if (!registry || !catalog) {
    fprintf(stderr, "fclaw action list: allocation failed\n");
    goto done;
  }
  if (rt_action_registry_load(registry, err, sizeof(err)) != 0) {
    fprintf(stderr, "fclaw action list: %s\n",
            err[0] ? err : "registry load failed");
    goto done;
  }
  /* Operator listing: the whole registry, config force_disable stated as a
   * per-entry fact. Model-facing catalogs omit masked actions instead. */
  if (rt_action_render_registry(registry, catalog, RT_XL) != 0) {
    fprintf(stderr, "fclaw action list: rendered catalog is too large\n");
    goto done;
  }
  if (mode.human) {
    printf(COLOR_CYAN "available actions" COLOR_RESET "\n%s\n", catalog);
  } else {
    compact = (char *)fc_xmalloc(RT_XL);
    if (!compact || json_compact(catalog, compact, RT_XL) != 0) {
      fprintf(stderr, "fclaw action list: catalog compaction failed\n");
      fc_cli_print_json_error("action.list", "catalog_compaction_failed",
                              "catalog compaction failed");
      goto done;
    }
    printf("%s\n", compact);
  }
  rc = 0;

done:
  fc_xfree(compact);
  fc_xfree(catalog);
  fc_xfree(registry);
  return rc;
}

static int parse_exec_args(int argc, char **argv,
                           const char **name, const char **args,
                           FcCliOutputMode *mode) {
  *name = NULL;
  *args = "{}";
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(mode, argv[i], "fclaw action exec");
    if (mode_rc < 0) return -1;
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--name") == 0) {
      if (++i >= argc || argv[i][0] == '-') return -1;
      *name = argv[i];
    } else if (strcmp(argv[i], "--args") == 0) {
      if (++i >= argc) return -1;
      *args = argv[i];
    } else {
      return -1;
    }
  }
  return *name && **name ? 0 : -1;
}

static int publish_action_exec(const char *name, const char *args,
                               char *origin_id, size_t origin_len) {
  JsonRef root;
  char compact_args[RT_LARGE];
  char ename[RT_MED], eorigin[RT_MED], etask[RT_MED] = "";
  char payload[RT_XL];
  const char *bound_task_id = getenv("FCLAW_BOUND_TASK_ID");
  if (json_ref_top_object(args, &root) != 0 ||
      json_compact(args, compact_args, sizeof(compact_args)) != 0) {
    fprintf(stderr, "fclaw action exec: --args must be a JSON object\n");
    return -1;
  }
  if (bus_reserve_envelope_id(origin_id, origin_len) != 0 ||
      json_escape(name, ename, sizeof(ename)) != 0 ||
      json_escape(origin_id, eorigin, sizeof(eorigin)) != 0 ||
      (bound_task_id && *bound_task_id &&
       json_escape(bound_task_id, etask, sizeof(etask)) != 0) ||
      snprintf(payload, sizeof(payload),
               "{\"action\":\"%s\",\"args\":%s,"
               "\"origin_event_id\":\"%s\"%s%s%s}",
               ename, compact_args, eorigin,
               etask[0] ? ",\"task_id\":\"" : "",
               etask[0] ? etask : "",
               etask[0] ? "\"" : "") >= (int)sizeof(payload) ||
      bus_publish_stable(origin_id, ACTION_CLI_CHANNEL, "action_exec",
                         payload) != 0) {
    fprintf(stderr, "fclaw action exec: could not publish action request\n");
    return -1;
  }
  return 0;
}

static int run_dir_for_origin(const char *origin_id,
                              char *out, size_t out_len) {
  DIR *dir;
  struct dirent *entry;
  int saved_errno = 0;
  dir = opendir("workspace/runs");
  if (!dir) return errno == ENOENT ? 1 : -1;
  errno = 0;
  while ((entry = fs_readdir_checked(dir)) != NULL) {
    char path[PATH_MAX];
    char *text = NULL;
    JsonRef state, created;
    char event_id[RT_SMALL] = "", type[RT_SMALL] = "";
    if (strncmp(entry->d_name, "run_", 4) != 0) continue;
    if (snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json",
                 entry->d_name) >= (int)sizeof(path))
      continue;
    if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
      continue;
    if (json_ref_top_object(text, &state) == 0 &&
        json_ref_object_get_object(&state, "created_from", &created) == 0 &&
        json_ref_object_get_string(&created, "event_id", event_id,
                                   sizeof(event_id)) == 0 &&
        json_ref_object_get_string(&created, "type", type,
                                   sizeof(type)) == 0 &&
        strcmp(event_id, origin_id) == 0 &&
        strcmp(type, "action_exec") == 0) {
      fc_xfree(text);
      if (snprintf(out, out_len, "workspace/runs/%s",
                   entry->d_name) >= (int)out_len) {
        (void)closedir(dir);
        return -1;
      }
      (void)closedir(dir);
      return 0;
    }
    fc_xfree(text);
  }
  saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  return saved_errno == 0 ? 1 : -1;
}

static int compact_ref(const JsonRef *value, char *out, size_t out_len) {
  char raw[RT_XL];
  if (!value || json_ref_value_copy(value, raw, sizeof(raw)) != 0 ||
      json_compact(raw, out, out_len) != 0)
    return -1;
  return 0;
}

/* 0 success terminal, 2 failure/rejection terminal, 1 not terminal yet,
 * -1 unreadable/malformed artifact. */
static int read_action_terminal(const char *run_dir, const char *name,
                                char *out, size_t out_len) {
  char path[PATH_MAX];
  char *text = NULL;
  char *line;
  int result = 1;
  if (snprintf(path, sizeof(path), "%s/event_log.jsonl", run_dir) >=
      (int)sizeof(path))
    return -1;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? 1 : -1;
  line = text;
  while (*line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload, value;
    char type[RT_SMALL] = "", action[RT_SMALL] = "";
    if (next) *next = '\0';
    if (json_ref_top_object(line, &event) == 0 &&
        json_ref_object_get_string(&event, "type", type,
                                   sizeof(type)) == 0 &&
        json_ref_object_get_object(&event, "payload", &payload) == 0) {
      if ((strcmp(type, "action_succeeded") == 0 ||
           strcmp(type, "action_failed") == 0 ||
           strcmp(type, "action_rejected") == 0) &&
          json_ref_object_get_string(&payload, "action", action,
                                     sizeof(action)) == 0 &&
          strcmp(action, name) == 0) {
        if (strcmp(type, "action_succeeded") == 0) {
          if (json_ref_object_get(&payload, "result", &value) != 0 ||
              compact_ref(&value, out, out_len) != 0)
            snprintf(out, out_len, "{}");
          result = 0;
        } else if (json_ref_object_get(&payload, "error", &value) == 0 &&
                   compact_ref(&value, out, out_len) == 0) {
          result = 2;
        } else if (compact_ref(&payload, out, out_len) == 0) {
          result = 2;
        } else {
          snprintf(out, out_len,
                   "{\"message\":\"action failed without readable error\"}");
          result = 2;
        }
        break;
      }
      if (strcmp(type, "run_failed") == 0) {
        if (compact_ref(&payload, out, out_len) != 0)
          snprintf(out, out_len, "{\"message\":\"action run failed\"}");
        result = 2;
        break;
      }
    }
    if (!next) break;
    line = next + 1;
  }
  fc_xfree(text);
  return result;
}

static long long action_timeout_ms(void) {
  const char *configured = getenv("FCLAW_ACTION_EXEC_TIMEOUT_MS");
  char *end = NULL;
  long long value;
  if (!configured || !*configured) return ACTION_CLI_TIMEOUT_MS;
  errno = 0;
  value = strtoll(configured, &end, 10);
  if (errno != 0 || end == configured || *end != '\0' ||
      value < 100 || value > ACTION_CLI_TIMEOUT_MS)
    return ACTION_CLI_TIMEOUT_MS;
  return value;
}

static void read_captured_stream(FILE *stream, char *out, size_t out_len,
                                 int *truncated) {
  size_t used = 0;
  if (truncated) *truncated = 0;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!stream || fseek(stream, 0, SEEK_SET) != 0) return;
  used = fread(out, 1, out_len - 1, stream);
  out[used] = '\0';
  if (truncated && used == out_len - 1 && fgetc(stream) != EOF)
    *truncated = 1;
}

static void print_action_auth_result(const char *name, int rc,
                                     FILE *captured_stdout,
                                     FILE *captured_stderr,
                                     const char *failure) {
  char *out = (char *)fc_xmalloc(RT_XL);
  char *err = (char *)fc_xmalloc(RT_XL);
  int out_truncated = 0, err_truncated = 0;
  if (!out || !err) {
    fc_xfree(out);
    fc_xfree(err);
    fc_cli_print_json_error("action.auth", "allocation_failed",
                            "could not render operator output");
    return;
  }
  read_captured_stream(captured_stdout, out, RT_XL, &out_truncated);
  read_captured_stream(captured_stderr, err, RT_XL, &err_truncated);
  printf("{\"ok\":%s,\"command\":\"action.auth\",\"action\":",
         rc == 0 ? "true" : "false");
  fc_cli_print_json_string(stdout, name ? name : "");
  printf(",\"exit_code\":%d,\"stdout\":", rc);
  fc_cli_print_json_string(stdout, out);
  fputs(",\"stderr\":", stdout);
  fc_cli_print_json_string(stdout, err);
  printf(",\"truncated\":%s", out_truncated || err_truncated ? "true" : "false");
  if (failure && *failure) {
    fputs(",\"error\":{\"code\":\"operator_auth_failed\",\"message\":",
          stdout);
    fc_cli_print_json_string(stdout, failure);
    putchar('}');
  }
  puts("}");
  fc_xfree(out);
  fc_xfree(err);
}

static int wait_action_exec(const char *origin_id, const char *name,
                            int human) {
  char run_dir[PATH_MAX] = "";
  char terminal[RT_XL] = "";
  long long deadline =
      (long long)timing_monotonic_ms() + action_timeout_ms();
  while ((long long)timing_monotonic_ms() < deadline) {
    int rc;
    if (!run_dir[0]) {
      rc = run_dir_for_origin(origin_id, run_dir, sizeof(run_dir));
      if (rc < 0) {
        fprintf(stderr, "fclaw action exec: could not inspect runtime runs\n");
        return 1;
      }
    }
    if (run_dir[0]) {
      rc = read_action_terminal(run_dir, name, terminal, sizeof(terminal));
      if (rc == 0 || rc == 2) {
        if (human)
          printf(COLOR_CYAN "action %s %s" COLOR_RESET "\n%s\n", name,
                 rc == 0 ? "succeeded" : "failed",
                 terminal[0] ? terminal : "{}");
        else
          printf("%s\n", terminal[0] ? terminal : "{}");
        return rc == 0 ? 0 : 1;
      }
      if (rc < 0) {
        fprintf(stderr, "fclaw action exec: terminal artifact is unreadable\n");
        return 1;
      }
    }
    usleep(ACTION_CLI_POLL_US);
  }
  fprintf(stderr,
          "fclaw action exec: timed out waiting for runtime owner "
          "(origin_event_id=%s)\n", origin_id);
  return 1;
}

static int action_exec_main(int argc, char **argv) {
  const char *name, *args;
  char origin_id[BUS_ID_MAX] = "";
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  if (parse_exec_args(argc, argv, &name, &args, &mode) != 0) {
    action_usage();
    if (!mode.human)
      fc_cli_print_json_error("action.exec", "invalid_arguments",
                              "invalid action exec arguments");
    return 2;
  }
  if (publish_action_exec(name, args, origin_id, sizeof(origin_id)) != 0)
    return 1;
  return wait_action_exec(origin_id, name, mode.human);
}

static int action_auth_main(int argc, char **argv) {
  RtActionRegistry *registry = NULL;
  const RtActionDef *def;
  const char *name = NULL;
  char err[RT_LARGE] = "";
  char exec_path[PATH_MAX];
  char **child_argv = NULL;
  int separator = -1;
  int broker_pair[2] = {-1, -1};
  ActionSecretBroker broker;
  pid_t pid;
  int status = 0, exited = 0, rc = 1;
  int child_started = 0;
  const char *failure = "operator authentication failed";
  FILE *captured_stdout = NULL, *captured_stderr = NULL;
  FcCliOutputMode mode;

  memset(&broker, 0, sizeof(broker));
  broker.fd = -1;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw action auth");
    if (mode_rc < 0) {
      fc_cli_print_json_error("action.auth", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return 2;
    }
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--name") == 0 && i + 1 < argc && !name) {
      name = argv[++i];
    } else if (strcmp(argv[i], "--") == 0) {
      separator = i;
      break;
    } else {
      action_usage();
      return 2;
    }
  }
  if (!name || !*name || separator < 0 || separator + 1 >= argc) {
    action_usage();
    if (!mode.human)
      fc_cli_print_json_error("action.auth", "invalid_arguments",
                              "action auth requires --name and operator arguments");
    return 2;
  }
  registry = (RtActionRegistry *)fc_xcalloc(1U, sizeof(*registry));
  if (!registry || rt_action_registry_load(registry, err, sizeof(err)) != 0) {
    failure = err[0] ? err : "registry load failed";
    fprintf(stderr, "fclaw action auth: %s\n",
            failure);
    goto done;
  }
  def = rt_action_registry_find(registry, name);
  if (!def) {
    failure = "unknown action";
    fprintf(stderr, "fclaw action auth: unknown action %s\n", name);
    goto done;
  }
  if (!def->has_operator_exec || !def->private_credentials) {
    failure = "action has no operator auth entry point";
    fprintf(stderr,
            "fclaw action auth: action %s has no operator auth entry point\n",
            name);
    goto done;
  }
  if (snprintf(exec_path, sizeof(exec_path), "%s/%s", def->dir,
               def->operator_arg) >= (int)sizeof(exec_path) ||
      access(exec_path, R_OK) != 0) {
    failure = "operator executable is missing";
    fprintf(stderr, "fclaw action auth: action %s operator executable is missing\n",
            name);
    goto done;
  }
  child_argv = (char **)fc_xcalloc((size_t)(argc - separator + 2),
                                   sizeof(*child_argv));
  if (!child_argv) goto done;
  child_argv[0] = (char *)def->operator_argv0;
  child_argv[1] = exec_path;
  for (int i = separator + 1; i < argc; ++i)
    child_argv[i - separator + 1] = argv[i];

  if (action_secret_broker_pair(broker_pair) != 0) {
    failure = "could not create private credential channel";
    fprintf(stderr, "fclaw action auth: could not create private credential channel\n");
    goto done;
  }
  if (!mode.human) {
    captured_stdout = tmpfile();
    captured_stderr = tmpfile();
    if (!captured_stdout || !captured_stderr) {
      failure = "could not create operator output capture";
      fprintf(stderr, "fclaw action auth: could not create operator output capture\n");
      goto done;
    }
  }
  if (mode.human)
    printf(COLOR_CYAN "action auth:" COLOR_RESET " %s\n", name);
  pid = fork();
  if (pid < 0) {
    failure = "could not start operator command";
    fprintf(stderr, "fclaw action auth: could not start operator command\n");
    goto done;
  }
  if (pid == 0) {
    char fd_text[32];
    close(broker_pair[0]);
    if (!mode.human) {
      if (dup2(fileno(captured_stdout), STDOUT_FILENO) < 0 ||
          dup2(fileno(captured_stderr), STDERR_FILENO) < 0)
        _exit(126);
    }
    snprintf(fd_text, sizeof(fd_text), "%d", broker_pair[1]);
    (void)setenv("FCLAW_SECRET_FD", fd_text, 1);
    execvp(child_argv[0], child_argv);
    _exit(127);
  }
  child_started = 1;
  close(broker_pair[1]);
  broker_pair[1] = -1;
  if (action_secret_broker_init(&broker, broker_pair[0], name) != 0) {
    broker_pair[0] = -1;
    (void)kill(pid, SIGTERM);
    (void)waitpid(pid, &status, 0);
    failure = "credential broker initialization failed";
    fprintf(stderr, "fclaw action auth: credential broker initialization failed\n");
    goto done;
  }
  broker_pair[0] = -1;

  while (!exited || !broker.closed) {
    struct pollfd pfd;
    int poll_rc;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = broker.fd;
    pfd.events = action_secret_broker_events(&broker);
    poll_rc = broker.closed ? 0 : poll(&pfd, 1, 100);
    if (poll_rc > 0)
      (void)action_secret_broker_on_events(&broker, pfd.revents);
    else if (poll_rc < 0 && errno != EINTR)
      action_secret_broker_close(&broker);
    if (!exited) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) exited = 1;
      else if (waited < 0 && errno != EINTR) {
        exited = 1;
        status = 1 << 8;
      }
    }
    if (exited && !broker.closed)
      (void)action_secret_broker_on_events(&broker, POLLIN | POLLHUP);
  }
  rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  failure = rc == 0 ? NULL : "operator command failed";

done:
  if (!mode.human) {
    if (child_started) {
      print_action_auth_result(name, rc, captured_stdout, captured_stderr,
                               failure);
    } else {
      fputs("{\"ok\":false,\"command\":\"action.auth\",\"action\":",
            stdout);
      fc_cli_print_json_string(stdout, name ? name : "");
      fputs(",\"exit_code\":null,\"error\":{\"code\":\"operator_auth_failed\",\"message\":",
            stdout);
      fc_cli_print_json_string(stdout, failure);
      puts("}}");
    }
  }
  if (broker.fd >= 0) action_secret_broker_close(&broker);
  if (broker_pair[0] >= 0) close(broker_pair[0]);
  if (broker_pair[1] >= 0) close(broker_pair[1]);
  if (captured_stdout) fclose(captured_stdout);
  if (captured_stderr) fclose(captured_stderr);
  fc_xfree(child_argv);
  fc_xfree(registry);
  return rc;
}

int rt_action_main(int argc, char **argv) {
  if (enter_runtime_root() != 0) return 1;
  if (argc < 1) {
    action_usage();
    return 2;
  }
  if (strcmp(argv[0], "list") == 0)
    return action_list_main(argc - 1, argv + 1);
  if (strcmp(argv[0], "exec") == 0)
    return action_exec_main(argc - 1, argv + 1);
  if (strcmp(argv[0], "auth") == 0)
    return action_auth_main(argc - 1, argv + 1);
  action_usage();
  return 2;
}
