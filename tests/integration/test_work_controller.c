#include "../test_support.h"

#include "../../runtime/action_internal.h"
#include "../../runtime/agent_exec.h"
#include "../../runtime/run_internal.h"
#include "../../runtime/runtime.h"
#include "../../runtime/runtime_kernel.h"
#include "../../runtime/support/fsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int a2_init_context(RtContext *ctx, const char *run_id,
                           const char *context_id) {
  char path[PATH_MAX];
  if (!ctx || !run_id || !context_id) return -1;
  memset(ctx, 0, sizeof(*ctx));
  snprintf(ctx->workspace, sizeof(ctx->workspace), "workspace");
  snprintf(ctx->run_id, sizeof(ctx->run_id), "%s", run_id);
  snprintf(ctx->run_dir, sizeof(ctx->run_dir), "workspace/runs/%s", run_id);
  snprintf(ctx->context_id, sizeof(ctx->context_id), "%s", context_id);
  snprintf(ctx->origin_event_id, sizeof(ctx->origin_event_id), "bus_origin_001");
  snprintf(ctx->origin_channel, sizeof(ctx->origin_channel), "tests");
  snprintf(ctx->origin_adapter_id, sizeof(ctx->origin_adapter_id), "adapter_a2");
  snprintf(ctx->origin_ref_json, sizeof(ctx->origin_ref_json),
           "{\"thread\":\"thread-a2\"}");
  snprintf(ctx->event_kind, sizeof(ctx->event_kind), "user_message");
  snprintf(ctx->event_payload_json, sizeof(ctx->event_payload_json),
           "{\"text\":\"do the work\",\"marker\":\"trigger-a2\"}");
  snprintf(ctx->user_text, sizeof(ctx->user_text), "do the work");
  ctx->next_event = 1;
  if (test_mkdir_p(ctx->run_dir) != 0) return -1;
  snprintf(path, sizeof(path), "%s/agent_outputs", ctx->run_dir);
  if (test_mkdir_p(path) != 0) return -1;
  snprintf(path, sizeof(path), "%s/action_calls", ctx->run_dir);
  return test_mkdir_p(path);
}

static int a2_create_work(RtContext *ctx, const char *task_id,
                          const char *context_id, const char *affair_id) {
  char payload[RT_XL];
  char etask[RT_MED], ectx[RT_LARGE], eaffair[RT_MED];
  long long ms = 1000;
  if (!ctx || !task_id || !context_id ||
      json_escape(task_id, etask, sizeof(etask)) != 0 ||
      json_escape(context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(affair_id ? affair_id : "", eaffair, sizeof(eaffair)) != 0)
    return -1;
  return rt_append_event_format(
      ctx, "task_created", "a2_test", payload, sizeof(payload), 0,
      "{\"task_id\":\"%s\",\"context_id\":\"%s\",\"kind\":\"work\","
      "\"work_rev\":1,\"input\":null,"
      "\"created_event_id\":\"created_%s\",\"created_ms\":%lld,"
      "\"state\":{\"status\":\"open\",\"work\":\"research the answer\","
      "\"done_when\":\"verified answer exists\",\"work_rev\":1,"
      "\"artifacts\":[{\"artifact_id\":\"art_a2\",\"parts\":[],\"work_rev\":1}],"
      "\"affair_id\":\"%s\",\"external\":{\"handle\":\"op_a2\"},"
      "\"updated_ms\":%lld}}",
      etask, ectx, etask, ms, eaffair, ms);
}

static RtScheduler *a2_scheduler(const char *agent_id, int bound,
                                 char *err, size_t err_len) {
  RtScheduler *scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  RtAgentMeta *meta;
  if (!scheduler) return NULL;
  if (rt_action_registry_load(&scheduler->actions, err, err_len) != 0) {
    free(scheduler);
    return NULL;
  }
  scheduler->agent_meta_count = 1;
  meta = &scheduler->agent_meta[0];
  snprintf(meta->id, sizeof(meta->id), "%s", agent_id);
  meta->bind_task_open_work = bound;
  meta->max_repair_attempts = RT_WORK_REPAIR_ATTEMPT_DEFAULT;
  return scheduler;
}

static RtRun *a2_run(RtScheduler *scheduler, const RtContext *ctx) {
  RtRun *run = (RtRun *)calloc(1, sizeof(*run));
  if (!run) return NULL;
  run->scheduler = scheduler;
  run->ctx = *ctx;
  return run;
}

static void a2_run_free(RtRun *run) {
  if (!run) return;
  while (run->pending_action_count > 0)
    rt_action_pending_remove_at(run, run->pending_action_count - 1);
  free(run);
}

static int a2_append_controller_call(RtRun *run, const RtStep *step,
                                     const RtAgentMeta *meta,
                                     const char *task_id, long long work_rev,
                                     const char *output) {
  char committed[RT_XL];
  if (rt_agent_append_output(&run->ctx, step, meta, task_id, work_rev,
                             output, committed, sizeof(committed)) != 0)
    return -1;
  rt_run_apply_output_events(run, step->target, committed);
  return 0;
}

int semantic_selection_is_durable_before_dispatch(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  char err[RT_LARGE] = "";
  char projection[RT_XL];
  char *log = NULL;
  unsigned long sync_before;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_501",
                               "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_selection_fail", "chat:a5",
                              "affair_a5") == 0,
               "create failed-selection durability fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize failed-selection runner");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  snprintf(step.gate, sizeof(step.gate), "work_consequence");
  fs_test_reset_append_sync();
  fs_test_fail_next_append_sync();
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_selection_fail", 1,
                     "{\"calls\":[{\"name\":\"work_complete\","
                     "\"args\":{\"summary\":\"must not dispatch\"}}]}") != 0,
                 "failed semantic-selection fsync is reported");
    rc |= expect(fs_test_append_sync_attempts() == 1,
                 "semantic selection uses the synchronous append path");
    rc |= expect(run->pending_action_count == 0,
                 "failed selection persistence leaves nothing dispatchable");
    rc |= test_read_file(
        "workspace/runs/run_501/event_log.jsonl", &log);
    rc |= expect_substr(log, "\"type\":\"action_request\"",
                        "failed fsync reached only the selection write");
    rc |= expect_no_substr(log, "\"type\":\"action_started\"",
                           "failed selection persistence starts no action");
  }
  free(log);
  log = NULL;
  a2_run_free(run);
  free(scheduler);
  run = NULL;
  scheduler = NULL;
  fs_test_reset_append_sync();

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_502",
                               "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_selection_commit", "chat:a5",
                              "affair_a5") == 0,
               "create committed-selection durability fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize committed-selection runner");
  sync_before = fs_test_append_sync_attempts();
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_selection_commit", 1,
                     "{\"calls\":[{\"name\":\"work_complete\","
                     "\"args\":{\"summary\":\"durable decision\"}}]}") == 0,
                 "commit one semantic controller selection");
    rc |= expect(fs_test_append_sync_attempts() == sync_before + 1,
                 "committed semantic selection is fsynced exactly once");
    rc |= expect(run->pending_action_count == 1,
                 "dispatch queue receives only the committed selection");
    rc |= expect(test_remove_path(
                     "workspace/memory/state/work_steps.json") == 0,
                 "remove materialized selection view before rebuild");
    rc |= expect(rt_work_state_rebuild_from_logs() == 0,
                 "rebuild selection view from committed event truth");
    rc |= expect(rt_work_state_projection_json(
                     "task_a5_selection_commit", 1,
                     RT_WORK_REPAIR_ATTEMPT_DEFAULT,
                     projection, sizeof(projection)) == 0,
                 "project rebuilt semantic selection");
    rc |= expect_substr(projection, "\"status\":\"selected\"",
                        "rebuilt view retains selected state");
    rc |= expect_substr(projection, "\"action\":\"work_complete\"",
                        "rebuilt view retains selected action");
    rc |= expect_substr(projection,
                        "\"trigger_event_id\":"
                        "\"evt_run_502_000001\"",
                        "rebuilt view retains exact controller consequence");
  }
  a2_run_free(run);
  free(scheduler);
  fs_test_reset_append_sync();
  rc |= test_reset_workspace();
  return rc;
}

static int a5_append_finished_selection(RtContext *ctx,
                                        const char *task_id,
                                        long long work_rev,
                                        int ordinal,
                                        char *result_event_id,
                                        size_t result_event_id_len) {
  char payload[RT_XL];
  char request_id[RT_SMALL];
  char trigger_event_id[RT_SMALL];
  if (!ctx || !task_id || work_rev <= 0 || ordinal <= 0 ||
      !result_event_id || result_event_id_len == 0)
    return -1;
  snprintf(request_id, sizeof(request_id), "request_budget_%02d", ordinal);
  snprintf(trigger_event_id, sizeof(trigger_event_id),
           "wake_budget_%02d", ordinal);
  if (rt_append_event_format(
          ctx, "action_request", "controller", payload, sizeof(payload), 1,
          "{\"request_id\":\"%s\",\"action\":\"note_add\","
          "\"args\":{\"note\":\"budget step %d\"},"
          "\"task_id\":\"%s\",\"work_rev\":%lld,"
          "\"context_id\":\"%s\",\"trigger_event_id\":\"%s\"}",
          request_id, ordinal, task_id, work_rev, ctx->context_id,
          trigger_event_id) != 0)
    return -1;
  snprintf(result_event_id, result_event_id_len, "evt_%s_%06d",
           ctx->run_id, ctx->next_event);
  return rt_append_event_format(
      ctx, "action_succeeded", "action_runner", payload, sizeof(payload), 0,
      "{\"request_id\":\"%s\",\"action\":\"note_add\","
      "\"task_id\":\"%s\",\"work_rev\":%lld,"
      "\"context_id\":\"%s\",\"trigger_event_id\":\"%s\","
      "\"source_event_id\":\"%s\",\"result\":{\"ok\":true}}",
      request_id, task_id, work_rev, ctx->context_id, trigger_event_id,
      result_event_id);
}

