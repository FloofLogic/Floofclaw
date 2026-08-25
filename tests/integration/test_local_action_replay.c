/* Crash recovery for local mutation actions.
 *
 * outside_world:false makes a started request without a terminal eligible
 * for replay: recovery clears the start claim and the runner dispatches the
 * same request id again. That is correct only if a second dispatch cannot
 * repeat an effect the first one already committed — and for every durable
 * mutation the effect is an appended event, which survives the crash whether
 * or not its terminal did.
 *
 * Each test here drives both halves of that window:
 *
 *   pre-effect   action_started committed, the intrinsic never ran. The
 *                request must stay launchable under its stable id and the
 *                replay must apply the effect exactly once.
 *   post-effect  action_started and the mutation event committed, no
 *                terminal. The replay must commit nothing further and
 *                report the effect that is already durable.
 *
 * The crash itself is modeled the way the runner sees it — a start claim
 * with no terminal — rather than by killing a process, because that is
 * precisely the state rt_action_runner_recover_started reconciles. */

#include "../test_support.h"

#include "../../runtime/action_internal.h"
#include "../../runtime/action_runtime_internal.h"
#include "../../runtime/agent_exec.h"
#include "../../runtime/run_internal.h"
#include "../../runtime/runtime.h"
#include "../../runtime/runtime_kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  RtScheduler *scheduler;
  RtRun       *run;
  char         input_task_id[RT_SMALL];
} ReplayFixture;

static int lr_init_context(RtContext *ctx, const char *run_id,
                           const char *context_id) {
  char path[PATH_MAX];
  if (!ctx || !run_id || !context_id) return -1;
  memset(ctx, 0, sizeof(*ctx));
  snprintf(ctx->workspace, sizeof(ctx->workspace), "workspace");
  snprintf(ctx->run_id, sizeof(ctx->run_id), "%s", run_id);
  snprintf(ctx->run_dir, sizeof(ctx->run_dir), "workspace/runs/%s", run_id);
  snprintf(ctx->context_id, sizeof(ctx->context_id), "%s", context_id);
  snprintf(ctx->origin_event_id, sizeof(ctx->origin_event_id), "bus_origin_lr");
  snprintf(ctx->origin_channel, sizeof(ctx->origin_channel), "tests");
  snprintf(ctx->event_kind, sizeof(ctx->event_kind), "user_message");
  ctx->next_event = 1;
  snprintf(path, sizeof(path), "%s/action_calls", ctx->run_dir);
  return test_mkdir_p(path);
}

static int lr_open(ReplayFixture *f, const char *run_id, const char *context_id) {
  RtContext ctx;
  char err[RT_LARGE] = "";
  memset(f, 0, sizeof(*f));
  if (lr_init_context(&ctx, run_id, context_id) != 0) return -1;
  f->scheduler = (RtScheduler *)calloc(1, sizeof(*f->scheduler));
  if (!f->scheduler) return -1;
  if (rt_action_registry_load(&f->scheduler->actions, err, sizeof(err)) != 0) {
    free(f->scheduler);
    f->scheduler = NULL;
    return -1;
  }
  f->scheduler->agent_meta_count = 1;
  snprintf(f->scheduler->agent_meta[0].id,
           sizeof(f->scheduler->agent_meta[0].id), "manager");
  f->scheduler->agent_meta[0].max_repair_attempts =
      RT_WORK_REPAIR_ATTEMPT_DEFAULT;
  f->run = (RtRun *)calloc(1, sizeof(*f->run));
  if (!f->run) return -1;
  f->run->scheduler = f->scheduler;
  f->run->ctx = ctx;
  return 0;
}

static void lr_close(ReplayFixture *f) {
  if (!f) return;
  if (f->run) {
    while (f->run->pending_action_count > 0)
      rt_action_pending_remove_at(f->run, f->run->pending_action_count - 1);
    free(f->run);
  }
  free(f->scheduler);
  memset(f, 0, sizeof(*f));
}

/* The durable input task every user_message run creates before an agent
 * runs. Action calls bind to it, which is what makes them queueable. */
static int lr_create_input_task(ReplayFixture *f, const char *task_id) {
  char payload[RT_XL];
  char etask[RT_MED], ectx[RT_LARGE];
  if (json_escape(task_id, etask, sizeof(etask)) != 0 ||
      json_escape(f->run->ctx.context_id, ectx, sizeof(ectx)) != 0)
    return -1;
  if (rt_append_event_format(
          &f->run->ctx, "task_created", "lr_test", payload, sizeof(payload), 0,
          "{\"task_id\":\"%s\",\"context_id\":\"%s\",\"kind\":\"input\","
          "\"input\":{\"type\":\"message\",\"text\":\"do it\"},"
          "\"created_event_id\":\"created_%s\",\"created_ms\":1000,"
          "\"state\":{\"status\":\"open\",\"work\":null,"
          "\"done_when\":null,\"artifacts\":[],\"updated_ms\":1000}}",
          etask, ectx, etask) != 0)
    return -1;
  snprintf(f->input_task_id, sizeof(f->input_task_id), "%s", task_id);
  return 0;
}

