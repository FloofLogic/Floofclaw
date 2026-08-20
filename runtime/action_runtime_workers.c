/* Runtime intrinsics for managed Codex and Claude worker processes. */

#include "action_runtime_internal.h"
#include "support/fsutil.h"
#include "support/heap_guard.h"
#include "support/json.h"
#include "support/timing.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  char op_id[RT_SMALL];
  char state[RT_SMALL];
  char permission_mode[RT_SMALL];
  char root[RT_SMALL];
  char cwd[PATH_MAX];
  char root_path[PATH_MAX];
  char launch_path[PATH_MAX];
  char task[RT_LARGE];
  char bound_task_id[RT_SMALL];
  char working_memory[RT_TASK_WORKING_MEMORY_MAX];
  char expected_path[PATH_MAX];
  char stdout_path[PATH_MAX];
  char stderr_path[PATH_MAX];
  char final_path[PATH_MAX];
  char prompt_path[PATH_MAX];
  char exit_path[PATH_MAX];
  char op_dir[PATH_MAX];
  long long pid;
  long long exit_code;
  long long last_checked_ms;
  int needs_input;
  char final_text[RT_LARGE];
  /* Completion capability, threaded IN MEMORY from the runtime's
   * preallocated pending action into the forked supervising runner.
   * Deliberately never serialized into state.json and never exposed to
   * the provider CLI's environment or prompt. */
  char fc_operation_id[RT_SMALL];
  char fc_completion_token[RT_OPERATION_TOKEN_HEX + 1];
} CodexOpState;

static int managed_worker_bind_task(RtRun *r, const char *rid,
                                    CodexOpState *st) {
  int pidx;
  if (!r || !rid || !st) return -1;
  pidx = rt_action_runner_pending_index(r, rid);
  if (pidx < 0) return 0;
  /* Completion capability from the runtime's launch-time allocation. */
  snprintf(st->fc_operation_id, sizeof(st->fc_operation_id), "%s",
           r->pending_actions[pidx].op_id);
  snprintf(st->fc_completion_token, sizeof(st->fc_completion_token), "%s",
           r->pending_actions[pidx].op_token);
  if (!r->pending_actions[pidx].task_id[0]) return 0;
  snprintf(st->bound_task_id, sizeof(st->bound_task_id), "%s",
           r->pending_actions[pidx].task_id);
  return rt_task_working_memory_of(st->bound_task_id, st->working_memory,
                                   sizeof(st->working_memory));
}

static int workspace_root_path(const RtRun *r, char *out, size_t out_len) {
  const char *runs;
  size_t n;
  if (!r || !r->ctx.run_dir[0] || !out || out_len == 0) return -1;
  runs = strstr(r->ctx.run_dir, "/runs/");
  if (!runs || runs == r->ctx.run_dir) return -1;
  n = (size_t)(runs - r->ctx.run_dir);
  if (n >= out_len) return -1;
  memcpy(out, r->ctx.run_dir, n);
  out[n] = '\0';
  return 0;
}

static int path_is_within(const char *root, const char *path) {
  size_t n;
  if (!root || !*root || !path || !*path) return 0;
  n = strlen(root);
  if (strcmp(root, "/") == 0) return path[0] == '/';
  return strcmp(root, path) == 0 ||
         (strncmp(root, path, n) == 0 && path[n] == '/');
}

static int relative_directory_valid(const char *path) {
  const char *component;
  if (!path || !*path || path[0] == '/') return 0;
  if (strcmp(path, ".") == 0) return 1;
  component = path;
  for (const char *p = path;; ++p) {
    if (*p == '/' || *p == '\0') {
      size_t n = (size_t)(p - component);
      if (n == 0 || (n == 1 && component[0] == '.') ||
          (n == 2 && component[0] == '.' && component[1] == '.'))
        return 0;
      if (*p == '\0') break;
      component = p + 1;
    }
  }
  return 1;
}

static int json_string_has_nul_escape(const JsonRef *value) {
  if (!value || value->type != JSON_REF_STRING) return 0;
  for (const char *p = value->start; p + 5 < value->end; ++p)
    if (p[0] == '\\' && p[1] == 'u' && p[2] == '0' &&
        p[3] == '0' && p[4] == '0' && p[5] == '0')
      return 1;
  return 0;
}

static int resolve_codex_launch_path(const RtRun *r, const RtActionDef *def,
                                     const char *root_kind, const char *cwd,
                                     char *root_abs, size_t root_len,
                                     char *cwd_abs, size_t cwd_len,
                                     char *error, size_t error_len) {
  char configured[PATH_MAX];
  char joined[PATH_MAX];
  struct stat st;
  const char *base;
  (void)cwd_len; /* realpath(3) writes at most PATH_MAX bytes here. */
  if (!relative_directory_valid(cwd)) {
    snprintf(error, error_len,
             "{\"message\":\"cwd must be a relative directory without empty, dot, or dot-dot components\"}");
    return -1;
  }
  if (strcmp(root_kind, "workspace") == 0) {
    if (workspace_root_path(r, configured, sizeof(configured)) != 0 ||
        !realpath(configured, root_abs)) {
      snprintf(error, error_len,
               "{\"message\":\"workspace root unavailable\"}");
      return -1;
    }
    base = root_abs;
  } else if (strcmp(root_kind, "project") == 0) {
    base = rt_action_cached_config(
        r && r->scheduler ? &r->scheduler->actions : NULL,
        def ? def->id : NULL, "allowed_project_root");
    if (!base || !*base) {
      snprintf(error, error_len,
               "{\"message\":\"root=project requires actions.manage_codex.allowed_project_root\"}");
      return -1;
    }
    if (snprintf(root_abs, root_len, "%s", base) >= (int)root_len) {
      snprintf(error, error_len,
               "{\"message\":\"configured project root is too long\"}");
      return -1;
    }
    base = root_abs;
  } else {
    snprintf(error, error_len,
             "{\"message\":\"root must be workspace or project\"}");
    return -1;
  }
  if (snprintf(joined, sizeof(joined), "%s/%s", base, cwd) >=
      (int)sizeof(joined)) {
    snprintf(error, error_len, "{\"message\":\"cwd is too long\"}");
    return -1;
  }
  if (!realpath(joined, cwd_abs) || stat(cwd_abs, &st) != 0 ||
      !S_ISDIR(st.st_mode)) {
    snprintf(error, error_len,
             "{\"message\":\"selected cwd must be an existing directory\"}");
    return -1;
  }
  if (!path_is_within(base, cwd_abs)) {
    snprintf(error, error_len,
             "{\"message\":\"selected cwd escapes the configured root\"}");
    return -1;
  }
  return 0;
}

static void sanitize_id(const char *in, const char *prefix, char *out, size_t out_len) {
  size_t pos = 0;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (prefix && *prefix) {
    pos = (size_t)snprintf(out, out_len, "%s", prefix);
    if (pos >= out_len) { out[out_len - 1] = '\0'; return; }
  }
  for (const char *p = in ? in : ""; *p && pos + 1 < out_len; ++p) {
    char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')
      out[pos++] = c;
    else
      out[pos++] = '_';
  }
  out[pos] = '\0';
}

static int codex_op_dir_for(const RtRun *r, const char *op_id,
                            char *out, size_t out_len) {
  char root[PATH_MAX];
  if (!op_id || !*op_id || workspace_root_path(r, root, sizeof(root)) != 0)
    return -1;
  return snprintf(out, out_len, "%s/logs/codex_ops/%s", root, op_id) >= (int)out_len ? -1 : 0;
}

static int write_long_file(const char *path, long value) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%ld\n", value);
  return fs_write_text_atomic(path, buf);
}