static int a5_append_rejected_selection(RtContext *ctx,
                                        const char *task_id,
                                        long long work_rev,
                                        int ordinal,
                                        const char *trigger_event_id,
                                        const char *problem,
                                        char *result_event_id,
                                        size_t result_event_id_len) {
  char payload[RT_XL];
  char request_id[RT_SMALL];
  char eproblem[RT_LARGE];
  if (!ctx || !task_id || work_rev <= 0 || ordinal <= 0 ||
      !trigger_event_id || !*trigger_event_id ||
      !problem || !*problem ||
      !result_event_id || result_event_id_len == 0 ||
      json_escape(problem, eproblem, sizeof(eproblem)) != 0)
    return -1;
  snprintf(request_id, sizeof(request_id), "request_repair_%02d", ordinal);
  if (rt_append_event_format(
          ctx, "action_request", "controller", payload, sizeof(payload), 1,
          "{\"request_id\":\"%s\",\"action\":\"gcal\","
          "\"args\":{\"calendar_id\":\"\"},"
          "\"task_id\":\"%s\",\"work_rev\":%lld,"
          "\"context_id\":\"%s\",\"trigger_event_id\":\"%s\"}",
          request_id, task_id, work_rev, ctx->context_id,
          trigger_event_id) != 0)
    return -1;
  snprintf(result_event_id, result_event_id_len, "evt_%s_%06d",
           ctx->run_id, ctx->next_event);
  return rt_append_event_format(
      ctx, "action_rejected", "action_runner", payload, sizeof(payload), 0,
      "{\"request_id\":\"%s\",\"action\":\"gcal\","
      "\"task_id\":\"%s\",\"work_rev\":%lld,"
      "\"context_id\":\"%s\",\"trigger_event_id\":\"%s\","
      "\"source_event_id\":\"%s\",\"reason\":\"schema_validation\","
      "\"message\":\"%s\",\"errors\":[{\"path\":\"calendar_id\","
      "\"message\":\"%s\"}]}",
      request_id, task_id, work_rev, ctx->context_id, trigger_event_id,
      result_event_id, eproblem, eproblem);
}

int configured_repair_budget_then_mechanical_block(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  RtWorkBudgetSnapshot budget;
  char err[RT_LARGE] = "";
  char first_result[RT_SMALL] = "";
  char repair_result[RT_SMALL] = "";
  char second_repair_result[RT_SMALL] = "";
  char repair_input[RT_XL];
  char *log = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_504", "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_repair", "chat:a5",
                              "affair_a5") == 0,
               "create configurable-repair work fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize configurable-repair runner");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  snprintf(step.gate, sizeof(step.gate), "work_consequence");

  if (run && scheduler) {
    scheduler->agent_meta[0].max_repair_attempts = 2;
    rc |= expect(a5_append_rejected_selection(
                     &ctx, "task_a5_repair", 1, 1,
                     "wake_repair_initial",
                     "calendar_id is required; provide a calendar id",
                     first_result, sizeof(first_result)) == 0,
                 "persist initial rejected selection");
    rc |= expect(rt_work_state_repair_disposition(
                     "task_a5_repair", 1, "request_repair_01",
                     first_result, 2) == 1,
                 "first rejection permits a configured repair");
    rc |= expect(rt_build_processor_input(
                     &ctx, &scheduler->agent_meta[0],
                     "task_a5_repair", 1,
                     repair_input, sizeof(repair_input)) == 0,
                 "build correlated repair-turn input");
    rc |= expect_substr(repair_input, "request_repair_01",
                        "repair input carries trusted request id");
    rc |= expect_substr(repair_input, "\"action\":\"gcal\"",
                        "repair input carries trusted action");
    rc |= expect_substr(repair_input, "calendar_id is required",
                        "repair input carries actionable validation detail");
    rc |= expect_substr(repair_input,
                        "\"repair_state\":\"repair_available\"",
                        "repair input says repair is available");
    rc |= expect_substr(repair_input, "\"repair_attempts_max\":2",
                        "repair input carries the agent-configured maximum");
    rc |= expect_substr(repair_input, "\"repair_attempts_remaining\":2",
                        "repair input carries the remaining repair budget");

    rc |= expect(a5_append_rejected_selection(
                     &ctx, "task_a5_repair", 1, 2, first_result,
                     "calendar_id is still missing after repair",
                     repair_result, sizeof(repair_result)) == 0,
                 "persist the first repair selection and rejection");
    rc |= expect(rt_work_state_budget_snapshot(
                     "task_a5_repair", 1, &budget) == 0 &&
                 budget.repair_attempts == 1 &&
                 budget.semantic_steps == 2,
                 "first repair advances both counters");
    rc |= expect(rt_work_state_repair_disposition(
                     "task_a5_repair", 1, "request_repair_02",
                     repair_result, 2) == 1,
                 "configured budget permits a second repair");
    rc |= expect(a5_append_rejected_selection(
                     &ctx, "task_a5_repair", 1, 3, repair_result,
                     "calendar_id is still missing after second repair",
                     second_repair_result,
                     sizeof(second_repair_result)) == 0,
                 "persist the second repair selection and rejection");
    rc |= expect(rt_work_state_budget_snapshot(
                     "task_a5_repair", 1, &budget) == 0 &&
                 budget.repair_attempts == 2 &&
                 budget.semantic_steps == 3,
                 "second repair advances counters to configured maximum");
    rc |= expect(rt_work_state_repair_disposition(
                     "task_a5_repair", 1, "request_repair_03",
                     second_repair_result, 2) == 2,
                 "failed second repair exhausts configured budget");
    rc |= expect(test_remove_path(
                     "workspace/memory/state/work_steps.json") == 0 &&
                 rt_work_state_rebuild_from_logs() == 0 &&
                 rt_work_state_budget_snapshot(
                     "task_a5_repair", 1, &budget) == 0 &&
                 budget.repair_attempts == 2 &&
                 budget.semantic_steps == 3,
                 "restart rebuild preserves configurable repair counters");

    snprintf(ctx.event_kind, sizeof(ctx.event_kind), "work_step_result");
    snprintf(ctx.event_payload_json, sizeof(ctx.event_payload_json),
             "{\"task_id\":\"task_a5_repair\",\"work_rev\":1,"
             "\"request_id\":\"request_repair_03\","
             "\"action\":\"gcal\",\"source_event_id\":\"%s\","
             "\"status\":\"rejected\"}",
             second_repair_result);
    rc |= expect(rt_agent_runner_start_step(run, &step, NULL) == 0,
                 "configured exhaustion blocks before another model turn");
    rc |= test_read_file("workspace/runs/run_504/event_log.jsonl", &log);
    rc |= expect(test_count_substr(log, "\"type\":\"action_request\"") == 3,
                 "only the original and two configured repairs are selected");
    rc |= expect(test_count_substr(log, "\"type\":\"work_blocked\"") == 1,
                 "repair exhaustion emits one correlated block");
    rc |= expect_substr(log,
                        "configured repair budget exhausted after 2 repair "
                        "attempts",
                        "repair blocker reports the configured bound");
    rc |= expect_no_substr(log, "request_repair_04",
                           "no selection beyond configured budget is persisted");
    rc |= expect_agent_output_not_exists("run_504", "controller",
                                         ".input.json");
    rc |= expect(rt_append_event_format(
                     &ctx, "task_updated", "external", (char[RT_XL]){0},
                     RT_XL, 0,
                     "{\"task_id\":\"task_a5_repair\",\"work_rev\":2,"
                     "\"state\":{\"status\":\"open\","
                     "\"work\":\"new externally revised repair\","
                     "\"done_when\":\"new repair completes\"}}") == 0 &&
                 rt_work_state_budget_snapshot(
                     "task_a5_repair", 2, &budget) == 0 &&
                 budget.repair_attempts == 0,
                 "a new external revision resets the configured repair count");
  }

  free(log);
  a2_run_free(run);
  free(scheduler);
  rc |= test_reset_workspace();
  return rc;
}

