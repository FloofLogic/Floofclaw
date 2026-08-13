/* runtime/agent_runner.c — agent execution.
 *
 * Owns the three agent executor paths (native / llm / script), the
 * agent-meta cache used to avoid re-reading floop-local agent.json
 * per event, and the LLM mock fixture sequencing for tests. The
 * scheduler dispatches an agent step here; this file decides how
 * the agent runs and how to handle its completion. */

#include "run_internal.h"
#include "agent_exec.h"
#include "agent_errors.h"
#include "agents.h"
#include "llm/llm.h"
#include "support/json.h"
#include "support/heap_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(RT_AGENT_TIMEOUT_MS_DEFAULT >= LLM_AGENT_JOB_BUDGET_MS,
               "agent owner timeout must cover the LLM child budget");

/* Picks the next fixture path out of LLM_MOCK_RESPONSE_PATHS (a
 * colon-separated list, test infrastructure only) using the
 * scheduler-owned index. On success, copies the chosen path into
 * `out` and increments the index. Returns:
 *   0  ok (path written)
 *   1  no PATHS env set (no-op; caller does nothing)
 *  -1  PATHS set but the index has run past the end (loud failure)
 *
 * The child mock provider does NOT know about sequencing; it only
 * reads singular LLM_MOCK_RESPONSE_PATH or LLM_MOCK_RESPONSE. This
 * keeps subprocess isolation intact. */
static int next_mock_fixture_path(RtScheduler *s, char *out, size_t out_len) {
  const char *paths = getenv("LLM_MOCK_RESPONSE_PATHS");
  const char *p;
  int target;
  int cur = 0;
  if (!paths || !*paths) return 1;
  if (!s) return -1;
  target = s->mock_seq_index;
  p = paths;
  while (*p) {
    const char *end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (cur == target) {
      if (len + 1 > out_len) return -1;
      memcpy(out, p, len);
      out[len] = '\0';
      s->mock_seq_index++;
      return 0;
    }
    cur++;
    if (!end) break;
    p = end + 1;
  }
  return -1;
}

static const RtAgentMeta *agent_meta_lookup(const RtScheduler *s, const char *agent_id) {
  if (!s || !agent_id) return NULL;
  for (size_t i = 0; i < s->agent_meta_count; ++i)
    if (strcmp(s->agent_meta[i].id, agent_id) == 0) return &s->agent_meta[i];
  return NULL;
}

static int validate_llm_startup(const RtScheduler *s, const RtAgentMeta *meta,
                                char *err, size_t err_len) {
  if (!meta->model_ref[0]) {
    if (err)
      snprintf(err, err_len,
               "LLM agent '%s' in floop '%s' is missing model.ref; fix: set "
               "model.ref in that agent's agent.json",
               meta->id, s->loop_name);
    return -1;
  }
  return llm_validate_profile_startup(meta->model_ref, err, err_len);
}

int rt_agent_runner_preload_meta(RtScheduler *s, const char *agent_id,
                                 char *err, size_t err_len) {
  RtAgentMeta *m;
  if (s->agent_meta_count >= RT_MAX_STEPS) {
    if (err) snprintf(err, err_len, "agent_meta cache full");
    return -1;
  }
  /* Dedupe: same agent_id may appear in multiple steps. */
  for (size_t i = 0; i < s->agent_meta_count; ++i)
    if (strcmp(s->agent_meta[i].id, agent_id) == 0) return 0;
  m = &s->agent_meta[s->agent_meta_count];
  memset(m, 0, sizeof(*m));
  snprintf(m->id, sizeof(m->id), "%s", agent_id);
  if (rt_agent_read_meta(s->loop_name, agent_id, m->executor, sizeof(m->executor),
                        m->model_ref, sizeof(m->model_ref),
                        err, err_len) != 0)
    return -1;
  if (!m->executor[0]) snprintf(m->executor, sizeof(m->executor), "script");
  if (rt_agent_read_action_allowlist(s->loop_name, agent_id, m->executor, &s->actions, m, err, err_len) != 0)
    return -1;
  if (rt_agent_read_listen_config(s->loop_name, agent_id, m->executor, m, err, err_len) != 0)
    return -1;
  if (strcmp(m->executor, "llm") == 0 &&
      validate_llm_startup(s, m, err, err_len) != 0)
    return -1;
  s->agent_meta_count++;
  return 0;
}

