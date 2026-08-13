/* Subprocess action launch.
 *
 * Actions that aren't runtime intrinsics fork+exec a run.sh under their
 * action dir. This file owns the input-envelope build, the
 * action_calls/<seq>_<rid>.* path naming, the readable-exec check, and
 * the rt_run_start_tracked_job spec construction. The terminal event
 * is emitted later by action_events.c when the jobrunner reports the
 * child exited. */

#include "action_internal.h"
#include "support/json.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int rt_action_call_paths_next(const RtRun *r, const char *rid,
                             RtActionCallPaths *out) {
  int seq = 1;
  const char *runs;
  if (!r || !rid || !*rid || !out) return -1;
  while (seq < 1000) {
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe),
             "%s/action_calls/%03d_%s.input.json", r->ctx.run_dir, seq, rid);
    if (!fs_file_exists(probe)) break;
    seq++;
  }
  out->seq = seq;
  snprintf(out->in_path,   sizeof(out->in_path),   "%s/action_calls/%03d_%s.input.json",  r->ctx.run_dir, seq, rid);
  snprintf(out->out_path,  sizeof(out->out_path),  "%s/action_calls/%03d_%s.output.json", r->ctx.run_dir, seq, rid);
  snprintf(out->err_path,  sizeof(out->err_path),  "%s/action_calls/%03d_%s.stderr",      r->ctx.run_dir, seq, rid);
  snprintf(out->call_id,   sizeof(out->call_id),   "%03d_%s", seq, rid);
  snprintf(out->body_path, sizeof(out->body_path), "%s/action_calls/%s.body.html",        r->ctx.run_dir, out->call_id);
  runs = strstr(out->body_path, "/runs/");
  if (!runs) return -1;
  snprintf(out->body_rel_path, sizeof(out->body_rel_path), "%s",
           runs + 1);
  return 0;
}

static int workspace_root_from_run_dir(const char *run_dir,
                                       char *out, size_t out_len) {
  const char *runs;
  size_t n;
  if (!run_dir || !*run_dir || !out || out_len == 0) return -1;
  runs = strstr(run_dir, "/runs/");
  if (!runs || runs == run_dir) return -1;
  n = (size_t)(runs - run_dir);
  if (n >= out_len) return -1;
  memcpy(out, run_dir, n);
  out[n] = '\0';
  return 0;
}

static int emit_start_failure(RtRun *r, const char *rid,
                              const RtActionDef *def,
                              const char *task_id,
                              const char *reason,
                              const char *detail) {
  char emessage[RT_LARGE];
  char error[RT_LARGE];
  if (!r || !rid || !def || !reason || !detail ||
      json_escape(detail, emessage, sizeof(emessage)) != 0)
    return -1;
  if (snprintf(error, sizeof(error), "{\"message\":\"%s\"}",
               emessage) >= (int)sizeof(error))
    return -1;
  return rt_action_emit_terminal(r, "action_failed", rid, def->id, NULL,
                                 task_id, reason, "{}", error);
}

int rt_action_subprocess_launch(RtRun *r, const char *rid, const RtActionDef *def,
                               const char *task_id, const char *args,
                               RtJobRunner *runner) {
  char body_path_esc[PATH_MAX * 2], input[RT_LARGE * 2];
  char exec_path[PATH_MAX], workspace_root[PATH_MAX];
  RtActionCallPaths paths;
  RtJob *job = NULL;
  uint64_t deadline;
  if (rt_jobrunner_count_active(runner, JOB_ACTION) > 0) return 1;
  if (rt_action_call_paths_next(r, rid, &paths) != 0)
    return emit_start_failure(
        r, rid, def, task_id, "action_artifact_path_failed",
        "action call artifact paths could not be allocated");
  if (workspace_root_from_run_dir(r->ctx.run_dir, workspace_root,
                                  sizeof(workspace_root)) != 0)
    return emit_start_failure(
        r, rid, def, task_id, "action_workspace_invalid",
        "action workspace root could not be resolved");
  if (json_escape(paths.body_rel_path, body_path_esc, sizeof(body_path_esc)) != 0) body_path_esc[0] = '\0';
  snprintf(input, sizeof(input),
           "{\"request_id\":\"%s\",\"action\":\"%s\",\"args\":%s,\"run_id\":\"%s\","
           "%s%s%s"
           "\"call_id\":\"%s\",\"artifacts\":{\"body\":\"%s\"}}\n",
           rid, def->id, args && args[0] ? args : "{}", r->ctx.run_id,
           task_id && *task_id ? "\"task_id\":\"" : "",
           task_id && *task_id ? task_id : "",
           task_id && *task_id ? "\"," : "",
           paths.call_id, body_path_esc);
  if (fs_write_text(paths.in_path, input) != 0)
    return emit_start_failure(
        r, rid, def, task_id, "action_input_write_failed",
        "action input artifact could not be written");
  snprintf(exec_path, sizeof(exec_path), "%s/%s", def->dir, def->exec_arg);
  deadline = fc_now_ms() + (def->timeout_ms > 0 ? (uint64_t)def->timeout_ms : RT_ACTION_TIMEOUT_MS_DEFAULT);
  if (access(exec_path, R_OK) != 0) {
    char error[RT_LARGE];
    snprintf(error, sizeof(error),
             "{\"message\":\"action %s missing executable %s\"}", def->id, def->exec_arg);
    return rt_action_emit_terminal(
        r, "action_failed", rid, def->id, NULL, task_id,
        "missing_executable", "{}", error);
  }
  /* This synchronous claim is the crash-recovery boundary. It must be
   * durable before fork, because the child may perform an outside-world
   * effect immediately after exec. A post-claim launch failure is converted
   * to the correlated terminal below. */
  if (rt_action_emit_started(r, rid, def->id, NULL, task_id) != 0)
    return -1;
  {
    char *argv[] = { (char *)def->exec_argv0, exec_path, NULL };
    /* Declared config resolved from config/floofclaw_config.json
     * actions.<id>. Stack-owned: fork copies it into the child, and the
     * launch call returns after the fork. */
    char cfg_buf[RT_ACTION_CONFIG_MAX][RT_MED];
    char *cfg_env[RT_ACTION_CONFIG_MAX + 1];
    int cfg_n = rt_action_resolve_config(def, cfg_buf, sizeof(cfg_buf[0]));
    int ci;
    if (cfg_n < 0) {
      char error[RT_LARGE];
      snprintf(error, sizeof(error),
               "{\"message\":\"action %s is missing required config; set it "
               "under actions.%s in config/floofclaw_config.json\"}",
               def->id, def->id);
      return rt_action_emit_terminal(
          r, "action_failed", rid, def->id, NULL, task_id,
          "config_missing", "{}", error);
    }
    for (ci = 0; ci < cfg_n; ++ci) cfg_env[ci] = cfg_buf[ci];
    cfg_env[cfg_n] = NULL;
    RtJobSpec spec = {
      .argv = argv, .stdin_path = paths.in_path,
      .stdout_path = paths.out_path, .stderr_path = paths.err_path,
      .kind = JOB_ACTION, .run_id = r->ctx.run_id, .step_id = NULL,
      .origin_event_id = r->ctx.origin_event_id,
      .request_id = rid, .workspace_root = workspace_root,
      .action_id = def->id,
      .private_channel = def->private_credentials,
      .env_extra = cfg_env,
      .deadline_ms = deadline,
    };
    if (rt_run_start_tracked_job(r, runner, &spec, &job) != 0)
      return emit_start_failure(
          r, rid, def, task_id, "action_start_failed",
          "action process could not be started");
  }
  return 0;
}