int rejected_and_start_failed_actions_wake_with_origin_request(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  char err[RT_LARGE] = "";
  char request_id[RT_SMALL] = "";
  char projection[RT_XL];
  char *log = NULL;
  char *bus_log = NULL;
  int rc = 0;

  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_505", "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_rejected", "chat:a5",
                              "affair_a5") == 0,
               "create correlated rejection fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize correlated rejection runner");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  snprintf(step.gate, sizeof(step.gate), "work_consequence");
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_rejected", 1,
                     "{\"calls\":[{\"name\":\"gcal\","
                     "\"args\":{}}]}") == 0 &&
                 run->pending_action_count == 1,
                 "queue invalid bound calendar selection");
    if (run->pending_action_count == 1)
      snprintf(request_id, sizeof(request_id), "%s",
               run->pending_actions[0].request_id);
    rc |= expect(request_id[0] &&
                 rt_action_runner_launch(run, request_id,
                                         "gcal", "{}", NULL) == 0,
                 "schema rejection terminalizes the selected request");
    rc |= test_read_file("workspace/runs/run_505/event_log.jsonl", &log);
    rc |= expect(test_count_substr(log, "\"type\":\"action_rejected\"") == 1,
                 "invalid selection has one rejection terminal");
    rc |= expect_substr(log, request_id,
                        "rejection terminal keeps originating request id");
    rc |= expect_substr(log, "\"reason\":\"invalid_args\"",
                        "rejection names the validation failure");
    rc |= test_read_file("workspace/logs/bus.jsonl", &bus_log);
    rc |= expect_substr(bus_log, "\"type\":\"work_step_result\"",
                        "rejection publishes a controller repair wake");
    rc |= expect_substr(bus_log, request_id,
                        "rejection wake keeps originating request id");
    rc |= expect(test_remove_path(
                     "workspace/memory/state/work_steps.json") == 0 &&
                 rt_work_state_rebuild_from_logs() == 0 &&
                 rt_work_state_projection_json(
                     "task_a5_rejected", 1,
                     RT_WORK_REPAIR_ATTEMPT_DEFAULT,
                     projection, sizeof(projection)) == 0,
                 "rebuild rejected selection from terminal event truth");
    rc |= expect_substr(projection, "\"result_kind\":\"action_rejected\"",
                        "rebuilt rejection remains a repair consequence");
  }
  free(log);
  free(bus_log);
  log = NULL;
  bus_log = NULL;
  a2_run_free(run);
  free(scheduler);
  run = NULL;
  scheduler = NULL;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_506", "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_start_fail", "chat:a5",
                              "affair_a5") == 0,
               "create correlated action-start failure fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize start-failure runner");
  request_id[0] = '\0';
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_start_fail", 1,
                     "{\"calls\":[{\"name\":\"bash\",\"args\":{"
                     "\"op\":\"start\",\"cmd\":\"printf safe\"}}]}") == 0 &&
                 run->pending_action_count == 1,
                 "queue valid bound subprocess selection");
    if (run->pending_action_count == 1)
      snprintf(request_id, sizeof(request_id), "%s",
               run->pending_actions[0].request_id);
    /* A NULL runner deterministically fails before fork, exactly at the
     * adapter/process-start boundary. */
    rc |= expect(request_id[0] &&
                 rt_action_runner_launch(run, request_id,
                                         "bash", "{}", NULL) == 0,
                 "pre-effect process-start failure terminalizes");
    rc |= test_read_file("workspace/runs/run_506/event_log.jsonl", &log);
    rc |= expect(test_count_substr(log, "\"type\":\"action_failed\"") == 1,
                 "start failure has one durable action terminal");
    rc |= expect(test_count_substr(log, "\"type\":\"action_started\"") == 1,
                 "process launch failure follows one durable start claim");
    rc |= expect_substr(log, "\"reason\":\"action_start_failed\"",
                        "start failure names its boundary");
    rc |= expect_substr(log, request_id,
                        "start failure keeps originating request id");
    rc |= test_read_file("workspace/logs/bus.jsonl", &bus_log);
    rc |= expect_substr(bus_log, "\"type\":\"work_step_result\"",
                        "start failure publishes a controller repair wake");
    rc |= expect_substr(bus_log, request_id,
                        "failure wake keeps originating request id");
    rc |= expect(test_remove_path(
                     "workspace/memory/state/work_steps.json") == 0 &&
                 rt_work_state_rebuild_from_logs() == 0 &&
                 rt_work_state_projection_json(
                     "task_a5_start_fail", 1,
                     RT_WORK_REPAIR_ATTEMPT_DEFAULT,
                     projection, sizeof(projection)) == 0,
                 "rebuild start failure from terminal event truth");
    rc |= expect_substr(projection, "\"result_kind\":\"action_failed\"",
                        "rebuilt start failure remains repairable");
    rc |= expect_substr(projection,
                        "action process could not be started",
                        "rebuilt failure retains actionable detail");
  }

  free(log);
  free(bus_log);
  a2_run_free(run);
  free(scheduler);
  rc |= test_reset_workspace();
  test_config_restore(saved_cfg);
  return rc;
}

int action_start_claim_is_sync_and_pre_effect(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  RtJobRunner runner;
  char err[RT_LARGE] = "";
  char request_id[RT_SMALL] = "";
  char *log = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_509", "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_start_claim", "chat:a5",
                              "affair_a5") == 0,
               "create checked start-claim fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize checked start-claim runner");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  snprintf(step.gate, sizeof(step.gate), "work_consequence");
  rt_jobrunner_init(&runner);
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_start_claim", 1,
                     "{\"calls\":[{\"name\":\"bash\",\"args\":{"
                     "\"op\":\"start\",\"cmd\":"
                     "\"printf effect > effect_claim.txt\"}}]}") == 0 &&
                 run->pending_action_count == 1,
                 "persist outside-world selection before start claim");
    if (run->pending_action_count == 1)
      snprintf(request_id, sizeof(request_id), "%s",
               run->pending_actions[0].request_id);
    fs_test_reset_append_sync();
    fs_test_fail_next_append_sync();
    rc |= expect(request_id[0] &&
                 rt_action_runner_launch(run, request_id, "bash", "{}",
                                         &runner) != 0,
                 "failed synchronous start claim stops dispatch");
    rc |= expect(fs_test_append_sync_attempts() == 1,
                 "action_started uses the synchronous append path");
    rc |= expect(rt_jobrunner_count_active_all(&runner) == 0,
                 "failed start claim forks no child");
    rc |= expect(run->pending_action_count == 1 &&
                 run->pending_actions[0].started == 0,
                 "failed start claim leaves stable request unstarted");
    rc |= expect_file_not_exists("workspace/effect_claim.txt");
    rc |= test_read_file("workspace/runs/run_509/event_log.jsonl", &log);
    rc |= expect_no_substr(log, "\"type\":\"action_succeeded\"",
                           "failed claim cannot report success");
    rc |= expect_no_substr(log, "\"type\":\"action_failed\"",
                           "failed claim does not invent a launched failure");
  }
  fs_test_reset_append_sync();
  rt_jobrunner_destroy(&runner);
  free(log);
  a2_run_free(run);
  free(scheduler);
  rc |= test_reset_workspace();
  return rc;
}