/* Native agent: synthesize the output in-process and return 0 (no job
 * in flight). Caller handles the phase advance. */
static int start_native_agent_inline(RtRun *r, const RtStep *step,
                                     RtAgentInvocation *inv) {
  RtAgentFn fn = rt_agent_lookup(step->target);
  const RtAgentMeta *meta = agent_meta_lookup(r->scheduler, step->target);
  char out_buf[RT_NATIVE_STDOUT_MAX];
  char normalized[RT_XL];
  char err_buf[RT_NATIVE_STDERR_MAX];
  int rc;
  if (!fn) {
    (void)rt_emit_error(&r->ctx, "kernel", "native agent %s not registered", step->target);
    return -1;
  }
  out_buf[0] = '\0'; err_buf[0] = '\0';
  rc = fn(&r->ctx, step, meta, &r->scheduler->actions,
          inv->input, out_buf, sizeof(out_buf), err_buf, sizeof(err_buf));
  if (err_buf[0]) (void)fs_write_text(inv->err_path, err_buf);
  if (rc == RT_ERR_OUTPUT_TOO_LARGE) {
    (void)rt_emit_error(&r->ctx, step->target,
                        "native agent %s output exceeded %d bytes",
                        step->target, RT_NATIVE_STDOUT_MAX);
    return -1;
  }
  if (rc != 0) {
    const char *artifact =
        err_buf[0] ? strstr(inv->err_path, "agent_outputs/") : NULL;
    if (artifact)
      (void)rt_emit_error_artifact(&r->ctx, step->target, artifact,
                                   "native agent %s failed (rc=%d)",
                                   step->target, rc);
    else
      (void)rt_emit_error(&r->ctx, step->target,
                          "native agent %s failed (rc=%d)", step->target, rc);
    return -1;
  }
  (void)fs_write_text(inv->out_path, out_buf);
  if (rt_agent_append_output(&r->ctx, step, meta, inv->bound_task_id,
                             inv->bound_work_rev, out_buf,
                             normalized, sizeof(normalized)) != 0) return -1;
  rt_run_apply_output_events(r, step->target, normalized);
  return 0;
}

static int agent_output_path_seq(const char *path, int *seq,
                                 char *dir, size_t dir_len) {
  const char *base;
  size_t n;
  int parsed = 0;
  if (!path || !seq || !dir || dir_len == 0) return -1;
  base = strrchr(path, '/');
  if (!base || base == path || sscanf(base + 1, "%3d_", &parsed) != 1 ||
      parsed <= 0 || parsed >= 999)
    return -1;
  n = (size_t)(base - path);
  if (n >= dir_len) return -1;
  memcpy(dir, path, n);
  dir[n] = '\0';
  *seq = parsed;
  return 0;
}

static int agent_output_repair_marker_path(const char *output_path,
                                           char *out, size_t out_len) {
  size_t n;
  if (!output_path || !out || out_len == 0) return -1;
  n = strlen(output_path);
  if (n <= 5 || strcmp(output_path + n - 5, ".json") != 0 ||
      n - 5 + strlen(".repair.json") + 1 > out_len)
    return -1;
  memcpy(out, output_path, n - 5);
  snprintf(out + n - 5, out_len - (n - 5), ".repair.json");
  return 0;
}