static int read_long_file(const char *path, long long *out) {
  char *text = NULL;
  char *end = NULL;
  long long v;
  if (!out || fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  v = strtoll(text, &end, 10);
  fc_xfree(text);
  if (!end) return -1;
  *out = v;
  return 0;
}

static int process_alive(long long pid) {
  if (pid <= 0) return 0;
  if (kill((pid_t)pid, 0) == 0) return 1;
  return errno == EPERM;
}

static int read_text_clip(const char *path, char *out, size_t out_len) {
  char *text = NULL;
  size_t n;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (!path || !*path || fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  n = strlen(text);
  while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r' ||
                   text[n - 1] == ' '  || text[n - 1] == '\t'))
    text[--n] = '\0';
  if (n >= out_len) n = out_len - 1;
  memcpy(out, text, n);
  out[n] = '\0';
  fc_xfree(text);
  return 0;
}

static int codex_named_file_deliverable(const char *s, char *name_out,
                                        size_t name_len) {
  static const char *ext = ".md .txt .json .html .htm .csv .yaml .yml"
    " .xml .log .sh .js .ts .toml .ini";
  char tok[8];
  if (name_out && name_len) name_out[0] = '\0';
  if (!s || !*s) return 0;
  for (const char *e = ext; *e;) {
    const char *p;
    int n = 0;
    while (*e == ' ') e++;
    while (*e && *e != ' ' && n < (int)sizeof(tok) - 1) tok[n++] = *e++;
    tok[n] = '\0';
    if (n && (p = strstr(s, tok)) != NULL) {
      char after = p[n];
      if (!((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
            (after >= '0' && after <= '9'))) {
        const char *b = p;
        while (b > s) {
          char c = b[-1];
          if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == '/')
            b--;
          else break;
        }
        if (name_out && name_len) {
          size_t len = (size_t)(p + n - b);
          if (len >= name_len) len = name_len - 1;
          memcpy(name_out, b, len);
          name_out[len] = '\0';
        }
        return 1;
      }
    }
  }
  return 0;
}

static int workspace_relative_expected(const char *cwd, const char *expected,
                                       char *out, size_t out_len) {
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (!expected || !*expected || expected[0] == '/' ||
      strstr(expected, "../") || strcmp(expected, "..") == 0)
    return -1;
  if (!cwd || !*cwd || strcmp(cwd, ".") == 0)
    return snprintf(out, out_len, "%s", expected) >= (int)out_len ? -1 : 0;
  if (cwd[0] == '/' || strstr(cwd, "../") || strcmp(cwd, "..") == 0)
    return -1;
  return snprintf(out, out_len, "%s/%s", cwd, expected) >= (int)out_len ? -1 : 0;
}

static int write_codex_state(const CodexOpState *st) {
  char eop[RT_MED], estate[RT_MED], eperm[RT_MED], eroot[RT_MED];
  char ecwd[PATH_MAX * 2], eroot_path[PATH_MAX * 2];
  char elaunch_path[PATH_MAX * 2];
  char etask[RT_LARGE * 2], eexpected[PATH_MAX * 2];
  char estdout[PATH_MAX * 2], estderr[PATH_MAX * 2], efinal[PATH_MAX * 2];
  char eprompt[PATH_MAX * 2], eexit[PATH_MAX * 2], eopdir[PATH_MAX * 2];
  char efinal_text[RT_LARGE * 2];
  char ebound_task[RT_MED];
  char eworking_memory[RT_TASK_WORKING_MEMORY_MAX * 2];
  char out[RT_XL * 2];
  char path[PATH_MAX];
  if (!st || !st->op_dir[0]) return -1;
  if (json_escape(st->op_id, eop, sizeof(eop)) != 0 ||
      json_escape(st->state, estate, sizeof(estate)) != 0 ||
      json_escape(st->permission_mode, eperm, sizeof(eperm)) != 0 ||
      json_escape(st->root, eroot, sizeof(eroot)) != 0 ||
      json_escape(st->cwd, ecwd, sizeof(ecwd)) != 0 ||
      json_escape(st->root_path, eroot_path, sizeof(eroot_path)) != 0 ||
      json_escape(st->launch_path, elaunch_path, sizeof(elaunch_path)) != 0 ||
      json_escape(st->task, etask, sizeof(etask)) != 0 ||
      json_escape(st->bound_task_id, ebound_task,
                  sizeof(ebound_task)) != 0 ||
      json_escape(st->working_memory, eworking_memory,
                  sizeof(eworking_memory)) != 0 ||
      json_escape(st->expected_path, eexpected, sizeof(eexpected)) != 0 ||
      json_escape(st->stdout_path, estdout, sizeof(estdout)) != 0 ||
      json_escape(st->stderr_path, estderr, sizeof(estderr)) != 0 ||
      json_escape(st->final_path, efinal, sizeof(efinal)) != 0 ||
      json_escape(st->prompt_path, eprompt, sizeof(eprompt)) != 0 ||
      json_escape(st->exit_path, eexit, sizeof(eexit)) != 0 ||
      json_escape(st->op_dir, eopdir, sizeof(eopdir)) != 0 ||
      json_escape(st->final_text, efinal_text, sizeof(efinal_text)) != 0)
    return -1;
  if (snprintf(out, sizeof(out),
               "{\"op_id\":\"%s\",\"state\":\"%s\",\"permission_mode\":\"%s\","
               "\"root\":\"%s\",\"cwd\":\"%s\",\"root_path\":\"%s\","
               "\"launch_path\":\"%s\",\"task\":\"%s\",\"expected_path\":\"%s\","
               "\"bound_task_id\":\"%s\",\"working_memory\":\"%s\","
               "\"stdout_path\":\"%s\",\"stderr_path\":\"%s\",\"final_path\":\"%s\","
               "\"prompt_path\":\"%s\",\"exit_path\":\"%s\",\"op_dir\":\"%s\","
               "\"pid\":%lld,\"exit_code\":%lld,\"last_checked_ms\":%lld,"
               "\"needs_input\":%s,\"pending_input\":null,\"final_text\":\"%s\"}\n",
               eop, estate, eperm, eroot, ecwd, eroot_path, elaunch_path,
               etask, eexpected, ebound_task, eworking_memory,
               estdout, estderr, efinal, eprompt, eexit, eopdir,
               st->pid, st->exit_code, st->last_checked_ms,
               st->needs_input ? "true" : "false", efinal_text) >= (int)sizeof(out))
    return -1;
  snprintf(path, sizeof(path), "%s/state.json", st->op_dir);
  return fs_write_text_atomic(path, out);
}

static int load_codex_state(const RtRun *r, const char *op_id, CodexOpState *st) {
  char path[PATH_MAX];
  char *text = NULL;
  JsonRef root;
  if (!r || !op_id || !*op_id || !st) return -1;
  memset(st, 0, sizeof(*st));
  if (codex_op_dir_for(r, op_id, st->op_dir, sizeof(st->op_dir)) != 0) return -1;
  snprintf(path, sizeof(path), "%s/state.json", st->op_dir);
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  if (json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_string(&root, "op_id", st->op_id, sizeof(st->op_id)) != 0) {
    fc_xfree(text);
    return -1;
  }
  (void)json_ref_object_get_string(&root, "state", st->state, sizeof(st->state));
  (void)json_ref_object_get_string(&root, "permission_mode", st->permission_mode, sizeof(st->permission_mode));
  (void)json_ref_object_get_string(&root, "root", st->root, sizeof(st->root));
  (void)json_ref_object_get_string(&root, "cwd", st->cwd, sizeof(st->cwd));
  (void)json_ref_object_get_string(&root, "root_path", st->root_path, sizeof(st->root_path));
  (void)json_ref_object_get_string(&root, "launch_path", st->launch_path, sizeof(st->launch_path));
  (void)json_ref_object_get_string(&root, "task", st->task, sizeof(st->task));
  (void)json_ref_object_get_string(&root, "bound_task_id",
                                   st->bound_task_id,
                                   sizeof(st->bound_task_id));
  (void)json_ref_object_get_string(&root, "working_memory",
                                   st->working_memory,
                                   sizeof(st->working_memory));
  (void)json_ref_object_get_string(&root, "expected_path", st->expected_path, sizeof(st->expected_path));
  (void)json_ref_object_get_string(&root, "stdout_path", st->stdout_path, sizeof(st->stdout_path));
  (void)json_ref_object_get_string(&root, "stderr_path", st->stderr_path, sizeof(st->stderr_path));
  (void)json_ref_object_get_string(&root, "final_path", st->final_path, sizeof(st->final_path));
  (void)json_ref_object_get_string(&root, "prompt_path", st->prompt_path, sizeof(st->prompt_path));
  (void)json_ref_object_get_string(&root, "exit_path", st->exit_path, sizeof(st->exit_path));
  (void)json_ref_object_get_long(&root, "pid", &st->pid);
  (void)json_ref_object_get_long(&root, "exit_code", &st->exit_code);
  (void)json_ref_object_get_long(&root, "last_checked_ms", &st->last_checked_ms);
  (void)json_ref_object_get_bool(&root, "needs_input", &st->needs_input);
  (void)json_ref_object_get_string(&root, "final_text", st->final_text, sizeof(st->final_text));
  if (!st->root[0]) snprintf(st->root, sizeof(st->root), "workspace");
  fc_xfree(text);
  return 0;
}