int started_outside_action_blocks_ambiguous_completion_without_replay(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  char err[RT_LARGE] = "";
  char request_id[RT_SMALL] = "";
  char peek_id[RT_SMALL] = "", peek_action[RT_SMALL] = "";
  char peek_args[RT_LARGE] = "";
  char input_path[PATH_MAX];
  char *log = NULL;
  char *tasks = NULL;
  char *effect = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_507", "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_pre_effect", "chat:a5",
                              "affair_a5") == 0,
               "create pre-effect recovery fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize pre-effect recovery runner");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  snprintf(step.gate, sizeof(step.gate), "work_consequence");
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_pre_effect", 1,
                     "{\"calls\":[{\"name\":\"bash\",\"args\":{"
                     "\"op\":\"start\",\"cmd\":\"printf effect\"}}]}") == 0 &&
                 run->pending_action_count == 1,
                 "persist pre-effect outside-world selection");
    if (run->pending_action_count == 1)
      snprintf(request_id, sizeof(request_id), "%s",
               run->pending_actions[0].request_id);
    rc |= expect(rt_action_runner_recover_started(run) == 0,
                 "pre-effect recovery accepts an unstarted selection");
    rc |= expect(rt_action_runner_peek_next(
                     run, peek_id, sizeof(peek_id),
                     peek_action, sizeof(peek_action),
                     peek_args, sizeof(peek_args)) == 0 &&
                 strcmp(peek_id, request_id) == 0 &&
                 strcmp(peek_action, "bash") == 0,
                 "pre-effect request remains launchable under the same id");
    rc |= test_read_file("workspace/runs/run_507/event_log.jsonl", &log);
    rc |= expect_no_substr(log, "\"type\":\"action_started\"",
                           "pre-effect crash has no checked start claim");
    rc |= expect_no_substr(log, "\"type\":\"work_blocked\"",
                           "pre-effect crash is not falsely ambiguous");
    rc |= expect_file_not_exists("workspace/effect_count.txt");
  }
  free(log);
  log = NULL;
  a2_run_free(run);
  free(scheduler);
  run = NULL;
  scheduler = NULL;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_508", "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_post_effect", "chat:a5",
                              "affair_a5") == 0,
               "create post-effect ambiguity fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize post-effect recovery runner");
  request_id[0] = '\0';
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_post_effect", 1,
                     "{\"calls\":[{\"name\":\"bash\",\"args\":{"
                     "\"op\":\"start\",\"cmd\":\"printf effect\"}}]}") == 0 &&
                 run->pending_action_count == 1,
                 "persist post-effect outside-world selection");
    if (run->pending_action_count == 1)
      snprintf(request_id, sizeof(request_id), "%s",
               run->pending_actions[0].request_id);
    rt_action_emit_started(run, request_id, "bash", NULL,
                           "task_a5_post_effect");
    rc |= expect(run->pending_action_count == 1 &&
                 run->pending_actions[0].started == 1,
                 "action_started is the checked ambiguity boundary");
    rc |= test_write_file("workspace/effect_count.txt", "1\n");
    rc |= expect(rt_action_runner_recover_started(run) == 0,
                 "post-start outside-world recovery blocks mechanically");
    rc |= expect(run->pending_action_count == 0,
                 "ambiguous request is retired without relaunch");
    rc |= expect(rt_action_runner_recover_started(run) == 0,
                 "repeated reconciliation is idempotent");
    rc |= test_read_file("workspace/runs/run_508/event_log.jsonl", &log);
    rc |= expect(test_count_substr(log, "\"type\":\"action_started\"") == 1,
                 "outside-world action has one checked start claim");
    rc |= expect(test_count_substr(log, "\"type\":\"work_blocked\"") == 1,
                 "ambiguous completion has one durable blocker");
    rc |= expect_substr(log, "\"blocker\":\"ambiguous_completion:",
                        "blocker names ambiguity rather than success");
    rc |= expect_substr(log, request_id,
                        "ambiguity blocker keeps originating request id");
    rc |= expect_no_substr(log, "\"type\":\"action_succeeded\"",
                           "ambiguous completion never infers success");
    rc |= expect_no_substr(log, "\"type\":\"action_failed\"",
                           "bound ambiguity uses one terminal source");
    snprintf(input_path, sizeof(input_path),
             "workspace/runs/run_508/action_calls/001_%s.input.json",
             request_id);
    rc |= expect_file_not_exists(input_path);
    rc |= test_read_file("workspace/effect_count.txt", &effect);
    rc |= expect(effect && strcmp(effect, "1\n") == 0,
                 "post-start recovery does not duplicate the possible effect");
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect_substr(tasks, "\"status\":\"blocked\"",
                        "ambiguous current revision is durably blocked");
    rc |= expect(test_remove_path(
                     "workspace/memory/state/work_steps.json") == 0 &&
                 rt_work_state_rebuild_from_logs() == 0,
                 "ambiguity terminal rebuilds from event truth");
    free(log);
    log = NULL;
    rc |= test_read_file("workspace/memory/state/work_steps.json", &log);
    rc |= expect_substr(log, "\"terminal_kind\":\"work_blocked\"",
                        "rebuilt work ledger retains ambiguity terminal");
  }

  free(log);
  free(tasks);
  free(effect);
  a2_run_free(run);
  free(scheduler);
  run = NULL;
  scheduler = NULL;

  /* Rehydrate the exact started request twice. The first cold start writes
   * the ambiguity blocker; replay of that blocker retires the request before
   * the second cold start can reconsider it. */
  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_001", "chat:tests") == 0,
               "initialize cold ambiguity fixture");
  snprintf(ctx.origin_event_id, sizeof(ctx.origin_event_id), "bus_000001");
  rc |= expect(rt_append_event(
                   &ctx, "user_message", "tests",
                   "{\"text\":\"cold ambiguous effect\","
                   "\"origin_event_id\":\"bus_000001\","
                   "\"channel\":\"tests\",\"adapter_id\":\"\"}") == 0 &&
               a2_create_work(&ctx, "task_a5_cold_ambiguity", "chat:tests",
                              "affair_a5") == 0,
               "commit cold-recovery inbound and work task");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize cold ambiguity source run");
  request_id[0] = '\0';
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_cold_ambiguity", 1,
                     "{\"calls\":[{\"name\":\"bash\",\"args\":{"
                     "\"op\":\"start\",\"cmd\":\"printf effect\"}}]}") == 0 &&
                 run->pending_action_count == 1,
                 "commit cold-recovery outside-world selection");
    if (run->pending_action_count == 1)
      snprintf(request_id, sizeof(request_id), "%s",
               run->pending_actions[0].request_id);
    rt_action_emit_started(run, request_id, "bash", NULL,
                           "task_a5_cold_ambiguity");
  }
  a2_run_free(run);
  free(scheduler);
  run = NULL;
  scheduler = NULL;
  rc |= expect(test_mkdir_p("workspace/bus/processed") == 0 &&
               test_write_file(
                   "workspace/bus/processed/bus_000001.json",
                   "{\"event_id\":\"bus_000001\","
                   "\"ts\":\"2026-07-26T00:00:00Z\","
                   "\"channel\":\"tests\",\"type\":\"user_message\","
                   "\"payload\":{\"text\":\"cold ambiguous effect\","
                   "\"adapter_id\":\"\"}}\n") == 0 &&
               test_write_file("workspace/bus/.counter", "1\n") == 0 &&
               test_write_file(
                   "workspace/runs/run_001/runstate.json",
                   "{\"run_id\":\"run_001\",\"context_id\":\"chat:tests\","
                   "\"profile\":\"floofclaw\",\"status\":\"running\","
                   "\"advance\":1,\"created_from\":{"
                   "\"event_id\":\"bus_000001\","
                   "\"type\":\"user_message\"},"
                   "\"waiting\":{\"jobs\":[],\"events\":[],"
                   "\"deadline\":null},\"cursors\":{\"phase_index\":1,"
                   "\"last_advanced_event_seq\":null},"
                   "\"last_error\":null}") == 0,
               "write cold-recovery control envelope");
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  err[0] = '\0';
  {
    int init_rc = scheduler
                      ? rt_scheduler_init(scheduler, "floofclaw",
                                          err, sizeof(err))
                      : -1;
    rc |= expect(scheduler && init_rc == 0,
               err[0] ? err : "first cold ambiguity recovery init");
  }
  if (scheduler) {
    rt_scheduler_destroy(scheduler);
    free(scheduler);
    scheduler = NULL;
  }
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &log);
  rc |= expect(test_count_substr(log, "\"type\":\"work_blocked\"") == 1,
               "first cold start writes one ambiguity blocker");
  free(log);
  log = NULL;
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  err[0] = '\0';
  {
    int init_rc = scheduler
                      ? rt_scheduler_init(scheduler, "floofclaw",
                                          err, sizeof(err))
                      : -1;
    rc |= expect(scheduler && init_rc == 0,
               err[0] ? err : "second cold ambiguity recovery init");
  }
  if (scheduler) {
    rt_scheduler_destroy(scheduler);
    free(scheduler);
    scheduler = NULL;
  }
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &log);
  rc |= expect(test_count_substr(log, "\"type\":\"work_blocked\"") == 1,
               "replayed ambiguity terminal prevents a duplicate blocker");
  rc |= expect_no_substr(log, "\"type\":\"action_succeeded\"",
                         "cold ambiguity recovery never replays the effect");
  free(log);
  log = NULL;
  rc |= test_reset_workspace();
  return rc;
}

int semantic_step_budget_is_durable_and_revision_safe(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  RtWorkBudgetSnapshot budget;
  char err[RT_LARGE] = "";
  char last_result_event_id[RT_SMALL] = "";
  char initial_request_id[RT_SMALL] = "";
  char *log = NULL;
  char *tasks = NULL;
  long long rev = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_503", "chat:a5") == 0 &&
               a2_create_work(&ctx, "task_a5_budget", "chat:a5",
                              "affair_a5") == 0,
               "create semantic-budget work fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize semantic-budget runner");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  snprintf(step.gate, sizeof(step.gate), "work_consequence");

  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a5_budget", 1,
                     "{\"calls\":[{\"name\":\"work\",\"args\":{"
                     "\"work\":\"continue the bounded task\","
                     "\"done_when\":\"bounded work is complete\"}}]}") == 0,
                 "select controller-originated work revision");
    rc |= expect(run->pending_action_count == 1,
                 "work revision selection queues once");
    if (run->pending_action_count == 1)
      snprintf(initial_request_id, sizeof(initial_request_id), "%s",
               run->pending_actions[0].request_id);
    rc |= expect(run->pending_action_count == 1 &&
                 rt_action_runner_launch(
                     run, run->pending_actions[0].request_id,
                     "work", "{}", NULL) == 0,
                 "controller-originated work revision executes");
    rc |= expect(rt_task_work_rev_of("task_a5_budget", &rev) == 0 &&
                 rev == 2,
                 "controller work creates the exact child revision");
    rc |= expect(rt_work_state_budget_snapshot(
                     "task_a5_budget", 2, &budget) == 0 &&
                 budget.lineage_root_rev == 1 &&
                 budget.semantic_steps == 1 &&
                 strcmp(budget.request_id, initial_request_id) == 0,
                 "child revision inherits the first semantic selection");

    for (int i = 2; i <= RT_WORK_SEMANTIC_STEP_LIMIT; ++i)
      rc |= expect(a5_append_finished_selection(
                       &ctx, "task_a5_budget", 2, i,
                       last_result_event_id,
                       sizeof(last_result_event_id)) == 0,
                   "append one bounded semantic selection and result");
    rc |= expect(rt_work_state_budget_snapshot(
                     "task_a5_budget", 2, &budget) == 0 &&
                 budget.lineage_root_rev == 1 &&
                 budget.semantic_steps == RT_WORK_SEMANTIC_STEP_LIMIT,
                 "exact semantic-step ceiling is durable");

    rc |= expect(test_remove_path(
                     "workspace/memory/state/work_steps.json") == 0,
                 "remove semantic budget projection before rebuild");
    rc |= expect(rt_work_state_rebuild_from_logs() == 0,
                 "rebuild semantic budget projection from event truth");
    rc |= expect(rt_work_state_budget_snapshot(
                     "task_a5_budget", 2, &budget) == 0 &&
                 budget.semantic_steps == RT_WORK_SEMANTIC_STEP_LIMIT &&
                 budget.lineage_root_rev == 1,
                 "rebuild preserves inherited semantic-step ceiling");

    snprintf(ctx.event_kind, sizeof(ctx.event_kind), "work_step_result");
    snprintf(ctx.event_payload_json, sizeof(ctx.event_payload_json),
             "{\"task_id\":\"task_a5_budget\",\"work_rev\":2,"
             "\"request_id\":\"request_budget_%02d\","
             "\"action\":\"note_add\",\"source_event_id\":\"%s\"}",
             RT_WORK_SEMANTIC_STEP_LIMIT, last_result_event_id);
    rc |= expect(rt_agent_runner_start_step(run, &step, NULL) == 0,
                 "ninth controller turn becomes a mechanical block");
    rc |= test_read_file("workspace/runs/run_503/event_log.jsonl", &log);
    rc |= expect(test_count_substr(log, "\"type\":\"action_request\"") ==
                     RT_WORK_SEMANTIC_STEP_LIMIT,
                 "budget permits exactly eight semantic selections");
    rc |= expect(test_count_substr(log, "\"type\":\"work_blocked\"") == 1,
                 "budget exhaustion emits one blocked terminal");
    rc |= expect_substr(log,
                        "semantic step budget exhausted after 8 selections",
                        "budget blocker is explicit");
    rc |= expect_no_substr(log, "request_budget_09",
                           "no ninth semantic request is persisted");
    rc |= expect_agent_output_not_exists("run_503", "controller",
                                         ".input.json");
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect_substr(tasks, "\"status\":\"blocked\"",
                        "budget exhaustion durably blocks current work");
    free(tasks);
    tasks = NULL;

    rc |= expect(rt_append_event_format(
                     &ctx, "task_updated", "external", (char[RT_XL]){0},
                     RT_XL, 0,
                     "{\"task_id\":\"task_a5_budget\",\"work_rev\":3,"
                     "\"state\":{\"status\":\"open\","
                     "\"work\":\"new external attempt\","
                     "\"done_when\":\"new attempt completes\"}}") == 0,
                 "external revision reopens blocked work");
    rc |= expect(rt_work_state_budget_snapshot(
                     "task_a5_budget", 3, &budget) == 0 &&
                 budget.lineage_root_rev == 3 &&
                 budget.semantic_steps == 0,
                 "new external revision receives a fresh budget");
  }

  free(log);
  free(tasks);
  a2_run_free(run);
  free(scheduler);
  rc |= test_reset_workspace();
  return rc;
}