static int agent_output_repair_attempt(const char *output_path,
                                       int *attempt,
                                       char *reason, size_t reason_len) {
  char marker[PATH_MAX];
  char *text = NULL;
  JsonRef root;
  long long value = 0;
  if (!attempt || !reason || reason_len == 0 ||
      agent_output_repair_marker_path(output_path, marker,
                                      sizeof(marker)) != 0)
    return -1;
  *attempt = 0;
  reason[0] = '\0';
  if (!fs_file_exists(marker)) return 0;
  if (fs_read_text(marker, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text ||
      json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_long(&root, "attempt", &value) != 0 ||
      value <= 0 || value > RT_AGENT_OUTPUT_REPAIR_LIMIT ||
      json_ref_object_get_string(&root, "reason", reason,
                                 reason_len) != 0 || !reason[0]) {
    fc_xfree(text);
    return -1;
  }
  fc_xfree(text);
  *attempt = (int)value;
  return 0;
}

static int apply_agent_output_repair(const RtStep *step,
                                     const RtAgentMeta *meta,
                                     RtAgentInvocation *inv) {
  char dir[PATH_MAX], marker_reason[RT_LARGE], previous_path[PATH_MAX];
  char *previous = NULL;
  char *repaired = NULL;
  int seq = 0, attempt = 0;
  int n;
  if (!step || !meta || !inv || !meta->output_contract[0]) return 0;
  if (agent_output_repair_attempt(inv->out_path, &attempt,
                                  marker_reason,
                                  sizeof(marker_reason)) != 0)
    return -1;
  if (attempt == 0) return 0;
  if (agent_output_path_seq(inv->out_path, &seq, dir, sizeof(dir)) != 0 ||
      seq <= 1 ||
      snprintf(previous_path, sizeof(previous_path), "%s/%03d_%s.json",
               dir, seq - 1, step->id) >= (int)sizeof(previous_path) ||
      fs_read_text(previous_path, &previous,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !previous)
    goto fail;
  repaired = (char *)fc_xmalloc(RT_XL);
  if (!repaired) goto fail;
  n = snprintf(
      repaired, RT_XL,
      "%s\n\n"
      "=== rejected decision; repair attempt %d ===\n"
      "Your previous response was rejected. Nothing from it was executed "
      "or delivered.\n"
      "Reason: %s\n"
      "Previous response (exactly as returned):\n%s\n"
      "Correction required: return one JSON object using exactly one of "
      "these forms. CONTINUE = one substantive action, optionally one "
      "message and one working_memory_append, with no task.update. "
      "FINALIZE = one final message plus task.update for the exact current "
      "task_id with state completed, with no substantive action or "
      "working_memory_append. A message alone is invalid.\n",
      inv->input, attempt, marker_reason, previous);
  if (n < 0 || n >= RT_XL) goto fail;
  snprintf(inv->input, sizeof(inv->input), "%s", repaired);
  if (fs_write_text(inv->in_path, inv->input) != 0) goto fail;
  fc_xfree(repaired);
  fc_xfree(previous);
  return 0;
fail:
  fc_xfree(repaired);
  fc_xfree(previous);
  return -1;
}

static int write_agent_decision_rejection(const RtStep *step,
                                          const RtJob *job,
                                          const char *reason,
                                          int repair_attempt,
                                          int schedule_next) {
  char dir[PATH_MAX], rejected_path[PATH_MAX], next_path[PATH_MAX];
  char ereason[RT_LARGE * 2], eartifact[PATH_MAX * 2];
  char body[RT_LARGE * 3];
  const char *artifact;
  int seq = 0;
  if (!step || !job || !reason ||
      agent_output_path_seq(job->out_path, &seq, dir, sizeof(dir)) != 0 ||
      json_escape(reason, ereason, sizeof(ereason)) != 0)
    return -1;
  artifact = strstr(job->out_path, "agent_outputs/");
  if (!artifact) artifact = job->out_path;
  if (json_escape(artifact, eartifact, sizeof(eartifact)) != 0 ||
      snprintf(rejected_path, sizeof(rejected_path), "%s.rejected.json",
               job->out_path) >= (int)sizeof(rejected_path) ||
      snprintf(body, sizeof(body),
               "{\"contract\":\"task_action_or_finalize\","
               "\"repair_attempt\":%d,\"reason\":\"%s\","
               "\"response_artifact\":\"%s\","
               "\"executed\":false,\"delivered\":false}\n",
               repair_attempt, ereason, eartifact) >= (int)sizeof(body) ||
      fs_write_text_atomic(rejected_path, body) != 0)
    return -1;
  if (!schedule_next) return 0;
  if (snprintf(next_path, sizeof(next_path), "%s/%03d_%s.repair.json",
               dir, seq + 1, step->id) >= (int)sizeof(next_path) ||
      snprintf(body, sizeof(body),
               "{\"contract\":\"task_action_or_finalize\","
               "\"attempt\":%d,\"reason\":\"%s\"}\n",
               repair_attempt + 1, ereason) >= (int)sizeof(body) ||
      fs_write_text_atomic(next_path, body) != 0)
    return -1;
  return 0;
}

/* LLM agent: spawn `fclaw internal agent-llm` so the blocking provider
 * call doesn't run in the reactor. Pipes the next mock fixture path
 * through env if the test harness queued one. */
static int start_llm_agent_job(RtRun *r, const RtStep *step,
                               RtAgentInvocation *inv,
                               const char *model_ref,
                               uint64_t deadline,
                               RtJobRunner *runner) {
  RtJob *job = NULL;
  char mock_path[PATH_MAX];
  int  mock_rc;
  int  launch_rc;
  const char *saved_mock_path = NULL;
  char saved_mock_path_copy[PATH_MAX] = "";
  int  had_saved = 0;
  if (!model_ref[0]) {
    (void)rt_emit_error(&r->ctx, "kernel", "agent %s declares executor=llm but missing model.ref", step->target);
    return -1;
  }
  /* Smoke harness sequencing: parent picks the next fixture token,
   * passes it down as the singular env var, restores parent env after
   * the launch. Loud failure on exhaustion — never wrap or reuse. */
  mock_rc = next_mock_fixture_path(r->scheduler, mock_path, sizeof(mock_path));
  if (mock_rc < 0) {
    (void)rt_emit_error(&r->ctx, "kernel",
                        "LLM mock fixture sequence exhausted before agent %s", step->target);
    return -1;
  }
  if (mock_rc == 0) {
    saved_mock_path = getenv("LLM_MOCK_RESPONSE_PATH");
    if (saved_mock_path) {
      snprintf(saved_mock_path_copy, sizeof(saved_mock_path_copy), "%s", saved_mock_path);
      had_saved = 1;
    }
    setenv("LLM_MOCK_RESPONSE_PATH", mock_path, 1);
  }
  {
    char *argv[20];
    size_t argc = 0;
    argv[argc++] = (char *)rt_exe_path();
    argv[argc++] = (char *)"internal";
    argv[argc++] = (char *)"agent-llm";
    argv[argc++] = (char *)"--agent";
    argv[argc++] = (char *)step->target;
    argv[argc++] = (char *)"--floop";
    argv[argc++] = (char *)r->scheduler->loop_name;
    argv[argc++] = (char *)"--run-dir";
    argv[argc++] = (char *)r->ctx.run_dir;
    argv[argc++] = (char *)"--step-id";
    argv[argc++] = (char *)step->id;
    argv[argc++] = (char *)"--in";
    argv[argc++] = (char *)inv->in_path;
    argv[argc++] = (char *)"--out";
    argv[argc++] = (char *)inv->out_path;
    if (inv->media_manifest_path[0]) {
      argv[argc++] = (char *)"--media";
      argv[argc++] = inv->media_manifest_path;
    }
    argv[argc] = NULL;
    RtJobSpec spec = {
      .argv = argv, .stdin_path = NULL,
      .stdout_path = inv->out_path, .stderr_path = inv->err_path,
      .kind = JOB_AGENT, .run_id = r->ctx.run_id, .step_id = step->id,
      .origin_event_id = r->ctx.origin_event_id,
      .request_id = NULL, .bound_task_id = inv->bound_task_id,
      .bound_work_rev = inv->bound_work_rev,
      .deadline_ms = deadline,
    };
    launch_rc = rt_run_start_tracked_job(r, runner, &spec, &job);
  }
  if (mock_rc == 0) {
    if (had_saved) setenv("LLM_MOCK_RESPONSE_PATH", saved_mock_path_copy, 1);
    else           unsetenv("LLM_MOCK_RESPONSE_PATH");
  }
  return launch_rc == 0 ? 1 : -1;
}

static int start_script_agent_job(RtRun *r, const RtStep *step,
                                  RtAgentInvocation *inv,
                                  uint64_t deadline,
                                  RtJobRunner *runner) {
  if (access(inv->script_path, X_OK) != 0) {
    (void)rt_emit_error(&r->ctx, "kernel", "agent %s run.sh missing or not executable", step->target);
    return -1;
  }
  {
    char *argv[] = { inv->script_path, NULL };
    RtJobSpec spec = {
      .argv = argv, .stdin_path = inv->in_path,
      .stdout_path = inv->out_path, .stderr_path = inv->err_path,
      .kind = JOB_AGENT, .run_id = r->ctx.run_id, .step_id = step->id,
      .origin_event_id = r->ctx.origin_event_id,
      .request_id = NULL, .bound_task_id = inv->bound_task_id,
      .bound_work_rev = inv->bound_work_rev,
      .deadline_ms = deadline,
    };
    if (rt_run_start_tracked_job(r, runner, &spec, NULL) != 0) return -1;
  }
  return 1;
}

int rt_agent_runner_start_step(RtRun *r, const RtStep *step, RtJobRunner *runner) {
  const RtAgentMeta *meta = agent_meta_lookup(r->scheduler, step->target);
  RtAgentInvocation inv;
  uint64_t deadline;
  if (!meta) {
    (void)rt_emit_error(&r->ctx, "kernel", "agent meta not preloaded: %s", step->target);
    return -1;
  }
  if (meta->bind_task_open_work) {
    char task_id[RT_SMALL];
    long long work_rev = 0;
    int consequence_gate =
        strcmp(step->gate, "work_consequence") == 0;
    int sr = consequence_gate
           ? rt_task_select_work_consequence(&r->ctx,
                                             task_id, sizeof(task_id),
                                             &work_rev, NULL, 0)
           : rt_task_select_unique_open_work(r->ctx.context_id,
                                             task_id, sizeof(task_id),
                                             &work_rev);
    if (sr == 1 || (consequence_gate &&
                    (sr == 3 || sr == 4 || sr == 5))) {
      (void)rt_write_phase_skipped_trace(&r->ctx, r->advance, step->id,
          sr == 3 ? "stale_work_revision" :
          sr == 4 ? "uncorrelated_work_result" :
          sr == 5 ? "work_consequence_consumed" :
                    "no_work_consequence");
      return 0;
    }
    if (sr == 2) {
      (void)rt_emit_error(&r->ctx, "kernel",
                          "agent %s requires exactly one current work consequence; found multiple",
                          step->target);
      return -1;
    }
    if (sr < 0) {
      (void)rt_emit_error(&r->ctx, "kernel",
                          "agent %s work-task binding lookup failed",
                          step->target);
      return -1;
    }
    if (consequence_gate) {
      RtWorkBudgetSnapshot budget;
      int br = rt_work_state_budget_snapshot(task_id, work_rev, &budget);
      if (br != 0 ||
          strcmp(budget.context_id, r->ctx.context_id) != 0) {
        (void)rt_emit_error(&r->ctx, "kernel",
                            "agent %s work budget lookup failed",
                            step->target);
        return -1;
      }
      if (budget.semantic_steps >= RT_WORK_SEMANTIC_STEP_LIMIT) {
        char blocker[RT_MED];
        int blocked;
        if (!budget.request_id[0] || !budget.action[0] ||
            !budget.trigger_event_id[0]) {
          (void)rt_emit_error(&r->ctx, "kernel",
                              "agent %s exhausted work budget lacks "
                              "originating selection identity",
                              step->target);
          return -1;
        }
        snprintf(blocker, sizeof(blocker),
                 "semantic step budget exhausted after %d selections",
                 RT_WORK_SEMANTIC_STEP_LIMIT);
        blocked = rt_task_block_bound_work(
            &r->ctx, task_id, work_rev, budget.request_id,
            budget.action, budget.trigger_event_id, blocker,
            "A new external instruction must revise the work before "
            "another semantic selection.");
        if (blocked < 0) {
          (void)rt_emit_error(&r->ctx, "kernel",
                              "agent %s failed to persist exhausted "
                              "work budget blocker",
                              step->target);
          return -1;
        }
        (void)rt_write_phase_skipped_trace(
            &r->ctx, r->advance, step->id,
            "semantic_step_budget_exhausted");
        return 0;
      }
      {
        char repair_request_id[RT_SMALL] = "";
        char repair_source_event_id[RT_SMALL] = "";
        long long repair_work_rev = work_rev;
        int repair = 0;
        snprintf(repair_request_id, sizeof(repair_request_id), "%s",
                 budget.request_id);
        if (strcmp(r->ctx.event_kind, "work_step_result") == 0 ||
            strcmp(r->ctx.event_kind, "operation_result") == 0) {
          JsonRef wake;
          if (json_ref_top_object(r->ctx.event_payload_json, &wake) != 0 ||
              json_ref_object_get_string(&wake, "request_id",
                                         repair_request_id,
                                         sizeof(repair_request_id)) != 0 ||
              json_ref_object_get_string(&wake, "source_event_id",
                                         repair_source_event_id,
                                         sizeof(repair_source_event_id)) != 0 ||
              json_ref_object_get_long(&wake, "work_rev",
                                       &repair_work_rev) != 0) {
            (void)rt_emit_error(&r->ctx, "kernel",
                                "agent %s work consequence lacks repair "
                                "correlation",
                                step->target);
            return -1;
          }
        }
        if (repair_request_id[0])
          repair = rt_work_state_repair_disposition(
              task_id, repair_work_rev, repair_request_id,
              repair_source_event_id, meta->max_repair_attempts);
        if (repair < 0) {
          (void)rt_emit_error(&r->ctx, "kernel",
                              "agent %s work repair lookup failed",
                              step->target);
          return -1;
        }
        if (repair == 2) {
          char blocker[RT_MED];
          int blocked;
          snprintf(blocker, sizeof(blocker),
                   "configured repair budget exhausted after %d repair "
                   "attempt%s",
                   meta->max_repair_attempts,
                   meta->max_repair_attempts == 1 ? "" : "s");
          blocked = rt_task_block_bound_work(
              &r->ctx, task_id, work_rev, budget.request_id,
              budget.action, budget.trigger_event_id, blocker,
              "A new external instruction must revise the work before "
              "another repair.");
          if (blocked < 0) {
            (void)rt_emit_error(&r->ctx, "kernel",
                                "agent %s failed to persist exhausted "
                                "repair blocker",
                                step->target);
            return -1;
          }
          (void)rt_write_phase_skipped_trace(
              &r->ctx, r->advance, step->id,
              "work_repair_exhausted");
          return 0;
        }
      }
    }
    if (rt_agent_prepare_invocation(&r->ctx, r->scheduler->loop_name, step,
                                    meta->executor, &r->scheduler->actions,
                                    meta, task_id, work_rev, &inv) != 0)
      return -1;
  } else if (meta->affair_extraction_context_only) {
    char task_id[RT_SMALL];
    int sr;
    const char *skip_reason;
    sr = rt_task_select_new_input_for_run(r->ctx.context_id, r->ctx.run_id,
                                          task_id, sizeof(task_id));
    skip_reason = "no_matching_task";
    if (sr > 0) {
      (void)rt_write_phase_skipped_trace(&r->ctx, r->advance, step->id, skip_reason);
      return 0;
    }
    if (sr < 0) {
      (void)rt_emit_error(&r->ctx, "kernel", "agent %s selected task lookup failed", step->target);
      return -1;
    }
    if (rt_agent_prepare_invocation(&r->ctx, r->scheduler->loop_name, step, meta->executor,
                                    &r->scheduler->actions, meta, task_id, 0, &inv) != 0) return -1;
  } else if (rt_agent_prepare_invocation(&r->ctx, r->scheduler->loop_name, step, meta->executor,
                                         &r->scheduler->actions, meta, NULL, 0, &inv) != 0) {
    return -1;
  }
  if (strcmp(meta->executor, "llm") == 0 &&
      apply_agent_output_repair(step, meta, &inv) != 0) {
    (void)rt_emit_error(&r->ctx, "kernel",
                        "agent %s could not render configured output-contract repair feedback",
                        step->target);
    return -1;
  }
  deadline = fc_now_ms() + RT_AGENT_TIMEOUT_MS_DEFAULT;

  if (strcmp(meta->executor, "native") == 0) return start_native_agent_inline(r, step, &inv);
  if (strcmp(meta->executor, "llm")    == 0) return start_llm_agent_job(r, step, &inv, meta->model_ref, deadline, runner);
  if (strcmp(meta->executor, "script") == 0) return start_script_agent_job(r, step, &inv, deadline, runner);
  (void)rt_emit_error(&r->ctx, "kernel",
                      "agent %s declares unknown executor %s",
                      step->target, meta->executor);
  return -1;
}

int rt_agent_runner_complete_job(RtRun *r, RtJob *job) {
  const RtStep *step = rt_run_find_step_by_id(r->profile, job->step_id);
  const char *agent_name = step ? step->target : (job->step_id[0] ? job->step_id : "agent");
  const RtAgentMeta *meta = step ? agent_meta_lookup(r->scheduler, step->target) : NULL;
  char normalized[RT_XL];
  if (job->state == JOB_FAILED) {
    char *stderr_text = NULL;
    char domain[RT_SMALL];
    char message[RT_MED];
    char esc_message[RT_MED * 2];
    char p[RT_LARGE];
    FcErrorCode class_code;
    (void)fs_read_text(job->err_path, &stderr_text, FS_READ_TEXT_DEFAULT_CAP);
    class_code = rt_classify_agent_failure(job, stderr_text, domain, sizeof(domain),
                                           message, sizeof(message));
    rt_run_set_error_code(r, class_code, domain, message, job->job_id);
    if (json_escape(message, esc_message, sizeof(esc_message)) != 0) esc_message[0] = '\0';
    {
      /* Run-relative stderr artifact for this exact attempt, so `view -d`
       * resolves the numbered file instead of guessing among attempts. */
      const char *artifact = strstr(job->err_path, "agent_outputs/");
      char eart[RT_MED * 2];
      if (!artifact || !stderr_text || !stderr_text[0] ||
          json_escape(artifact, eart, sizeof(eart)) != 0)
        eart[0] = '\0';
      (void)rt_append_event_format(
          &r->ctx, "error", agent_name, p, sizeof(p), 0,
          "{\"class\":%d,\"code\":\"%s\",\"message\":\"%s\","
          "\"agent\":\"%s\",\"exit_status\":%d,\"timeout\":%s,\"output_limit\":%s,"
          "\"artifact\":\"%s\"}",
          (int)class_code, domain, esc_message, agent_name, job->exit_status,
          job->killed_for_timeout ? "true" : "false",
          job->killed_for_output_limit ? "true" : "false", eart);
    }
    fc_xfree(stderr_text);
    return -1;
  }
  if (step && meta && meta->output_contract[0]) {
    char *output = NULL;
    char reason[RT_LARGE] = "";
    char marker_reason[RT_LARGE] = "";
    int repair_attempt = 0;
    int contract_rc;
    if (fs_read_text(job->out_path, &output,
                     FS_READ_TEXT_DEFAULT_CAP) != 0 || !output) {
      rt_run_set_error_code(r, FC_ERROR_MALFORMED, "agent_output_invalid",
                            "Agent output was missing or malformed.",
                            job->job_id);
      fc_xfree(output);
      return -1;
    }
    contract_rc = rt_agent_validate_output_contract(
        &r->ctx, meta, output, reason, sizeof(reason));
    fc_xfree(output);
    if (contract_rc < 0) {
      int can_repair;
      if (agent_output_repair_attempt(job->out_path, &repair_attempt,
                                      marker_reason,
                                      sizeof(marker_reason)) != 0) {
        rt_run_set_error_code(
            r, FC_ERROR_RUNTIME, "agent_decision_repair_state_invalid",
            "Agent decision repair state was missing or malformed.",
            job->job_id);
        return -1;
      }
      can_repair = repair_attempt < meta->output_repair_attempts;
      if (write_agent_decision_rejection(step, job, reason,
                                         repair_attempt,
                                         can_repair) != 0) {
        rt_run_set_error_code(
            r, FC_ERROR_RUNTIME, "agent_decision_repair_state_failed",
            "Agent decision rejection could not be preserved.",
            job->job_id);
        return -1;
      }
      rt_log_rejected_agent_event(step->target,
                                  "agent_decision_contract_rejected",
                                  reason);
      if (can_repair) {
        char detail[RT_LARGE];
        if (r->phase_index == 0) {
          rt_run_set_error_code(
              r, FC_ERROR_RUNTIME, "agent_decision_repair_cursor_invalid",
              "Agent decision repair could not rewind its phase.",
              job->job_id);
          return -1;
        }
        snprintf(detail, sizeof(detail),
                 "output contract rejected decision; repair %d of %d: %s",
                 repair_attempt + 1, meta->output_repair_attempts, reason);
        (void)rt_write_trace(&r->ctx, step->id, detail);
        r->phase_index--;
        if (rt_run_persist_state(r) != 0) {
          rt_run_set_error_code(
              r, FC_ERROR_RUNTIME, "agent_decision_repair_state_failed",
              "Agent decision repair cursor could not be persisted.",
              job->job_id);
          return -1;
        }
        return 0;
      }
      rt_run_set_error_code(
          r, FC_ERROR_MALFORMED, "agent_decision_contract_exhausted",
          reason[0] ? reason : "Agent decision contract repair was exhausted.",
          job->job_id);
      return -1;
    }
  }
  if (!step ||
      rt_agent_append_output_file(&r->ctx, step, meta ? meta->executor : "script",
                                  meta, job->bound_task_id,
                                  job->bound_work_rev,
                                  job->out_path, normalized, sizeof(normalized)) != 0) {
    rt_run_set_error_code(r, FC_ERROR_MALFORMED, "agent_output_invalid",
                          "Agent output was missing or malformed.", job->job_id);
    return -1;
  }
  /* Mirror the agent's emitted events into our in-memory state. */
  rt_run_apply_output_events(r, agent_name, normalized);
  return 0;
}