/* Queue one action call the way an accepted agent decision does, and hand
 * back the runtime-owned request id it was given. */
static int lr_queue(ReplayFixture *f, const char *calls_json,
                    char *rid_out, size_t rid_len) {
  RtStep step;
  char committed[RT_XL];
  size_t before = f->run->pending_action_count;
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "manage");
  snprintf(step.target, sizeof(step.target), "manager");
  if (rt_agent_append_output(&f->run->ctx, &step, &f->scheduler->agent_meta[0],
                             f->input_task_id, 0, calls_json,
                             committed, sizeof(committed)) != 0)
    return -1;
  rt_run_apply_output_events(f->run, step.target, committed);
  if (f->run->pending_action_count != before + 1) return -1;
  snprintf(rid_out, rid_len, "%s",
           f->run->pending_actions[before].request_id);
  return 0;
}

static const RtActionDef *lr_def(ReplayFixture *f, const char *id) {
  return rt_action_registry_find(&f->scheduler->actions, id);
}

static int lr_pending_started(ReplayFixture *f, const char *rid) {
  int idx = rt_action_runner_pending_index(f->run, rid);
  return idx >= 0 ? f->run->pending_actions[idx].started : -1;
}

/* The crash: a durable start claim and a committed effect, with no
 * terminal. Calls the intrinsic directly because no dispatch path can be
 * interrupted between its own two halves. */
static int lr_crash_after_effect(ReplayFixture *f, const char *rid,
                                 const RtActionDef *def, const char *args) {
  char result[RT_XL] = "{}";
  char error[RT_LARGE] = "null";
  RuntimeActionFn fn = NULL;
  if (!def) return -1;
  if (rt_action_emit_started(f->run, rid, def->id, NULL, "") != 0) return -1;
  if (strcmp(def->id, "affair_open") == 0)            fn = fc_affair_open;
  else if (strcmp(def->id, "affair_close") == 0)      fn = fc_affair_close;
  else if (strcmp(def->id, "defer") == 0)             fn = fc_defer;
  else if (strcmp(def->id, "note_add") == 0)          fn = fc_note_add;
  else if (strcmp(def->id, "counter_bump") == 0)      fn = fc_counter_bump;
  else if (strcmp(def->id, "work") == 0)              fn = fc_work;
  else return -1;
  return fn(f->run, rid, def, args, result, sizeof(result),
            error, sizeof(error));
}

static char *lr_log(const char *run_id) {
  char path[PATH_MAX];
  char *text = NULL;
  snprintf(path, sizeof(path), "workspace/runs/%s/event_log.jsonl", run_id);
  (void)test_read_file(path, &text);
  return text;
}

/* affair_open mints its id from the run's event counter, so a replay does
 * not merely repeat the effect — it opens a second, differently named
 * concern for one request, and the operator sees two watches where they
 * asked for one. */