static int a2_binding_and_output_contract(void) {
  RtContext ctx;
  RtAgentMeta meta;
  RtStep step;
  char selected[RT_SMALL], input[RT_XL], normalized[RT_XL];
  char payload[RT_XL];
  char *event_log = NULL;
  long long rev = 0;
  int before_event;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= test_write_file(
      "workspace/memory/state/tasks.json",
      "{\"tasks\":[{"
      "\"task_id\":\"task_input_zero\","
      "\"context_id\":\"chat:a2\",\"kind\":\"input\","
      "\"input\":{\"type\":\"message\",\"text\":\"inspect me\"},"
      "\"created_event_id\":\"evt_input_zero\",\"created_ms\":1,"
      "\"state\":{\"status\":\"open\",\"work\":null,"
      "\"done_when\":null,\"work_rev\":0,"
      "\"artifacts\":[{\"artifact_id\":\"art_input_zero\","
      "\"parts\":[],\"work_rev\":0}],\"updated_ms\":1}}]}\n");
  rc |= expect(rt_tasks_active_agent_state_json(
                   "chat:a2", input, sizeof(input)) == 0,
               "input-task revision-zero artifact projects for agents");
  rc |= expect_substr(
      input,
      "\"artifacts\":{\"current\":[{\"artifact_id\":\"art_input_zero\","
      "\"parts\":[],\"work_rev\":0}],\"historical\":[]}",
      "valid input-task evidence does not erase the active-task projection");
  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_a2_binding", "chat:a2") == 0,
               "initialize binding run context");
  rc |= expect(a2_create_work(&ctx, "task_a2_bound", "chat:a2",
                              "affair_a2") == 0,
               "create same-context work task");
  rc |= expect(a2_create_work(&ctx, "task_a2_cross", "chat:other",
                              "affair_other") == 0,
               "create cross-context work task");
  rc |= expect(rt_task_select_unique_open_work(
                   "chat:a2", selected, sizeof(selected), &rev) == 0 &&
                   strcmp(selected, "task_a2_bound") == 0 && rev == 1,
               "selector returns exact unique same-context work revision");
  rc |= expect(rt_task_select_unique_open_work(
                   "chat:none", selected, sizeof(selected), &rev) == 1,
               "selector distinguishes no eligible work");

  memset(&meta, 0, sizeof(meta));
  snprintf(meta.id, sizeof(meta.id), "controller");
  meta.bind_task_open_work = 1;
  meta.listen_event = meta.listen_tasks = 1;
  rc |= expect(rt_build_processor_input(
                   &ctx, &meta, "task_a2_bound", 1,
                   input, sizeof(input)) == 0,
               "bound controller input builds");
  rc |= expect_substr(input, "\"marker\":\"trigger-a2\"",
                      "bound controller receives exact trigger");
  rc |= expect_substr(input, "\"work_task\":{\"task_id\":\"task_a2_bound\"",
                      "bound controller receives exact work task");
  rc |= expect_substr(input, "\"work_rev\":1",
                      "bound controller receives reducer-owned work revision");
  rc |= expect_substr(
      input,
      "\"artifacts\":{\"current\":[{\"artifact_id\":\"art_a2\","
      "\"parts\":[],\"work_rev\":1}],\"historical\":[]}",
      "bound controller receives revision-one artifacts as current");
  rc |= expect_no_substr(input, "task_a2_cross",
                         "bound controller does not receive cross-context task");
  rc |= expect_no_substr(input, "\"active\":[",
                         "bound controller does not receive an all-task projection");

  rc |= expect(rt_append_event_format(
                   &ctx, "task_updated", "a2_test",
                   payload, sizeof(payload), 0,
                   "{\"task_id\":\"task_a2_bound\",\"work_rev\":2,"
                   "\"state\":{\"status\":\"open\","
                   "\"work\":\"research the revised answer\","
                   "\"done_when\":\"revised answer exists\"}}") == 0,
               "revise bound work before artifact partition check");
  rc |= expect(rt_task_append_action_failure(
                   &ctx, "bash", "task_a2_bound", 2,
                   "process_failed",
                   "{\"message\":\"current revision failure\"}") == 0,
               "append revision-stamped current diagnostic");
  rc |= expect(rt_build_processor_input(
                   &ctx, &meta, "task_a2_bound", 2,
                   input, sizeof(input)) == 0,
               "revised bound controller input builds");
  rc |= expect_substr(input, "\"artifacts\":{\"current\":[",
                      "agent artifact projection has a current array");
  rc |= expect_substr(input, "current revision failure",
                      "current array retains current-revision evidence");
  rc |= expect_substr(
      input,
      "\"historical\":[{\"artifact_id\":\"art_a2\","
      "\"parts\":[],\"work_rev\":1}]",
      "agent artifact projection structurally separates old evidence");
  rc |= expect(rt_tasks_active_agent_state_json(
                   "chat:a2", input, sizeof(input)) == 0 &&
               strstr(input, "\"artifacts\":{\"current\":[") != NULL &&
               strstr(input, "\"historical\":[") != NULL,
               "active-task agent projection uses the same artifact partition");

  rc |= expect(a2_create_work(&ctx, "task_a2_second", "chat:a2",
                              "affair_a2") == 0,
               "create second same-context work task");
  rc |= expect(rt_task_select_unique_open_work(
                   "chat:a2", selected, sizeof(selected), &rev) == 2,
               "selector reports multiple eligible work tasks");

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_a2_output", "chat:a2") == 0 &&
               a2_create_work(&ctx, "task_a2_bound", "chat:a2",
                              "affair_a2") == 0 &&
               a2_create_work(&ctx, "task_a2_cross", "chat:other",
                              "affair_other") == 0,
               "reset exact output fixture");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  before_event = ctx.next_event;
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[]}", normalized, sizeof(normalized)) != 0,
               "zero controller calls are rejected before append");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"name\":\"work_complete\","
                   "\"args\":{\"summary\":\"done\"}},"
                   "{\"name\":\"message\",\"args\":{\"message\":\"extra\"}}]}",
                   normalized, sizeof(normalized)) != 0,
               "multiple controller calls are rejected before append");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"name\":\"work_complete\","
                   "\"args\":{\"summary\":\"done\"}},{\"name\":"
                   "\"working_memory_append\",\"args\":{"
                   "\"working_memory\":\"checked primary calendar\"}}]}",
                   normalized, sizeof(normalized)) == 0,
               "one working-memory sidecar may accompany one controller call");
  {
    const char *sidecar = strstr(normalized,
                                 "\"action\":\"working_memory_append\"");
    const char *ordinary = strstr(normalized, "\"action\":\"work_complete\"");
    const char *work_rev = sidecar ? strstr(sidecar, "\"work_rev\":1") : NULL;
    rc |= expect(sidecar && ordinary && sidecar < ordinary,
                 "sidecar is normalized before the ordinary call");
    rc |= expect(sidecar && ordinary && work_rev && work_rev > ordinary,
                 "sidecar carries no semantic work revision");
  }
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"name\":\"working_memory_append\","
                   "\"args\":{\"working_memory\":\"one\"}},{\"name\":"
                   "\"working_memory_append\",\"args\":{"
                   "\"working_memory\":\"two\"}}]}",
                   normalized, sizeof(normalized)) != 0,
               "two working-memory sidecars remain invalid");
  rc |= expect(ctx.next_event == before_event,
               "invalid controller call count appends no selected action");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"name\":\"work_complete\","
                   "\"args\":{\"summary\":\"done\","
                   "\"task_id\":\"task_a2_cross\"}}]}",
                   normalized, sizeof(normalized)) != 0,
               "model task id cannot retarget controller across contexts");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 2,
                   "{\"calls\":[{\"name\":\"work_complete\","
                   "\"args\":{\"summary\":\"done\"}}]}",
                   normalized, sizeof(normalized)) != 0,
               "stale reducer revision cannot produce a controller call");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"name\":\"work_complete\","
                   "\"args\":{\"summary\":\"done\",\"work_rev\":99}}]}",
                   normalized, sizeof(normalized)) != 0,
               "model cannot supply runtime-owned work revision");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"action\":\"work_blocked\","
                   "\"args\":{\"blocker\":\"tool missing\","
                   "\"needed\":\"install tool\"}}]}",
                   normalized, sizeof(normalized)) == 0,
               "safe action alias normalizes as call name");
  rc |= expect_substr(normalized, "\"action\":\"work_blocked\"",
                      "action alias preserves selected action intent");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"name\":\"work_complete\","
                   "\"action\":\"work_blocked\","
                   "\"args\":{\"summary\":\"ambiguous\"}}]}",
                   normalized, sizeof(normalized)) != 0,
               "name and action together remain an ambiguous invalid call");
  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a2_bound", 1,
                   "{\"calls\":[{\"name\":\"work_complete\","
                   "\"args\":{\"summary\":\"done\","
                   "\"task_id\":\"task_a2_bound\"}}]}",
                   normalized, sizeof(normalized)) == 0,
               "matching model task id is accepted only as an assertion");
  rc |= expect_substr(normalized, "\"task_id\":\"task_a2_bound\"",
                      "normalized call stamps trusted task id");
  rc |= expect_substr(normalized, "\"work_rev\":1",
                      "normalized call stamps trusted work revision");
  rc |= test_read_file("workspace/runs/run_a2_output/event_log.jsonl",
                       &event_log);
  rc |= expect_no_substr(event_log, "\"type\":\"action_request\"",
                         "normalization rejection never leaks an action request");
  free(event_log);
  return rc;
}