/* Runner-side terminalization: the supervising fork already owns the
 * durable exit code; compute the honest terminal state — including the
 * named-deliverable override that used to live in the poll path — then
 * submit the completion claim through the bus. This is runtime-owned C
 * executing in the detached process, strictly stronger than instructing
 * the model-driven worker to call the helper. */
static void runner_finalize_and_submit(CodexOpState *st, const char *tool) {
  if (!st) return;
  if (read_long_file(st->exit_path, &st->exit_code) != 0 || st->exit_code < 0)
    st->exit_code = 127;
  if (read_text_clip(st->final_path, st->final_text,
                     sizeof(st->final_text)) != 0)
    (void)read_text_clip(st->stdout_path, st->final_text,
                         sizeof(st->final_text));
  if (!st->final_text[0])
    snprintf(st->final_text, sizeof(st->final_text), "%s %s.",
             tool, st->exit_code == 0 ? "completed" : "failed");
  snprintf(st->state, sizeof(st->state), "%s",
           st->exit_code == 0 ? "succeeded" : "failed");
  if (st->expected_path[0] && strcmp(st->state, "succeeded") == 0) {
    char rel[PATH_MAX];
    char abs[PATH_MAX];
    const char *root = st->root_path[0] ? st->root_path : st->launch_path;
    int present = 0;
    if (st->launch_path[0] &&
        snprintf(rel, sizeof(rel), "%s/%s", st->launch_path,
                 st->expected_path) < (int)sizeof(rel) &&
        realpath(rel, abs) && path_is_within(root, abs) &&
        fs_file_exists(abs))
      present = 1;
    if (!present) {
      /* Trust the runtime over the exit code: a named deliverable that
       * is not on disk means the op did not actually succeed. */
      char missing[PATH_MAX];
      snprintf(missing, sizeof(missing), "%s", st->expected_path);
      st->expected_path[0] = '\0';
      snprintf(st->state, sizeof(st->state), "failed");
      snprintf(st->final_text, sizeof(st->final_text),
               "%s exited 0 but the named deliverable %s is not present.",
               tool, missing);
    }
  }
  st->last_checked_ms = (long long)timing_wall_ms();
  (void)write_codex_state(st);
  if (st->fc_operation_id[0] && st->fc_completion_token[0]) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (rt_operation_publish_worker_claim(
              st->fc_operation_id, st->fc_completion_token,
              strcmp(st->state, "succeeded") == 0 ? "succeeded" : "failed",
              st->final_text) == 0)
        break;
      sleep(1);
    }
  }
}

static char *managed_worker_prompt_dup(const CodexOpState *st,
                                       const char *instructions) {
  static const char shared_instruction[] =
      "# Shared task working memory\n\n"
      "working_memory is unstructured working text shared by agents handling "
      "this task. Read it before acting and append useful discoveries, "
      "corrections, attempted approaches, unresolved issues, or likely next "
      "steps. Existing text cannot be replaced or erased. To append, run "
      "`fclaw action exec --name working_memory_append --args "
      "'{\"working_memory\":\"new text\"}'`; task routing is inherited.\n\n";
  const char *memory;
  size_t needed;
  char *prompt;
  if (!st) return NULL;
  memory = st->working_memory[0] ? st->working_memory : "(empty)";
  needed = strlen(instructions ? instructions : "") + strlen(st->task) +
           strlen(shared_instruction) + strlen(memory) + 80U;
  prompt = (char *)fc_xmalloc(needed);
  if (!prompt) return NULL;
  snprintf(prompt, needed, "%s%s# Assigned task\n\n%s\n\n%s%s\n",
           instructions && *instructions ? instructions : "",
           instructions && *instructions ? "\n" : "",
           st->task, shared_instruction, memory);
  return prompt;
}

static int write_managed_codex_prompt(const CodexOpState *st,
                                      const char *instructions_path) {
  char *instructions = NULL;
  char *prompt = NULL;
  int rc;
  if (!st || !st->prompt_path[0]) return -1;
  if (instructions_path && *instructions_path &&
      fs_read_text(instructions_path, &instructions,
                   FS_READ_TEXT_DEFAULT_CAP) == 0 &&
      instructions)
    (void)strlen(instructions);
  prompt = managed_worker_prompt_dup(st, instructions);
  if (!prompt) {
    fc_xfree(instructions);
    return -1;
  }
  rc = fs_write_text(st->prompt_path, prompt);
  fc_xfree(prompt);
  fc_xfree(instructions);
  return rc;
}

/* Managed Codex starts below the runtime root, usually in workspace/.
 * Put the already-running fclaw binary on PATH and tell its action subcommand
 * where the installation root lives. `action exec` then publishes back to
 * the runtime owner instead of trying to perform the effect inside Codex's
 * sandbox. */
static int expose_action_cli_to_worker(const char *bound_task_id) {
  char runtime_root[PATH_MAX];
  char executable[PATH_MAX];
  char bin_dir[PATH_MAX];
  char path_value[PATH_MAX * 2];
  const char *configured_exe = rt_exe_path();
  const char *old_path = getenv("PATH");
  char *slash;
  if (!getcwd(runtime_root, sizeof(runtime_root))) return -1;
  if (!configured_exe || !*configured_exe) return -1;
  if (!realpath(configured_exe, executable)) {
    if (configured_exe[0] == '/' ||
        snprintf(executable, sizeof(executable), "%s/%s",
                 runtime_root, configured_exe) >= (int)sizeof(executable))
      return -1;
  }
  snprintf(bin_dir, sizeof(bin_dir), "%s", executable);
  slash = strrchr(bin_dir, '/');
  if (!slash) return -1;
  *slash = '\0';
  if (snprintf(path_value, sizeof(path_value), "%s%s%s", bin_dir,
               old_path && *old_path ? ":" : "",
               old_path && *old_path ? old_path : "") >=
      (int)sizeof(path_value))
    return -1;
  if (setenv("FCLAW_ACTION_ROOT", runtime_root, 1) != 0 ||
      setenv("FCLAW_BIN", executable, 1) != 0 ||
      setenv("PATH", path_value, 1) != 0)
    return -1;
  if (bound_task_id && *bound_task_id)
    return setenv("FCLAW_BOUND_TASK_ID", bound_task_id, 1);
  return unsetenv("FCLAW_BOUND_TASK_ID");
}