int local_mutation_affair_open_replays_without_a_second_concern(void) {
  ReplayFixture f;
  char rid[RT_SMALL] = "";
  char *log = NULL, *affairs = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(lr_open(&f, "run_lr1", "chat:lr") == 0,
               "open the affair_open replay fixture");
  if (!f.run) return 1;
  rc |= expect(lr_create_input_task(&f, "task_lr_1") == 0,
               "an input task exists for calls to bind to");

  /* pre-effect: started, nothing applied. */
  rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
                            "\"manage\":\"watch the roof\"}}]}",
                        rid, sizeof(rid)) == 0,
               "queue the affair_open request");
  rc |= expect(rt_action_emit_started(f.run, rid, "affair_open", NULL, "") == 0,
               "the start claim is durable");
  rc |= expect(lr_pending_started(&f, rid) == 1, "the request is claimed");
  rc |= expect(rt_action_runner_recover_started(f.run) == 0,
               "recovery reconciles a local pre-effect crash");
  rc |= expect(lr_pending_started(&f, rid) == 0,
               "a local action stays launchable under its stable id");
  log = lr_log("run_lr1");
  rc |= expect(test_count_substr(log ? log : "", "\"type\":\"affair_patch\"") == 0,
               "the pre-effect crash applied nothing");
  free(log);

  /* the replay applies it exactly once. */
  rc |= expect(rt_action_runtime_complete(
                   f.run, rid, lr_def(&f, "affair_open"), "",
                   "{\"manage\":\"watch the roof\"}") == 0,
               "the replay dispatches the intrinsic");
  log = lr_log("run_lr1");
  rc |= expect(test_count_substr(log ? log : "", "\"action\":\"create\"") == 1,
               "one concern is created, not none and not two");
  free(log);
  lr_close(&f);

  /* post-effect: started and applied, no terminal. */
  rc |= test_reset_workspace();
  rc |= expect(lr_open(&f, "run_lr2", "chat:lr") == 0,
               "open the affair_open post-effect fixture");
  if (!f.run) return 1;
  rc |= expect(lr_create_input_task(&f, "task_lr_2") == 0,
               "an input task exists for calls to bind to");
  rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
                            "\"manage\":\"watch the roof\"}}]}",
                        rid, sizeof(rid)) == 0,
               "queue the post-effect affair_open request");
  rc |= expect(lr_crash_after_effect(&f, rid, lr_def(&f, "affair_open"),
                                     "{\"manage\":\"watch the roof\"}") == 0,
               "the effect commits before the crash");
  rc |= expect(rt_action_runner_recover_started(f.run) == 0,
               "recovery reconciles a local post-effect crash");
  rc |= expect(rt_action_runtime_complete(
                   f.run, rid, lr_def(&f, "affair_open"), "",
                   "{\"manage\":\"watch the roof\"}") == 0,
               "the replay dispatches the same request again");
  log = lr_log("run_lr2");
  rc |= expect(test_count_substr(log ? log : "", "\"action\":\"create\"") == 1,
               "the replay opens no second concern");
  rc |= expect_substr(log ? log : "", "\"recovered\":true",
                      "the replay reports the concern already committed");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  rc |= expect(test_count_substr(affairs ? affairs : "",
                                 "\"manage\":\"watch the roof\"") == 1,
               "the affair store holds exactly one watch");
  free(affairs);
  free(log);
  lr_close(&f);
  return rc;
}

/* A duplicated note is visible noise; a duplicated counter delta is not —
 * it just reads as a number that is quietly wrong. */
int local_mutation_note_and_counter_replay_without_doubling(void) {
  ReplayFixture f;
  char open_rid[RT_SMALL] = "", rid[RT_SMALL] = "";
  char affair_id[RT_SMALL] = "";
  char *log = NULL, *affairs = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(lr_open(&f, "run_lr3", "chat:lr") == 0,
               "open the note/counter replay fixture");
  if (!f.run) return 1;
  rc |= expect(lr_create_input_task(&f, "task_lr_3") == 0,
               "an input task exists for calls to bind to");
  rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
                            "\"manage\":\"track the sensor\"}}]}",
                        open_rid, sizeof(open_rid)) == 0 &&
               rt_action_runtime_complete(
                   f.run, open_rid, lr_def(&f, "affair_open"), "",
                   "{\"manage\":\"track the sensor\"}") == 0,
               "an affair exists to mutate");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  if (affairs) {
    const char *at = strstr(affairs, "\"affair_id\":\"");
    if (at) sscanf(at, "\"affair_id\":\"%127[^\"]", affair_id);
    free(affairs);
    affairs = NULL;
  }
  rc |= expect(affair_id[0] != '\0', "the created affair id is readable");
  snprintf(f.run->ctx.inbound_affair_id, sizeof(f.run->ctx.inbound_affair_id),
           "%s", affair_id);

  {
    char args[RT_LARGE];
    /* note_add */
    rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"note_add\",\"args\":{"
                              "\"text\":\"the reading held overnight\"}}]}",
                          rid, sizeof(rid)) == 0,
                 "queue the note_add request");
    rc |= expect(lr_crash_after_effect(&f, rid, lr_def(&f, "note_add"),
                                       "{\"text\":\"the reading held overnight\"}") == 0,
                 "the note commits before the crash");
    rc |= expect(rt_action_runner_recover_started(f.run) == 0,
                 "recovery reconciles the note crash");
    rc |= expect(rt_action_runtime_complete(
                     f.run, rid, lr_def(&f, "note_add"), "",
                     "{\"text\":\"the reading held overnight\"}") == 0,
                 "the note replay dispatches");
    log = lr_log("run_lr3");
    /* Count the committed event, not the prose: the same text also rides
     * the action_request payload that queued the call. */
    rc |= expect(test_count_substr(log ? log : "",
                                   "\"type\":\"affair_note_added\"") == 1,
                 "the note is recorded once, not twice");
    free(log);
    log = NULL;

    /* counter_bump, the silent one */
    snprintf(args, sizeof(args),
             "{\"affair_id\":\"%s\",\"name\":\"misses\",\"delta\":1}",
             affair_id);
    rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"counter_bump\",\"args\":{"
                              "\"affair_id\":\"AFFAIR\",\"name\":\"misses\","
                              "\"delta\":1}}]}", rid, sizeof(rid)) == 0,
                 "queue the counter_bump request");
    rc |= expect(lr_crash_after_effect(&f, rid, lr_def(&f, "counter_bump"),
                                       args) == 0,
                 "the counter delta commits before the crash");
    rc |= expect(rt_action_runner_recover_started(f.run) == 0,
                 "recovery reconciles the counter crash");
    rc |= expect(rt_action_runtime_complete(
                     f.run, rid, lr_def(&f, "counter_bump"), "", args) == 0,
                 "the counter replay dispatches");
    log = lr_log("run_lr3");
    rc |= expect(test_count_substr(log ? log : "",
                                   "\"type\":\"affair_counter_bumped\"") == 1,
                 "one delta is committed for one request");
    rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
    rc |= expect_substr(affairs ? affairs : "", "\"misses\"",
                        "the counter exists in durable state");
    rc |= expect_no_substr(affairs ? affairs : "", "\"misses\":2",
                           "a replayed delta does not double the count");
    free(affairs);
    free(log);
  }
  lr_close(&f);
  return rc;
}

