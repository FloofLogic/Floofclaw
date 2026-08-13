/* Source-scan budget checks — pure C, no fork. They read runtime
 * source files and verify hot-path discipline (single profile load,
 * centralized agent.json read). Heap-sample budget tests live in
 * tests/integration/test_budget.c because they drive a real gateway. */

#include "test_support.h"

#include <stdlib.h>
#include <string.h>

int profile_loaded_once_per_scheduler_init(void) {
  char *src = NULL;
  int rc = 0;
  rc |= test_read_file("runtime/scheduler.c", &src);
  rc |= expect(test_count_substr(src, "rt_profile_load(s->loop_name") == 1,
               "scheduler loads profile once at init");
  rc |= expect_substr(src, "s->profile_loaded = 1",
                      "profile load is cached on scheduler");
  free(src);
  return rc;
}

int agent_metadata_loaded_once_per_profile_agent(void) {
  char *agent_runner = NULL, *agent_exec = NULL, *scheduler = NULL;
  int rc = 0;
  rc |= test_read_file("runtime/agent_runner.c", &agent_runner);
  rc |= test_read_file("runtime/agent_exec.c", &agent_exec);
  rc |= test_read_file("runtime/scheduler.c", &scheduler);
  rc |= expect_substr(agent_runner, "rt_agent_runner_preload_meta",
                      "agent metadata preload exists");
  rc |= expect_substr(agent_runner, "agent_meta_lookup",
                      "hot path uses agent metadata lookup");
  rc |= expect(test_count_substr(scheduler, "agents/%s/agent.json") == 0,
               "scheduler does not read agent agent.json directly");
  rc |= expect(test_count_substr(agent_runner, "agents/%s/agent.json") == 0,
               "agent_runner does not read agent.json directly either");
  rc |= expect(test_count_substr(agent_exec, "rt_floop_agent_dir") == 1,
               "agent directory resolution is centralized in shared agent_exec");
  free(scheduler); free(agent_exec); free(agent_runner);
  return rc;
}