static int a2_complete_and_direct_rejection(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  char err[RT_LARGE] = "";
  char *log = NULL, *tasks = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_a2_complete", "chat:a2") == 0 &&
               a2_create_work(&ctx, "task_a2_complete", "chat:a2",
                              "affair_a2") == 0,
               "create completion fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize controller action runner");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "controller");
  snprintf(step.target, sizeof(step.target), "controller");
  if (run && scheduler) {
    char first_rid[RT_SMALL], second_rid[RT_SMALL];
    RtWorkBudgetSnapshot budget;
    char working_memory[RT_TASK_WORKING_MEMORY_MAX];
    rc |= expect(a2_append_controller_call(
                     run, &step, &scheduler->agent_meta[0],
                     "task_a2_complete", 1,
                     "{\"calls\":[{\"name\":\"work_complete\","
                     "\"args\":{\"summary\":\"verified answer exists\"}},"
                     "{\"name\":\"working_memory_append\",\"args\":{"
                     "\"working_memory\":\"verified the exact answer\"}}]}") == 0,
                 "append one sidecar and one bound terminal action");
    rc |= expect(run->pending_action_count == 2 &&
                 strcmp(run->pending_actions[0].action,
                        "working_memory_append") == 0 &&
                 run->pending_actions[0].work_rev == 0 &&
                 run->pending_actions[1].work_rev == 1 &&
                 strcmp(run->pending_actions[0].task_id,
                        "task_a2_complete") == 0,
                 "pending sidecar precedes the trusted semantic selection");
    snprintf(first_rid, sizeof(first_rid), "%s",
             run->pending_actions[0].request_id);
    rc |= expect(rt_action_runner_launch(
                     run, first_rid,
                     "working_memory_append", "{}", NULL) == 0,
                 "working-memory sidecar executes first");
    rc |= expect(rt_task_working_memory_of(
                     "task_a2_complete", working_memory,
                     sizeof(working_memory)) == 0 &&
                 strcmp(working_memory, "verified the exact answer") == 0,
                 "sidecar durably updates the exact bound task");
    rc |= expect(rt_work_state_budget_snapshot(
                     "task_a2_complete", 1, &budget) == 0 &&
                 budget.semantic_steps == 0 && budget.repair_attempts == 0,
                 "sidecar consumes no semantic or repair budget");
    rc |= expect(run->pending_action_count == 1,
                 "sidecar terminal leaves only the ordinary call pending");
    snprintf(second_rid, sizeof(second_rid), "%s",
             run->pending_actions[0].request_id);
    rc |= expect(rt_action_runner_launch(
                     run, second_rid,
                     "work_complete", "{}", NULL) == 0,
                 "bound work_complete intrinsic executes");
    rc |= test_read_file("workspace/runs/run_a2_complete/event_log.jsonl",
                         &log);
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect_substr(log, "\"type\":\"work_completed\"",
                        "completion appends durable terminal work truth");
    rc |= expect_substr(log, "\"task_id\":\"task_a2_complete\"",
                        "terminal outcome preserves exact task id");
    rc |= expect_substr(log, "\"work_rev\":1",
                        "terminal outcome preserves exact revision");
    rc |= expect_substr(log, "\"context_id\":\"chat:a2\"",
                        "terminal outcome preserves context correlation");
    rc |= expect_substr(log, "\"affair_id\":\"affair_a2\"",
                        "terminal outcome preserves affair correlation");
    rc |= expect_substr(log, "\"channel\":\"tests\"",
                        "terminal outcome preserves channel correlation");
    rc |= expect_substr(log, "\"adapter_id\":\"adapter_a2\"",
                        "terminal outcome preserves adapter correlation");
    rc |= expect_substr(log, "\"ref\":{\"thread\":\"thread-a2\"}",
                        "terminal outcome preserves opaque channel ref");
    rc |= expect_substr(log, "\"operation_ref\":{\"handle\":\"op_a2\"}",
                        "terminal outcome carries operation reference");
    rc |= expect_substr(log, "\"evidence_refs\":[{\"artifact_id\":\"art_a2\"",
                        "terminal outcome carries durable evidence references");
    rc |= expect_substr(tasks, "\"status\":\"completed\"",
                        "task reducer persists completed state");
    rc |= expect_substr(log, "\"type\":\"task_working_memory_appended\"",
                        "sidecar append is durable event truth");
  }
  free(log);
  free(tasks);
  a2_run_free(run);
  free(scheduler);

  log = tasks = NULL;
  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_a2_direct", "chat:a2") == 0 &&
               a2_create_work(&ctx, "task_a2_direct", "chat:a2",
                              "affair_a2") == 0,
               "create direct terminal rejection fixture");
  scheduler = a2_scheduler("ordinary_agent", 0, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize ordinary action caller");
  if (run) {
    rc |= expect(rt_action_runner_note_request(
                     run, "ordinary_agent", "direct_terminal",
                     "work_complete", "{\"summary\":\"forged\"}",
                     "task_a2_direct", 0) == 1,
                 "queue direct terminal call for validation");
    rc |= expect(rt_action_runner_launch(
                     run, "direct_terminal", "work_complete", "{}", NULL) == 0,
                 "direct terminal call reaches loud rejection");
    rc |= test_read_file("workspace/runs/run_a2_direct/event_log.jsonl", &log);
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect_substr(log, "\"reason\":\"trusted_work_binding_required\"",
                        "terminal action rejects caller without trusted binding");
    rc |= expect_no_substr(log, "\"type\":\"work_completed\"",
                           "direct terminal call appends no terminal truth");
    rc |= expect_substr(tasks, "\"status\":\"open\"",
                        "direct terminal rejection leaves task open");
  }
  free(log);
  free(tasks);
  a2_run_free(run);
  free(scheduler);
  return rc;
}

static int a2_block_and_revise_same_task(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep controller_step, chat_step;
  RtAgentMeta chat_meta;
  char err[RT_LARGE] = "";
  char *log = NULL, *tasks = NULL, *narration = NULL;
  char committed[RT_XL];
  char normalized[RT_XL], normalize_err[RT_LARGE] = "";
  JsonRef reopen_payload;
  long long rev = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_a2_block", "chat:a2") == 0 &&
               a2_create_work(&ctx, "task_a2_block", "chat:a2",
                              "affair_a2") == 0,
               "create blocked-revision fixture");
  scheduler = a2_scheduler("controller", 1, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize blocked controller runner");
  memset(&controller_step, 0, sizeof(controller_step));
  snprintf(controller_step.id, sizeof(controller_step.id), "controller");
  snprintf(controller_step.target, sizeof(controller_step.target), "controller");
  if (run && scheduler) {
    rc |= expect(a2_append_controller_call(
                     run, &controller_step, &scheduler->agent_meta[0],
                     "task_a2_block", 1,
                     "{\"calls\":[{\"name\":\"work_blocked\","
                     "\"args\":{\"blocker\":\"calendar id is missing\","
                     "\"needed\":\"provide the calendar id\"}}]}") == 0,
                 "append bound work_blocked action");
    rc |= expect(rt_action_runner_launch(
                     run, run->pending_actions[0].request_id,
                     "work_blocked", "{}", NULL) == 0,
                 "bound work_blocked intrinsic executes");
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect_substr(tasks, "\"status\":\"blocked\"",
                        "blocked work remains in active task state");
    rc |= expect_substr(tasks, "\"work_rev\":1",
                        "blocking does not invent a new revision");
    free(tasks);
    tasks = NULL;

    rc |= expect(json_ref_from_text(
                     "{\"task_id\":\"task_a2_block\","
                     "\"state\":{\"status\":\"open\"}}",
                     &reopen_payload) == 0,
                 "same-revision reopen payload parses");
    rc |= expect(rt_task_normalize_event(
                     &run->ctx, "task_updated", &reopen_payload,
                     run->ctx.next_event, normalized, sizeof(normalized),
                     normalize_err, sizeof(normalize_err)) == 0,
                 "same-revision reopen is an otherwise normal task update");
    rc |= expect(rt_task_apply_event("task_updated", normalized) != 0,
                 "reducer rejects blocked-to-open without revision fields");
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect_substr(tasks, "\"status\":\"blocked\"",
                        "rejected same-revision reopen stays blocked");
    rc |= expect_substr(tasks, "\"work_rev\":1",
                        "rejected same-revision reopen preserves revision");
    free(tasks);
    tasks = NULL;

    memset(&chat_meta, 0, sizeof(chat_meta));
    snprintf(chat_meta.id, sizeof(chat_meta.id), "chat_manager");
    scheduler->agent_meta[1] = chat_meta;
    scheduler->agent_meta_count = 2;
    memset(&chat_step, 0, sizeof(chat_step));
    snprintf(chat_step.id, sizeof(chat_step.id), "chat");
    snprintf(chat_step.target, sizeof(chat_step.target), "chat_manager");
    rc |= expect(rt_agent_append_output(
                     &run->ctx, &chat_step, &scheduler->agent_meta[1],
                     NULL, 0,
                     "{\"calls\":[{\"name\":\"work\",\"args\":{"
                     "\"task_id\":\"task_a2_block\","
                     "\"work\":\"research the answer\","
                     "\"done_when\":\"verified answer exists\"}}]}",
                     committed, sizeof(committed)) == 0,
                 "explicit same-context blocked task is a valid work revision");
    rt_run_apply_output_events(run, "chat_manager", committed);
    rc |= expect(run->pending_action_count == 1,
                 "blocked revision queues one work action");
    rc |= expect(rt_action_runner_launch(
                     run, run->pending_actions[0].request_id,
                     "work", "{}", NULL) == 0,
                 "work intrinsic revises the exact blocked task");
    rc |= expect(rt_task_work_rev_of("task_a2_block", &rev) == 0 && rev == 2,
                 "blocked same-task revision bumps reducer work_rev");
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect_substr(tasks, "\"task_id\":\"task_a2_block\"",
                        "revised task keeps its durable id");
    rc |= expect_substr(tasks, "\"status\":\"open\"",
                        "new revision reopens blocked work");
    rc |= expect(test_count_substr(tasks,
                                  "\"task_id\":\"task_a2_block\"") == 1,
                 "blocked revision creates no duplicate work task");
    rc |= expect(test_count_substr(tasks, "\"kind\":\"work\"") == 1,
                 "task store contains only one work task");
    rc |= test_read_file("workspace/runs/run_a2_block/event_log.jsonl", &log);
    rc |= expect(test_count_substr(log, "\"type\":\"work_blocked\"") == 1,
                 "blocked revision emits one terminal blocked outcome");
    rc |= expect_substr(log, "\"blocker\":\"calendar id is missing\"",
                        "blocked outcome records concrete blocker");
    rc |= expect_substr(log, "\"needed\":\"provide the calendar id\"",
                        "blocked outcome records exact unblocking input");
    rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
    rc |= expect(test_count_substr(narration, "work blocked: task_a2_block") == 1,
                 "blocked outcome is narrated exactly once");
  }
  free(log);
  free(tasks);
  free(narration);
  a2_run_free(run);
  free(scheduler);
  return rc;
}