/* defer recomputes its deadline from the current clock. A replay at
 * restart is not a duplicate — it is a different, later answer, so the
 * committed one has to win. */
int local_mutation_defer_and_close_replay_to_the_committed_decision(void) {
  ReplayFixture f;
  char open_rid[RT_SMALL] = "", rid[RT_SMALL] = "";
  char affair_id[RT_SMALL] = "";
  char *log = NULL, *affairs = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(lr_open(&f, "run_lr4", "chat:lr") == 0,
               "open the defer/close replay fixture");
  if (!f.run) return 1;
  rc |= expect(lr_create_input_task(&f, "task_lr_4") == 0,
               "an input task exists for calls to bind to");
  rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
                            "\"manage\":\"chase the invoice\"}}]}",
                        open_rid, sizeof(open_rid)) == 0 &&
               rt_action_runtime_complete(
                   f.run, open_rid, lr_def(&f, "affair_open"), "",
                   "{\"manage\":\"chase the invoice\"}") == 0,
               "an affair exists to defer");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  if (affairs) {
    const char *at = strstr(affairs, "\"affair_id\":\"");
    if (at) sscanf(at, "\"affair_id\":\"%127[^\"]", affair_id);
    free(affairs);
    affairs = NULL;
  }
  rc |= expect(affair_id[0] != '\0', "the affair id is readable");
  snprintf(f.run->ctx.inbound_affair_id, sizeof(f.run->ctx.inbound_affair_id),
           "%s", affair_id);

  rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"defer\",\"args\":{"
                            "\"value\":\"1d\"}}]}", rid, sizeof(rid)) == 0,
               "queue the defer request");
  rc |= expect(lr_crash_after_effect(&f, rid, lr_def(&f, "defer"),
                                     "{\"value\":\"1d\"}") == 0,
               "the deferred deadline commits before the crash");
  {
    long long committed_ms = 0;
    char *state = NULL;
    rc |= test_read_file("workspace/memory/state/affairs.json", &state);
    if (state) {
      const char *at = strstr(state, "\"next_review_at_ms\":");
      if (at) sscanf(at, "\"next_review_at_ms\":%lld", &committed_ms);
      free(state);
    }
    rc |= expect(committed_ms > 0, "a deadline is durably recorded");
    rc |= expect(rt_action_runner_recover_started(f.run) == 0,
                 "recovery reconciles the defer crash");
    rc |= expect(rt_action_runtime_complete(
                     f.run, rid, lr_def(&f, "defer"), "",
                     "{\"value\":\"1d\"}") == 0,
                 "the defer replay dispatches");
    state = NULL;
    rc |= test_read_file("workspace/memory/state/affairs.json", &state);
    if (state) {
      long long after_ms = 0;
      const char *at = strstr(state, "\"next_review_at_ms\":");
      if (at) sscanf(at, "\"next_review_at_ms\":%lld", &after_ms);
      rc |= expect(after_ms == committed_ms,
                   "the replay keeps the deadline the crash committed");
      free(state);
    }
    log = lr_log("run_lr4");
    rc |= expect(test_count_substr(log ? log : "",
                                   "\"type\":\"affair_patch\"") == 2,
                 "one create and one defer, with no third patch");
    free(log);
    log = NULL;
  }

  rc |= expect(lr_queue(&f, "{\"calls\":[{\"name\":\"affair_close\",\"args\":{}}]}",
                        rid, sizeof(rid)) == 0,
               "queue the affair_close request");
  rc |= expect(lr_crash_after_effect(&f, rid, lr_def(&f, "affair_close"),
                                     "{}") == 0,
               "the close commits before the crash");
  rc |= expect(rt_action_runner_recover_started(f.run) == 0,
               "recovery reconciles the close crash");
  rc |= expect(rt_action_runtime_complete(
                   f.run, rid, lr_def(&f, "affair_close"), "", "{}") == 0,
               "the close replay dispatches");
  log = lr_log("run_lr4");
  rc |= expect(test_count_substr(log ? log : "", "\"status\":\"closed\"") == 1,
               "the concern is closed once");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  rc |= expect_substr(affairs ? affairs : "", "\"status\":\"closed\"",
                      "the affair is durably closed");
  free(affairs);
  free(log);
  lr_close(&f);
  return rc;
}

