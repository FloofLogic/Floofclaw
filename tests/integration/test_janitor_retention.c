/* Terminal-run retention.
 *
 * The janitor decided "is this run finished?" by reading a key the
 * serializer never wrote, so `run_is_terminal` was always false, nothing
 * was ever pruned, and the fail-safe ("can't read → keep") made it silent:
 * zero errors reported while every deployment retained 100% of its runs.
 * These tests pin both halves of the decision — the status the janitor
 * reads, and which entries a capped scan even considers. */

#include "test_support.h"

#include "../../runtime/gateway/janitor_module.h"
#include "../../runtime/run_state.h"
#include "../../runtime/support/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A runstate.json exactly as the serializer writes it: the lifecycle
 * status under its one owned key, and no legacy alias to fall back on. */
static int write_run(const char *run_id, const char *status) {
  char path[256], body[512];
  snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json", run_id);
  snprintf(body, sizeof(body),
           "{\"run_id\":\"%s\",\"context_id\":\"chat:tests\","
           "\"profile\":\"tests\",\"" RT_RUNSTATE_STATUS_KEY "\":\"%s\","
           "\"advance\":1,\"created_from\":{\"event_id\":\"bus_000001\","
           "\"type\":\"user_message\"},"
           "\"waiting\":{\"jobs\":[],\"events\":[],\"deadline\":null},"
           "\"cursors\":{\"phase_index\":0,\"last_advanced_event_seq\":null},"
           "\"last_error\":null}\n",
           run_id, status);
  return test_write_file(path, body);
}

static int run_exists(const char *run_id) {
  char path[256];
  snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json", run_id);
  return test_file_size(path) >= 0;
}

int janitor_prunes_terminal_runs_and_keeps_live_ones(void) {
  int rc = 0;

  rc |= test_reset_workspace();
  /* Oldest first: two terminals, a live run between them, then newer
   * runs the pressure should not reach. */
  rc |= write_run("run_001", "done");
  rc |= write_run("run_002", "running");
  rc |= write_run("run_003", "failed");
  rc |= write_run("run_004", "done");
  rc |= write_run("run_005", "waiting_jobs");
  rc |= write_run("run_006", "done");

  /* Six runs, keep four: the janitor may drop two, and only terminals
   * are eligible, walking from the oldest end. */
  rc |= expect(fc_janitor_test_runs_prune("workspace/runs", 4) == 0,
               "janitor prune runs under retention pressure");
  rc |= expect_not(run_exists("run_001"),
                   "the oldest terminal run is pruned");
  rc |= expect_not(run_exists("run_003"),
                   "the next-oldest terminal run is pruned");
  rc |= expect(run_exists("run_002"),
               "a running run between them is never pruned");
  rc |= expect(run_exists("run_005"),
               "a run waiting on jobs is never pruned");
  rc |= expect(run_exists("run_004"),
               "pruning stops once the drop budget is spent");
  rc |= expect(run_exists("run_006"),
               "the newest terminal run survives the budget");

  /* An unreadable or status-less runstate is kept, not discarded: the
   * fail-safe stays a fail-safe now that the key is read correctly. */
  rc |= test_write_file("workspace/runs/run_007/runstate.json",
                        "{\"run_id\":\"run_007\"}\n");
  rc |= test_write_file("workspace/runs/run_008/runstate.json", "not json\n");
  rc |= expect(fc_janitor_test_runs_prune("workspace/runs", 1) == 0,
               "janitor prune with malformed runstates present");
  rc |= expect(run_exists("run_007"),
               "a runstate with no status is kept, not pruned");
  rc |= expect(run_exists("run_008"),
               "an unreadable runstate is kept, not pruned");
  return rc;
}

int janitor_scan_cap_drops_the_newest_not_readdir_order(void) {
  char csv[1024] = "";
  int rc = 0;
  int i;

  rc |= test_reset_workspace();
  /* Created out of numeric order so a readdir-ordered scan cannot pass
   * by accident on a filesystem that happens to return them sorted. */
  static const int kOrder[] = {7, 2, 9, 1, 5, 10, 3, 8, 4, 6};
  for (i = 0; i < (int)(sizeof(kOrder) / sizeof(kOrder[0])); ++i) {
    char run_id[32];
    snprintf(run_id, sizeof(run_id), "run_%03d", kOrder[i]);
    rc |= write_run(run_id, "done");
  }

  /* The pruner deletes from the old end, so a capped scan must carry the
   * OLDEST entries — the newest are the ones that lose their slot. */
  rc |= expect(fc_janitor_test_list_oldest("workspace/runs", "run_", 4,
                                           csv, sizeof(csv)) == 4,
               "capped scan returns exactly the cap");
  rc |= expect(strcmp(csv, "run_001,run_002,run_003,run_004") == 0,
               csv[0] ? csv : "capped scan selects the four oldest in order");

  /* Below the cap, every entry is present and still ascending. */
  rc |= expect(fc_janitor_test_list_oldest("workspace/runs", "run_", 32,
                                           csv, sizeof(csv)) == 10,
               "an uncapped scan returns every entry");
  rc |= expect(strcmp(csv,
                      "run_001,run_002,run_003,run_004,run_005,run_006,"
                      "run_007,run_008,run_009,run_010") == 0,
               csv[0] ? csv : "uncapped scan is ascending");

  /* And the newest runs survive an actual prune under that cap. */
  rc |= expect(fc_janitor_test_runs_prune("workspace/runs", 6) == 0,
               "prune ten terminal runs down to six");
  rc |= expect_not(run_exists("run_001"), "oldest pruned");
  rc |= expect_not(run_exists("run_004"), "fourth-oldest pruned");
  rc |= expect(run_exists("run_005"), "the newest six survive");
  rc |= expect(run_exists("run_010"), "the newest run survives");
  return rc;
}