static int a2_replay_task_events(const char *event_text) {
  char *copy = NULL;
  char *line;
  int rc = 0;
  if (!event_text) return -1;
  copy = strdup(event_text);
  if (!copy) return -1;
  line = copy;
  while (line && *line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload;
    char type[RT_SMALL] = "";
    char payload_json[RT_XL];
    if (next) *next = '\0';
    if (*line && json_ref_top_object(line, &event) == 0 &&
        json_ref_object_get_string(&event, "type", type, sizeof(type)) == 0 &&
        rt_task_is_event_type(type) &&
        (json_ref_object_get_object(&event, "payload", &payload) != 0 ||
         json_ref_value_copy(&payload, payload_json,
                             sizeof(payload_json)) != 0 ||
         rt_task_apply_event(type, payload_json) != 0)) {
      rc = -1;
      break;
    }
    line = next ? next + 1 : NULL;
  }
  free(copy);
  return rc;
}

int task_store_compatibility_shapes_project_and_archive_identically(void) {
  static const char nested_and_legacy[] =
      "{\"tasks\":["
      "{\"task_id\":\"task_nested\",\"context_id\":\"chat:nested\","
      "\"kind\":\"work\",\"input\":null,\"created_event_id\":\"evt_nested\","
      "\"created_ms\":10,\"state\":{\"status\":\"working\","
      "\"work\":\"nested work\",\"done_when\":\"nested done\","
      "\"work_rev\":7,\"working_memory\":\"nested memory\","
      "\"artifacts\":[],\"updated_ms\":11}},"
      "{\"task_id\":\"task_legacy\",\"context_id\":\"chat:legacy\","
      "\"kind\":\"work\",\"input\":null,\"created_event_id\":\"evt_legacy\","
      "\"created_ms\":20,\"state\":\"open\",\"work\":\"legacy work\","
      "\"done_when\":\"legacy done\",\"work_rev\":9,"
      "\"working_memory\":\"legacy memory\",\"artifacts\":[],"
      "\"updated_ms\":21}]}\n";
  static const char legacy_completed[] =
      "{\"tasks\":[{\"task_id\":\"task_legacy_done\","
      "\"context_id\":\"chat:archive\",\"kind\":\"work\",\"input\":null,"
      "\"created_event_id\":\"evt_legacy_done\",\"created_ms\":30,"
      "\"state\":\"completed\",\"work\":\"archived work\","
      "\"done_when\":\"archived done\",\"work_rev\":3,"
      "\"working_memory\":\"archived memory\",\"artifacts\":[],"
      "\"updated_ms\":31}]}\n";
  char task_id[RT_SMALL] = "";
  char binding[RT_XL];
  char *archive = NULL;
  char *remaining = NULL;
  long long work_rev = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/memory/state/tasks.json",
                        nested_and_legacy);
  rc |= expect(rt_task_select_unique_open_work(
                   "chat:nested", task_id, sizeof(task_id), &work_rev) == 0 &&
               strcmp(task_id, "task_nested") == 0 && work_rev == 7,
               "nested task state selects its exact work revision");
  rc |= expect(rt_task_work_binding_json(
                   "chat:nested", "task_nested", 7, 0,
                   binding, sizeof(binding)) == 0,
               "nested task state produces a work binding");

  task_id[0] = '\0';
  work_rev = 0;
  rc |= expect(rt_task_select_unique_open_work(
                   "chat:legacy", task_id, sizeof(task_id), &work_rev) == 0 &&
               strcmp(task_id, "task_legacy") == 0 && work_rev == 9,
               "legacy top-level task state selects its exact work revision");
  rc |= expect(rt_task_work_binding_json(
                   "chat:legacy", "task_legacy", 9, 0,
                   binding, sizeof(binding)) == 0,
               "legacy top-level task state produces a work binding");

  rc |= test_write_file("workspace/memory/state/tasks.json",
                        legacy_completed);
  rc |= expect(rt_tasks_archive_completed_for_context("chat:archive") == 0,
               "legacy top-level completed task archives");
  rc |= test_read_file("workspace/memory/state/tasks_archive.jsonl", &archive);
  rc |= test_read_file("workspace/memory/state/tasks.json", &remaining);
  rc |= expect_substr(archive, "\"task_id\":\"task_legacy_done\"",
                      "legacy archive keeps task identity");
  rc |= expect_substr(archive, "\"working_memory\":\"archived memory\"",
                      "legacy archive keeps working memory");
  rc |= expect_substr(archive, "\"final_state\":\"completed\"",
                      "legacy archive records its terminal state");
  rc |= expect_no_substr(remaining, "task_legacy_done",
                         "archived legacy task leaves the active store");

  free(archive);
  free(remaining);
  return rc;
}