/* work is the compounding one. A replayed create mints a second task id
 * from the run's event counter, and from then on
 * rt_task_select_unique_revisable_work sees two revisable work tasks in the
 * context and refuses every later work call as ambiguous. */
int local_mutation_work_replays_without_a_second_task(void) {
  ReplayFixture f;
  char rid[RT_SMALL] = "", second_rid[RT_SMALL] = "";
  char *log = NULL, *tasks = NULL;
  int rc = 0;
  const char *args =
      "{\"work\":\"mirror the playlist\",\"done_when\":\"it is mirrored\"}";
  const char *calls =
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"mirror the playlist\",\"done_when\":\"it is mirrored\"}}]}";

  rc |= test_reset_workspace();
  rc |= expect(lr_open(&f, "run_lr5", "chat:lr") == 0,
               "open the work replay fixture");
  if (!f.run) return 1;
  rc |= expect(lr_create_input_task(&f, "task_lr_5") == 0,
               "an input task exists for calls to bind to");
  rc |= expect(lr_queue(&f, calls, rid, sizeof(rid)) == 0,
               "queue the work request");
  rc |= expect(lr_crash_after_effect(&f, rid, lr_def(&f, "work"), args) == 0,
               "the work task commits before the crash");
  rc |= expect(rt_action_runner_recover_started(f.run) == 0,
               "recovery reconciles the work crash");
  rc |= expect(rt_action_runtime_complete(
                   f.run, rid, lr_def(&f, "work"), "", args) == 0,
               "the work replay dispatches");
  log = lr_log("run_lr5");
  rc |= expect(test_count_substr(log ? log : "", "\"kind\":\"work\"") == 1,
               "one work task is created for one request");
  rc |= expect_substr(log ? log : "", "\"recovered\":true",
                      "the replay reports the task already committed");
  free(log);
  log = NULL;
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect(test_count_substr(tasks ? tasks : "",
                                 "mirror the playlist") == 1,
               "the task store holds exactly one work task");
  /* Without the guard the replay does not create a second task -- it
   * finds the first one and revises it, inventing a revision no agent
   * asked for and moving the number managed operations compare against. */
  rc |= expect_substr(tasks ? tasks : "", "\"work_rev\":1",
                      "the replay invents no revision of its own");
  free(tasks);
  tasks = NULL;

  /* And because it holds one, the next work call still resolves a unique
   * revision target instead of failing as ambiguous. */
  rc |= expect(lr_queue(&f,
                        "{\"calls\":[{\"name\":\"work\",\"args\":{"
                        "\"work\":\"mirror it to the other account too\","
                        "\"done_when\":\"both are mirrored\"}}]}",
                        second_rid, sizeof(second_rid)) == 0,
               "queue a follow-up work revision");
  rc |= expect(rt_action_runtime_complete(
                   f.run, second_rid, lr_def(&f, "work"), "",
                   "{\"work\":\"mirror it to the other account too\","
                   "\"done_when\":\"both are mirrored\"}") == 0,
               "the follow-up revision dispatches");
  log = lr_log("run_lr5");
  rc |= expect_no_substr(log ? log : "", "work revision is ambiguous",
                         "a replayed create does not wedge later revisions");
  rc |= expect_substr(log ? log : "", "\"work_rev\":2",
                      "the follow-up revises the one existing task");
  free(log);
  lr_close(&f);
  return rc;
}
