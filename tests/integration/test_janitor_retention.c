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