static int spawn_codex_runner(CodexOpState *st, const char *cwd_abs,
                              const char *binary,
                              const char *model,
                              const char *instructions_path,
                              char *err, size_t err_len) {
  int pipefd[2];
  pid_t first;
  char perm[RT_SMALL];
  if (!st || !cwd_abs || !binary || !*binary) return -1;
  if (pipe(pipefd) != 0) {
    snprintf(err, err_len, "{\"message\":\"pipe failed\"}");
    return -1;
  }
  first = fork();
  if (first < 0) {
    close(pipefd[0]); close(pipefd[1]);
    snprintf(err, err_len, "{\"message\":\"fork failed\"}");
    return -1;
  }
  if (first == 0) {
    pid_t runner;
    close(pipefd[0]);
    if (setsid() < 0) _exit(111);
    runner = fork();
    if (runner < 0) _exit(112);
    if (runner > 0) {
      (void)write(pipefd[1], &runner, sizeof(runner));
      close(pipefd[1]);
      _exit(0);
    }
    close(pipefd[1]);
    if (write_managed_codex_prompt(st, instructions_path) != 0)
      _exit(113);
    (void)write_long_file(st->exit_path, -1);
    {
      pid_t codex_pid = fork();
      int status = 127;
      if (codex_pid == 0) {
        char final_abs[PATH_MAX];
        if (expose_action_cli_to_worker(st->bound_task_id) != 0) _exit(126);
        int in_fd = open(st->prompt_path, O_RDONLY);
        int out_fd = open(st->stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int err_fd = open(st->stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in_fd < 0 || out_fd < 0 || err_fd < 0) _exit(126);
        if (dup2(in_fd, STDIN_FILENO) < 0 ||
            dup2(out_fd, STDOUT_FILENO) < 0 ||
            dup2(err_fd, STDERR_FILENO) < 0) _exit(126);
        close(in_fd); close(out_fd); close(err_fd);
        /* -o must survive the child's chdir into the task cwd: resolve
         * the final-output path against the launch cwd first, or codex
         * writes it under <task cwd>/workspace/... where the poller
         * never looks. */
        if (st->final_path[0] == '/') {
          snprintf(final_abs, sizeof(final_abs), "%s", st->final_path);
        } else {
          char launch_cwd[PATH_MAX];
          if (!getcwd(launch_cwd, sizeof(launch_cwd)) ||
              snprintf(final_abs, sizeof(final_abs), "%s/%s",
                       launch_cwd, st->final_path) >= (int)sizeof(final_abs))
            _exit(126);
        }
        if (chdir(cwd_abs) != 0) _exit(126);
        snprintf(perm, sizeof(perm), "%s", st->permission_mode[0] ? st->permission_mode : "safe");
        {
          char *child_argv[20];
          int ai = 0;
          child_argv[ai++] = (char *)binary;
          child_argv[ai++] = "exec";
          child_argv[ai++] = "--json";
          child_argv[ai++] = "-o";
          child_argv[ai++] = final_abs;
          child_argv[ai++] = "-C";
          child_argv[ai++] = (char *)cwd_abs;
          if (model && *model) {
            child_argv[ai++] = "--model";
            child_argv[ai++] = (char *)model;
          }
          if (strcmp(perm, "dangerous") == 0) {
            child_argv[ai++] =
                "--dangerously-bypass-approvals-and-sandbox";
          } else {
            /* `codex exec` is already non-interactive — it never prompts
             * for approval. Earlier flag --ask-for-approval was removed
             * from the CLI; passing it makes the codex binary error out
             * (real-codex smoke 2026-05-27). */
            child_argv[ai++] = "--sandbox";
            child_argv[ai++] = "workspace-write";
          }
          child_argv[ai++] = "--skip-git-repo-check";
          child_argv[ai++] = "-";
          child_argv[ai] = NULL;
          execvp(binary, child_argv);
        }
        _exit(127);
      }
      if (codex_pid > 0 && waitpid(codex_pid, &status, 0) >= 0) {
        if (WIFEXITED(status)) (void)write_long_file(st->exit_path, WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) (void)write_long_file(st->exit_path, 128 + WTERMSIG(status));
        else (void)write_long_file(st->exit_path, 125);
      } else {
        (void)write_long_file(st->exit_path, 127);
      }
    }
    /* Terminal result is durable; the knock is next. The runner — not
     * the model-driven worker — verifies and submits completion. */
    runner_finalize_and_submit(st, "Codex");
    _exit(0);
  }
  close(pipefd[1]);
  {
    pid_t runner_pid = 0;
    ssize_t got = read(pipefd[0], &runner_pid, sizeof(runner_pid));
    int status = 0;
    close(pipefd[0]);
    (void)waitpid(first, &status, 0);
    if (got != (ssize_t)sizeof(runner_pid) || runner_pid <= 0) {
      snprintf(err, err_len, "{\"message\":\"codex runner launch failed\"}");
      return -1;
    }
    st->pid = (long long)runner_pid;
  }
  return 0;
}

/* `contract` selects the result shape. The start op speaks the managed
 * contract (status/handle) so the driver records the detachment; status
 * and abort ops deliberately omit those keys — they are immediate
 * informational calls about ANOTHER operation, and echoing its running
 * state as this call's contract would bind a phantom worker to this
 * call's own preallocated operation. */
static int codex_result_json(const CodexOpState *st, int contract,
                             char *out, size_t out_len) {
  char eop[RT_MED], estate[RT_MED], eroot[RT_MED];
  char ecwd[PATH_MAX * 2], efinal[RT_LARGE * 2];
  char elogdir[PATH_MAX * 2], epath[PATH_MAX * 2];
  char contract_field[RT_MED + 64] = "";
  char path_field[PATH_MAX * 2 + 32] = "";
  char log_dir[PATH_MAX] = "";
  if (!st || !out || out_len == 0) return -1;
  {
    const char *logs = strstr(st->op_dir, "/logs/");
    snprintf(log_dir, sizeof(log_dir), "%s", logs ? logs + 1 : st->op_dir);
  }
  if (json_escape(st->op_id, eop, sizeof(eop)) != 0 ||
      json_escape(st->state, estate, sizeof(estate)) != 0 ||
      json_escape(st->root[0] ? st->root : "workspace",
                  eroot, sizeof(eroot)) != 0 ||
      json_escape(st->cwd, ecwd, sizeof(ecwd)) != 0 ||
      json_escape(st->final_text, efinal, sizeof(efinal)) != 0 ||
      json_escape(log_dir, elogdir, sizeof(elogdir)) != 0) return -1;
  if (st->expected_path[0] && strcmp(st->state, "succeeded") == 0) {
    char relative[PATH_MAX];
    if (!st->cwd[0] || strcmp(st->cwd, ".") == 0)
      snprintf(relative, sizeof(relative), "%s", st->expected_path);
    else if (snprintf(relative, sizeof(relative), "%s/%s", st->cwd,
                      st->expected_path) >= (int)sizeof(relative))
      return -1;
    if (json_escape(relative, epath, sizeof(epath)) != 0) return -1;
    snprintf(path_field, sizeof(path_field), ",\"path\":\"%s\"", epath);
  }
  if (contract)
    snprintf(contract_field, sizeof(contract_field),
             "\"status\":\"%s\",\"handle\":\"%s\",",
             strcmp(st->state, "running") == 0 ? "running" : "finished",
             eop);
  return snprintf(out, out_len,
                  "{\"op_id\":\"%s\",%s"
                  "\"state\":\"%s\",\"tool\":\"manage_codex\","
                  "\"root\":\"%s\",\"cwd\":\"%s\",\"last_checked_ms\":%lld,"
                  "\"needs_input\":%s,\"pending_input\":null,"
                  "\"text\":\"%s\",\"final_text\":\"%s\",\"log_dir\":\"%s\"%s}",
                  eop, contract_field,
                  estate, eroot, ecwd, st->last_checked_ms,
                  st->needs_input ? "true" : "false",
                  efinal, efinal, elogdir, path_field) >= (int)out_len ? -1 : 0;
}

static int refresh_codex_state(RtRun *r, CodexOpState *st) {
  char rel[PATH_MAX];
  char abs[PATH_MAX];
  st->last_checked_ms = (long long)timing_wall_ms();
  if (strcmp(st->state, "running") == 0) {
    if (read_long_file(st->exit_path, &st->exit_code) == 0 && st->exit_code >= 0) {
      /* Runner has recorded a terminal result; prefer that over process liveness
       * so fast local/fake executions become observable in the same run pass. */
    } else if (process_alive(st->pid)) {
      (void)write_codex_state(st);
      return 0;
    } else if (read_long_file(st->exit_path, &st->exit_code) != 0 || st->exit_code < 0) {
      st->exit_code = 127;
      snprintf(st->state, sizeof(st->state), "failed");
      snprintf(st->final_text, sizeof(st->final_text),
               "Codex operation ended without a readable exit status.");
    }
    if (strcmp(st->state, "running") == 0) {
      if (read_text_clip(st->final_path, st->final_text, sizeof(st->final_text)) != 0)
        (void)read_text_clip(st->stdout_path, st->final_text, sizeof(st->final_text));
      if (!st->final_text[0]) {
        snprintf(st->final_text, sizeof(st->final_text), "%s",
                 st->exit_code == 0 ? "Codex completed." : "Codex failed.");
      }
      snprintf(st->state, sizeof(st->state), "%s", st->exit_code == 0 ? "succeeded" : "failed");
    }
  }
  if (st->expected_path[0] && st->launch_path[0] && st->root_path[0] &&
      snprintf(rel, sizeof(rel), "%s/%s", st->launch_path,
               st->expected_path) < (int)sizeof(rel) &&
      realpath(rel, abs) && path_is_within(st->root_path, abs) &&
      fs_file_exists(abs)) {
    /* New operation state uses the canonical selected launch/root paths. */
  } else if (st->expected_path[0] && !st->launch_path[0] &&
             workspace_relative_expected(st->cwd, st->expected_path,
                                         rel, sizeof(rel)) == 0 &&
             rt_action_resolve_workspace_path(r, rel, abs, sizeof(abs),
                                    (char[RT_LARGE]){0}, RT_LARGE) == 0 &&
             fs_file_exists(abs)) {
    /* Backward-compatible recovery for older workspace-only state. */
  } else if (st->expected_path[0] && strcmp(st->state, "succeeded") == 0) {
    /* The task named a file deliverable but it isn't on disk. Trust the
     * runtime over the exit code: this is "failed" so the reviewer's
     * failed-path notification fires instead of a silent quiesce. A
     * task with no named deliverable trusts the exit code as-is. */
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s", st->expected_path);
    st->expected_path[0] = '\0';
    snprintf(st->state, sizeof(st->state), "failed");
    snprintf(st->final_text, sizeof(st->final_text),
             "Codex exited 0 but the named deliverable %s is not present.",
             missing);
  }
  (void)write_codex_state(st);
  return 0;
}

int fc_manage_codex(RtRun *r, const char *rid, const RtActionDef *def,
                           const char *args, char *result, size_t result_len,
                           char *error, size_t error_len) {
  JsonRef root;
  CodexOpState st;
  char op[RT_SMALL] = "", task[RT_LARGE] = "", op_id[RT_SMALL] = "";
  char root_arg[RT_SMALL] = "workspace";
  char cwd_arg[PATH_MAX] = ".", cwd_abs[PATH_MAX];
  char permission_mode[RT_SMALL] = "safe";
  char binary_abs[PATH_MAX];
  char workspace_root[PATH_MAX], op_base[PATH_MAX];
  const char *binary = getenv("FCLAW_CODEX_BIN");
  const char *model = rt_action_cached_config(
      r && r->scheduler ? &r->scheduler->actions : NULL,
      def ? def->id : NULL, "model");
  if (!binary || !*binary) binary = "codex";
  if (binary[0] != '/' && strchr(binary, '/')) {
    char launch_cwd[PATH_MAX];
    if (getcwd(launch_cwd, sizeof(launch_cwd)) &&
        snprintf(binary_abs, sizeof(binary_abs), "%s/%s", launch_cwd, binary) < (int)sizeof(binary_abs))
      binary = binary_abs;
  }
  if (!r || !rid || !args || json_ref_first_object(args, &root) != 0 ||
      json_ref_object_get_string(&root, "op", op, sizeof(op)) != 0 || !op[0]) {
    snprintf(error, error_len, "{\"message\":\"args.op is required\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  (void)json_ref_object_get_string(&root, "task", task, sizeof(task));
  (void)json_ref_object_get_string(&root, "op_id", op_id, sizeof(op_id));
  (void)json_ref_object_get_string(&root, "cwd", cwd_arg, sizeof(cwd_arg));
  if (!cwd_arg[0]) snprintf(cwd_arg, sizeof(cwd_arg), ".");
  (void)json_ref_object_get_string(&root, "root", root_arg, sizeof(root_arg));
  if (!root_arg[0]) snprintf(root_arg, sizeof(root_arg), "workspace");
  (void)json_ref_object_get_string(&root, "permission_mode", permission_mode, sizeof(permission_mode));
  if (!permission_mode[0]) snprintf(permission_mode, sizeof(permission_mode), "safe");
  if (strcmp(permission_mode, "safe") != 0 && strcmp(permission_mode, "dangerous") != 0) {
    snprintf(error, error_len, "{\"message\":\"permission_mode must be safe or dangerous\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (strcmp(op, "start") == 0) {
    char err_obj[RT_LARGE] = "null";
    char instructions_path[PATH_MAX] = "";
    JsonRef cwd_ref;
    if (!task[0]) {
      snprintf(error, error_len, "{\"message\":\"args.task is required for op=start\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    if (json_ref_object_get(&root, "cwd", &cwd_ref) == 0 &&
        json_string_has_nul_escape(&cwd_ref)) {
      snprintf(error, error_len,
               "{\"message\":\"cwd contains an embedded NUL\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    memset(&st, 0, sizeof(st));
    sanitize_id(rid, "codex_", st.op_id, sizeof(st.op_id));
    if (managed_worker_bind_task(r, rid, &st) != 0 ||
        workspace_root_path(r, workspace_root, sizeof(workspace_root)) != 0 ||
        resolve_codex_launch_path(r, def, root_arg, cwd_arg,
                                  st.root_path, sizeof(st.root_path),
                                  cwd_abs, sizeof(cwd_abs),
                                  error, error_len) != 0 ||
        snprintf(op_base, sizeof(op_base), "%s/logs/codex_ops", workspace_root) >= (int)sizeof(op_base) ||
        fs_mkdir_p(op_base) != 0 ||
        codex_op_dir_for(r, st.op_id, st.op_dir, sizeof(st.op_dir)) != 0 ||
        fs_mkdir_p(st.op_dir) != 0) {
      snprintf(result, result_len, "{}");
      if (!error[0]) snprintf(error, error_len, "{\"message\":\"codex op setup failed\"}");
      return -1;
    }
    snprintf(st.state, sizeof(st.state), "running");
    snprintf(st.permission_mode, sizeof(st.permission_mode), "%s", permission_mode);
    snprintf(st.root, sizeof(st.root), "%s", root_arg);
    snprintf(st.cwd, sizeof(st.cwd), "%s", cwd_arg);
    snprintf(st.launch_path, sizeof(st.launch_path), "%s", cwd_abs);
    snprintf(st.task, sizeof(st.task), "%s", task);
    {
      /* The named-deliverable heuristic applies to the CURRENT work
       * text only. The workers phase appends the previous attempt's
       * result under this marker for continuation context; filenames
       * mentioned in old results are not deliverables of this attempt. */
      char head[RT_LARGE];
      const char *cut = strstr(task, "\n\nPrior attempt result:");
      size_t hn = cut ? (size_t)(cut - task) : strlen(task);
      if (hn >= sizeof(head)) hn = sizeof(head) - 1;
      memcpy(head, task, hn);
      head[hn] = '\0';
      (void)codex_named_file_deliverable(head, st.expected_path, sizeof(st.expected_path));
    }
    snprintf(st.stdout_path, sizeof(st.stdout_path), "%s/stdout.jsonl", st.op_dir);
    snprintf(st.stderr_path, sizeof(st.stderr_path), "%s/stderr.log", st.op_dir);
    snprintf(st.final_path, sizeof(st.final_path), "%s/final.txt", st.op_dir);
    snprintf(st.prompt_path, sizeof(st.prompt_path), "%s/prompt.txt", st.op_dir);
    snprintf(st.exit_path, sizeof(st.exit_path), "%s/exit_code", st.op_dir);
    st.exit_code = -1;
    st.last_checked_ms = (long long)timing_wall_ms();
    if (def && def->dir[0])
      (void)snprintf(instructions_path, sizeof(instructions_path),
                     "%s/AGENTS.md", def->dir);
    if (spawn_codex_runner(&st, cwd_abs, binary, model, instructions_path,
                           err_obj, sizeof(err_obj)) != 0) {
      snprintf(error, error_len, "%s", err_obj);
      snprintf(result, result_len, "{}");
      return -1;
    }
    (void)write_codex_state(&st);
    {
      const char *sync_ms = getenv("FCLAW_CODEX_START_SYNC_MS");
      if (sync_ms && *sync_ms) {
        long wait_ms = strtol(sync_ms, NULL, 10);
        if (wait_ms > 0 && wait_ms <= 5000) {
          usleep((useconds_t)wait_ms * 1000U);
          (void)refresh_codex_state(r, &st);
        }
      }
    }
    (void)codex_result_json(&st, 1, result, result_len);
    snprintf(error, error_len, "null");
    return 0;
  }
  if (strcmp(op, "poll") == 0 || strcmp(op, "status") == 0) {
    if (!op_id[0] || load_codex_state(r, op_id, &st) != 0) {
      snprintf(error, error_len, "{\"message\":\"unknown or missing op_id\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    (void)refresh_codex_state(r, &st);
    if (codex_result_json(&st, 0, result, result_len) != 0) {
      snprintf(error, error_len, "{\"message\":\"codex status result too large\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    snprintf(error, error_len, "null");
    return 0;
  }
  if (strcmp(op, "abort") == 0) {
    if (!op_id[0] || load_codex_state(r, op_id, &st) != 0) {
      snprintf(error, error_len, "{\"message\":\"unknown or missing op_id\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    if (process_alive(st.pid)) (void)kill((pid_t)st.pid, SIGTERM);
    snprintf(st.state, sizeof(st.state), "failed");
    snprintf(st.final_text, sizeof(st.final_text), "Codex operation aborted.");
    st.exit_code = 130;
    st.last_checked_ms = (long long)timing_wall_ms();
    (void)write_codex_state(&st);
    (void)rt_operation_submit_abort_claim(st.op_id,
                                          "Codex operation aborted.");
    (void)codex_result_json(&st, 0, result, result_len);
    snprintf(error, error_len, "null");
    return 0;
  }
  if (strcmp(op, "reply") == 0) {
    snprintf(error, error_len, "{\"message\":\"manage_codex reply is not supported for noninteractive codex exec operations\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(error, error_len, "{\"message\":\"args.op must be start, poll, reply, abort, or status\"}");
  snprintf(result, result_len, "{}");
  return -1;
}

/* ----- manage_claude -------------------------------------------------- *
 * Mirrors the codex path: spawn-poll-collect lifecycle around a single
 * external child. The state shape is identical; only the spawn invocation
 * and the path/id prefixes differ. Per the project's "extract on third
 * use" rule, this is kept as a near-copy of the codex helpers rather
 * than factored out — that comes if a third delegate ever lands. */

typedef CodexOpState ClaudeOpState;

static int claude_op_dir_for(const RtRun *r, const char *op_id,
                             char *out, size_t out_len) {
  char root[PATH_MAX];
  if (!op_id || !*op_id || workspace_root_path(r, root, sizeof(root)) != 0)
    return -1;
  return snprintf(out, out_len, "%s/logs/claude_ops/%s", root, op_id) >= (int)out_len ? -1 : 0;
}

static int write_claude_state(const ClaudeOpState *st) {
  /* The on-disk fields are the same as codex; reuse the writer. */
  return write_codex_state(st);
}

static int load_claude_state(const RtRun *r, const char *op_id, ClaudeOpState *st) {
  char path[PATH_MAX];
  char *text = NULL;
  JsonRef root;
  if (!r || !op_id || !*op_id || !st) return -1;
  if (claude_op_dir_for(r, op_id, path, sizeof(path)) != 0) return -1;
  {
    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/state.json", path);
    if (fs_read_text(file, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  }
  if (json_ref_first_object(text, &root) != 0) { fc_xfree(text); return -1; }
  memset(st, 0, sizeof(*st));
  (void)json_ref_object_get_string(&root, "op_id", st->op_id, sizeof(st->op_id));
  (void)json_ref_object_get_string(&root, "state", st->state, sizeof(st->state));
  (void)json_ref_object_get_string(&root, "permission_mode", st->permission_mode, sizeof(st->permission_mode));
  (void)json_ref_object_get_string(&root, "cwd", st->cwd, sizeof(st->cwd));
  (void)json_ref_object_get_string(&root, "task", st->task, sizeof(st->task));
  (void)json_ref_object_get_string(&root, "bound_task_id",
                                   st->bound_task_id,
                                   sizeof(st->bound_task_id));
  (void)json_ref_object_get_string(&root, "working_memory",
                                   st->working_memory,
                                   sizeof(st->working_memory));
  (void)json_ref_object_get_string(&root, "expected_path", st->expected_path, sizeof(st->expected_path));
  (void)json_ref_object_get_string(&root, "stdout_path", st->stdout_path, sizeof(st->stdout_path));
  (void)json_ref_object_get_string(&root, "stderr_path", st->stderr_path, sizeof(st->stderr_path));
  (void)json_ref_object_get_string(&root, "final_path", st->final_path, sizeof(st->final_path));
  (void)json_ref_object_get_string(&root, "prompt_path", st->prompt_path, sizeof(st->prompt_path));
  (void)json_ref_object_get_string(&root, "exit_path", st->exit_path, sizeof(st->exit_path));
  (void)json_ref_object_get_string(&root, "op_dir", st->op_dir, sizeof(st->op_dir));
  (void)json_ref_object_get_long(&root, "pid", &st->pid);
  (void)json_ref_object_get_long(&root, "exit_code", &st->exit_code);
  (void)json_ref_object_get_long(&root, "last_checked_ms", &st->last_checked_ms);
  (void)json_ref_object_get_string(&root, "final_text", st->final_text, sizeof(st->final_text));
  fc_xfree(text);
  return 0;
}

static int spawn_claude_runner(ClaudeOpState *st, const char *cwd_abs,
                               const char *binary, const char *model,
                               char *err, size_t err_len) {
  int pipefd[2];
  pid_t first;
  char perm[RT_SMALL];
  const char *model_arg = (model && *model) ? model : "claude-sonnet-4-6";
  if (!st || !cwd_abs || !binary || !*binary) return -1;
  if (pipe(pipefd) != 0) {
    snprintf(err, err_len, "{\"message\":\"pipe failed\"}");
    return -1;
  }
  first = fork();
  if (first < 0) {
    close(pipefd[0]); close(pipefd[1]);
    snprintf(err, err_len, "{\"message\":\"fork failed\"}");
    return -1;
  }
  if (first == 0) {
    pid_t runner;
    char *managed_prompt;
    close(pipefd[0]);
    if (setsid() < 0) _exit(111);
    runner = fork();
    if (runner < 0) _exit(112);
    if (runner > 0) {
      (void)write(pipefd[1], &runner, sizeof(runner));
      close(pipefd[1]);
      _exit(0);
    }
    close(pipefd[1]);
    managed_prompt = managed_worker_prompt_dup(st, NULL);
    if (!managed_prompt ||
        fs_write_text(st->prompt_path, managed_prompt) != 0)
      _exit(113);
    (void)write_long_file(st->exit_path, -1);
    {
      pid_t child_pid = fork();
      int status = 127;
      if (child_pid == 0) {
        if (expose_action_cli_to_worker(st->bound_task_id) != 0) _exit(126);
        int in_fd = open("/dev/null", O_RDONLY);
        int out_fd = open(st->stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int err_fd = open(st->stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in_fd < 0 || out_fd < 0 || err_fd < 0) _exit(126);
        if (dup2(in_fd, STDIN_FILENO) < 0 ||
            dup2(out_fd, STDOUT_FILENO) < 0 ||
            dup2(err_fd, STDERR_FILENO) < 0) _exit(126);
        close(in_fd); close(out_fd); close(err_fd);
        if (chdir(cwd_abs) != 0) _exit(126);
        snprintf(perm, sizeof(perm), "%s", st->permission_mode[0] ? st->permission_mode : "safe");
        if (strcmp(perm, "dangerous") == 0) {
          execlp(binary, binary, "--print",
                 "--output-format", "stream-json",
                 "--include-partial-messages",
                 "--add-dir", cwd_abs,
                 "--dangerously-skip-permissions",
                 "--model", model_arg,
                 "--", managed_prompt,
                 (char *)NULL);
        } else {
          execlp(binary, binary, "--print",
                 "--output-format", "stream-json",
                 "--include-partial-messages",
                 "--add-dir", cwd_abs,
                 "--model", model_arg,
                 "--", managed_prompt,
                 (char *)NULL);
        }
        _exit(127);
      }
      if (child_pid > 0 && waitpid(child_pid, &status, 0) >= 0) {
        if (WIFEXITED(status)) {
          int code = WEXITSTATUS(status);
          (void)write_long_file(st->exit_path, code);
          /* Mirror the codex final-text fallback: when --print exits 0
           * the assistant message is on stdout (stream-json); we
           * surface the last line here so the artifact has something
           * compact even before the reviewer parses the full stream. */
          if (code == 0) {
            char tail[RT_LARGE] = "";
            if (read_text_clip(st->stdout_path, tail, sizeof(tail)) == 0 && tail[0])
              (void)fs_write_text(st->final_path, tail);
          }
        } else if (WIFSIGNALED(status)) (void)write_long_file(st->exit_path, 128 + WTERMSIG(status));
        else (void)write_long_file(st->exit_path, 125);
      } else {
        (void)write_long_file(st->exit_path, 127);
      }
    }
    /* Terminal result is durable; the knock is next. The runner — not
     * the model-driven worker — verifies and submits completion. */
    runner_finalize_and_submit(st, "Claude");
    fc_xfree(managed_prompt);
    _exit(0);
  }
  close(pipefd[1]);
  {
    pid_t runner_pid = 0;
    ssize_t got = read(pipefd[0], &runner_pid, sizeof(runner_pid));
    int status = 0;
    close(pipefd[0]);
    (void)waitpid(first, &status, 0);
    if (got != (ssize_t)sizeof(runner_pid) || runner_pid <= 0) {
      snprintf(err, err_len, "{\"message\":\"claude runner launch failed\"}");
      return -1;
    }
    st->pid = (long long)runner_pid;
  }
  return 0;
}

/* Same contract split as codex_result_json: only the start op emits the
 * managed status/handle fields. */
static int claude_result_json(const ClaudeOpState *st, int contract,
                              char *out, size_t out_len) {
  char eop[RT_MED], estate[RT_MED], ecwd[PATH_MAX * 2], efinal[RT_LARGE * 2];
  char elogdir[PATH_MAX * 2], epath[PATH_MAX * 2];
  char contract_field[RT_MED + 64] = "";
  char path_field[PATH_MAX * 2 + 32] = "";
  char log_dir[PATH_MAX] = "";
  if (!st || !out || out_len == 0) return -1;
  {
    const char *logs = strstr(st->op_dir, "/logs/");
    snprintf(log_dir, sizeof(log_dir), "%s", logs ? logs + 1 : st->op_dir);
  }
  if (json_escape(st->op_id, eop, sizeof(eop)) != 0 ||
      json_escape(st->state, estate, sizeof(estate)) != 0 ||
      json_escape(st->cwd, ecwd, sizeof(ecwd)) != 0 ||
      json_escape(st->final_text, efinal, sizeof(efinal)) != 0 ||
      json_escape(log_dir, elogdir, sizeof(elogdir)) != 0) return -1;
  if (st->expected_path[0] && strcmp(st->state, "succeeded") == 0) {
    if (json_escape(st->expected_path, epath, sizeof(epath)) != 0) return -1;
    snprintf(path_field, sizeof(path_field), ",\"path\":\"%s\"", epath);
  }
  if (contract)
    snprintf(contract_field, sizeof(contract_field),
             "\"status\":\"%s\",\"handle\":\"%s\",",
             strcmp(st->state, "running") == 0 ? "running" : "finished",
             eop);
  return snprintf(out, out_len,
                  "{\"op_id\":\"%s\",%s"
                  "\"state\":\"%s\",\"tool\":\"manage_claude\","
                  "\"cwd\":\"%s\",\"last_checked_ms\":%lld,"
                  "\"needs_input\":%s,\"pending_input\":null,"
                  "\"text\":\"%s\",\"final_text\":\"%s\",\"log_dir\":\"%s\"%s}",
                  eop, contract_field,
                  estate, ecwd, st->last_checked_ms,
                  st->needs_input ? "true" : "false",
                  efinal, efinal, elogdir, path_field) >= (int)out_len ? -1 : 0;
}

static int refresh_claude_state(RtRun *r, ClaudeOpState *st) {
  char rel[PATH_MAX];
  char abs[PATH_MAX];
  st->last_checked_ms = (long long)timing_wall_ms();
  if (strcmp(st->state, "running") == 0) {
    if (read_long_file(st->exit_path, &st->exit_code) == 0 && st->exit_code >= 0) {
      /* terminal */
    } else if (process_alive(st->pid)) {
      (void)write_claude_state(st);
      return 0;
    } else if (read_long_file(st->exit_path, &st->exit_code) != 0 || st->exit_code < 0) {
      st->exit_code = 127;
      snprintf(st->state, sizeof(st->state), "failed");
      snprintf(st->final_text, sizeof(st->final_text),
               "Claude operation ended without a readable exit status.");
    }
    if (strcmp(st->state, "running") == 0) {
      if (read_text_clip(st->final_path, st->final_text, sizeof(st->final_text)) != 0)
        (void)read_text_clip(st->stdout_path, st->final_text, sizeof(st->final_text));
      if (!st->final_text[0]) {
        snprintf(st->final_text, sizeof(st->final_text), "%s",
                 st->exit_code == 0 ? "Claude completed." : "Claude failed.");
      }
      snprintf(st->state, sizeof(st->state), "%s", st->exit_code == 0 ? "succeeded" : "failed");
    }
  }
  if (st->expected_path[0] &&
      workspace_relative_expected(st->cwd, st->expected_path, rel, sizeof(rel)) == 0 &&
      rt_action_resolve_workspace_path(r, rel, abs, sizeof(abs), (char[RT_LARGE]){0}, RT_LARGE) == 0 &&
      fs_file_exists(abs)) {
    snprintf(st->expected_path, sizeof(st->expected_path), "%s", rel);
  } else if (st->expected_path[0] && strcmp(st->state, "succeeded") == 0) {
    /* Same honest-truth rule as codex: a named deliverable that's not
     * on disk means the op didn't actually succeed, regardless of exit
     * code. The reviewer's failed-path notification then fires. */
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s", st->expected_path);
    st->expected_path[0] = '\0';
    snprintf(st->state, sizeof(st->state), "failed");
    snprintf(st->final_text, sizeof(st->final_text),
             "Claude exited 0 but the named deliverable %s is not present.",
             missing);
  }
  (void)write_claude_state(st);
  return 0;
}

int fc_manage_claude(RtRun *r, const char *rid, const RtActionDef *def,
                            const char *args, char *result, size_t result_len,
                            char *error, size_t error_len) {
  JsonRef root;
  ClaudeOpState st;
  char op[RT_SMALL] = "", task[RT_LARGE] = "", op_id[RT_SMALL] = "";
  char cwd_arg[PATH_MAX] = ".", cwd_abs[PATH_MAX];
  char permission_mode[RT_SMALL] = "safe";
  char model[RT_SMALL] = "";
  char binary_abs[PATH_MAX];
  char workspace_root[PATH_MAX], op_base[PATH_MAX], rel_expected[PATH_MAX] = "";
  const char *binary = getenv("FCLAW_CLAUDE_BIN");
  (void)def;
  if (!binary || !*binary) binary = "claude";
  if (binary[0] != '/' && strchr(binary, '/')) {
    char launch_cwd[PATH_MAX];
    if (getcwd(launch_cwd, sizeof(launch_cwd)) &&
        snprintf(binary_abs, sizeof(binary_abs), "%s/%s", launch_cwd, binary) < (int)sizeof(binary_abs))
      binary = binary_abs;
  }
  if (!r || !rid || !args || json_ref_first_object(args, &root) != 0 ||
      json_ref_object_get_string(&root, "op", op, sizeof(op)) != 0 || !op[0]) {
    snprintf(error, error_len, "{\"message\":\"args.op is required\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  (void)json_ref_object_get_string(&root, "task", task, sizeof(task));
  (void)json_ref_object_get_string(&root, "op_id", op_id, sizeof(op_id));
  (void)json_ref_object_get_string(&root, "cwd", cwd_arg, sizeof(cwd_arg));
  if (!cwd_arg[0]) snprintf(cwd_arg, sizeof(cwd_arg), ".");
  (void)json_ref_object_get_string(&root, "model", model, sizeof(model));
  (void)json_ref_object_get_string(&root, "permission_mode", permission_mode, sizeof(permission_mode));
  if (!permission_mode[0]) snprintf(permission_mode, sizeof(permission_mode), "safe");
  if (strcmp(permission_mode, "safe") != 0 && strcmp(permission_mode, "dangerous") != 0) {
    snprintf(error, error_len, "{\"message\":\"permission_mode must be safe or dangerous\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (strcmp(op, "start") == 0) {
    char err_obj[RT_LARGE] = "null";
    if (!task[0]) {
      snprintf(error, error_len, "{\"message\":\"args.task is required for op=start\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    memset(&st, 0, sizeof(st));
    sanitize_id(rid, "claude_", st.op_id, sizeof(st.op_id));
    if (managed_worker_bind_task(r, rid, &st) != 0 ||
        workspace_root_path(r, workspace_root, sizeof(workspace_root)) != 0 ||
        rt_action_resolve_workspace_path(r, cwd_arg, cwd_abs, sizeof(cwd_abs), error, error_len) != 0 ||
        snprintf(op_base, sizeof(op_base), "%s/logs/claude_ops", workspace_root) >= (int)sizeof(op_base) ||
        fs_mkdir_p(op_base) != 0 ||
        claude_op_dir_for(r, st.op_id, st.op_dir, sizeof(st.op_dir)) != 0 ||
        fs_mkdir_p(st.op_dir) != 0) {
      snprintf(result, result_len, "{}");
      if (!error[0]) snprintf(error, error_len, "{\"message\":\"claude op setup failed\"}");
      return -1;
    }
    snprintf(st.state, sizeof(st.state), "running");
    snprintf(st.permission_mode, sizeof(st.permission_mode), "%s", permission_mode);
    snprintf(st.cwd, sizeof(st.cwd), "%s", cwd_arg);
    snprintf(st.launch_path, sizeof(st.launch_path), "%s", cwd_abs);
    snprintf(st.task, sizeof(st.task), "%s", task);
    {
      /* The named-deliverable heuristic applies to the CURRENT work
       * text only. The workers phase appends the previous attempt's
       * result under this marker for continuation context; filenames
       * mentioned in old results are not deliverables of this attempt. */
      char head[RT_LARGE];
      const char *cut = strstr(task, "\n\nPrior attempt result:");
      size_t hn = cut ? (size_t)(cut - task) : strlen(task);
      if (hn >= sizeof(head)) hn = sizeof(head) - 1;
      memcpy(head, task, hn);
      head[hn] = '\0';
      (void)codex_named_file_deliverable(head, st.expected_path, sizeof(st.expected_path));
    }
    snprintf(st.stdout_path, sizeof(st.stdout_path), "%s/stdout.jsonl", st.op_dir);
    snprintf(st.stderr_path, sizeof(st.stderr_path), "%s/stderr.log", st.op_dir);
    snprintf(st.final_path, sizeof(st.final_path), "%s/final.txt", st.op_dir);
    snprintf(st.prompt_path, sizeof(st.prompt_path), "%s/prompt.txt", st.op_dir);
    snprintf(st.exit_path, sizeof(st.exit_path), "%s/exit_code", st.op_dir);
    st.exit_code = -1;
    st.last_checked_ms = (long long)timing_wall_ms();
    if (spawn_claude_runner(&st, cwd_abs, binary, model, err_obj, sizeof(err_obj)) != 0) {
      snprintf(error, error_len, "%s", err_obj);
      snprintf(result, result_len, "{}");
      return -1;
    }
    (void)write_claude_state(&st);
    if (st.expected_path[0] &&
        workspace_relative_expected(cwd_arg, st.expected_path, rel_expected, sizeof(rel_expected)) == 0)
      snprintf(st.expected_path, sizeof(st.expected_path), "%s", rel_expected);
    {
      const char *sync_ms = getenv("FCLAW_CLAUDE_START_SYNC_MS");
      if (sync_ms && *sync_ms) {
        long wait_ms = strtol(sync_ms, NULL, 10);
        if (wait_ms > 0 && wait_ms <= 5000) {
          usleep((useconds_t)wait_ms * 1000U);
          (void)refresh_claude_state(r, &st);
        }
      }
    }
    (void)claude_result_json(&st, 1, result, result_len);
    snprintf(error, error_len, "null");
    return 0;
  }
  if (strcmp(op, "poll") == 0 || strcmp(op, "status") == 0) {
    if (!op_id[0] || load_claude_state(r, op_id, &st) != 0) {
      snprintf(error, error_len, "{\"message\":\"unknown or missing op_id\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    (void)refresh_claude_state(r, &st);
    if (claude_result_json(&st, 0, result, result_len) != 0) {
      snprintf(error, error_len, "{\"message\":\"claude status result too large\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    snprintf(error, error_len, "null");
    return 0;
  }
  if (strcmp(op, "abort") == 0) {
    if (!op_id[0] || load_claude_state(r, op_id, &st) != 0) {
      snprintf(error, error_len, "{\"message\":\"unknown or missing op_id\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    if (process_alive(st.pid)) (void)kill((pid_t)st.pid, SIGTERM);
    snprintf(st.state, sizeof(st.state), "failed");
    snprintf(st.final_text, sizeof(st.final_text), "Claude operation aborted.");
    st.exit_code = 130;
    st.last_checked_ms = (long long)timing_wall_ms();
    (void)write_claude_state(&st);
    (void)rt_operation_submit_abort_claim(st.op_id,
                                          "Claude operation aborted.");
    (void)claude_result_json(&st, 0, result, result_len);
    snprintf(error, error_len, "null");
    return 0;
  }
  if (strcmp(op, "reply") == 0) {
    snprintf(error, error_len, "{\"message\":\"manage_claude reply is not supported for noninteractive claude --print operations\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(error, error_len, "{\"message\":\"args.op must be start, poll, reply, abort, or status\"}");
  snprintf(result, result_len, "{}");
  return -1;
}