static int a2_working_memory_contract(void) {
  RtContext ctx;
  RtScheduler *scheduler = NULL;
  RtRun *run = NULL;
  RtStep step;
  char err[RT_LARGE] = "";
  char committed[RT_XL], memory[RT_TASK_WORKING_MEMORY_MAX];
  char *tasks = NULL, *log = NULL, *overflow_args = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(a2_init_context(&ctx, "run_a2_memory", "chat:a2") == 0 &&
               a2_create_work(&ctx, "task_a2_memory", "chat:a2",
                              "affair_a2") == 0 &&
               a2_create_work(&ctx, "task_a2_memory_cross", "chat:other",
                              "affair_other") == 0,
               "create working-memory fixtures without the new field");
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect_substr(tasks, "\"working_memory\":\"\"",
                      "legacy task events default working_memory to empty");
  free(tasks);
  tasks = NULL;

  scheduler = a2_scheduler("ordinary_agent", 0, err, sizeof(err));
  run = scheduler ? a2_run(scheduler, &ctx) : NULL;
  rc |= expect(scheduler && run, "initialize task-list working-memory caller");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "ordinary");
  snprintf(step.target, sizeof(step.target), "ordinary_agent");
  if (run && scheduler) {
    rc |= expect(rt_agent_append_output(
                     &run->ctx, &step, &scheduler->agent_meta[0], NULL, 0,
                     "{\"calls\":[{\"name\":\"working_memory_append\","
                     "\"args\":{\"task_id\":\"task_a2_memory\","
                     "\"working_memory\":\"first discovery\"}}]}",
                     committed, sizeof(committed)) == 0,
                 "task-list agent may target an exact projected task");
    rt_run_apply_output_events(run, "ordinary_agent", committed);
    rc |= expect(run->pending_action_count == 1 &&
                 strcmp(run->pending_actions[0].task_id,
                        "task_a2_memory") == 0 &&
                 run->pending_actions[0].work_rev == 0,
                 "ordinary append carries exact task routing without a work revision");
    if (run->pending_action_count == 1) {
      char rid[RT_SMALL];
      snprintf(rid, sizeof(rid), "%s", run->pending_actions[0].request_id);
      rc |= expect(rt_action_runner_launch(
                       run, rid, "working_memory_append", "{}", NULL) == 0,
                   "task-list append executes through the base action");
    }
    rc |= expect(rt_task_working_memory_of(
                     "task_a2_memory", memory, sizeof(memory)) == 0 &&
                 strcmp(memory, "first discovery") == 0,
                 "first append changes no prior text");
    rc |= expect(rt_agent_append_output(
                     &run->ctx, &step, &scheduler->agent_meta[0], NULL, 0,
                     "{\"calls\":[{\"name\":\"working_memory_append\","
                     "\"args\":{\"task_id\":\"task_a2_memory\","
                     "\"working_memory\":\"second discovery\"}}]}",
                     committed, sizeof(committed)) == 0,
                 "same task accepts a later append");
    rt_run_apply_output_events(run, "ordinary_agent", committed);
    if (run->pending_action_count == 1) {
      char rid[RT_SMALL];
      snprintf(rid, sizeof(rid), "%s", run->pending_actions[0].request_id);
      rc |= expect(rt_action_runner_launch(
                       run, rid, "working_memory_append", "{}", NULL) == 0,
                   "second append executes through the same action path");
    }
    rc |= expect(rt_task_working_memory_of(
                     "task_a2_memory", memory, sizeof(memory)) == 0 &&
                 strcmp(memory, "first discovery\nsecond discovery") == 0,
                 "append order is preserved with one plain newline");
    rc |= expect(rt_append_event_format(
                     &run->ctx, "task_updated", "a2_test",
                     (char[RT_XL]){0}, RT_XL, 0,
                     "{\"task_id\":\"task_a2_memory\",\"state\":{"
                     "\"work\":\"revised objective\","
                     "\"done_when\":\"revised completion\"}}") == 0,
                 "task revision commits after working-memory appends");
    rc |= expect(rt_task_working_memory_of(
                     "task_a2_memory", memory, sizeof(memory)) == 0 &&
                 strcmp(memory, "first discovery\nsecond discovery") == 0,
                 "task revisions preserve working_memory");

    overflow_args = (char *)malloc(RT_TASK_WORKING_MEMORY_MAX + 96U);
    if (overflow_args) {
      size_t current_len = strlen(memory);
      size_t incoming_len = RT_TASK_WORKING_MEMORY_MAX - current_len - 1U;
      size_t prefix_len = strlen("{\"working_memory\":\"");
      memcpy(overflow_args, "{\"working_memory\":\"", prefix_len);
      memset(overflow_args + prefix_len, 'x', incoming_len);
      snprintf(overflow_args + prefix_len + incoming_len, 4, "\"}");
      rc |= expect(rt_action_runner_note_request(
                       run, "ordinary_agent", "overflow_memory",
                       "working_memory_append", overflow_args,
                       "task_a2_memory", 0) == 1,
                   "queue an append exactly beyond the fixed limit");
      rc |= expect(rt_action_runner_launch(
                       run, "overflow_memory", "working_memory_append",
                       "{}", NULL) == 0,
                   "overflow is a loud terminal action failure");
      rc |= expect(rt_task_working_memory_of(
                       "task_a2_memory", memory, sizeof(memory)) == 0 &&
                   strcmp(memory,
                          "first discovery\nsecond discovery") == 0,
                   "overflow failure leaves existing working_memory unchanged");
    } else {
      rc |= expect(0, "allocate overflow fixture");
    }
    rc |= expect(rt_agent_normalize_output(
                     &run->ctx, &step, &scheduler->agent_meta[0], NULL, 0,
                     "{\"calls\":[{\"name\":\"working_memory_append\","
                     "\"args\":{\"task_id\":\"task_a2_memory_cross\","
                     "\"working_memory\":\"wrong context\"}}]}",
                     committed, sizeof(committed)) != 0,
                 "task-list agent cannot target a task outside its context");
  }
  rc |= test_read_file("workspace/runs/run_a2_memory/event_log.jsonl", &log);
  rc |= expect(test_count_substr(
                   log, "\"type\":\"task_working_memory_appended\"") == 2,
               "only accepted appends become durable task events");
  rc |= expect(test_remove_path("workspace/memory/state/tasks.json") == 0 &&
               a2_replay_task_events(log) == 0,
               "task projection rebuilds from event truth");
  rc |= expect(rt_task_working_memory_of(
                   "task_a2_memory", memory, sizeof(memory)) == 0 &&
               strcmp(memory, "first discovery\nsecond discovery") == 0,
               "replay reconstructs append order exactly");
  rc |= expect(run && rt_append_event_format(
                   &run->ctx, "task_completed", "a2_test",
                   (char[RT_XL]){0}, RT_XL, 0,
                   "{\"task_id\":\"task_a2_memory\","
                   "\"state\":{\"status\":\"completed\","
                   "\"updated_ms\":9000}}") == 0 &&
               rt_tasks_archive_completed_for_context("chat:a2") == 0,
               "completed task with working memory archives normally");
  free(tasks);
  tasks = NULL;
  rc |= test_read_file("workspace/memory/state/tasks_archive.jsonl", &tasks);
  rc |= expect_substr(tasks,
                      "\"working_memory\":\"first discovery\\nsecond discovery\"",
                      "archive preserves the exact working_memory string");

  free(overflow_args);
  free(tasks);
  free(log);
  a2_run_free(run);
  free(scheduler);
  rc |= test_reset_workspace();
  return rc;
}

int deterministic_work_controller_binding_and_terminal_controls(void) {
  static const char *const conflicting_modes[] = {
    "conversational_payload_only",
    "affair_extraction_context_only",
    "memory_compaction_context_only"
  };
  char err[RT_LARGE] = "";
  char config[RT_LARGE];
  RtAgentMeta meta;
  int rc = 0;

  rc |= test_mkdir_p("floops/a2_meta/agents/controller");
  rc |= test_write_file(
      "floops/a2_meta/agents/controller/agent.json",
      "{\"executor\":\"native\",\"listen\":[\"event\"],"
      "\"bind_task\":\"open_work\"}\n");
  memset(&meta, 0, sizeof(meta));
  rc |= expect(rt_agent_read_listen_config(
                   "a2_meta", "controller", "native",
                   &meta, err, sizeof(err)) == 0 &&
                   meta.bind_task_open_work == 1 &&
                   meta.max_repair_attempts ==
                       RT_WORK_REPAIR_ATTEMPT_DEFAULT,
               "bound controller loads binding and compatibility repair default");
  rc |= test_write_file(
      "floops/a2_meta/agents/controller/agent.json",
      "{\"executor\":\"native\",\"listen\":[\"event\"],"
      "\"bind_task\":\"open_work\","
      "\"work_policy\":{\"max_repair_attempts\":3}}\n");
  memset(&meta, 0, sizeof(meta));
  rc |= expect(rt_agent_read_listen_config(
                   "a2_meta", "controller", "native",
                   &meta, err, sizeof(err)) == 0 &&
                   meta.bind_task_open_work == 1 &&
                   meta.max_repair_attempts == 3,
               "bound controller loads floop-owned repair policy");
  rc |= test_write_file(
      "floops/a2_meta/agents/controller/agent.json",
      "{\"executor\":\"native\",\"listen\":[\"event\"],"
      "\"bind_task\":\"open_work\","
      "\"work_policy\":{\"max_repair_attempts\":9}}\n");
  memset(&meta, 0, sizeof(meta));
  err[0] = '\0';
  rc |= expect(rt_agent_read_listen_config(
                   "a2_meta", "controller", "native",
                   &meta, err, sizeof(err)) != 0,
               "repair policy above engine safety ceiling fails startup");
  rc |= expect_substr(err, "use an integer from 0 through 8",
                      "repair-policy ceiling error names the valid range");
  rc |= test_write_file(
      "floops/a2_meta/agents/controller/agent.json",
      "{\"executor\":\"native\",\"listen\":[\"event\"],"
      "\"work_policy\":{\"max_repair_attempts\":3}}\n");
  memset(&meta, 0, sizeof(meta));
  err[0] = '\0';
  rc |= expect(rt_agent_read_listen_config(
                   "a2_meta", "controller", "native",
                   &meta, err, sizeof(err)) != 0,
               "repair policy without a bound work controller fails startup");
  rc |= expect_substr(err, "without bind_task \"open_work\"",
                      "unbound repair-policy error names the missing binding");
  rc |= test_write_file(
      "floops/a2_meta/agents/controller/agent.json",
      "{\"executor\":\"native\",\"listen\":[\"event\"],"
      "\"bind_task\":\"first_task\"}\n");
  memset(&meta, 0, sizeof(meta));
  err[0] = '\0';
  rc |= expect(rt_agent_read_listen_config(
                   "a2_meta", "controller", "native",
                   &meta, err, sizeof(err)) != 0,
               "unknown bind_task fails startup");
  rc |= expect_substr(err, "fix: set bind_task to \"open_work\" or remove it",
                      "bind_task startup error names its fix");
  for (size_t i = 0;
       i < sizeof(conflicting_modes) / sizeof(conflicting_modes[0]); ++i) {
    snprintf(config, sizeof(config),
             "{\"executor\":\"native\",\"listen\":[\"event\"],"
             "\"bind_task\":\"open_work\",\"%s\":true}\n",
             conflicting_modes[i]);
    rc |= test_write_file(
        "floops/a2_meta/agents/controller/agent.json", config);
    memset(&meta, 0, sizeof(meta));
    err[0] = '\0';
    rc |= expect(rt_agent_read_listen_config(
                     "a2_meta", "controller", "native",
                     &meta, err, sizeof(err)) != 0,
                 "bound controller rejects a conflicting input mode");
    rc |= expect_substr(err, conflicting_modes[i],
                        "controller startup error names conflicting mode");
    rc |= expect_substr(err, "fix: remove",
                        "controller input-mode conflict names its fix");
  }
  rc |= test_remove_path("floops/a2_meta");

  rc |= a2_binding_and_output_contract();
  rc |= a2_complete_and_direct_rejection();
  rc |= a2_block_and_revise_same_task();
  rc |= a2_working_memory_contract();
  rc |= test_reset_workspace();
  return rc;
}
