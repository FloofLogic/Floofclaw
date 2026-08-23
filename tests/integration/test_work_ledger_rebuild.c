/* Work-ledger rebuild must not brick the gateway.
 *
 * `rt_work_state_rebuild_from_logs()` runs at startup, and it replays a
 * WINDOW of history: run directories older than the retention keep count are
 * pruned away, and the scan is capped at WORK_REBUILD_RUN_CAP. An event
 * whose ancestor fell outside that window cannot be applied — and aborting
 * the whole rebuild there left the ledger unwritten and the gateway unable
 * to start at all. The production incident (run_043, task
 * task_run_038_000008) was recovered only by moving a run to
 * workspace/runs_quarantine/ by hand, which frees a run number for reuse and
 * risks workspace_task_id_collision.
 *
 * Skipping is not the same as ignoring: every skipped event is named
 * exactly, so the operator has a run and an event id to inspect. */

#include "test_support.h"

#include "../../runtime/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEDGER_CONTEXT "chat:tests:ledger"

static int write_run_log(const char *run_id, const char *events) {
  char path[256], body[512];
  int rc = 0;
  snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json", run_id);
  snprintf(body, sizeof(body),
           "{\"run_id\":\"%s\",\"context_id\":\"" LEDGER_CONTEXT "\","
           "\"profile\":\"tests\",\"status\":\"done\"}\n", run_id);
  rc |= test_write_file(path, body);
  snprintf(path, sizeof(path), "workspace/runs/%s/event_log.jsonl", run_id);
  rc |= test_write_file(path, events);
  return rc;
}

/* The exact shapes the runtime emits. */
#define EV(seq, type, payload)                                             \
  "{\"event_id\":\"evt_run_001_" seq "\",\"ts\":\"2026-07-29T00:00:00Z\"," \
  "\"type\":\"" type "\",\"source\":\"tests\",\"run_id\":\"run_001\","     \
  "\"payload\":" payload "}\n"

#define TASK_CREATED                                                       \
  EV("000001", "task_created",                                             \
     "{\"task_id\":\"task_run_001_000001\",\"kind\":\"work\","             \
     "\"work_rev\":1,\"context_id\":\"" LEDGER_CONTEXT "\"}")

/* A controller selection: this is what stamps a revision's correlation. */
#define SELECT(seq, rev, rid, trigger)                                     \
  EV(seq, "action_request",                                                \
     "{\"task_id\":\"task_run_001_000001\",\"work_rev\":" rev ","          \
     "\"request_id\":\"" rid "\",\"action\":\"work\","                     \
     "\"trigger_event_id\":\"" trigger "\","                               \
     "\"context_id\":\"" LEDGER_CONTEXT "\",\"args\":{}}")

/* A revision-bearing task_updated with no revision_origin: it mints a
 * revision carrying no request_id, action, or trigger_event_id. */
#define BARE_REVISION(seq, rev)                                            \
  EV(seq, "task_updated",                                                  \
     "{\"task_id\":\"task_run_001_000001\",\"work_rev\":" rev ","          \
     "\"context_id\":\"" LEDGER_CONTEXT "\","                              \
     "\"state\":{\"status\":\"open\",\"work\":\"w\",\"done_when\":\"d\"}}")

#define CONTROLLER_REVISION(seq, rev, parent, rid, trigger)                \
  EV(seq, "task_updated",                                                  \
     "{\"task_id\":\"task_run_001_000001\",\"work_rev\":" rev ","          \
     "\"state\":{\"status\":\"open\",\"work\":\"w\",\"done_when\":\"d\"},"  \
     "\"revision_origin\":\"controller\",\"parent_work_rev\":" parent ","  \
     "\"request_id\":\"" rid "\",\"action\":\"work\","                     \
     "\"trigger_event_id\":\"" trigger "\","                               \
     "\"context_id\":\"" LEDGER_CONTEXT "\"}")

int work_ledger_rebuild_survives_a_lineage_outside_the_window(void) {
  char *ledger = NULL;
  char *narration = NULL;
  int rc = 0;

  rc |= test_reset_workspace();

  /* The production shape: a bare update mints revision 2, and a controller
   * revision 3 names it as parent — but the selection that connected them
   * is in a run this replay can no longer see. Live, that selection was in
   * the store and the sequence applied cleanly. */
  rc |= write_run_log("run_001",
                      TASK_CREATED
                      SELECT("000002", "1", "actionreq_a", "evt_trigger_a")
                      BARE_REVISION("000003", "2")
                      CONTROLLER_REVISION("000005", "3", "2",
                                          "actionreq_b", "evt_trigger_b"));

  rc |= expect(rt_work_state_rebuild_from_logs() == 0,
               "rebuild completes despite an unreachable lineage");
  rc |= expect(test_read_file("workspace/memory/state/work_steps.json",
                              &ledger) == 0,
               "the ledger is written, so the gateway can start");
  if (ledger) {
    rc |= expect_substr(ledger, "\"work_rev\":1",
                        "applicable history is still rebuilt");
    rc |= expect_substr(ledger, "actionreq_a",
                        "the reachable step survives the rebuild");
    free(ledger); ledger = NULL;
  }

  /* Skipped, not ignored: the operator gets the exact run and event. */
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration) == 0
            ? 0 : expect(0, "the skip is narrated");
  if (narration) {
    rc |= expect_substr(narration, "run_001",
                        "the skip names the run");
    rc |= expect_substr(narration, "evt_run_001_000005",
                        "the skip names the exact event");
    rc |= expect_substr(narration, "task_updated",
                        "the skip names the event type");
    rc |= expect_substr(narration, "1 event(s) skipped",
                        "the rebuild reports how much it could not apply");
    free(narration); narration = NULL;
  }

  /* A restart repeats this, so it has to be stable: rebuilding again over
   * the ledger it just wrote must also succeed. */
  rc |= expect(rt_work_state_rebuild_from_logs() == 0,
               "a second rebuild over the written ledger succeeds");
  return rc;
}

int work_ledger_rebuild_applies_a_complete_lineage(void) {
  char *ledger = NULL;
  char *narration = NULL;
  int rc = 0;

  rc |= test_reset_workspace();

  /* Same shape, but the selection at revision 2 is inside the window. The
   * controller revision must be APPLIED, not skipped — otherwise the fix
   * above would have quietly turned rebuild into a no-op. */
  rc |= write_run_log("run_001",
                      TASK_CREATED
                      SELECT("000002", "1", "actionreq_a", "evt_trigger_a")
                      BARE_REVISION("000003", "2")
                      SELECT("000004", "2", "actionreq_b", "evt_trigger_b")
                      CONTROLLER_REVISION("000005", "3", "2",
                                          "actionreq_b", "evt_trigger_b"));

  rc |= expect(rt_work_state_rebuild_from_logs() == 0,
               "rebuild accepts a complete lineage");
  rc |= expect(test_read_file("workspace/memory/state/work_steps.json",
                              &ledger) == 0,
               "rebuild wrote the ledger");
  if (ledger) {
    rc |= expect_substr(ledger, "\"work_rev\":3",
                        "the controller revision is applied, not skipped");
    rc |= expect_substr(ledger, "\"parent_work_rev\":2",
                        "the controller revision keeps its parent");
    rc |= expect_substr(ledger, "actionreq_b",
                        "the selected step survives the rebuild");
    free(ledger); ledger = NULL;
  }
  if (test_read_file("workspace/logs/narration.jsonl", &narration) == 0 &&
      narration) {
    rc |= expect_no_substr(narration, "skipped",
                           "a complete lineage skips nothing");
    free(narration); narration = NULL;
  }
  return rc;
}