/* Managed-worker store retention.
 *
 * Only codex_ops had a retention task. Claude Code directories under
 * workspace/logs/claude_ops/<op_id> and Hermes handles under
 * workspace/logs/hermes_ops/ had none and grew unbounded. All three now
 * share one task, one config shape, and one exception to count-only
 * retention: an operation that is still running owns its entry. */

static int write_worker_entry(const char *store, const char *name,
                              int as_directory) {
  char path[320];
  if (as_directory)
    snprintf(path, sizeof(path), "workspace/logs/%s/%s/rollout.jsonl",
             store, name);
  else
    snprintf(path, sizeof(path), "workspace/logs/%s/%s", store, name);
  return test_write_file(path, "{}\n");
}

static int worker_entry_exists(const char *store, const char *name,
                               int as_directory) {
  char path[320];
  if (as_directory)
    snprintf(path, sizeof(path), "workspace/logs/%s/%s/rollout.jsonl",
             store, name);
  else
    snprintf(path, sizeof(path), "workspace/logs/%s/%s", store, name);
  return test_file_size(path) >= 0;
}

/* One operations.json in the shape the store's serializer writes, so
 * rt_operation_status reads it the way the janitor will in deployment. */
static int write_operations(const char *running_handle) {
  char body[1024];
  snprintf(body, sizeof(body),
           "{\"operations\":[{\"handle\":\"%s\",\"worker_handle\":\"1234\","
           "\"action\":\"manage_codex\",\"request_id\":\"actionreq_x\","
           "\"trigger_event_id\":\"\",\"task_id\":\"task_x\","
           "\"context_id\":\"chat:tests\",\"channel\":\"tests\","
           "\"adapter_id\":\"\",\"origin_event_id\":\"\",\"ref\":null,"
           "\"status\":\"running\",\"work_rev\":1,\"deadline_ms\":0,"
           "\"completion_token\":\"\",\"claim_envelope_id\":\"\"}]}\n",
           running_handle);
  return test_write_file("workspace/memory/state/operations.json", body);
}

int janitor_prunes_every_managed_worker_store(void) {
  int rc = 0;
  int i;

  rc |= test_reset_workspace();
  /* Six per store, oldest first by trailing number. Directories for the
   * process workers, single JSON files for Hermes handles. */
  for (i = 1; i <= 6; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "op_actionreq_run_001_%06d", i);
    rc |= write_worker_entry("codex_ops", name, 1);
    rc |= write_worker_entry("claude_ops", name, 1);
    snprintf(name, sizeof(name), "hermes_actionreq_run_001_%06d.json", i);
    rc |= write_worker_entry("hermes_ops", name, 0);
  }

  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/codex_ops", "", 4) == 0,
               "codex_ops retention runs");
  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/claude_ops", "", 4) == 0,
               "claude_ops retention runs");
  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/hermes_ops", "hermes_", 4) == 0,
               "hermes_ops retention runs");

  rc |= expect_not(worker_entry_exists("codex_ops",
                                       "op_actionreq_run_001_000001", 1),
                   "the oldest codex_ops directory is pruned");
  rc |= expect(worker_entry_exists("codex_ops",
                                   "op_actionreq_run_001_000006", 1),
               "the newest codex_ops directory survives");
  rc |= expect_not(worker_entry_exists("claude_ops",
                                       "op_actionreq_run_001_000002", 1),
                   "claude_ops is pruned to its keep count");
  rc |= expect(worker_entry_exists("claude_ops",
                                   "op_actionreq_run_001_000003", 1),
               "claude_ops keeps exactly the newest four");
  rc |= expect_not(worker_entry_exists("hermes_ops",
                                       "hermes_actionreq_run_001_000001.json", 0),
                   "the oldest hermes handle file is pruned");
  rc |= expect(worker_entry_exists("hermes_ops",
                                   "hermes_actionreq_run_001_000006.json", 0),
               "the newest hermes handle file survives");

  /* Below the keep count nothing moves, in every store. */
  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/codex_ops", "", 200) == 0,
               "retention below the keep count is a no-op");
  rc |= expect(worker_entry_exists("codex_ops",
                                   "op_actionreq_run_001_000003", 1),
               "nothing is dropped under no pressure");

  /* A store that has never been written is not an error. */
  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/never_ran", "", 4) == 0,
               "a missing store directory is not a failure");
  return rc;
}

int janitor_never_prunes_a_running_operations_store_entry(void) {
  int rc = 0;
  int i;

  rc |= test_reset_workspace();
  for (i = 1; i <= 6; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "op_actionreq_run_001_%06d", i);
    rc |= write_worker_entry("claude_ops", name, 1);
    snprintf(name, sizeof(name), "hermes_actionreq_run_001_%06d.json", i);
    rc |= write_worker_entry("hermes_ops", name, 0);
  }
  /* The oldest entry in each store belongs to an operation still in
   * flight. Its worker is writing into it right now. */
  rc |= write_operations("op_actionreq_run_001_000001");

  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/claude_ops", "", 4) == 0,
               "retention runs with a live operation present");
  rc |= expect(worker_entry_exists("claude_ops",
                                   "op_actionreq_run_001_000001", 1),
               "a running operation's directory is never pruned");
  rc |= expect_not(worker_entry_exists("claude_ops",
                                       "op_actionreq_run_001_000002", 1),
                   "the next-oldest finished entry is pruned instead");

  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/hermes_ops", "hermes_", 4) == 0,
               "hermes retention runs with a live operation present");
  rc |= expect(worker_entry_exists("hermes_ops",
                                   "hermes_actionreq_run_001_000001.json", 0),
               "a running hermes handle is never pruned");

  /* Once it terminalizes, the same entry is ordinary debris. Dropping
   * the record entirely is the same case: the operation store keeps a
   * bounded window of finished records, so an id it has forgotten must
   * not be protected forever. */
  rc |= test_write_file("workspace/memory/state/operations.json",
                        "{\"operations\":[]}\n");
  rc |= expect(fc_janitor_test_worker_store_prune(
                   "workspace/logs/claude_ops", "", 3) == 0,
               "retention runs after the operation is forgotten");
  rc |= expect_not(worker_entry_exists("claude_ops",
                                       "op_actionreq_run_001_000001", 1),
                   "a forgotten operation's directory is prunable again");
  return rc;
}

/* Retention is scheduled state, not a one-shot: a gateway that restarts
 * must re-apply it to whatever accumulated while it was down. */
int janitor_restart_reapplies_worker_store_retention(void) {
  FcReactorModule *m = NULL;
  int rc = 0;
  int i;

  rc |= test_reset_workspace();
  rc |= test_write_file("config/floofclaw_config.json",
                        "{\"appliance\":{"
                        "\"codex_ops\":{\"keep\":2,\"check_interval_ms\":1000},"
                        "\"claude_ops\":{\"keep\":2,\"check_interval_ms\":1000},"
                        "\"hermes_ops\":{\"keep\":2,\"check_interval_ms\":1000}"
                        "}}\n");
  for (i = 1; i <= 5; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "op_actionreq_run_001_%06d", i);
    rc |= write_worker_entry("claude_ops", name, 1);
  }
  m = fc_janitor_module_create();
  rc |= expect(m != NULL && m->init(m) == 0, "janitor initializes");
  if (m) {
    rc |= expect(m->tick(m, fc_now_ms() + 400000U) == 0, "janitor ticks");
    fc_janitor_module_destroy(m);
    m = NULL;
  }
  rc |= expect_not(worker_entry_exists("claude_ops",
                                       "op_actionreq_run_001_000001", 1),
                   "configured claude_ops keep is honored on the first run");
  rc |= expect(worker_entry_exists("claude_ops",
                                   "op_actionreq_run_001_000005", 1),
               "the newest configured-keep entries survive");

  /* More arrive while the gateway is down; a restart cleans them too. */
  for (i = 6; i <= 9; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "op_actionreq_run_001_%06d", i);
    rc |= write_worker_entry("claude_ops", name, 1);
  }
  m = fc_janitor_module_create();
  rc |= expect(m != NULL && m->init(m) == 0, "janitor initializes after restart");
  if (m) {
    rc |= expect(m->tick(m, fc_now_ms() + 400000U) == 0,
                 "janitor ticks after restart");
    fc_janitor_module_destroy(m);
  }
  rc |= expect_not(worker_entry_exists("claude_ops",
                                       "op_actionreq_run_001_000007", 1),
                   "a restart re-applies retention to what accumulated");
  rc |= expect(worker_entry_exists("claude_ops",
                                   "op_actionreq_run_001_000009", 1),
               "the newest two survive the restart");
  return rc;
}
