#include "../../runtime/gateway/gateway.h"
#include "../../runtime/gateway/reactor.h"
#include "../../runtime/gateway/janitor_module.h"
#include "../../runtime/gateway/status_module.h"
#include "../../runtime/inspection_stream.h"
#include "../../runtime/action_internal.h"
#include "../../adapters/discord/discord_internal.h"
#include "../../adapters/irc/irc_adapter.h"
#include "../../runtime/bus/bus.h"
#include "../../runtime/ports.h"
#include "../../runtime/runtime.h"
#include "../../runtime/runtime_kernel.h"
#include "../../runtime/secure/secret_store.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/net.h"
#include "../../runtime/support/timing.h"
#include "../harness.h"
#include "../test_support.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int scheduler_startup_error(const char *floop, char *err,
                                   size_t err_len) {
  RtScheduler *scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  int init_rc;
  if (!scheduler) return -1;
  init_rc = rt_scheduler_init(scheduler, floop, err, err_len);
  rt_scheduler_destroy(scheduler);
  free(scheduler);
  return init_rc;
}

int view_t_rejects_unknown_flag(void) {
  char *out = NULL;
  char *argv[] = { "-t", "run_001" };
  int exit_code = 0;
  int rc = 0;
  rc |= expect(harness_cli_view(2, argv, &out, &exit_code) == 0,
               "capture view -t rejection");
  rc |= expect(exit_code != 0, "view -t exits nonzero");
  rc |= expect(out && strcmp(out, "view: unknown flag -t\n") == 0,
               "view -t fails with one exact unknown-flag line");
  free(out);
  return rc;
}

int persisted_run_status_vocabulary_has_one_owner(void) {
  static const char *const active[] = {
    "ready", "running", "waiting_jobs", "waiting_event"
  };
  static const char *const terminal[] = { "done", "failed" };
  int rc = 0;
  for (size_t i = 0; i < sizeof(active) / sizeof(active[0]); ++i) {
    rc |= expect(rt_run_status_is_valid(active[i]),
                 "active persisted run status is valid");
    rc |= expect(!rt_run_status_is_terminal(active[i]),
                 "active persisted run status is nonterminal");
  }
  for (size_t i = 0; i < sizeof(terminal) / sizeof(terminal[0]); ++i) {
    rc |= expect(rt_run_status_is_valid(terminal[i]),
                 "terminal persisted run status is valid");
    rc |= expect(rt_run_status_is_terminal(terminal[i]),
                 "terminal persisted run status is terminal");
  }
  rc |= expect(!rt_run_status_is_valid(NULL) &&
               !rt_run_status_is_valid("") &&
               !rt_run_status_is_valid("canceled") &&
               !rt_run_status_is_valid("waiting_action_slot"),
               "unknown or internal-only statuses are not persisted values");
  rc |= expect(!rt_run_status_is_terminal(NULL) &&
               !rt_run_status_is_terminal("unknown"),
               "unknown statuses are never terminal");
  return rc;
}

int view_debug_correlates_failed_action_stderr(void) {
  static const char request_id[] = "actionreq_run_view_action_failure_000005";
  static const char events[] =
      "{\"event_id\":\"evt_view_000001\",\"ts\":\"2026-07-21T00:00:00Z\","
      "\"type\":\"action_failed\",\"source\":\"action_runner\","
      "\"run_id\":\"run_view_action_failure\",\"payload\":{"
      "\"request_id\":\"actionreq_run_view_action_failure_000005\","
      "\"action\":\"fixture_fail\",\"run_id\":\"run_view_action_failure\","
      "\"job_id\":\"job_002\",\"reason\":\"exit_nonzero\","
      "\"result\":{},\"error\":{\"message\":\"fixture failed\"}}}\n"
      "{\"event_id\":\"evt_view_000002\",\"ts\":\"2026-07-21T00:00:01Z\","
      "\"type\":\"action_rejected\",\"source\":\"action_runner\","
      "\"run_id\":\"run_view_action_failure\",\"payload\":{"
      "\"request_id\":\"actionreq_run_view_action_failure_000006\","
      "\"action\":\"missing_action\",\"reason\":\"unknown_action\","
      "\"message\":\"action is not registered\",\"errors\":[]}}\n"
      "{\"event_id\":\"evt_view_000003\",\"ts\":\"2026-07-21T00:00:02Z\","
      "\"type\":\"run_failed\",\"source\":\"scheduler\","
      "\"run_id\":\"run_view_action_failure\","
      "\"payload\":{\"reason\":\"action failed\"}}\n";
  char input[RT_LARGE];
  char *out = NULL;
  char *argv[] = { "-h", "-d", "run_view_action_failure" };
  int exit_code = 0;
  int rc = 0;
  snprintf(input, sizeof(input),
           "{\"request_id\":\"%s\",\"action\":\"fixture_fail\","
           "\"args\":{},\"run_id\":\"run_view_action_failure\"}\n",
           request_id);
  rc |= test_reset_workspace();
  rc |= test_mkdir_p("workspace/runs/run_view_action_failure/action_calls");
  rc |= test_write_file("workspace/runs/run_view_action_failure/event_log.jsonl",
                        events);
  rc |= test_write_file(
      "workspace/runs/run_view_action_failure/action_calls/001_actionreq_run_view_action_failure_000005.input.json",
      input);
  rc |= test_write_file(
      "workspace/runs/run_view_action_failure/action_calls/001_actionreq_run_view_action_failure_000005.stderr",
      "STALE_ACTION_STDERR_MUST_NOT_RENDER\n");
  rc |= test_write_file(
      "workspace/runs/run_view_action_failure/action_calls/002_actionreq_run_view_action_failure_000005.input.json",
      input);
  rc |= test_write_file(
      "workspace/runs/run_view_action_failure/action_calls/002_actionreq_run_view_action_failure_000005.stderr",
      "CURRENT_ACTION_STDERR_MARKER\n");
  rc |= test_write_file(
      "workspace/runs/run_view_action_failure/action_calls/001_unrelated_request.stderr",
      "UNRELATED_ACTION_STDERR_MUST_NOT_RENDER\n");
  rc |= expect(harness_cli_view(3, argv, &out, &exit_code) == 0,
               "capture view -d action failure evidence");
  rc |= expect(exit_code == 0, "view -d exits successfully");
  rc |= expect_substr(out, "action_failed", "view -d shows failed terminal payload");
  rc |= expect_substr(out, "action_rejected", "view -d shows rejected terminal payload");
  rc |= expect_substr(
      out,
      "action_calls/002_actionreq_run_view_action_failure_000005.stderr",
      "view -d names the newest request-correlated stderr artifact");
  rc |= expect(out &&
                   test_count_substr(out, "CURRENT_ACTION_STDERR_MARKER") == 1,
               "view -d prints relevant action stderr exactly once");
  rc |= expect_no_substr(out, "STALE_ACTION_STDERR_MUST_NOT_RENDER",
                         "view -d ignores an older recovered attempt");
  rc |= expect_no_substr(out, "UNRELATED_ACTION_STDERR_MUST_NOT_RENDER",
                         "view -d ignores unrelated action stderr");
  free(out);
  rc |= test_reset_workspace();
  return rc;
}

static pid_t start_gateway_lock_holder(int *release_fd) {
  int ready[2] = {-1, -1};
  int release[2] = {-1, -1};
  pid_t pid;
  char state = '0';
  if (!release_fd || pipe(ready) != 0 || pipe(release) != 0) {
    if (ready[0] >= 0) close(ready[0]);
    if (ready[1] >= 0) close(ready[1]);
    if (release[0] >= 0) close(release[0]);
    if (release[1] >= 0) close(release[1]);
    return -1;
  }
  pid = fork();
  if (pid == 0) {
    int lock_fd;
    char done;
    close(ready[0]);
    close(release[1]);
    lock_fd = open(GW_RUNTIME_OWNER_LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
      state = '1';
    (void)write(ready[1], &state, 1);
    close(ready[1]);
    if (state == '1') (void)read(release[0], &done, 1);
    close(release[0]);
    if (lock_fd >= 0) {
      (void)flock(lock_fd, LOCK_UN);
      close(lock_fd);
    }
    _exit(state == '1' ? 0 : 1);
  }
  close(ready[1]);
  close(release[0]);
  if (pid < 0 || read(ready[0], &state, 1) != 1 || state != '1') {
    close(ready[0]);
    close(release[1]);
    if (pid > 0) (void)waitpid(pid, NULL, 0);
    return -1;
  }
  close(ready[0]);
  *release_fd = release[1];
  return pid;
}

static void stop_gateway_lock_holder(pid_t pid, int release_fd) {
  if (release_fd >= 0) {
    (void)write(release_fd, "x", 1);
    close(release_fd);
  }
  if (pid > 0) (void)waitpid(pid, NULL, 0);
}

int conflicting_client_floop_rejected_before_publish(void) {
  static const char expected[] =
      "cli: --floop 'openclaw' conflicts with live gateway floop 'hello'; "
      "fix: run `./bin/fclaw gateway reload --floop openclaw`\n";
  char pid_text[32];
  char actual[RT_SMALL] = "";
  char *out = NULL;
  char *argv[] = { "cli", "--floop", "openclaw" };
  int release_fd = -1;
  int exit_code = 0;
  int rc = 0;
  pid_t owner_pid;
  rc |= test_reset_workspace();
  rc |= test_mkdir_p(".fclaw/run");
  owner_pid = start_gateway_lock_holder(&release_fd);
  rc |= expect(owner_pid > 1, "fixture runtime owner holds the gateway lock");
  if (owner_pid > 1) {
    snprintf(pid_text, sizeof(pid_text), "%d\n", (int)owner_pid);
    rc |= test_write_file(GW_PID_PATH, pid_text);
    rc |= test_write_file(GW_LOOP_PATH, "hello\n");
    rc |= expect(gateway_running_floop(actual, sizeof(actual)) == 0 &&
                     strcmp(actual, "hello") == 0,
                 "gateway boundary reports the live owner's floop");
    rc |= expect(harness_cli_channel(3, argv, &out, &exit_code) == 0,
                 "capture conflicting channel client floop");
    rc |= expect(exit_code != 0, "conflicting client floop exits nonzero");
    rc |= expect(out && strcmp(out, expected) == 0,
                 "conflict names both floops and the gateway reload fix once");
    rc |= expect(access("workspace/logs/bus.jsonl", F_OK) != 0 &&
                     access("workspace/bus/inbox", F_OK) != 0,
                 "conflicting client floop is rejected before bus publication");
  }
  free(out);
  stop_gateway_lock_holder(owner_pid, release_fd);
  rc |= test_reset_workspace();
  return rc;
}

int stale_live_gateway_pid_without_owner_lock_is_rejected(void) {
  char pid_text[32];
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= test_mkdir_p(".fclaw/run");
  snprintf(pid_text, sizeof(pid_text), "%d\n", getpid());
  rc |= test_write_file(GW_PID_PATH, pid_text);
  rc |= test_write_file(GW_LOOP_PATH, "hello\n");
  rc |= expect(gateway_running_pid() == 0,
               "live unrelated PID without the owner lock is not a gateway");
  rc |= expect(access(GW_PID_PATH, F_OK) != 0 &&
                   access(GW_LOOP_PATH, F_OK) != 0,
               "stale PID and floop control files are removed");
  rc |= test_reset_workspace();
  return rc;
}

int usage_cli_states_and_enforces_current_segment_scope(void) {
  static const char current[] =
      "{\"ts\":\"2026-08-08T00:00:00Z\",\"floop\":\"hello\","
      "\"run_id\":\"run_001\",\"call_seq\":1,"
      "\"agent\":\"current_agent\",\"profile\":\"current_profile\","
      "\"provider\":\"gemini_key\","
      "\"model\":\"gemini-2.5-flash-lite\",\"input_chars\":10,"
      "\"input_media_count\":0,\"input_media_bytes\":0,\"output_chars\":20,"
      "\"input_tokens\":3,\"output_tokens\":4,\"total_tokens\":7,"
      "\"provider_cached_input_tokens\":2,"
      "\"duration_ms\":5}\n"
      "{\"ts\":\"2026-08-08T00:00:01Z\",\"floop\":\"hello\","
      "\"run_id\":\"run_002\",\"call_seq\":1,"
      "\"agent\":\"current_agent\",\"profile\":\"current_profile\","
      "\"provider\":\"gemini_key\","
      "\"model\":\"gemini-2.5-flash\",\"input_chars\":11,"
      "\"input_media_count\":0,\"input_media_bytes\":0,\"output_chars\":21,"
      "\"input_tokens\":5,\"output_tokens\":6,\"total_tokens\":11,"
      "\"provider_cached_input_tokens\":null,"
      "\"duration_ms\":7}\n";
  static const char rotated[] =
      "{\"ts\":\"2026-08-07T00:00:00Z\",\"floop\":\"hello\","
      "\"run_id\":\"run_900\",\"call_seq\":1,"
      "\"agent\":\"rotated_agent\",\"profile\":\"rotated_profile\","
      "\"provider\":\"mock\",\"model\":\"rotated-model\","
      "\"input_chars\":999,\"input_media_count\":0,"
      "\"input_media_bytes\":0,\"output_chars\":999,"
      "\"input_tokens\":999,\"output_tokens\":999,\"total_tokens\":1998,"
      "\"provider_cached_input_tokens\":null,"
      "\"duration_ms\":999}\n";
  char *out = NULL;
  char *agent_out = NULL;
  char *filtered_out = NULL;
  char *fixture = NULL;
  char projection[RT_XL];
  int exit_code = 0;
  int agent_exit_code = 0;
  int filtered_exit_code = 0;
  int rc = 0;
  char *human_argv[] = {(char *)"-h"};
  char *agent_argv[] = {(char *)"-a"};
  char *filtered_argv[] = {(char *)"--agent", (char *)"missing_agent",
                           (char *)"-a"};
  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/logs/llm_usage.jsonl", current);
  rc |= test_write_file("workspace/logs/llm_usage.jsonl.1", rotated);
  rc |= expect(harness_cli_usage(1, human_argv, &out, &exit_code) == 0,
               "capture human usage output");
  rc |= expect(exit_code == 0, "usage exits successfully with current records");
  rc |= expect(out &&
                   test_count_substr(out,
                       "scope: current log segment only; rotated history not included\n") == 1,
               "usage states the current-segment scope exactly once");
  rc |= expect_substr(out, "current_agent", "usage includes the current segment");
  rc |= expect_substr(out, "gemini-2.5-flash-lite",
                      "usage table identifies the first model");
  rc |= expect_substr(out, "gemini-2.5-flash",
                      "usage table keeps a changed model separate");
  rc |= expect_no_substr(out, "rotated_agent", "usage excludes rotated history");
  rc |= expect(rt_usage_client_json(projection, sizeof(projection)) == 0,
               "versioned client usage projection builds");
  rc |= expect_substr(projection, "\"schema_version\":1",
                      "client usage projection is versioned");
  rc |= expect(test_count_substr(projection,
                                 "\"agent\":\"current_agent\"") == 2,
               "client usage groups by agent, profile, and model");
  rc |= expect_substr(projection, "\"total_tokens\":18",
                      "client usage totals include all model groups");
  rc |= expect_substr(projection,
                      "\"provider_cached_input_tokens\":2",
                      "client usage preserves provider-reported cache tokens");
  rc |= expect_substr(projection, "\"estimated_usd\":0.00001822",
                      "client usage reports the complete estimated USD total");
  rc |= expect_substr(projection, "\"pricing_complete\":true",
                      "client usage states complete pricing coverage");
  rc |= expect(harness_cli_usage(1, agent_argv, &agent_out,
                                 &agent_exit_code) == 0,
               "capture agent-readable usage output");
  rc |= expect(agent_exit_code == 0,
               "usage -a exits successfully");
  rc |= expect(agent_out && strcmp(agent_out, projection) == 0,
               "usage -a and local API share one JSON projection");
  rc |= expect_no_substr(agent_out, "\033[",
                         "usage -a contains no terminal color escapes");
  rc |= expect(harness_cli_usage(3, filtered_argv, &filtered_out,
                                 &filtered_exit_code) == 0,
               "capture filtered agent-readable usage output");
  rc |= expect(filtered_exit_code == 0,
               "usage -a accepts an agent filter with no matches");
  rc |= expect_substr(filtered_out, "\"calls\":0",
                      "usage -a filter excludes nonmatching records");
  rc |= expect_substr(filtered_out, "\"rows\":[]",
                      "usage -a filter returns an empty valid row array");
  rc |= test_read_file("tests/fixtures/local_client/usage_v1.json",
                       &fixture);
  rc |= expect(fixture && strcmp(projection, fixture) == 0,
               "client usage output matches the frozen v1 golden fixture");
  free(fixture);
  free(filtered_out);
  free(agent_out);
  free(out);
  rc |= test_reset_workspace();
  return rc;
}

int cacheview_modes_are_compact_and_agent_readable(void) {
  static const char meta_1[] =
      "{\"floop\":\"hello\",\"agent\":\"chat_manager\","
      "\"profile\":\"chat\","
      "\"provider\":\"mock\",\"model\":\"mock-v1\","
      "\"url\":\"mock://chat\",\"input_media_count\":0,"
      "\"input_tokens\":12,"
      "\"provider_cached_input_tokens\":8}\n";
  static const char meta_2[] =
      "{\"floop\":\"hello\",\"agent\":\"chat_manager\","
      "\"profile\":\"chat\","
      "\"provider\":\"mock\",\"model\":\"mock-v1\","
      "\"url\":\"mock://chat\",\"input_media_count\":0,"
      "\"input_tokens\":13,\"provider_cached_input_tokens\":null}\n";
  static const char request_1[] =
      "{\"shared\":\"CACHE-AAAA\",\"tail\":\"ONE\"}\n";
  static const char request_2[] =
      "{\"shared\":\"CACHE-AAAA\",\"tail\":\"TWO\"}\n";
  static const char request_3[] =
      "{\"shared\":\"CACHE-AAAA\",\"tail\":\"THREE\"}\n";
  char *normal = NULL;
  char *run = NULL;
  char *list = NULL;
  char *agent = NULL;
  char *agent_run = NULL;
  char *agent_list = NULL;
  char *prefix_marker;
  char *heatmap_marker;
  char *summary_marker;
  JsonRef root, calls, summary;
  int exit_code = 0;
  int rc = 0;
  char *normal_argv[] = {"-h", "--agent", "chat_manager", "-n", "3"};
  char *run_argv[] = {"-h", "--agent", "chat_manager", "-n", "3",
                      "--run", "run_002"};
  char *list_argv[] = {"-h", "--agent", "chat_manager", "-n", "3", "--list"};
  char *agent_argv[] = {"--agent", "chat_manager", "-n", "10", "-a"};
  char *agent_run_argv[] = {"--agent", "chat_manager", "-n", "3", "-a",
                            "--run", "run_002"};
  char *agent_list_argv[] = {"--agent", "chat_manager", "-n", "3", "-a",
                             "--list"};

  rc |= test_reset_workspace();
  rc |= test_mkdir_p("workspace/runs/run_001/provider_calls");
  rc |= test_mkdir_p("workspace/runs/run_002/provider_calls");
  rc |= test_mkdir_p("workspace/runs/run_003/provider_calls");
  rc |= test_write_file("workspace/runs/run_001/provider_calls/001_meta.json",
                        meta_1);
  rc |= test_write_file(
      "workspace/runs/run_001/provider_calls/001_raw_request.json", request_1);
  rc |= test_write_file("workspace/runs/run_002/provider_calls/001_meta.json",
                        meta_2);
  rc |= test_write_file(
      "workspace/runs/run_002/provider_calls/001_raw_request.json", request_2);
  rc |= test_write_file("workspace/runs/run_003/provider_calls/001_meta.json",
                        meta_1);
  rc |= test_write_file(
      "workspace/runs/run_003/provider_calls/001_raw_request.json", request_3);

  rc |= expect(harness_cli_cacheview(5, normal_argv, &normal, &exit_code) == 0,
               "capture default cacheview");
  rc |= expect(exit_code == 0, "default cacheview exits successfully");
  prefix_marker = normal ? strstr(normal, "=== cached raw prefix ===") : NULL;
  heatmap_marker = normal ? strstr(normal, "=== aggregate request heatmap ===") : NULL;
  summary_marker = normal ? strstr(normal, "=== summary ===") : NULL;
  rc |= expect(prefix_marker && heatmap_marker && summary_marker &&
                   prefix_marker < heatmap_marker && heatmap_marker < summary_marker,
               "default cacheview puts prefix and heatmap before its summary");
  rc |= expect_no_substr(normal, "=== divergent tails",
                         "default cacheview does not print every divergent tail");
  rc |= expect_no_substr(normal, "[1] run_",
                         "default cacheview does not list individual runs");

  exit_code = 0;
  rc |= expect(harness_cli_cacheview(7, run_argv, &run, &exit_code) == 0,
               "capture cacheview run drilldown");
  rc |= expect(exit_code == 0, "cacheview run drilldown exits successfully");
  rc |= expect_substr(run, "=== divergence: run_002/001",
                      "run drilldown names the selected call");
  rc |= expect_substr(run, "TWO", "run drilldown prints the selected divergence");
  rc |= expect_no_substr(run, "[1] run_",
                         "run drilldown does not restore the multi-run list");

  exit_code = 0;
  rc |= expect(harness_cli_cacheview(6, list_argv, &list, &exit_code) == 0,
               "capture cacheview list mode");
  rc |= expect(exit_code == 0, "cacheview list mode exits successfully");
  rc |= expect_substr(list, "=== divergent tails",
                      "list mode opts into divergent tails");
  rc |= expect(list && test_count_substr(list, "] run_") == 3,
               "list mode prints every selected call");

  exit_code = 0;
  rc |= expect(harness_cli_cacheview(5, agent_argv, &agent, &exit_code) == 0,
               "capture cacheview agent-readable output");
  rc |= expect(exit_code == 0, "cacheview -a exits successfully");
  rc |= expect(agent && json_ref_top_object(agent, &root) == 0,
               "cacheview -a is one valid JSON object");
  if (agent && json_ref_top_object(agent, &root) == 0) {
    rc |= expect(json_ref_object_get_array(&root, "calls", &calls) == 0 &&
                     json_ref_array_size(&calls) == 3,
                 "cacheview -a includes the selected raw calls");
    rc |= expect(json_ref_object_get_object(&root, "summary", &summary) == 0,
                 "cacheview -a includes its summary projection");
  }
  rc |= expect_substr(agent, "\"provider_cached_input_tokens\":null",
                      "cacheview -a preserves unavailable provider cache evidence");
  rc |= expect_substr(agent, "\"limit\":10",
                      "cacheview -a accepts ten-request aggregate selection");
  rc |= expect_no_substr(agent, "\033[",
                         "cacheview -a contains no terminal color escapes");

  exit_code = 0;
  rc |= expect(harness_cli_cacheview(7, agent_run_argv, &agent_run,
                                     &exit_code) == 0 && exit_code == 0,
               "cacheview --run -a exits successfully");
  rc |= expect(agent_run && json_ref_top_object(agent_run, &root) == 0,
               "cacheview --run -a is valid JSON");
  rc |= expect_substr(agent_run, "\"mode\":\"run\"",
                      "cacheview --run -a reports run mode");
  rc |= expect_substr(agent_run, "\"baseline_run_id\":\"run_002\"",
                      "cacheview --run -a selects the exact baseline");

  exit_code = 0;
  rc |= expect(harness_cli_cacheview(6, agent_list_argv, &agent_list,
                                     &exit_code) == 0 && exit_code == 0,
               "cacheview --list -a exits successfully");
  rc |= expect(agent_list && json_ref_top_object(agent_list, &root) == 0,
               "cacheview --list -a is valid JSON");
  rc |= expect_substr(agent_list, "\"mode\":\"list\"",
                      "cacheview --list -a reports list mode");

  free(agent_list);
  free(agent_run);
  free(agent);
  free(list);
  free(run);
  free(normal);
  rc |= test_reset_workspace();
  return rc;
}

int inspection_record_streams_read_existing_files_and_scope_agents(void) {
  static const char usage[] =
      "{\"ts\":\"2026-08-08T00:00:00Z\",\"floop\":\"hello\","
      "\"run_id\":\"run_001\",\"call_seq\":1,"
      "\"agent\":\"used_agent\",\"profile\":\"known\","
      "\"provider\":\"mock\",\"model\":\"mock-v1\","
      "\"input_chars\":30,\"input_media_count\":0,"
      "\"input_media_bytes\":0,\"output_chars\":8,"
      "\"input_tokens\":10,\"output_tokens\":2,\"total_tokens\":12,"
      "\"provider_cached_input_tokens\":7,\"duration_ms\":5}\n"
      "{\"ts\":\"2026-08-08T00:00:01Z\",\"floop\":\"other\","
      "\"run_id\":\"run_002\",\"call_seq\":1,"
      "\"agent\":\"used_agent\",\"profile\":\"known\","
      "\"provider\":\"mock\",\"model\":\"mock-v1\","
      "\"input_chars\":99,\"input_media_count\":0,"
      "\"input_media_bytes\":0,\"output_chars\":1,"
      "\"input_tokens\":99,\"output_tokens\":1,\"total_tokens\":100,"
      "\"provider_cached_input_tokens\":null,\"duration_ms\":5}\n";
  static const char meta[] =
      "{\"floop\":\"hello\",\"agent\":\"used_agent\","
      "\"profile\":\"known\",\"provider\":\"mock\","
      "\"model\":\"mock-v1\",\"url\":\"mock://known\","
      "\"input_media_count\":0,\"input_tokens\":10,"
      "\"provider_cached_input_tokens\":7}\n";
  static const char raw[] =
      "{\"messages\":[{\"role\":\"user\",\"content\":\"fixture\"}]}\n";
  RtAgentMeta agents[2];
  RtInspectionOptions options;
  RtInspectionStream *stream = NULL;
  char *record = NULL;
  int next;
  int rc = 0;
  memset(agents, 0, sizeof(agents));
  snprintf(agents[0].id, sizeof(agents[0].id), "used_agent");
  snprintf(agents[0].executor, sizeof(agents[0].executor), "llm");
  snprintf(agents[0].model_ref, sizeof(agents[0].model_ref), "known");
  snprintf(agents[1].id, sizeof(agents[1].id), "zero_agent");
  snprintf(agents[1].executor, sizeof(agents[1].executor), "llm");
  snprintf(agents[1].model_ref, sizeof(agents[1].model_ref), "unknown");
  memset(&options, 0, sizeof(options));
  options.floop = "hello";
  options.agents = agents;
  options.agent_count = 2;
  options.agent = "used_agent";
  options.recent = 4;
  record = (char *)malloc(RT_INSPECTION_RECORD_MAX);
  rc |= expect(record != NULL, "allocate inspection record fixture buffer");
  if (!record) return rc;

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/logs/llm_usage.jsonl", usage);
  rc |= test_mkdir_p("workspace/runs/run_001/provider_calls");
  rc |= test_write_file("workspace/runs/run_001/provider_calls/001_meta.json",
                        meta);
  rc |= test_write_file(
      "workspace/runs/run_001/provider_calls/001_raw_request.json", raw);

  stream = rt_usage_record_stream_open(&options);
  rc |= expect(stream != NULL, "open usage record stream");
  next = stream ? rt_inspection_stream_next(
                      stream, record, RT_INSPECTION_RECORD_MAX) : -1;
  rc |= expect(next == 1, "usage stream emits its scope first");
  rc |= expect_substr(record, "\"id\":\"used_agent\"",
                      "usage scope includes an agent with calls");
  rc |= expect_substr(record, "\"id\":\"zero_agent\"",
                      "usage scope includes a configured zero-call agent");
  next = stream ? rt_inspection_stream_next(
                      stream, record, RT_INSPECTION_RECORD_MAX) : -1;
  rc |= expect(next == 1, "usage stream emits the matching ledger record");
  rc |= expect_substr(record, "\"run_id\":\"run_001\"",
                      "usage record preserves run correlation");
  next = stream ? rt_inspection_stream_next(
                      stream, record, RT_INSPECTION_RECORD_MAX) : -1;
  rc |= expect(next == 0,
               "usage stream filters records from another floop");
  rt_inspection_stream_close(stream);
  stream = NULL;

  stream = rt_cacheview_record_stream_open(&options);
  rc |= expect(stream != NULL, "open cacheview record stream");
  next = stream ? rt_inspection_stream_next(
                      stream, record, RT_INSPECTION_RECORD_MAX) : -1;
  rc |= expect(next == 1 && strstr(record, "\"type\":\"scope\""),
               "cacheview stream emits its scope first");
  next = stream ? rt_inspection_stream_next(
                      stream, record, RT_INSPECTION_RECORD_MAX) : -1;
  rc |= expect(next == 1, "cacheview stream emits one provider call record");
  rc |= expect_substr(record, "\"type\":\"cacheview_call\"",
                      "cacheview record has a versioned record type");
  rc |= expect_substr(record, "\"provider_cached_input_tokens\":7",
                      "cacheview record preserves provider cache evidence");
  rc |= expect_substr(record, "\\\"content\\\":\\\"fixture\\\"",
                      "cacheview record streams the existing raw request");
  next = stream ? rt_inspection_stream_next(
                      stream, record, RT_INSPECTION_RECORD_MAX) : -1;
  rc |= expect(next == 0, "cacheview stream ends after selected artifacts");
  rt_inspection_stream_close(stream);
  free(record);
  rc |= test_reset_workspace();
  return rc;
}

int pulse_projection_is_shared_versioned_and_bounded(void) {
  char projection[RT_XL * 4];
  char error[RT_MED] = "";
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= test_write_file(
      "workspace/memory/state/affairs.json",
      "{\"affairs\":[{\"affair_id\":\"aff_1\","
      "\"context_id\":\"chat:tests\",\"status\":\"active\","
      "\"manage\":\"watch it\",\"next_review_at_ms\":0,"
      "\"created_ms\":10,\"notes\":[{\"text\":\"note one\"}]}]}\n");
  rc |= test_write_file(
      "workspace/memory/state/tasks.json",
      "{\"tasks\":[{\"task_id\":\"task_1\",\"kind\":\"work\","
      "\"input\":null,\"state\":{\"status\":\"open\",\"work\":\"do it\","
      "\"done_when\":\"done\",\"work_rev\":2,\"external\":null,"
      "\"updated_ms\":20}}]}\n");
  rc |= test_write_file(
      "workspace/memory/state/operations.json",
      "{\"operations\":[{\"handle\":\"op_1\",\"action\":\"fixture\","
      "\"task_id\":\"task_1\",\"status\":\"running\",\"work_rev\":2,"
      "\"next_poll_at_ms\":0,\"poll_interval_ms\":1000}]}\n");
  rc |= test_write_file(
      "workspace/logs/bus.jsonl",
      "{\"event_id\":\"bus_1\",\"ts\":\"2023-11-14T22:13:19Z\","
      "\"channel\":\"tests\",\"type\":\"user_message\","
      "\"payload\":{\"text\":\"hello\",\"ref\":{\"nick\":\"tester\"}}}\n");
  rc |= test_write_file(
      "workspace/logs/deliveries.jsonl",
      "{\"run_id\":\"run_001\",\"request_id\":\"req_1\","
      "\"delivery\":{\"origin_event_id\":\"bus_1\","
      "\"text\":\"world\"}}\n");
  rc |= test_write_file(
      "workspace/runs/run_001/event_log.jsonl",
      "{\"event_id\":\"evt_1\",\"ts\":\"2023-11-14T22:13:19Z\","
      "\"type\":\"user_message\",\"payload\":{}}\n"
      "{\"event_id\":\"evt_2\",\"ts\":\"2023-11-14T22:13:20Z\","
      "\"type\":\"action_request\",\"payload\":{\"action\":\"message\"}}\n"
      "{\"event_id\":\"evt_3\",\"ts\":\"2023-11-14T22:13:21Z\","
      "\"type\":\"run_done\",\"payload\":{}}\n");
  rc |= expect(rt_pulse_projection_json(
                   ".", 1700000000000LL, 40, 25,
                   projection, sizeof(projection),
                   error, sizeof(error)) == 0,
               "C Pulse projection builds from file truth");
  rc |= expect_substr(projection, "\"schema_version\":1",
                      "Pulse projection is versioned");
  rc |= expect_substr(projection, "\"affair_id\":\"aff_1\"",
                      "Pulse retains affair identity");
  rc |= expect_substr(projection, "\"dormant\":true",
                      "Pulse preserves dormant-affair semantics");
  rc |= expect_substr(projection, "\"task_id\":\"task_1\"",
                      "Pulse retains task identity");
  rc |= expect_substr(projection, "\"handle\":\"op_1\"",
                      "Pulse retains operation identity");
  rc |= expect_substr(projection, "\"text\":\"hello\"",
                      "Pulse contains inbound messages");
  rc |= expect_substr(projection, "\"text\":\"world\"",
                      "Pulse contains committed outbound deliveries");
  rc |= expect_substr(projection, "\"outcome\":\"run_done\"",
                      "Pulse contains the recent durable run outcome");
  rc |= expect_substr(projection,
                      "\"limits\":{\"messages\":40,\"runs\":25",
                      "Pulse publishes its collection limits");
  rc |= expect(rt_pulse_projection_json(
                   ".", 1700000000000LL, 1, 0,
                   projection, sizeof(projection),
                   error, sizeof(error)) == 0,
               "Pulse accepts explicit smaller client collection limits");
  rc |= expect_substr(
      projection,
      "\"limits\":{\"messages\":1,\"runs\":0",
      "Pulse reports the exact requested collection limits");
  rc |= expect_substr(
      projection,
      "\"truncated\":{\"any\":true,\"messages\":true,\"runs\":true",
      "Pulse reports deterministic message and run truncation");
  return rc;
}

static int capture_discord_startup(char *out, size_t out_len,
                                   int *startup_error) {
  int fds[2];
  pid_t pid;
  int status = 0;
  size_t used = 0;
  ssize_t n;
  if (!out || out_len == 0 || !startup_error || pipe(fds) != 0) return -1;
  out[0] = '\0';
  pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    FcReactorModule *adapter;
    int child_error = 0;
    close(fds[0]);
    if (dup2(fds[1], STDERR_FILENO) < 0) _exit(2);
    close(fds[1]);
    adapter = fc_adapter_discord.create(NULL, &child_error);
    if (adapter) fc_adapter_discord.destroy(adapter);
    fflush(stderr);
    _exit(child_error ? 0 : 1);
  }
  close(fds[1]);
  while (used + 1 < out_len &&
         (n = read(fds[0], out + used, out_len - used - 1)) > 0)
    used += (size_t)n;
  out[used] = '\0';
  close(fds[0]);
  if (waitpid(pid, &status, 0) != pid) return -1;
  *startup_error = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  return 0;
}

int startup_preconditions_name_their_fix(void) {
  static const char missing_profile[] =
      "{\"version\":1,\"profiles\":[]}";
  static const char hello_unknown[] =
      "{\"version\":1,\"profiles\":[{\"id\":\"hello\","
      "\"provider\":\"missing_provider\",\"model\":\"mock\"}]}";
  static const char hello_unsupported[] =
      "{\"version\":1,\"profiles\":[{\"id\":\"hello\","
      "\"provider\":\"oauth_only\",\"model\":\"unused\"}]}";
  static const char hello_missing_key[] =
      "{\"version\":1,\"profiles\":[{\"id\":\"hello\","
      "\"provider\":\"needs_key\",\"model\":\"unused\"}]}";
  static const char mock_registry[] =
      "{\"version\":1,\"providers\":[{\"id\":\"mock\","
      "\"kind\":\"mock\"}]}";
  static const char unsupported_registry[] =
      "{\"version\":1,\"providers\":[{\"id\":\"oauth_only\","
      "\"kind\":\"openai_codex_oauth\",\"unsupported\":true}]}";
  static const char keyed_registry[] =
      "{\"version\":1,\"providers\":[{\"id\":\"needs_key\","
      "\"kind\":\"gemini_key\",\"url\":\"https://example.invalid\","
      "\"auth_endpoint\":\"gs_missing_key\","
      "\"auth_env\":\"GS_MISSING_KEY\"}]}";
  static const char discord_missing_guild[] =
      "{\"channels\":{\"discord\":{\"enabled\":true,"
      "\"token_key\":\"gs_missing_discord_token\","
      "\"allowed_channel_ids\":[\"123\"]}}}";
  static const char discord_missing_token[] =
      "{\"channels\":{\"discord\":{\"enabled\":true,"
      "\"token_key\":\"gs_missing_discord_token\","
      "\"guild_id\":\"123\",\"allowed_channel_ids\":[\"456\"],"
      "\"tls_verify\":true}}}";
  char *profiles_original = NULL;
  char *registry_original = NULL;
  char *config_original = NULL;
  char err[RT_LARGE] = "";
  char output[RT_LARGE] = "";
  int startup_error = 0;
  int rc = 0;

  rc |= test_read_file("config/model_profiles.json", &profiles_original);
  rc |= test_read_file("config/llm_registry.json", &registry_original);
  rc |= test_read_file("config/floofclaw_config.json", &config_original);
  if (rc != 0 || !profiles_original || !registry_original || !config_original)
    goto cleanup;

  rc |= expect(scheduler_startup_error("does_not_exist", err, sizeof(err)) != 0,
               "missing floop blocks startup");
  rc |= expect_substr(err, "default_floop 'does_not_exist' not found — available:",
                      "missing floop names available choices");
  rc |= expect_substr(err, "fix: pass --floop <name>",
                      "missing floop names its fix");

  rc |= test_write_file("config/model_profiles.json", missing_profile);
  err[0] = '\0';
  rc |= expect(scheduler_startup_error("hello", err, sizeof(err)) != 0,
               "missing profile blocks startup");
  rc |= expect_substr(err, "profile 'hello' not found; fix: add it to config/model_profiles.json",
                      "missing profile names its fix");

  rc |= test_write_file("config/model_profiles.json", hello_unknown);
  rc |= test_write_file("config/llm_registry.json", mock_registry);
  err[0] = '\0';
  rc |= expect(scheduler_startup_error("hello", err, sizeof(err)) != 0,
               "unknown provider blocks startup");
  rc |= expect_substr(err, "unknown provider 'missing_provider'; fix: add it to config/llm_registry.json",
                      "unknown provider names its fix");

  rc |= test_write_file("config/model_profiles.json", hello_unsupported);
  rc |= test_write_file("config/llm_registry.json", unsupported_registry);
  err[0] = '\0';
  rc |= expect(scheduler_startup_error("hello", err, sizeof(err)) != 0,
               "unsupported provider kind blocks startup");
  rc |= expect_substr(err, "unimplemented provider kind 'openai_codex_oauth'; fix:",
                      "unsupported provider kind names its fix");

  (void)unsetenv("GS_MISSING_KEY");
  rc |= test_write_file("config/model_profiles.json", hello_missing_key);
  rc |= test_write_file("config/llm_registry.json", keyed_registry);
  err[0] = '\0';
  rc |= expect(scheduler_startup_error("hello", err, sizeof(err)) != 0,
               "missing provider key blocks startup");
  rc |= expect_substr(err, "missing key 'gs_missing_key'; fix: run ./bin/fclaw auth set-stdin gs_missing_key or set GS_MISSING_KEY",
                      "missing provider key names its fix");

  rc |= test_write_file("config/floofclaw_config.json", discord_missing_guild);
  rc |= expect(capture_discord_startup(output, sizeof(output), &startup_error) == 0 &&
                   startup_error,
               "missing Discord guild blocks adapter startup");
  rc |= expect_substr(output, "missing guild_id; fix: set channels.discord.guild_id",
                      "missing Discord guild names its fix");

  rc |= test_write_file("config/floofclaw_config.json", discord_missing_token);
  output[0] = '\0';
  startup_error = 0;
  rc |= expect(capture_discord_startup(output, sizeof(output), &startup_error) == 0 &&
                   startup_error,
               "missing Discord token blocks adapter startup");
  rc |= expect_substr(output,
                      "missing token for token_key 'gs_missing_discord_token'; fix: run ./bin/fclaw auth set-stdin gs_missing_discord_token",
                      "missing Discord token names its fix");

cleanup:
  if (profiles_original)
    rc |= test_write_file("config/model_profiles.json", profiles_original);
  if (registry_original)
    rc |= test_write_file("config/llm_registry.json", registry_original);
  if (config_original)
    rc |= test_write_file("config/floofclaw_config.json", config_original);
  free(profiles_original);
  free(registry_original);
  free(config_original);
  return rc;
}

int committed_default_floop_resolves_at_startup(void) {
  char *config = NULL;
  JsonRef root;
  char floop[RT_SMALL] = "";
  char err[RT_LARGE] = "";
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= test_read_file("config/floofclaw_config.json", &config);
  rc |= expect(config && json_ref_first_object(config, &root) == 0,
               "committed config parses");
  if (config && json_ref_first_object(config, &root) == 0)
    rc |= expect(json_ref_object_get_string(&root, "default_floop", floop,
                                            sizeof(floop)) == 0 && floop[0],
                 "committed config names a default floop");
  if (floop[0])
    rc |= expect(scheduler_startup_error(floop, err, sizeof(err)) == 0,
                 err[0] ? err : "committed default floop resolves at startup");
  free(config);
  rc |= test_reset_workspace();
  return rc;
}

int gateway_sigpipe_policy_survives_closed_socket_write(void) {
  pid_t pid = fork();
  int status = 0;
  if (pid < 0) return expect(0, "fork SIGPIPE probe");
  if (pid == 0) {
    int pair[2] = {-1, -1};
    ssize_t n;
    (void)signal(SIGPIPE, SIG_DFL);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) _exit(10);
    gateway_install_signal_policy();
    close(pair[1]);
    errno = 0;
    n = net_write_nb(pair[0], "x", 1);
    close(pair[0]);
    _exit(n == -1 && errno == EPIPE ? 0 : 11);
  }
  if (waitpid(pid, &status, 0) != pid)
    return expect(0, "wait for SIGPIPE probe");
  if (expect(WIFEXITED(status), "closed socket write does not kill process") != 0)
    return -1;
  return expect(WEXITSTATUS(status) == 0,
                "closed socket write returns EPIPE under gateway policy");
}

int clock_domains_keep_deadlines_monotonic(void) {
  pid_t pid;
  int status = 0;
  uint64_t mono = fc_now_ms();
  uint64_t wall = timing_wall_ms();
  int rc = 0;
  rc |= expect(mono > 0, "monotonic clock is available");
  rc |= expect(wall > 0, "wall clock is available");
  rc |= expect(wall != mono, "wall and monotonic clocks are distinct domains");
  pid = fork();
  if (pid < 0) return expect(0, "fork clock-jump regression child");
  if (pid == 0) {
    uint64_t child_mono = timing_monotonic_ms();
    uint64_t before;
    uint64_t after;
    long long system_wall;
    before = timing_stable_wall_from_monotonic(child_mono);
    (void)setenv("FCLAW_FAKE_WALL_MS", "1", 1);
    after = timing_stable_wall_from_monotonic(child_mono + 4321U);
    system_wall = timing_system_wall_ms();
    _exit(timing_wall_ms() == 1U && system_wall > 1000 &&
                  after >= before &&
                  after - before == 4321U
              ? 0
              : 1);
  }
  if (waitpid(pid, &status, 0) != pid)
    return expect(0, "wait for clock-jump regression child");
  rc |= expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "wall-clock jump cannot move an active persisted deadline");
  return rc;
}

int synchronous_run_peak_is_sampled_before_retirement(void) {
  RtScheduler *scheduler = NULL;
  char err[RT_LARGE] = "";
  char envelope_id[BUS_ID_MAX];
  char payload[RT_SMALL];
  int rc = 0;

  rc |= test_reset_workspace();
  for (int i = 0; i < RT_INTAKE_PER_TICK; ++i) {
    snprintf(payload, sizeof(payload),
             "{\"text\":\"peak sample %d\",\"adapter_id\":\"peak\"}", i);
    rc |= expect(bus_publish("peak", "user_message", payload,
                             envelope_id, sizeof(envelope_id)) == 0,
                 "publish synchronous peak fixture");
  }
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  rc |= expect(scheduler != NULL, "allocate synchronous peak scheduler");
  if (scheduler) {
    rc |= expect(rt_scheduler_init(scheduler, "fast", err, sizeof(err)) == 0,
                 err[0] ? err : "initialize synchronous peak scheduler");
    rt_ports_drain_sources(scheduler, RT_INTAKE_PER_TICK);
    rc |= expect(scheduler->run_count == RT_INTAKE_PER_TICK,
                 "one intake batch fills four synchronous run slots");
    rt_scheduler_advance_and_retire(scheduler, fc_now_ms());
    rc |= expect(scheduler->run_count == 0,
                 "native synchronous runs retire in the admission tick");
    rc |= expect(scheduler->run_count_peak == RT_INTAKE_PER_TICK,
                 "run peak is sampled before same-tick retirement");
    rt_scheduler_destroy(scheduler);
  }
  free(scheduler);
  rc |= test_reset_workspace();
  return rc;
}

static int write_recovery_control_fixture(const char *event_log) {
  static const char envelope[] =
      "{\"event_id\":\"bus_000001\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"channel\":\"chaos\",\"type\":\"user_message\","
      "\"payload\":{\"text\":\"recover me\"}}\n";
  static const char runstate[] =
      "{\"run_id\":\"run_001\",\"context_id\":\"chat:chaos\","
      "\"profile\":\"fast\",\"status\":\"running\",\"advance\":1,"
      "\"created_from\":{\"event_id\":\"bus_000001\","
      "\"type\":\"user_message\"},"
      "\"waiting\":{\"jobs\":[],\"events\":[],\"deadline\":null},"
      "\"cursors\":{\"phase_index\":1,"
      "\"last_advanced_event_seq\":null},\"last_error\":null}";
  if (test_reset_workspace() != 0 ||
      test_mkdir_p("workspace/bus/processed") != 0 ||
      test_mkdir_p("workspace/runs/run_001/agent_outputs") != 0 ||
      test_mkdir_p("workspace/runs/run_001/action_calls") != 0 ||
      test_write_file("workspace/bus/processed/bus_000001.json", envelope) != 0 ||
      test_write_file("workspace/runs/run_001/runstate.json", runstate) != 0 ||
      test_write_file("workspace/runs/run_001/event_log.jsonl", event_log) != 0)
    return -1;
  return 0;
}

static int drive_recovered_fast_run(RtScheduler *scheduler) {
  int ticks = 0;
  while (scheduler->run_count > 0 && ticks++ < 32)
    rt_scheduler_tick(scheduler, fc_now_ms());
  return scheduler->run_count == 0 ? 0 : -1;
}

int gateway_restart_recommits_torn_inbound_event(void) {
  RtScheduler *scheduler;
  char err[RT_LARGE] = "";
  char *event_log = NULL;
  char *deliveries = NULL;
  int rc = 0;
  if (write_recovery_control_fixture("{\"event_id\":\"torn") != 0)
    return expect(0, "write torn-inbound recovery fixture");
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  if (!scheduler) return expect(0, "allocate recovery scheduler");
  rc |= expect(rt_scheduler_init(scheduler, "fast", err, sizeof(err)) == 0,
               err[0] ? err : "recover torn inbound event");
  rc |= expect(scheduler->run_count == 1,
               "torn inbound control claim restores one active run");
  rc |= expect(drive_recovered_fast_run(scheduler) == 0,
               "recommitted inbound run reaches terminal state");
  rt_scheduler_destroy(scheduler);
  free(scheduler);
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &event_log);
  rc |= expect(event_log && !strstr(event_log, "torn") &&
                   test_count_substr(event_log, "\"type\":\"user_message\"") == 1 &&
                   test_count_substr(event_log, "\"type\":\"run_done\"") == 1,
               "torn tail is removed and inbound truth commits once");
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(deliveries && test_count_substr(deliveries, "bus_000001") == 1,
               "recovered inbound event delivers once");
  free(event_log);
  free(deliveries);
  return rc;
}

int gateway_restart_does_not_redeliver_committed_message(void) {
  static const char event_log_fixture[] =
      "{\"event_id\":\"evt_run_001_000001\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"user_message\",\"source\":\"chaos\",\"run_id\":\"run_001\","
      "\"payload\":{\"text\":\"recover me\",\"origin_event_id\":\"bus_000001\","
      "\"channel\":\"chaos\",\"adapter_id\":\"\"}}\n"
      "{\"event_id\":\"evt_run_001_000002\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"run_started\",\"source\":\"kernel\",\"run_id\":\"run_001\","
      "\"payload\":{\"run_id\":\"run_001\"}}\n"
      "{\"event_id\":\"evt_run_001_000003\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"action_request\",\"source\":\"noop_agent\",\"run_id\":\"run_001\","
      "\"payload\":{\"request_id\":\"actionreq_000003\",\"action\":\"message\","
      "\"args\":{\"message\":\"ok\"}}}\n"
      "{\"event_id\":\"evt_run_001_000004\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"action_started\",\"source\":\"action_runner\",\"run_id\":\"run_001\","
      "\"payload\":{\"request_id\":\"actionreq_000003\",\"action\":\"message\","
      "\"run_id\":\"run_001\",\"job_id\":null}}\n";
  static const char delivery_fixture[] =
      "{\"run_id\":\"run_001\",\"request_id\":\"actionreq_000003\","
      "\"delivery\":{\"delivery_id\":\"deliv_run_001_000001\","
      "\"origin_event_id\":\"bus_000001\",\"channel\":\"chaos\","
      "\"adapter_id\":\"\",\"text\":\"ok\"}}\n";
  RtScheduler *scheduler;
  char err[RT_LARGE] = "";
  char *deliveries = NULL;
  char *event_log = NULL;
  int rc = 0;
  if (write_recovery_control_fixture(event_log_fixture) != 0 ||
      test_write_file("workspace/logs/deliveries.jsonl", delivery_fixture) != 0)
    return expect(0, "write delivered-message recovery fixture");
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  if (!scheduler) return expect(0, "allocate delivered-message scheduler");
  rc |= expect(rt_scheduler_init(scheduler, "fast", err, sizeof(err)) == 0,
               err[0] ? err : "recover delivered message");
  rc |= expect(drive_recovered_fast_run(scheduler) == 0,
               "delivered-message recovery reaches terminal state");
  rt_scheduler_destroy(scheduler);
  free(scheduler);
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(deliveries && test_count_substr(deliveries, "bus_000001") == 1,
               "committed delivery is never handed to the port twice");
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &event_log);
  rc |= expect(event_log &&
                   test_count_substr(event_log, "\"type\":\"run_done\"") == 1 &&
                   test_count_substr(event_log, "memoized_duplicate") == 0,
               "reconciled run commits one terminal lifecycle");
  free(deliveries);
  free(event_log);
  return rc;
}

int gateway_restart_resumes_message_before_delivery_commit(void) {
  static const char event_log_fixture[] =
      "{\"event_id\":\"evt_run_001_000001\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"user_message\",\"source\":\"chaos\",\"run_id\":\"run_001\","
      "\"payload\":{\"text\":\"recover me\",\"origin_event_id\":\"bus_000001\","
      "\"channel\":\"chaos\",\"adapter_id\":\"\"}}\n"
      "{\"event_id\":\"evt_run_001_000002\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"run_started\",\"source\":\"kernel\",\"run_id\":\"run_001\","
      "\"payload\":{\"run_id\":\"run_001\"}}\n"
      "{\"event_id\":\"evt_run_001_000003\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"action_request\",\"source\":\"noop_agent\",\"run_id\":\"run_001\","
      "\"payload\":{\"request_id\":\"actionreq_000003\",\"action\":\"message\","
      "\"args\":{\"message\":\"ok\"}}}\n"
      "{\"event_id\":\"evt_run_001_000004\",\"ts\":\"2026-07-19T00:00:00Z\","
      "\"type\":\"action_started\",\"source\":\"action_runner\",\"run_id\":\"run_001\","
      "\"payload\":{\"request_id\":\"actionreq_000003\",\"action\":\"message\","
      "\"run_id\":\"run_001\",\"job_id\":null}}\n";
  RtScheduler *scheduler;
  char err[RT_LARGE] = "";
  char *deliveries = NULL;
  char *event_log = NULL;
  int rc = 0;
  if (write_recovery_control_fixture(event_log_fixture) != 0)
    return expect(0, "write pre-delivery message recovery fixture");
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  if (!scheduler) return expect(0, "allocate pre-delivery recovery scheduler");
  rc |= expect(rt_scheduler_init(scheduler, "fast", err, sizeof(err)) == 0,
               err[0] ? err : "recover pre-delivery message");
  rc |= expect(drive_recovered_fast_run(scheduler) == 0,
               "pre-delivery message recovery reaches terminal state");
  rt_scheduler_destroy(scheduler);
  free(scheduler);
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(deliveries &&
                   test_count_substr(deliveries, "bus_000001") == 1 &&
                   test_count_substr(deliveries, "actionreq_000003") == 1,
               "absent message commit resumes once under the stable request");
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &event_log);
  rc |= expect(event_log &&
                   test_count_substr(event_log, "\"type\":\"action_started\"") == 2 &&
                   test_count_substr(event_log, "\"type\":\"action_failed\"") == 0 &&
                   test_count_substr(event_log, "\"type\":\"run_done\"") == 1,
               "message replay records one resumed claim and terminal lifecycle");
  rc |= expect_no_substr(event_log, "ambiguous_completion",
                         "provably absent message delivery is not ambiguous");
  free(deliveries);
  free(event_log);
  return rc;
}

int append_helpers_preserve_bounds_and_format_policy(void) {
  const char *run_dir = "workspace/runs/run_append_helpers";
  const char *log_path = "workspace/runs/run_append_helpers/event_log.jsonl";
  RtContext ctx;
  char fixed[4] = "";
  char long_text[300];
  char payload[8];
  char *dynamic = NULL;
  char *event_log = NULL;
  size_t fixed_pos = 0;
  size_t dynamic_len = 0;
  size_t dynamic_pos = 0;
  int before_checked;
  int rc = 0;

  rc |= expect(rt_append_text(fixed, sizeof(fixed), &fixed_pos, "abc") == 0,
               "bounded text append accepts exact fit");
  rc |= expect(strcmp(fixed, "abc") == 0 && fixed_pos == 3,
               "bounded text append writes and advances");
  rc |= expect(rt_append_text(fixed, sizeof(fixed), &fixed_pos, "x") != 0,
               "bounded text append rejects overflow");
  rc |= expect(strcmp(fixed, "abc") == 0 && fixed_pos == 3,
               "bounded text rejection leaves output unchanged");

  memset(long_text, 'x', sizeof(long_text) - 1U);
  long_text[sizeof(long_text) - 1U] = '\0';
  rc |= expect(rt_dyn_append_text(&dynamic, &dynamic_len, &dynamic_pos, "abc") == 0,
               "dynamic text append allocates initial buffer");
  rc |= expect(dynamic_len == 256U && strcmp(dynamic, "abc") == 0,
               "dynamic text append keeps original initial capacity");
  rc |= expect(rt_dyn_append_text(&dynamic, &dynamic_len, &dynamic_pos,
                                  long_text) == 0,
               "dynamic text append grows for long input");
  rc |= expect(dynamic_len == 512U && dynamic_pos == 302U,
               "dynamic text append keeps original doubling policy");
  fc_xfree(dynamic);

  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create append-helper run directory");
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.workspace, sizeof(ctx.workspace), "%s", "workspace");
  snprintf(ctx.run_dir, sizeof(ctx.run_dir), "%s", run_dir);
  snprintf(ctx.run_id, sizeof(ctx.run_id), "%s", "run_append_helpers");
  ctx.next_event = 1;
  rc |= expect(rt_append_event_format(&ctx, "probe", "test", payload,
                                      sizeof(payload), 0, "%s",
                                      "123456789") == 0,
               "unchecked format policy appends truncated payload");
  before_checked = ctx.next_event;
  rc |= expect(rt_append_event_format(&ctx, "probe", "test", payload,
                                      sizeof(payload), 1, "%s",
                                      "123456789") != 0,
               "checked format policy rejects truncated payload");
  rc |= expect(ctx.next_event == before_checked,
               "checked truncation emits no event");
  rc |= test_read_file(log_path, &event_log);
  rc |= expect_substr(event_log, "\"payload\":1234567",
                      "unchecked truncation preserves prior append behavior");
  free(event_log);
  return rc;
}

int failed_action_task_record_preserves_operator_context(void) {
  const char *run_dir = "workspace/runs/run_failed_action_task";
  RtScheduler *scheduler = NULL;
  RtRun run;
  char *tasks = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create failed-action run directory");
  rc |= expect(test_mkdir_p("workspace/memory/state") == 0,
               "create task-state directory");
  rc |= test_write_file(
      "workspace/memory/state/tasks.json",
      "{\"tasks\":[{\"task_id\":\"task_failed_action\","
      "\"context_id\":\"chat:tests\",\"kind\":\"work\",\"input\":null,"
      "\"created_event_id\":\"evt_seed_000001\",\"created_ms\":1,"
      "\"state\":{\"status\":\"working\",\"work\":\"run command\","
      "\"done_when\":\"command succeeds\",\"work_rev\":1,"
      "\"artifacts\":[],\"external\":null,"
      "\"updated_ms\":1}}]}\n");
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  rc |= expect(scheduler != NULL, "allocate scheduler fixture");
  if (!scheduler) return rc ? rc : -1;
  memset(&run, 0, sizeof(run));
  run.scheduler = scheduler;
  run.ctx.next_event = 1;
  snprintf(run.ctx.workspace, sizeof(run.ctx.workspace), "%s", "workspace");
  snprintf(run.ctx.run_dir, sizeof(run.ctx.run_dir), "%s", run_dir);
  snprintf(run.ctx.run_id, sizeof(run.ctx.run_id), "%s",
           "run_failed_action_task");
  snprintf(run.ctx.context_id, sizeof(run.ctx.context_id), "%s", "chat:tests");
  rt_action_emit_terminal(&run, "action_failed", "req_failure", "bash", NULL,
                          "task_failed_action", "exit_status", "{}",
                          "{\"message\":\"command exited 7\"}");
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  if (tasks && getenv("FCLAW_TEST_PRINT_TASK_RECORD"))
    printf("failed_action_task_record=%s\n", tasks);
  rc |= expect_substr(tasks, "\"kind\":\"error\",\"tool\":\"bash\","
                             "\"reason\":\"exit_status\","
                             "\"message\":\"command exited 7\"",
                      "failed task keeps action, reason, and error message");
  rc |= expect_substr(tasks, "\"artifact_id\":"
                             "\"art_run_failed_action_task_000002_001\"",
                      "failed task keeps normalized artifact identity");
  free(tasks);
  free(scheduler);
  return rc;
}

int repeated_error_append_failure_is_stack_bounded(void) {
  const char *run_dir = "workspace/runs/run_error_recursion";
  const char *log_path = "workspace/runs/run_error_recursion/event_log.jsonl";
  const char *diagnostic_path = "workspace/error_append.stderr";
  RtContext ctx;
  char *diagnostic = NULL;
  char *event_log = NULL;
  int saved_stderr = -1;
  int diagnostic_fd = -1;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create error-recursion run directory");
  rc |= expect(mkdir(log_path, 0755) == 0,
               "replace event log with an unwritable directory");
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.workspace, sizeof(ctx.workspace), "%s", "workspace");
  snprintf(ctx.run_dir, sizeof(ctx.run_dir), "%s", run_dir);
  snprintf(ctx.run_id, sizeof(ctx.run_id), "%s", "run_error_recursion");
  ctx.next_event = 1;

  saved_stderr = dup(STDERR_FILENO);
  diagnostic_fd = open(diagnostic_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  rc |= expect(saved_stderr >= 0 && diagnostic_fd >= 0,
               "open direct stderr capture for error fallback");
  if (saved_stderr >= 0 && diagnostic_fd >= 0 &&
      dup2(diagnostic_fd, STDERR_FILENO) >= 0) {
    close(diagnostic_fd);
    diagnostic_fd = -1;
    for (int i = 0; i < 256; ++i)
      rc |= expect(rt_emit_error(&ctx, "test", "forced failure %d", i) != 0,
                   "failed error append returns without recursion");
    fflush(stderr);
    rc |= expect(dup2(saved_stderr, STDERR_FILENO) >= 0,
                 "restore stderr after error fallback");
  } else {
    rc = 1;
  }
  if (diagnostic_fd >= 0) close(diagnostic_fd);
  if (saved_stderr >= 0) close(saved_stderr);

  rc |= test_read_file(diagnostic_path, &diagnostic);
  rc |= expect_substr(diagnostic, "event_log: error append failed",
                      "failed error append uses direct stderr fallback");
  rc |= expect(rmdir(log_path) == 0, "remove event-log failure fixture");
  rc |= expect(rt_emit_error(&ctx, "test", "recovered") == 0,
               "error guard releases after append failure");
  rc |= test_read_file(log_path, &event_log);
  rc |= expect_substr(event_log, "\"message\":\"recovered\"",
                      "later error event persists after recovery");

  free(event_log);
  free(diagnostic);
  return rc;
}

int terminal_fsync_failure_retires_failed_without_recursive_append(void) {
  const char *run_dir = "workspace/runs/run_terminal_sync_failure";
  const char *log_path =
      "workspace/runs/run_terminal_sync_failure/event_log.jsonl";
  const char *state_path =
      "workspace/runs/run_terminal_sync_failure/runstate.json";
  const char *diagnostic_path = "workspace/terminal_sync.stderr";
  RtProfile profile;
  RtRun run;
  char *diagnostic = NULL;
  char *event_log = NULL;
  char *narration = NULL;
  char *run_state = NULL;
  int saved_stderr = -1;
  int diagnostic_fd = -1;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create terminal-sync run directory");
  memset(&profile, 0, sizeof(profile));
  snprintf(profile.name, sizeof(profile.name), "%s", "test");
  memset(&run, 0, sizeof(run));
  run.profile = &profile;
  run.state = RT_RUN_RUNNING;
  snprintf(run.ctx.workspace, sizeof(run.ctx.workspace), "%s", "workspace");
  snprintf(run.ctx.run_dir, sizeof(run.ctx.run_dir), "%s", run_dir);
  snprintf(run.ctx.run_id, sizeof(run.ctx.run_id), "%s",
           "run_terminal_sync_failure");
  run.ctx.next_event = 1;

  saved_stderr = dup(STDERR_FILENO);
  diagnostic_fd = open(diagnostic_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  rc |= expect(saved_stderr >= 0 && diagnostic_fd >= 0,
               "open direct stderr capture for terminal sync failure");
  fs_test_reset_append_sync();
  fs_test_fail_next_append_sync();
  if (saved_stderr >= 0 && diagnostic_fd >= 0 &&
      dup2(diagnostic_fd, STDERR_FILENO) >= 0) {
    close(diagnostic_fd);
    diagnostic_fd = -1;
    rt_run_finish(&run);
    fflush(stderr);
    rc |= expect(dup2(saved_stderr, STDERR_FILENO) >= 0,
                 "restore stderr after terminal sync failure");
  } else {
    rc = 1;
  }
  if (diagnostic_fd >= 0) close(diagnostic_fd);
  if (saved_stderr >= 0) close(saved_stderr);

  rc |= expect(run.state == RT_RUN_FAILED,
               "terminal sync failure retires the run failed");
  rc |= expect(run.lifecycle_terminal_emitted == 1,
               "terminal sync failure closes lifecycle emission");
  rc |= expect(strcmp(run.last_error_code, "terminal_event_sync_failed") == 0,
               "terminal sync failure records a stable error code");
  rc |= expect(fs_test_append_sync_attempts() == 1,
               "terminal sync failure does not append a second terminal event");
  rc |= test_read_file(diagnostic_path, &diagnostic);
  rc |= expect_substr(diagnostic, "terminal event append/fsync failed",
                      "terminal sync failure reports directly to stderr");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration, "RUN FAILED:",
                      "terminal sync failure narrates the failed run");
  rc |= expect_substr(narration, "terminal event append/fsync failed",
                      "terminal sync narration names the filesystem boundary");
  rc |= test_read_file(log_path, &event_log);
  rc |= expect_substr(event_log, "\"type\":\"run_done\"",
                      "fault injection reaches fsync after terminal write");
  rc |= expect(strstr(event_log, "\"type\":\"run_failed\"") == NULL,
               "terminal sync failure does not recursively append run_failed");
  rc |= test_read_file(state_path, &run_state);
  rc |= expect_substr(run_state, "\"status\":\"failed\"",
                      "terminal sync failure persists failed retirement state");

  fs_test_reset_append_sync();
  free(run_state);
  free(narration);
  free(event_log);
  free(diagnostic);
  return rc;
}

int narration_failure_retries_when_storage_returns(void) {
  char *narration = NULL;
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= expect(rt_narrate_flush_pending() == 0,
               "narration retry slot starts empty");
  rc |= expect(test_mkdir_p("workspace/logs/narration.jsonl") == 0,
               "create unwritable narration-path fixture");
  rc |= expect(rt_narrate("RUN FAILED: deferred filesystem diagnostic") != 0,
               "failed narration append is reported to its caller");
  rc |= expect(rmdir("workspace/logs/narration.jsonl") == 0,
               "restore writable narration path");
  rc |= expect(rt_narrate_flush_pending() == 0,
               "pending narration flushes after storage returns");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration, "RUN FAILED: deferred filesystem diagnostic",
                      "first filesystem diagnostic survives the outage");
  free(narration);
  return rc;
}

int replay_torn_tail_sweep_and_nonfinal_corruption_warning(void) {
  static const char prefix[] =
      "{\"event_id\":\"evt_1\",\"type\":\"user_message\","
      "\"payload\":{\"text\":\"before\"}}\n";
  static const char final_event[] =
      "{\"event_id\":\"evt_2\",\"type\":\"action_succeeded\","
      "\"payload\":{\"action\":\"message\","
      "\"result\":{\"message\":\"after\"}}}\n";
  static const char later_event[] =
      "{\"event_id\":\"evt_3\",\"type\":\"user_message\","
      "\"payload\":{\"text\":\"later\"}}\n";
  const char *diagnostic_path = "workspace/replay_corruption.stderr";
  char event_text[sizeof(prefix) + sizeof(final_event) + sizeof(later_event)];
  char expected[RT_XL];
  char actual[RT_XL];
  RtReplayReport report;
  char *diagnostic = NULL;
  char *narration = NULL;
  int saved_stderr = -1;
  int diagnostic_fd = -1;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_reduce_event_log_text_report(prefix, expected,
                                                sizeof(expected), &report) == 0,
               "replay pre-final state");
  for (size_t cut = 0; cut < strlen(final_event); ++cut) {
    size_t prefix_len = strlen(prefix);
    memcpy(event_text, prefix, prefix_len);
    memcpy(event_text + prefix_len, final_event, cut);
    event_text[prefix_len + cut] = '\0';
    if (rt_reduce_event_log_text_report(event_text, actual, sizeof(actual),
                                        &report) != 0 ||
        strcmp(actual, expected) != 0 ||
        report.malformed_nonfinal_lines != 0 ||
        report.malformed_lines != (cut == 0 ? 0U : 1U)) {
      fprintf(stderr, "torn-tail sweep failed at byte offset %zu\n", cut);
      rc = 1;
      break;
    }
  }
  snprintf(event_text, sizeof(event_text), "%s%s", prefix, final_event);
  rc |= expect(rt_reduce_event_log_text_report(event_text, actual,
                                                sizeof(actual), &report) == 0,
               "replay complete final event");
  rc |= expect_substr(actual, "\"latest_message\":\"after\"",
                      "complete final event still changes replayed state");

  snprintf(event_text, sizeof(event_text), "%s{torn}\n%s",
           prefix, later_event);
  saved_stderr = dup(STDERR_FILENO);
  diagnostic_fd = open(diagnostic_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  rc |= expect(saved_stderr >= 0 && diagnostic_fd >= 0,
               "open replay corruption stderr capture");
  if (saved_stderr >= 0 && diagnostic_fd >= 0 &&
      dup2(diagnostic_fd, STDERR_FILENO) >= 0) {
    close(diagnostic_fd);
    diagnostic_fd = -1;
    rc |= expect(rt_reduce_event_log_text(event_text, actual,
                                           sizeof(actual)) == 0,
                 "non-final malformed line is skipped");
    fflush(stderr);
    rc |= expect(dup2(saved_stderr, STDERR_FILENO) >= 0,
                 "restore stderr after replay warning");
  } else {
    rc = 1;
  }
  if (diagnostic_fd >= 0) close(diagnostic_fd);
  if (saved_stderr >= 0) close(saved_stderr);
  rc |= expect_substr(actual, "\"user_text\":\"later\"",
                      "replay continues after non-final malformed line");
  rc |= test_read_file(diagnostic_path, &diagnostic);
  rc |= expect_substr(diagnostic, "WARNING: replay: malformed non-final line 2",
                      "non-final malformed line warns loudly as corruption");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration, "replay: skipped 1 malformed line(s)",
                      "replay narrates the malformed-line count");
  rc |= expect_substr(narration, "event log may be corrupt",
                      "replay narrates non-final corruption");

  free(narration);
  free(diagnostic);
  return rc;
}

int concurrent_bus_publish_mints_unique_envelope_ids(void) {
  enum { CHILDREN = 2, PUBLISHES_PER_CHILD = 500, TOTAL = 1000 };
  int start_pipe[2] = {-1, -1};
  pid_t children[CHILDREN] = {-1, -1};
  unsigned char seen[TOTAL + 1];
  int child_failures = 0;
  int duplicates = 0;
  int unique = 0;
  int rc = 0;
  char *counter = NULL;

  memset(seen, 0, sizeof(seen));
  rc |= test_reset_workspace();
  if (pipe(start_pipe) != 0) return expect(0, "create bus publish barrier");
  for (int child = 0; child < CHILDREN; ++child) {
    children[child] = fork();
    if (children[child] < 0) {
      rc |= expect(0, "fork concurrent bus publisher");
      break;
    }
    if (children[child] == 0) {
      char result_path[128];
      char start;
      FILE *results;
      close(start_pipe[1]);
      while (read(start_pipe[0], &start, 1) < 0) {
        if (errno == EINTR) continue;
        _exit(10);
      }
      close(start_pipe[0]);
      snprintf(result_path, sizeof(result_path),
               "workspace/bus_publish_child_%d.ids", child);
      results = fopen(result_path, "w");
      if (!results) _exit(11);
      for (int i = 0; i < PUBLISHES_PER_CHILD; ++i) {
        char id[BUS_ID_MAX];
        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"child\":%d,\"sequence\":%d}", child, i);
        if (bus_publish("concurrency", "user_message", payload,
                        id, sizeof(id)) != 0 ||
            fprintf(results, "%s\n", id) < 0) {
          (void)fclose(results);
          _exit(12);
        }
      }
      if (fclose(results) != 0) _exit(13);
      _exit(0);
    }
  }
  close(start_pipe[0]);
  if (children[0] > 0 && children[1] > 0) {
    char starts[CHILDREN] = {'x', 'x'};
    rc |= expect(write(start_pipe[1], starts, sizeof(starts)) ==
                     (ssize_t)sizeof(starts),
                 "release concurrent bus publishers together");
  }
  close(start_pipe[1]);
  for (int child = 0; child < CHILDREN; ++child) {
    int status = 0;
    if (children[child] <= 0 || waitpid(children[child], &status, 0) < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      child_failures++;
  }
  rc |= expect(child_failures == 0,
               "both concurrent bus publishers complete 500 writes");

  for (int child = 0; child < CHILDREN; ++child) {
    char result_path[128];
    char line[BUS_ID_MAX + 8];
    FILE *results;
    snprintf(result_path, sizeof(result_path),
             "workspace/bus_publish_child_%d.ids", child);
    results = fopen(result_path, "r");
    if (!results) {
      rc |= expect(0, "open concurrent publisher id record");
      continue;
    }
    while (fgets(line, sizeof(line), results)) {
      long id = 0;
      if (sscanf(line, "bus_%ld", &id) != 1 || id < 1 || id > TOTAL) {
        rc |= expect(0, "published bus id is in the expected range");
        continue;
      }
      if (seen[id]) duplicates++;
      else {
        seen[id] = 1;
        unique++;
      }
    }
    if (fclose(results) != 0)
      rc |= expect(0, "close concurrent publisher id record");
  }
  rc |= expect(duplicates == 0, "concurrent publishers mint no duplicate ids");
  rc |= expect(unique == TOTAL,
               "concurrent publishers return 1000 unique ids");
  rc |= expect(bus_inbox_count() == TOTAL,
               "all 1000 unique envelopes exist in the inbox");
  rc |= test_read_file("workspace/bus/.counter", &counter);
  rc |= expect(counter && strcmp(counter, "1000\n") == 0,
               "guarded bus counter commits exactly 1000 claims");
  free(counter);
  counter = NULL;
  rc |= expect(test_write_file("workspace/bus/.counter", "corrupt\n") == 0,
               "write corrupt counter rejection fixture");
  rc |= expect(bus_publish("concurrency", "user_message", "{}",
                           NULL, 0) != 0,
               "corrupt counter fails closed instead of reusing an id");
  rc |= test_read_file("workspace/bus/.counter", &counter);
  rc |= expect(counter && strcmp(counter, "corrupt\n") == 0,
               "failed corrupt-counter claim does not rewrite the counter");
  rc |= expect(bus_inbox_count() == TOTAL,
               "failed corrupt-counter claim creates no envelope");

  free(counter);
  return rc;
}

int bus_inbox_ignores_uncommitted_atomic_publish_temps(void) {
  static const char canonical[] =
      "workspace/bus/inbox/bus_000002.json";
  static const char temporary[] =
      "workspace/bus/inbox/bus_000001.json.tmp.123.1";
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= expect(test_write_file(temporary, "{\"partial\":true}") == 0,
               "create visible atomic-publish temporary fixture");
  rc |= expect(test_write_file(canonical, "{}\n") == 0,
               "create committed envelope fixture");
  rc |= expect(bus_inbox_count() == 1,
               "inbox count excludes atomic-publish temporary files");
  rc |= expect(bus_inbox_has_pending() == 1,
               "committed envelope remains pending beside temporary file");
  rc |= expect(bus_pop_oldest(NULL, 0) == 0,
               "intake moves the committed envelope only");
  rc |= expect_file_exists(
      "workspace/bus/processed/bus_000002.json");
  rc |= expect_file_exists(temporary);
  rc |= expect(bus_inbox_count() == 0 && bus_inbox_has_pending() == 0,
               "temporary file alone is not pending traffic");
  return rc;
}

int readdir_failures_abort_partial_scans_loudly(void) {
  const char *diagnostic_path = "workspace/readdir_failure.stderr";
  FcReactorModule *janitor = NULL;
  char *diagnostic = NULL;
  char *narration = NULL;
  int saved_stderr = -1;
  int diagnostic_fd = -1;
  int rc = 0;

  if (getenv("FCLAW_JANITOR_RSS_MEASURE")) {
    janitor = fc_janitor_module_create();
    rc |= expect(janitor != NULL && janitor->init(janitor) == 0,
                 "initialize populated janitor measurement");
    if (janitor)
      rc |= expect(janitor->tick(janitor, fc_now_ms() + 60000U) == 0,
                   "run populated janitor measurement tick");
    rc |= expect_file_not_exists(
        "workspace/bus/processed/bus_000001.json");
    if (janitor) fc_janitor_module_destroy(janitor);
    return rc;
  }

  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p("workspace/runs/run_001") == 0,
               "create first partial run scan fixture");
  rc |= expect(test_mkdir_p("workspace/runs/run_002") == 0,
               "create second partial run scan fixture");
  rc |= expect(test_write_file("workspace/bus/inbox/bus_000001.json", "{}\n") == 0,
               "create first partial bus scan fixture");
  rc |= expect(test_write_file("workspace/bus/inbox/bus_000002.json", "{}\n") == 0,
               "create second partial bus scan fixture");
  rc |= expect(test_mkdir_p("workspace/logs/codex_ops/op_001") == 0,
               "create codex-ops readdir fixture");

  saved_stderr = dup(STDERR_FILENO);
  diagnostic_fd = open(diagnostic_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  rc |= expect(saved_stderr >= 0 && diagnostic_fd >= 0,
               "open readdir failure stderr capture");
  if (saved_stderr >= 0 && diagnostic_fd >= 0 &&
      dup2(diagnostic_fd, STDERR_FILENO) >= 0) {
    close(diagnostic_fd);
    diagnostic_fd = -1;
    fs_test_fail_readdir_after(3);
    rc |= expect(rt_next_run_number_for_workspace("workspace") == -1,
                 "run-number scan aborts after partial readdir failure");
    fs_test_fail_readdir_after(3);
    rc |= expect(bus_inbox_count() == -1,
                 "bus count aborts after partial readdir failure");
    fs_test_fail_readdir_after(3);
    rc |= expect(bus_pop_oldest(NULL, 0) == -1,
                 "bus pop aborts before acting on a partial scan");
    rc |= expect_file_exists("workspace/bus/inbox/bus_000001.json");
    rc |= expect_file_exists("workspace/bus/inbox/bus_000002.json");
    rc |= expect(unlink("workspace/bus/inbox/bus_000001.json") == 0 &&
                     unlink("workspace/bus/inbox/bus_000002.json") == 0,
                 "empty bus inbox for pending-probe failure");
    fs_test_fail_readdir_after(2);
    rc |= expect(bus_inbox_has_pending() == -1,
                 "bus pending probe distinguishes readdir failure from empty");

    janitor = fc_janitor_module_create();
    rc |= expect(janitor != NULL && janitor->init(janitor) == 0,
                 "initialize janitor readdir fixture");
    if (janitor) {
      fs_test_fail_readdir_after(3);
      rc |= expect(janitor->tick(janitor, fc_now_ms() + 60000U) == 0,
                   "janitor isolates a failed partial directory scan");
      fs_test_fail_readdir_after(5);
      rc |= expect(janitor->tick(janitor, fc_now_ms() + 400000U) == 0,
                   "janitor isolates a codex-ops readdir failure");
    }
    fs_test_reset_readdir();
    fflush(stderr);
    rc |= expect(dup2(saved_stderr, STDERR_FILENO) >= 0,
                 "restore stderr after readdir failures");
  } else {
    rc = 1;
  }
  if (janitor) fc_janitor_module_destroy(janitor);
  fs_test_reset_readdir();
  if (diagnostic_fd >= 0) close(diagnostic_fd);
  if (saved_stderr >= 0) close(saved_stderr);

  rc |= test_read_file(diagnostic_path, &diagnostic);
  rc |= expect_substr(diagnostic, "event_log: readdir failed",
                      "run-number scan reports readdir failure directly");
  rc |= expect_substr(diagnostic, "bus: readdir failed",
                      "bus scan reports readdir failure directly");
  rc |= expect_substr(diagnostic, "janitor: readdir failed",
                      "janitor scan reports readdir failure directly");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration, "janitor: task runs_prune failed",
                      "janitor narrates the failed scan task");
  rc |= expect_substr(narration, "janitor: task codex_ops_prune failed",
                      "janitor narrates the second directory walker failure");

  free(narration);
  free(diagnostic);
  return rc;
}

int janitor_scratch_reentry_guard_fails_safely(void) {
  const char *diagnostic_path = "workspace/janitor_scratch_reentry.stderr";
  char *diagnostic = NULL;
  int saved_stderr = -1;
  int diagnostic_fd = -1;
  int rc = 0;

  rc |= test_reset_workspace();
  saved_stderr = dup(STDERR_FILENO);
  diagnostic_fd = open(diagnostic_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  rc |= expect(saved_stderr >= 0 && diagnostic_fd >= 0,
               "open janitor scratch diagnostic capture");
  if (saved_stderr >= 0 && diagnostic_fd >= 0 &&
      dup2(diagnostic_fd, STDERR_FILENO) >= 0) {
    close(diagnostic_fd);
    diagnostic_fd = -1;
    rc |= expect(fc_janitor_test_scratch_reentry_guard() == 0,
                 "nested janitor scratch borrow fails safely");
    rc |= expect(fc_janitor_test_scratch_reentry_guard() == 0,
                 "janitor scratch guard releases after rejection");
    fflush(stderr);
    rc |= expect(dup2(saved_stderr, STDERR_FILENO) >= 0,
                 "restore stderr after janitor scratch rejection");
  } else {
    rc = 1;
  }
  if (diagnostic_fd >= 0) close(diagnostic_fd);
  if (saved_stderr >= 0) close(saved_stderr);
  rc |= test_read_file(diagnostic_path, &diagnostic);
  rc |= expect_substr(diagnostic,
                      "janitor: scratch re-entry violates single-threaded runtime invariant",
                      "janitor scratch rejection reports directly");
  rc |= expect(test_write_file(
                   "workspace/bus/processed/bus_000001.json", "{}\n") == 0 &&
                   test_write_file(
                   "workspace/bus/processed/bus_000002.json", "{}\n") == 0,
               "create processed-bus prune filename fixtures");
  rc |= expect(fc_janitor_test_bus_processed_prune(
                   "workspace/bus/processed", 1) == 0,
               "processed-bus scan accepts json filenames");
  rc |= expect_file_not_exists(
      "workspace/bus/processed/bus_000001.json");
  rc |= expect_file_exists("workspace/bus/processed/bus_000002.json");
  free(diagnostic);
  return rc;
}

static int count_directory_entries(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  int count = 0;
  if (!dir) return -1;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
      count++;
  }
  closedir(dir);
  return count;
}

int pending_action_capacity_boundary_is_loud(void) {
  const char *run_dir = "workspace/runs/run_pending_capacity";
  RtRun run;
  char request_id[RT_SMALL];
  char action[RT_SMALL];
  char *event_log = NULL;
  int rc = 0;

  memset(&run, 0, sizeof(run));
  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create pending-action capacity run directory");
  snprintf(run.ctx.workspace, sizeof(run.ctx.workspace), "%s", "workspace");
  snprintf(run.ctx.run_dir, sizeof(run.ctx.run_dir), "%s", run_dir);
  snprintf(run.ctx.run_id, sizeof(run.ctx.run_id), "%s", "run_pending_capacity");
  run.ctx.next_event = 1;
  rc |= expect(RT_MAX_PENDING_ACTIONS == 16,
               "pending-action queue cap stays at sixteen");
  for (int i = 0; i < RT_MAX_PENDING_ACTIONS; ++i) {
    snprintf(request_id, sizeof(request_id), "pending_cap_%03d", i + 1);
    snprintf(action, sizeof(action), "cap_action_%03d", i + 1);
    rc |= expect(rt_action_runner_note_request(
                     &run, "capacity_test", request_id, action, "{}", NULL, 0) == 1,
                 "every pending-action slot accepts one request");
  }
  rc |= expect(run.pending_action_count == RT_MAX_PENDING_ACTIONS,
               "pending-action queue reaches its exact cap");
  rc |= expect(rt_action_runner_note_request(
                   &run, "capacity_test", "pending_cap_overflow",
                   "cap_action_overflow", "{}", NULL, 0) == 0,
               "pending-action cap-plus-one request is rejected");
  rc |= expect(run.pending_action_count == RT_MAX_PENDING_ACTIONS,
               "pending-action rejection does not corrupt the queue");
  rc |= test_read_file("workspace/runs/run_pending_capacity/event_log.jsonl",
                       &event_log);
  rc |= expect_substr(event_log, "pending_action overflow: cap=16",
                      "pending-action rejection is loud on the event log");
  rc |= expect_substr(event_log, "pending_cap_overflow",
                      "pending-action rejection identifies the request");
  while (run.pending_action_count > 0)
    rt_action_pending_remove_at(&run, run.pending_action_count - 1);
  free(event_log);
  rc |= test_reset_workspace();
  return rc;
}

int waiting_job_capacity_boundary_is_loud(void) {
  const char *run_dir = "workspace/runs/run_waiting_capacity";
  RtRun run;
  char job_id[RT_SMALL];
  char *runstate = NULL;
  int rc = 0;

  memset(&run, 0, sizeof(run));
  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create waiting-job capacity run directory");
  snprintf(run.ctx.workspace, sizeof(run.ctx.workspace), "%s", "workspace");
  snprintf(run.ctx.run_dir, sizeof(run.ctx.run_dir), "%s", run_dir);
  snprintf(run.ctx.run_id, sizeof(run.ctx.run_id), "%s", "run_waiting_capacity");
  snprintf(run.context_id, sizeof(run.context_id), "%s", "chat:capacity");
  run.ctx.next_event = 1;
  rc |= expect(RT_MAX_WAITING_JOBS == 16,
               "waiting-job queue cap stays at sixteen");
  for (int i = 0; i < RT_MAX_WAITING_JOBS; ++i) {
    snprintf(job_id, sizeof(job_id), "job_cap_%03d", i + 1);
    rt_run_waiting_job_add(&run, job_id);
  }
  rc |= expect(run.waiting_job_count == RT_MAX_WAITING_JOBS,
               "waiting-job queue reaches its exact cap");
  rt_run_waiting_job_add(&run, "job_cap_overflow");
  rc |= expect(run.waiting_job_count == RT_MAX_WAITING_JOBS,
               "waiting-job cap-plus-one request is rejected");
  rc |= expect(strcmp(run.last_error_code, "waiting_jobs_overflow") == 0 &&
                   strcmp(run.last_error_message, "too many waiting jobs") == 0 &&
                   strcmp(run.last_error_job_id, "job_cap_overflow") == 0,
               "waiting-job rejection records its designed diagnostic");
  rc |= test_read_file("workspace/runs/run_waiting_capacity/runstate.json",
                       &runstate);
  rc |= expect_substr(runstate, "\"code\":\"waiting_jobs_overflow\"",
                      "waiting-job rejection is loud in durable run state");
  rc |= expect_substr(runstate, "\"job_id\":\"job_cap_overflow\"",
                      "durable waiting-job rejection identifies the job");
  free(runstate);
  rc |= test_reset_workspace();
  return rc;
}

int operation_capacity_boundary_is_loud(void) {
  const char *run_dir = "workspace/runs/run_operation_capacity";
  RtContext ctx;
  char payload[RT_LARGE];
  char expected_last[RT_SMALL];
  char rejected[RT_SMALL];
  char *operations = NULL;
  char *event_log = NULL;
  int rc = 0;

  memset(&ctx, 0, sizeof(ctx));
  rc |= test_reset_workspace();
  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create operation capacity run directory");
  snprintf(ctx.workspace, sizeof(ctx.workspace), "%s", "workspace");
  snprintf(ctx.run_dir, sizeof(ctx.run_dir), "%s", run_dir);
  snprintf(ctx.run_id, sizeof(ctx.run_id), "%s", "run_operation_capacity");
  snprintf(ctx.context_id, sizeof(ctx.context_id), "%s", "chat:capacity");
  ctx.next_event = 1;
  rc |= expect(OPERATION_MAX_ITEMS == 128,
               "operation-store cap stays at one hundred twenty-eight");
  for (int i = 1; i <= OPERATION_MAX_ITEMS + 1; ++i) {
    snprintf(payload, sizeof(payload),
             "{\"handle\":\"op_cap_%06d\",\"action\":\"capacity\","
             "\"task_id\":\"\",\"context_id\":\"chat:capacity\","
             "\"channel\":\"capacity\",\"adapter_id\":\"test\","
             "\"ref\":null,\"work_rev\":1,\"poll_interval_ms\":1000,"
             "\"next_poll_at_ms\":1}", i);
    rc |= expect(rt_append_event(&ctx, "operation_started", "capacity_test",
                                 payload) == 0,
                 "append operation capacity event");
  }
  snprintf(expected_last, sizeof(expected_last), "op_cap_%06d",
           OPERATION_MAX_ITEMS);
  snprintf(rejected, sizeof(rejected), "op_cap_%06d",
           OPERATION_MAX_ITEMS + 1);
  rc |= test_read_file("workspace/memory/state/operations.json", &operations);
  rc |= test_read_file(
      "workspace/runs/run_operation_capacity/event_log.jsonl", &event_log);
  rc |= expect_substr(operations, expected_last,
                      "last operation-store slot accepts a record");
  rc |= expect_no_substr(operations, rejected,
                         "operation cap-plus-one record is rejected");
  rc |= expect_substr(event_log, "operation event failed reducer",
                      "operation cap rejection is loud on the event log");
  rc |= expect_substr(event_log, rejected,
                      "operation cap rejection identifies the handle");
  free(event_log);
  free(operations);
  rc |= test_reset_workspace();
  return rc;
}

int right_sized_capacity_boundaries_are_loud(void) {
  const char *run_dir = "workspace/runs/run_capacity_limits";
  const int bus_scan_cap = fc_janitor_test_bus_scan_cap();
  RtScheduler *scheduler = NULL;
  RtRun *runs[RT_MAX_ACTIVE_RUNS];
  RtContext ctx;
  FcReactorModule *janitor = NULL;
  char *original_config = NULL;
  char *event_log = NULL;
  char *narration = NULL;
  char *affairs = NULL;
  char *tasks = NULL;
  char payload[RT_LARGE];
  char path[512];
  int fd;
  int rc = 0;

  memset(runs, 0, sizeof(runs));
  memset(&ctx, 0, sizeof(ctx));
  rc |= test_read_file("config/floofclaw_config.json", &original_config);
  rc |= test_reset_workspace();
  rc |= expect(RT_MAX_ACTIVE_RUNS == 16,
               "active-run pool is right-sized to sixteen");
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  rc |= expect(scheduler != NULL, "allocate right-sized scheduler pool");
  if (scheduler) {
    for (int i = 0; i < RT_MAX_ACTIVE_RUNS; ++i) {
      runs[i] = rt_scheduler_alloc_run(scheduler);
      rc |= expect(runs[i] != NULL, "allocate every active-run pool slot");
      if (runs[i]) scheduler->runs[scheduler->run_count++] = runs[i];
    }
    rc |= expect(scheduler->run_count == 16 &&
                     rt_scheduler_alloc_run(scheduler) == NULL,
                 "seventeenth concurrent run is rejected at pool boundary");
    for (int i = 0; i < RT_MAX_ACTIVE_RUNS; ++i)
      rt_scheduler_free_run(scheduler, runs[i]);
    scheduler->run_count = 0;
    rc |= expect(rt_scheduler_alloc_run(scheduler) != NULL,
                 "released run slot is reusable after saturation");
  }

  rc |= expect(test_mkdir_p(run_dir) == 0,
               "create capacity-boundary run directory");
  snprintf(ctx.workspace, sizeof(ctx.workspace), "%s", "workspace");
  snprintf(ctx.run_dir, sizeof(ctx.run_dir), "%s", run_dir);
  snprintf(ctx.run_id, sizeof(ctx.run_id), "%s", "run_capacity_limits");
  snprintf(ctx.context_id, sizeof(ctx.context_id), "%s", "chat:capacity");
  ctx.next_event = 1;
  for (int i = 1; i <= AFFAIR_MAX_ITEMS + 1; ++i) {
    snprintf(payload, sizeof(payload),
             "{\"action\":\"create\",\"affair_id\":\"aff_cap_%06d\","
             "\"context_id\":\"chat:capacity\",\"manage\":\"cap %d\","
             "\"status\":\"active\",\"created_ms\":1,\"updated_ms\":1}",
             i, i);
    rc |= expect(rt_append_event(&ctx, "affair_patch", "capacity_test",
                                 payload) == 0,
                 "append affair capacity event");
  }
  for (int i = 1; i <= 33; ++i) {
    snprintf(payload, sizeof(payload),
             "{\"task_id\":\"task_cap_%06d\","
             "\"context_id\":\"chat:capacity\",\"kind\":\"work\","
             "\"created_event_id\":\"evt_seed_%06d\",\"created_ms\":1,"
             "\"state\":{\"status\":\"working\",\"work\":\"cap\","
             "\"done_when\":\"cap\",\"work_rev\":1,"
             "\"artifacts\":[],\"external\":null,\"updated_ms\":1}}",
             i, i);
    rc |= expect(rt_append_event(&ctx, "task_created", "capacity_test",
                                 payload) == 0,
                 "append task capacity event");
  }
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= test_read_file("workspace/runs/run_capacity_limits/event_log.jsonl",
                       &event_log);
  snprintf(path, sizeof(path), "aff_cap_%06d", AFFAIR_MAX_ITEMS);
  rc |= expect_substr(affairs, path, "last affair-store slot accepts a record");
  snprintf(path, sizeof(path), "aff_cap_%06d", AFFAIR_MAX_ITEMS + 1);
  rc |= expect_no_substr(affairs, path, "affair cap-plus-one record is rejected");
  rc |= expect_substr(tasks, "task_cap_000032",
                      "thirty-second task fits new cap");
  rc |= expect_no_substr(tasks, "task_cap_000033",
                         "thirty-third task is rejected");
  rc |= expect_substr(event_log, "affair event failed reducer",
                      "affair cap rejection is loud on the event log");
  rc |= expect_substr(event_log, "task event failed reducer",
                      "task cap rejection is loud on the event log");

  rc |= expect(bus_scan_cap == 12288,
               "processed-bus scan cap is right-sized to 12288");
  rc |= expect(test_write_file(
                   "config/floofclaw_config.json",
                   "{\"appliance\":{\"bus_processed\":{"
                   "\"keep\":12288,\"check_interval_ms\":60000}}}\n") == 0,
               "write over-cap janitor keep fixture");
  janitor = fc_janitor_module_create();
  rc |= expect(janitor && janitor->init(janitor) == 0,
               "initialize janitor keep-clamp fixture");
  if (janitor) fc_janitor_module_destroy(janitor);
  janitor = NULL;
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(
      narration,
      "janitor: bus_processed keep 12288 reaches scan cap 12288; clamped to 12287",
      "janitor narrates configured keep clamp loudly");

  rc |= expect(test_mkdir_p("workspace/bus/processed") == 0,
               "create processed-bus cap-minus-one fixture directory");
  for (int i = 1; i < bus_scan_cap; ++i) {
    snprintf(path, sizeof(path),
             "workspace/bus/processed/bus_%06d.json", i);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      rc |= expect(0, "create processed-bus cap-minus-one fixture");
      break;
    }
    close(fd);
  }
  rc |= expect(count_directory_entries("workspace/bus/processed") ==
                   bus_scan_cap - 1,
               "processed-bus fixture reaches exactly scan cap minus one");
  rc |= expect(fc_janitor_test_bus_processed_prune(
                   "workspace/bus/processed", 10000) == 0,
               "janitor prunes cap-minus-one processed entries");
  rc |= expect(count_directory_entries("workspace/bus/processed") == 10000,
               "janitor leaves configured processed-bus keep count");
  rc |= expect_file_not_exists(
      "workspace/bus/processed/bus_000001.json");
  snprintf(path, sizeof(path),
           "workspace/bus/processed/bus_%06d.json", bus_scan_cap - 1);
  rc |= expect_file_exists(path);

  if (original_config)
    rc |= expect(test_write_file("config/floofclaw_config.json",
                                 original_config) == 0,
                 "restore config after capacity boundary test");
  rc |= test_reset_workspace();
  free(tasks);
  free(affairs);
  free(narration);
  free(event_log);
  free(original_config);
  free(scheduler);
  return rc;
}

typedef struct {
  volatile sig_atomic_t *stop;
  int read_fd;
  int ticks;
} SlowTickCanary;

static int slow_canary_init(FcReactorModule *module) {
  (void)module;
  return 0;
}

static int slow_canary_collect(FcReactorModule *module, FcPollSet *pollset) {
  SlowTickCanary *canary = module ? (SlowTickCanary *)module->state : NULL;
  if (!canary || fc_pollset_add(pollset, canary->read_fd, POLLIN,
                                module, NULL) != 0)
    return -1;
  return -2; /* deliberate callback error: reactor must isolate and count it */
}

static int slow_canary_on_fd(FcReactorModule *module, int fd, int events,
                             void *tag) {
  char byte;
  (void)module;
  (void)tag;
  if (events & POLLIN) (void)read(fd, &byte, 1);
  return -3; /* deliberate callback error */
}

static int slow_canary_tick(FcReactorModule *module, uint64_t now_ms) {
  SlowTickCanary *canary = module ? (SlowTickCanary *)module->state : NULL;
  char fake_now[32];
  if (!canary) return -1;
  canary->ticks++;
  snprintf(fake_now, sizeof(fake_now), "%llu",
           (unsigned long long)(now_ms + FC_REACTOR_SLOW_TICK_MS));
  (void)setenv("FCLAW_FAKE_MONOTONIC_MS", fake_now, 1);
  if (canary->ticks >= 2 && canary->stop) *canary->stop = 1;
  return -4; /* deliberate repeated error and exactly-threshold slow tick */
}

int reactor_slow_tick_canary_and_module_errors_reach_status(void) {
  FcReactor reactor;
  FcReactorModule module;
  FcReactorModule filler_modules[FC_REACTOR_MAX_MODULES - 1];
  SlowTickCanary canary;
  volatile sig_atomic_t stop = 0;
  int pipe_fds[2] = {-1, -1};
  char status[FC_STATUS_JSON_CAP];
  char *narration = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(FC_REACTOR_SLOW_TICK_MS == 250U,
               "slow tick threshold stays at defended named constant");
  rc |= expect(pipe(pipe_fds) == 0, "create canary reactor fd");
  if (pipe_fds[1] >= 0)
    rc |= expect(write(pipe_fds[1], "x", 1) == 1,
                 "prime canary fd callback");
  memset(&canary, 0, sizeof(canary));
  canary.stop = &stop;
  canary.read_fd = pipe_fds[0];
  memset(&module, 0, sizeof(module));
  module.name = "slow_canary";
  module.module_id = "slow-canary";
  module.state = &canary;
  module.init = slow_canary_init;
  module.collect_fds = slow_canary_collect;
  module.on_fd = slow_canary_on_fd;
  module.tick = slow_canary_tick;

  fc_reactor_init(&reactor);
  rc |= expect(fc_reactor_register(&reactor, &module) == 0,
               "register slow tick canary");
  (void)setenv("FCLAW_FAKE_MONOTONIC_MS", "1000", 1);
  rc |= expect(fc_reactor_run(&reactor, &stop, 1) == 0,
               "reactor isolates callback failures and keeps ticking");
  (void)unsetenv("FCLAW_FAKE_MONOTONIC_MS");

  rc |= expect(fc_reactor_total_module_errors(&reactor) == 5,
               "collect, fd, and repeated tick failures are counted");
  rc |= expect(fc_reactor_total_slow_ticks(&reactor) == 2,
               "both exactly-threshold slow ticks are counted");
  memset(filler_modules, 0, sizeof(filler_modules));
  for (size_t i = 0; i < FC_REACTOR_MAX_MODULES - 1; ++i) {
    filler_modules[i].name =
        "status-capacity-canary-with-a-deliberately-descriptive-module-name";
    filler_modules[i].module_id =
        "status-capacity-canary-with-a-deliberately-descriptive-module-id";
    rc |= expect(fc_reactor_register(&reactor, &filler_modules[i]) == 0,
                 "fill reactor registry for status capacity regression");
  }
  rc |= expect(fc_status_build_json_with_reactor(NULL, &reactor,
                                                  status, sizeof(status)) == 0,
               "build gateway status with reactor health");
  rc |= expect(strlen(status) > 2047U && strlen(status) < sizeof(status),
               "full module status exceeds old cap and fits shared cap");
  rc |= expect_substr(status,
                      "\"module_errors\": 5",
                      "gateway status surfaces aggregate module errors");
  rc |= expect_substr(status,
                      "\"module_id\":\"slow-canary\",\"errors\":5,"
                      "\"collect_fds_errors\":2,\"on_fd_errors\":1,"
                      "\"tick_errors\":2,\"slow_ticks\":2,"
                      "\"last_tick_ms\":250,\"max_tick_ms\":250",
                      "gateway status attributes callback health to module");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect(test_count_substr(
                   narration,
                   "reactor: slow tick module=slow-canary duration_ms=250 "
                   "threshold_ms=250") == 2,
               "slow tick canary emits attributed narration each time");

  free(narration);
  if (pipe_fds[0] >= 0) close(pipe_fds[0]);
  if (pipe_fds[1] >= 0) close(pipe_fds[1]);
  return rc;
}

static int expect_dns_backoff_narration(const char *narration,
                                        const char *adapter,
                                        const char *host, unsigned attempt_no) {
  char prefix[512];
  char attempt_text[64];
  const char *found;
  const char *attempt = NULL;
  const char *line_end = NULL;
  snprintf(prefix, sizeof(prefix),
           "%s: dns lookup failed for %s; reconnect in ", adapter, host);
  snprintf(attempt_text, sizeof(attempt_text), ", attempt %u", attempt_no);
  found = narration ? strstr(narration, prefix) : NULL;
  while (found) {
    line_end = strchr(found, '\n');
    attempt = strstr(found, attempt_text);
    if (attempt && (!line_end || attempt < line_end)) break;
    found = strstr(found + 1, prefix);
  }
  return expect(found && attempt && (!line_end || attempt < line_end),
                "DNS failure narration names host and backoff attempt");
}

int dns_failure_uses_channel_backoff_and_narration(void) {
  FcReactorModule *irc = NULL;
  DcAdapter *discord = NULL;
  char *original_config = NULL;
  char *narration = NULL;
  char *ws_source = NULL;
  int rc = 0;

  rc |= test_read_file("config/floofclaw_config.json", &original_config);
  rc |= test_reset_workspace();
  rc |= expect(test_write_file(
                   "config/floofclaw_config.json",
                   "{\"channels\":{\"irc\":{\"enabled\":true,"
                   "\"server\":\"irc.dns-test.invalid\",\"port\":6667,"
                   "\"tls\":false}}}\n") == 0,
               "write isolated IRC DNS-failure config");
  irc = fc_adapter_irc.create(NULL, NULL);
  rc |= expect(irc != NULL, "create IRC DNS-failure adapter");
  if (irc) {
    uint64_t retry_at;
    rc |= expect(irc->init(irc) == 0, "initialize IRC reconnect state");
    net_test_fail_next_dns();
    rc |= expect(irc->tick(irc, 1000U) == 0,
                 "IRC treats failed getaddrinfo as connection failure");
    rc |= expect(irc->next_deadline_ms(irc, 1000U) > 1000U,
                 "IRC waits in shared reconnect backoff after DNS failure");
    retry_at = irc->next_deadline_ms(irc, 1000U);
    net_test_fail_next_dns();
    rc |= expect(irc->tick(irc, retry_at) == 0 &&
                     irc->next_deadline_ms(irc, retry_at) > retry_at,
                 "IRC remains patient across repeated DNS failure");
  }
  net_test_reset_dns();

  discord = (DcAdapter *)calloc(1, sizeof(*discord));
  rc |= expect(discord != NULL, "allocate Discord DNS-failure adapter");
  if (discord) {
    uint64_t retry_at;
    discord->enabled = 1;
    discord->gw_fd = -1;
    snprintf(discord->module_id_buf, sizeof(discord->module_id_buf),
             "discord-dns-test");
    fc_reconnect_backoff_init(&discord->reconnect_backoff,
                              discord->module_id_buf);
    net_test_fail_next_dns();
    rc |= expect(dc_gateway_start_connect(discord, 2000U) != 0,
                 "Discord treats failed getaddrinfo as connection failure");
    rc |= expect(discord->next_reconnect_ms > 2000U,
                 "Discord waits in shared reconnect backoff after DNS failure");
    retry_at = discord->next_reconnect_ms;
    net_test_fail_next_dns();
    rc |= expect(dc_gateway_start_connect(discord, retry_at) != 0 &&
                     discord->next_reconnect_ms > retry_at,
                 "Discord remains patient across repeated DNS failure");
  }
  net_test_reset_dns();

  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_dns_backoff_narration(
      narration, "irc", "irc.dns-test.invalid", 1);
  rc |= expect_dns_backoff_narration(
      narration, "irc", "irc.dns-test.invalid", 2);
  rc |= expect_dns_backoff_narration(
      narration, "discord", DC_GATEWAY_HOST, 1);
  rc |= expect_dns_backoff_narration(
      narration, "discord", DC_GATEWAY_HOST, 2);
  rc |= test_read_file("adapters/ws/ws_adapter.c", &ws_source);
  rc |= expect(ws_source && strstr(ws_source, "net_connect_nb(") == NULL &&
                   strstr(ws_source, "getaddrinfo(") == NULL,
               "WebSocket listener has no outbound DNS failure path");

  if (irc) fc_adapter_irc.destroy(irc);
  if (original_config)
    rc |= expect(test_write_file("config/floofclaw_config.json",
                                 original_config) == 0,
                 "restore config after DNS-failure probe");
  free(discord);
  free(original_config);
  free(narration);
  free(ws_source);
  return rc;
}

int socket_parse_progress_deadlines_are_bounded(void) {
  DcAdapter *discord = NULL;
  char *irc_source = NULL;
  char *ws_source = NULL;
  char *narration = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(!net_progress_timed_out(1000U, 30999U, 30000U) &&
                   net_progress_timed_out(1000U, 31000U, 30000U) &&
                   !net_progress_timed_out(0, 999999U, 1U) &&
                   !net_progress_timed_out(1000U, 999U, 1U),
               "monotonic progress deadline has exact safe boundaries");

  discord = (DcAdapter *)calloc(1, sizeof(*discord));
  rc |= expect(discord != NULL, "allocate Discord progress canary");
  if (discord) {
    discord->enabled = 1;
    discord->gw_fd = -1;
    discord->rest_fd = -1;
    discord->gw_state = DC_GATEWAY_OPEN;
    discord->gw_rx_len = 1;
    discord->gw_parse_progress_ms = 1000U;
    snprintf(discord->module_id_buf, sizeof(discord->module_id_buf),
             "discord-progress-test");
    fc_reconnect_backoff_init(&discord->reconnect_backoff,
                              discord->module_id_buf);
    (void)setenv("FCLAW_FAKE_MONOTONIC_MS", "31000", 1);
    rc |= expect(dc_gateway_progress_expired(discord, 31000U) == 1 &&
                     discord->gw_state == DC_DISCONNECTED &&
                     discord->next_reconnect_ms > 31000U,
                 "stalled Discord frame closes into reconnect backoff");

    rc |= expect(dc_queue_raw(discord, "progress-channel",
                              "{\"content\":\"fixture\"}") == 0,
                 "queue the REST response whose progress will expire");
    discord->rest_cur = discord->outq[discord->out_head];
    discord->rest_has_cur = 1;
    discord->rest_state = DC_REST_READING;
    discord->rest_read_started_ms = 1000U;
    rc |= expect(dc_rest_progress_expired(discord, 31000U) == 1 &&
                     discord->rest_state == DC_REST_IDLE &&
                     discord->rest_retry_after_ms > 31000U,
                 "stalled Discord REST response closes into retry delay");
  }

  (void)unsetenv("FCLAW_FAKE_MONOTONIC_MS");

  rc |= test_read_file("adapters/irc/irc_adapter.c", &irc_source);
  rc |= expect(irc_source &&
                   strstr(irc_source, "IRC_PARSE_PROGRESS_TIMEOUT_MS") &&
                   strstr(irc_source, "net_progress_timed_out") &&
                   strstr(irc_source, "partial line made no parse progress"),
               "IRC partial-line buffer is wired to close and backoff");
  rc |= test_read_file("adapters/ws/ws_adapter.c", &ws_source);
  rc |= expect(ws_source &&
                   strstr(ws_source, "WS_PARSE_PROGRESS_TIMEOUT_MS") &&
                   strstr(ws_source, "net_progress_timed_out") &&
                   strstr(ws_source,
                          "(size_t)n >= sizeof(c->rx) - c->rx_len") &&
                   strstr(ws_source, "c->rx[c->rx_len] = '\\0'") &&
                   strstr(ws_source, "client input made no parse progress"),
               "WebSocket buffers reserve terminator and close stalled clients");

  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration,
                      "cause=gateway input made no parse progress",
                      "Discord progress expiry is narrated");
  free(discord);
  free(irc_source);
  free(ws_source);
  free(narration);
  return rc;
}

static char *discord_save_env(const char *name) {
  const char *value = getenv(name);
  return value ? strdup(value) : NULL;
}

static void discord_restore_env(const char *name, char *saved) {
  if (saved) {
    (void)setenv(name, saved, 1);
    free(saved);
  } else {
    (void)unsetenv(name);
  }
}

static int discord_install_test_token(DcAdapter *a, const char *home) {
  if (!a || !home || setenv("FCLAW_HOME", home, 1) != 0 ||
      secret_store_set("endpoint:discord_fixture_token", "fixture-token") !=
          0)
    return -1;
  snprintf(a->token_key, sizeof(a->token_key), "discord_fixture_token");
  return 0;
}

static int discord_decode_client_frame(const DcAdapter *a, char *out,
                                       size_t out_len) {
  const unsigned char *p;
  const unsigned char *mask;
  uint64_t payload_len;
  size_t header = 2U;
  if (!a || !out || out_len == 0U || a->gw_tx_len < 6U) return -1;
  p = (const unsigned char *)a->gw_tx;
  if ((p[1] & 0x80U) == 0U) return -1;
  payload_len = p[1] & 0x7fU;
  if (payload_len == 126U) {
    if (a->gw_tx_len < 8U) return -1;
    payload_len = ((uint64_t)p[2] << 8) | p[3];
    header = 4U;
  } else if (payload_len == 127U) {
    if (a->gw_tx_len < 14U) return -1;
    payload_len = 0;
    for (int i = 0; i < 8; ++i)
      payload_len = (payload_len << 8) | p[2 + i];
    header = 10U;
  }
  if (payload_len >= out_len || payload_len > a->gw_tx_len - header - 4U)
    return -1;
  mask = p + header;
  header += 4U;
  for (size_t i = 0; i < (size_t)payload_len; ++i)
    out[i] = (char)(p[header + i] ^ mask[i & 3U]);
  out[payload_len] = '\0';
  return 0;
}

int discord_reconnect_causes_and_heartbeat_ack_are_narrated(void) {
  DcAdapter *a = NULL;
  char *saved_home = discord_save_env("FCLAW_HOME");
  char *narration = NULL;
  char frame_payload[12000];
  int rc = 0;

  rc |= test_reset_workspace();
  a = (DcAdapter *)calloc(1, sizeof(*a));
  rc |= expect(a != NULL, "allocate Discord reconnect fixture");
  if (!a) {
    discord_restore_env("FCLAW_HOME", saved_home);
    return 1;
  }
  a->enabled = 1;
  a->gw_fd = -1;
  a->rest_fd = -1;
  rc |= expect(discord_install_test_token(a, "workspace/discord-gateway-auth") ==
                   0,
               "install isolated Discord gateway token");
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "discord-cause-test");
  fc_reconnect_backoff_init(&a->reconnect_backoff, a->module_id_buf);
  (void)setenv("FCLAW_FAKE_MONOTONIC_MS", "1000", 1);

  rc |= expect(dc_test_gateway_close_frame(a, 4000U, "fixture close") != 0 &&
                   a->gw_state == DC_DISCONNECTED,
               "server close frame enters reconnect state");

  a->gw_state = DC_GATEWAY_OPEN;
  a->resume_eligible = 1;
  snprintf(a->session_id, sizeof(a->session_id), "session-before-reconnect");
  a->sequence = 41;
  rc |= expect(dc_test_gateway_json(a, "{\"op\":7,\"d\":null}") != 0 &&
                   a->gw_state == DC_DISCONNECTED && a->resume_eligible &&
                   strcmp(a->session_id, "session-before-reconnect") == 0 &&
                   a->sequence == 41,
               "server RECONNECT preserves resumable session state");
  a->gw_state = DC_GATEWAY_OPEN;
  rc |= expect(dc_test_gateway_json(
                   a,
                   "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}") ==
                       0 &&
                   discord_decode_client_frame(a, frame_payload,
                                               sizeof(frame_payload)) == 0 &&
                   strstr(frame_payload, "\"op\":6") != NULL &&
                   strstr(frame_payload, "session-before-reconnect") != NULL &&
                   strstr(frame_payload, "\"seq\":41") != NULL,
               "HELLO after op 7 queues RESUME with the preserved session");

  a->gw_tx_len = 0;
  a->gw_state = DC_GATEWAY_OPEN;
  a->resume_eligible = 1;
  snprintf(a->session_id, sizeof(a->session_id), "session-resumable-invalid");
  a->sequence = 42;
  rc |= expect(dc_test_gateway_json(a, "{\"op\":9,\"d\":true}") != 0 &&
                   a->gw_state == DC_DISCONNECTED && a->resume_eligible &&
                   strcmp(a->session_id, "session-resumable-invalid") == 0 &&
                   a->sequence == 42,
               "resumable invalid session preserves resume state");
  a->gw_state = DC_GATEWAY_OPEN;
  rc |= expect(dc_test_gateway_json(
                   a,
                   "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}") ==
                       0 &&
                   discord_decode_client_frame(a, frame_payload,
                                               sizeof(frame_payload)) == 0 &&
                   strstr(frame_payload, "\"op\":6") != NULL &&
                   strstr(frame_payload, "session-resumable-invalid") != NULL,
               "HELLO after resumable op 9 queues RESUME");

  a->gw_tx_len = 0;
  a->gw_state = DC_GATEWAY_OPEN;
  snprintf(a->session_id, sizeof(a->session_id), "session-before-invalid");
  a->sequence = 42;
  rc |= expect(dc_test_gateway_json(a, "{\"op\":9,\"d\":false}") != 0 &&
                   a->gw_state == DC_DISCONNECTED &&
                   !a->resume_eligible && !a->session_id[0] && a->sequence == 0,
               "non-resumable invalid session clears resume state");
  a->gw_state = DC_GATEWAY_OPEN;
  rc |= expect(dc_test_gateway_json(
                   a,
                   "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}") ==
                       0 &&
                   discord_decode_client_frame(a, frame_payload,
                                               sizeof(frame_payload)) == 0 &&
                   strstr(frame_payload, "\"op\":2") != NULL,
               "HELLO after non-resumable op 9 queues IDENTIFY");

  a->gw_tx_len = 0;
  a->gw_state = DC_GATEWAY_OPEN;
  a->heartbeat_interval_ms = 100;
  a->next_heartbeat_ms = 1100;
  rc |= expect(dc_test_heartbeat_due(a, 1100) == 1 &&
                   a->heartbeat_ack_pending,
               "heartbeat records its pending ACK");
  rc |= expect(dc_test_gateway_json(a, "{\"op\":11,\"d\":null}") == 0 &&
                   !a->heartbeat_ack_pending,
               "op 11 clears the pending heartbeat ACK");
  a->next_heartbeat_ms = 1200;
  rc |= expect(dc_test_heartbeat_due(a, 1200) == 1,
               "next heartbeat starts a fresh ACK window");
  rc |= expect(dc_test_heartbeat_due(a, 1300) != 0 &&
                   a->gw_state == DC_DISCONNECTED,
               "next interval without op 11 reconnects deliberately");

  a->gw_state = DC_GATEWAY_OPEN;
  rc |= expect(dc_test_gateway_json(
                   a, "{\"op\":0,\"t\":\"READY\",\"d\":{\"session_id\":\"s\",\"user\":{\"id\":\"u\"}}}") == 0,
               "READY dispatch is accepted");
  rc |= expect(dc_test_gateway_json(
                   a, "{\"op\":0,\"t\":\"RESUMED\",\"d\":{}}") == 0,
               "RESUMED dispatch is accepted");

  (void)unsetenv("FCLAW_FAKE_MONOTONIC_MS");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration, "close code=4000 reason=fixture close",
                      "close narration carries code and reason");
  rc |= expect_substr(narration, "server RECONNECT (op 7)",
                      "server RECONNECT cause is narrated");
  rc |= expect_substr(narration, "invalid session resumable=false",
                      "invalid-session resumability is narrated");
  rc |= expect_substr(narration, "invalid session resumable=true",
                      "resumable invalid-session decision is narrated");
  rc |= expect_substr(narration, "heartbeat ACK missed",
                      "missed heartbeat ACK is narrated");
  rc |= expect_substr(narration, "session ready via IDENTIFY",
                      "fresh IDENTIFY session is distinguished");
  rc |= expect_substr(narration, "session ready via RESUME",
                      "resumed session is distinguished");
  free(narration);
  free(a);
  discord_restore_env("FCLAW_HOME", saved_home);
  return rc;
}

int discord_gateway_oversized_frame_is_rejected(void) {
  static const unsigned char oversized[] = {
      0x81U, 0x7fU, 0xffU, 0xffU, 0xffU, 0xffU,
      0xffU, 0xffU, 0xffU, 0xffU};
  DcAdapter *a = (DcAdapter *)calloc(1, sizeof(*a));
  int rc = 0;

  rc |= expect(a != NULL, "allocate Discord oversized-frame fixture");
  if (!a) return 1;
  rc |= test_reset_workspace();
  a->enabled = 1;
  a->gw_fd = -1;
  a->rest_fd = -1;
  snprintf(a->module_id_buf, sizeof(a->module_id_buf),
           "discord-frame-test");
  fc_reconnect_backoff_init(&a->reconnect_backoff, a->module_id_buf);
  rc |= expect(dc_test_gateway_raw_frame(a, oversized, sizeof(oversized)) != 0 &&
                   a->gw_state == DC_DISCONNECTED && a->gw_rx_len == 0,
               "UINT64_MAX WebSocket payload length is rejected before arithmetic");
  free(a);
  return rc;
}

int discord_rest_429_then_200_preserves_delivery_order(void) {
  static const char limited[] =
      "HTTP/1.1 429 Too Many Requests\r\n"
      "Retry-After: 2.5\r\nContent-Length: 17\r\n\r\n"
      "{\"retry_after\":9}";
  static const char ok[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
  static const char server_error[] =
      "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\n\r\n";
  static const char huge_retry_after[] =
      "HTTP/1.1 429 Too Many Requests\r\n"
      "Retry-After: 999999\r\nContent-Length: 0\r\n\r\n";
  DcAdapter *a = (DcAdapter *)calloc(1, sizeof(*a));
  int rc = 0;

  rc |= expect(a != NULL, "allocate Discord REST fixture");
  if (!a) return 1;
  a->enabled = 1;
  a->gw_fd = -1;
  a->rest_fd = -1;
  a->rest_state = DC_REST_IDLE;
  rc |= expect(dc_queue_raw(a, "chan", "{\"content\":\"first\"}") == 0 &&
                   dc_queue_raw(a, "chan", "{\"content\":\"second\"}") == 0,
               "queue two ordered Discord deliveries");

  rc |= expect(dc_test_rest_response(a, limited, 1000U) == 0 &&
                   a->out_count == 2 && a->out_head == 0 &&
                   a->rest_retry_after_ms == 3500U &&
                   strstr(a->outq[a->out_head].payload, "first") != NULL,
               "429 Retry-After keeps the first delivery at queue head");
  rc |= expect(dc_test_rest_response(a, ok, 3500U) == 0 &&
                   a->out_count == 1 &&
                   strstr(a->outq[a->out_head].payload, "second") != NULL,
               "successful retry commits exactly the first delivery");
  rc |= expect(dc_test_rest_response(a, ok, 3501U) == 0 &&
                   a->out_count == 0,
               "the following delivery remains ordered and commits once");

  rc |= expect(dc_queue_raw(a, "chan", "{\"content\":\"server\"}") == 0,
               "queue server-error retry fixture");
  rc |= expect(dc_test_rest_response(a, server_error, 4000U) == 0 &&
                   a->out_count == 1 && a->rest_server_retries == 1U,
               "first 5xx retains the delivery for one retry");
  rc |= expect(dc_test_rest_response(a, server_error, 5500U) == 0 &&
                   a->out_count == 0 && a->rest_server_retries == 0U,
               "second 5xx drops after the designed single retry");

  rc |= expect(dc_queue_raw(a, "chan", "{\"content\":\"clamped\"}") == 0,
               "queue oversized Retry-After fixture");
  rc |= expect(dc_test_rest_response(a, huge_retry_after, 6000U) == 0 &&
                   a->out_count == 1 &&
                   a->rest_retry_after_ms ==
                       6000U + DC_REST_RETRY_AFTER_MAX_MS,
               "oversized Retry-After clamps to the one-day maximum");
  rc |= expect(dc_test_rest_response(a, ok, a->rest_retry_after_ms) == 0 &&
                   a->out_count == 0,
               "delivery succeeds after the clamped rate-limit delay");

  free(a);
  return rc;
}

/* A reply Discord will never accept -- a non-retryable 4xx, or a 5xx that
 * failed its one retry -- is committed off the queue so the head cannot
 * wedge. Until 2026-08-25 that drop went only to the debug-only diagnostic,
 * so a revoked token or a lost channel permission silently ate every reply
 * in production. It is a delivery the operator did not get: narrate it with
 * the channel, the status, and Discord's own reason. */
int discord_rest_permanent_failure_is_narrated_and_dropped(void) {
  static const char forbidden_body[] =
      "{\"message\":\"Missing Access\",\"code\":50001}";
  static const char server_error[] =
      "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
  char forbidden[256];
  DcAdapter *a = (DcAdapter *)calloc(1, sizeof(*a));
  char *narration = NULL;
  int rc = 0;

  rc |= expect(a != NULL, "allocate Discord REST fixture");
  if (!a) return 1;
  rc |= test_reset_workspace();
  a->enabled = 1;
  a->gw_fd = -1;
  a->rest_fd = -1;
  a->rest_state = DC_REST_IDLE;
  snprintf(forbidden, sizeof(forbidden),
           "HTTP/1.1 403 Forbidden\r\nContent-Length: %zu\r\n\r\n%s",
           strlen(forbidden_body), forbidden_body);

  rc |= expect(dc_queue_raw(a, "1234567890", "{\"content\":\"hello\"}") == 0,
               "queue one delivery to a channel the bot cannot post to");
  rc |= expect(dc_test_rest_response(a, forbidden, 1000U) == 0 &&
                   a->out_count == 0 && a->rest_retry_after_ms == 0,
               "a non-retryable 4xx is dropped without a retry so the head cannot wedge");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration,
                      "discord: REST post to channel 1234567890 failed status=403"
                      " code=50001 message=Missing Access; reply dropped",
                      "the permanent drop is narrated with channel, status, and Discord's reason");
  free(narration);
  narration = NULL;

  rc |= expect(dc_queue_raw(a, "1234567890", "{\"content\":\"again\"}") == 0,
               "queue a delivery that will meet two server errors");
  rc |= expect(dc_test_rest_response(a, server_error, 2000U) == 0 &&
                   a->out_count == 1 && a->rest_server_retries == 1U,
               "first 5xx retains the delivery for its one retry");
  rc |= expect(dc_test_rest_response(a, server_error, 4000U) == 0 &&
                   a->out_count == 0 && a->rest_server_retries == 0U,
               "second 5xx drops the delivery");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration,
                      "failed status=502; reply dropped after one retry",
                      "the post-retry drop is narrated as such");

  free(narration);
  free(a);
  return rc;
}

int discord_rest_ambiguous_completion_is_bounded_and_narrated(void) {
  static const char ok[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
  DcAdapter *a = (DcAdapter *)calloc(1, sizeof(*a));
  char *saved_home = discord_save_env("FCLAW_HOME");
  char *narration = NULL;
  int rc = 0;

  rc |= expect(a != NULL, "allocate Discord ambiguous-completion fixture");
  if (!a) {
    discord_restore_env("FCLAW_HOME", saved_home);
    return 1;
  }
  rc |= test_reset_workspace();
  a->enabled = 1;
  a->gw_fd = -1;
  a->rest_fd = -1;
  a->rest_state = DC_REST_IDLE;
  rc |= expect(discord_install_test_token(a, "workspace/discord-rest-auth") ==
                   0,
               "install isolated Discord REST token");

  rc |= expect(dc_queue_raw(a, "timeout-channel",
                            "{\"content\":\"timeout\"}") == 0,
               "queue response-timeout fixture");
  a->rest_cur = a->outq[a->out_head];
  a->rest_has_cur = 1;
  a->rest_state = DC_REST_READING;
  a->rest_read_started_ms = 1000U;
  rc |= expect(dc_rest_progress_expired(a, 31000U) == 1 &&
                   a->out_count == 1 && a->rest_state == DC_REST_IDLE &&
                   a->rest_ambiguous_retries == 1U &&
                   a->rest_retry_after_ms == 32500U,
               "a no-response outcome retains the head for one bounded retry");
  rc |= expect(dc_test_rest_response(a, "not an HTTP response", 32500U) == 0 &&
                   a->out_count == 0 && a->rest_ambiguous_retries == 0U &&
                   a->rest_retry_after_ms == 0,
               "a second ambiguous outcome drops instead of retrying forever");

  rc |= expect(dc_queue_raw(a, "tls-read-channel",
                            "{\"content\":\"tls read\"}") == 0,
               "queue TLS-read failure fixture");
  a->rest_cur = a->outq[a->out_head];
  a->rest_has_cur = 1;
  a->rest_state = DC_REST_READING;
  (void)setenv("FCLAW_FAKE_MONOTONIC_MS", "50000", 1);
  dc_drive_rest(a, POLLIN);
  rc |= expect(a->out_count == 1 && a->rest_state == DC_REST_IDLE &&
                   a->rest_ambiguous_retries == 1U,
               "TLS read failure takes the shared one-retry path");
  a->rest_cur = a->outq[a->out_head];
  a->rest_has_cur = 1;
  a->rest_state = DC_REST_READING;
  (void)setenv("FCLAW_FAKE_MONOTONIC_MS", "52000", 1);
  dc_drive_rest(a, POLLIN);
  rc |= expect(a->out_count == 0 && a->rest_state == DC_REST_IDLE &&
                   a->rest_ambiguous_retries == 0U,
               "repeated TLS read failure cannot duplicate without bound");
  (void)unsetenv("FCLAW_FAKE_MONOTONIC_MS");

  snprintf(a->token_key, sizeof(a->token_key), "missing_discord_fixture");
  rc |= expect(dc_queue_raw(a, "build-channel",
                            "{\"content\":\"build\"}") == 0 &&
                   dc_rest_start(a, 53000U) != 0 && a->out_count == 0 &&
                   a->rest_retry_after_ms == 0,
               "an unbuildable request is dropped before opening a connection");

  snprintf(a->token_key, sizeof(a->token_key), "discord_fixture_token");
  rc |= expect(dc_queue_raw(a, "connect-channel",
                            "{\"content\":\"connect\"}") == 0,
               "queue pre-send connect failure fixture");
  net_test_fail_next_dns();
  rc |= expect(dc_rest_start(a, 54000U) != 0 && a->out_count == 1 &&
                   a->rest_state == DC_REST_IDLE &&
                   a->rest_ambiguous_retries == 0U &&
                   a->rest_retry_after_ms == 55500U,
               "pre-send connection failure retains the head without spending the ambiguous budget");
  net_test_reset_dns();
  rc |= expect(dc_test_rest_response(a, ok, 55500U) == 0 &&
                   a->out_count == 0,
               "delivery can commit after a safe pre-send retry");

  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect_substr(narration, "response timeout after request write",
                      "response timeout ambiguity is narrated");
  rc |= expect_substr(narration, "unreadable HTTP response after request write",
                      "malformed response ambiguity is narrated");
  rc |= expect_substr(narration, "TLS read failed after request write",
                      "TLS read ambiguity is narrated");
  rc |= expect_substr(narration,
                      "reply dropped after one retry to avoid duplicate delivery",
                      "ambiguous retry cap explains its permanent drop");
  rc |= expect_substr(narration,
                      "REST request for channel build-channel could not be built; reply dropped",
                      "permanent local request-build failure is narrated");

  net_test_reset_dns();
  (void)unsetenv("FCLAW_FAKE_MONOTONIC_MS");
  free(narration);
  free(a);
  discord_restore_env("FCLAW_HOME", saved_home);
  return rc;
}

int discord_outbox_backpressure_preserves_whole_deliveries(void) {
  DcAdapter *a = (DcAdapter *)calloc(1, sizeof(*a));
  char long_text[DC_CHUNK_MAX + 101U];
  char delivery_log[DC_CHUNK_MAX + 1024U];
  char *narration = NULL;
  long log_size;
  int n;
  int rc = 0;

  rc |= expect(a != NULL, "allocate Discord outbox backpressure fixture");
  if (!a) return 1;
  rc |= test_reset_workspace();
  memset(long_text, 'x', sizeof(long_text) - 1U);
  long_text[sizeof(long_text) - 1U] = '\0';
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "discord-main");
  a->delivery_cursor_init = 1;
  a->delivery_cursor = 0;
  for (int i = 0; i < DC_OUT_QUEUE_MAX - 1; ++i)
    rc |= expect(dc_queue_raw(a, "blocked-head", "{\"content\":\"held\"}") ==
                     0,
                 "fill Discord queue to one free slot");
  n = snprintf(
      delivery_log, sizeof(delivery_log),
      "{\"delivery\":{\"channel\":\"discord\","
      "\"adapter_id\":\"discord-main\",\"text\":\"%s\","
      "\"ref\":{\"channel_id\":\"first-channel\"}}}\n"
      "{\"delivery\":{\"channel\":\"discord\","
      "\"adapter_id\":\"discord-main\",\"text\":\"later\","
      "\"ref\":{\"channel_id\":\"later-channel\"}}}\n",
      long_text);
  rc |= expect(n > 0 && (size_t)n < sizeof(delivery_log) &&
                   test_write_file(DC_DELIVERIES_LOG, delivery_log) == 0,
               "write a two-chunk delivery followed by another delivery");
  log_size = test_file_size(DC_DELIVERIES_LOG);

  dc_drain_deliveries(a);
  dc_drain_deliveries(a);
  rc |= expect(a->out_count == DC_OUT_QUEUE_MAX - 1 &&
                   a->delivery_cursor == 0,
               "insufficient ring space enqueues no partial chunks and advances no cursor");
  rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
  rc |= expect(narration &&
                   test_count_substr(
                       narration,
                       "delivery to channel first-channel deferred at log offset 0; outbound queue is full") ==
                       1,
               "one production narration identifies the deferred delivery");

  a->out_head = 0;
  a->out_count = 0;
  dc_drain_deliveries(a);
  rc |= expect(a->out_count == 3 && a->delivery_cursor == log_size,
               "released ring capacity queues the whole blocked delivery and its successor");
  rc |= expect(strstr(a->outq[0].payload, "xxxxxxxx") != NULL &&
                   strstr(a->outq[1].payload, "xxxxxxxx") != NULL &&
                   strstr(a->outq[2].payload, "later") != NULL,
               "resumed drain preserves chunk and delivery order");

  free(narration);
  free(a);
  return rc;
}

/* The make-robustness fuzz gate's event_reducer invariant, as a permanent
 * regression: if the reducer accepts an event log, the reduced state must
 * parse as a top-level JSON object — even when a field's escaped form
 * exceeds the projection buffers (json_escape truncation, 2026-07-20). */
int reducer_output_parses_with_oversized_escaped_fields(void) {
  static char event[RT_XL];
  static char state[RT_XL];
  RtReplayReport report;
  JsonRef root;
  size_t i;
  size_t off;
  int rc = 0;

  off = (size_t)snprintf(event, sizeof(event),
                         "{\"type\":\"user_message\",\"payload\":{\"text\":\"");
  for (i = 0; i < 3000 && off + 2 < sizeof(event); ++i) {
    event[off++] = '\\';
    event[off++] = '"';
  }
  off += (size_t)snprintf(event + off, sizeof(event) - off, "\"}}\n");
  rc |= expect(off < sizeof(event), "fixture event fits its buffer");

  rc |= expect(rt_reduce_event_log_text_report(event, state, sizeof(state),
                                               &report) == 0,
               "reducer accepts an event with an oversized text field");
  rc |= expect(json_ref_top_object(state, &root) == 0,
               "reduced state parses as a top-level JSON object");
  return rc;
}

/* #12: hand-written CLI parsers must be strict — a typo can never
 * silently run under the default or live floop. Table-driven over the
 * channel and gateway surfaces (the run subcommand uses the identical
 * value checks in main.c). */
int cli_parsers_reject_malformed_arguments(void) {
  struct Case {
    const char *label;
    int gateway; /* 0 = harness_cli_channel, 1 = harness_cli_gateway */
    int argc;
    const char *argv[4];
    const char *needle;
  };
  static const struct Case cases[] = {
    {"channel cli trailing floop", 0, 2, {"cli", "--floop"}, "missing value for --floop"},
    {"channel cli flag-shaped floop value", 0, 3, {"cli", "--floop", "--json"}, "missing value for --floop"},
    {"channel cli unknown argument", 0, 2, {"cli", "bogus"}, "unknown argument 'bogus'"},
    {"channel cli --loop correction", 0, 3, {"cli", "--loop", "x"}, "Did you mean --floop?"},
    {"gateway status trailing junk", 1, 2, {"status", "bogus"}, "unknown argument 'bogus'"},
    {"gateway stop trailing junk", 1, 2, {"stop", "bogus"}, "unknown argument 'bogus'"},
    {"gateway start trailing floop", 1, 2, {"start", "--floop"}, "missing value for --floop"},
    {"gateway reload unknown argument", 1, 2, {"reload", "bogus"}, "unknown argument 'bogus'"},
  };
  int rc = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    char *out = NULL;
    int exit_code = 0;
    char label[128];
    int cap_rc = cases[i].gateway
        ? harness_cli_gateway(cases[i].argc, (char **)cases[i].argv, &out, &exit_code)
        : harness_cli_channel(cases[i].argc, (char **)cases[i].argv, &out, &exit_code);
    snprintf(label, sizeof(label), "%s: captured", cases[i].label);
    rc |= expect(cap_rc == 0, label);
    snprintf(label, sizeof(label), "%s: exits nonzero", cases[i].label);
    rc |= expect(exit_code != 0, label);
    snprintf(label, sizeof(label), "%s: names the problem", cases[i].label);
    rc |= expect_substr(out ? out : "", cases[i].needle, label);
    free(out);
  }
  return rc;
}

/* #15: agent terminal errors carry their exact run-relative stderr
 * artifact; view -d resolves each error to its own attempt's file, never
 * guessing among multiple failures in one run. */
int view_debug_correlates_agent_stderr_by_artifact(void) {
  static const char events[] =
      "{\"event_id\":\"evt_agent_000001\",\"ts\":\"2026-07-21T00:00:00Z\","
      "\"type\":\"error\",\"source\":\"chat_manager\","
      "\"run_id\":\"run_view_agent_failure\",\"payload\":{"
      "\"message\":\"agent chat_manager failed (attempt 1)\","
      "\"artifact\":\"agent_outputs/003_manage.stderr\"}}\n"
      "{\"event_id\":\"evt_agent_000002\",\"ts\":\"2026-07-21T00:00:01Z\","
      "\"type\":\"error\",\"source\":\"chat_manager\","
      "\"run_id\":\"run_view_agent_failure\",\"payload\":{"
      "\"message\":\"agent chat_manager failed (attempt 2)\","
      "\"artifact\":\"agent_outputs/005_manage.stderr\"}}\n"
      "{\"event_id\":\"evt_agent_000003\",\"ts\":\"2026-07-21T00:00:02Z\","
      "\"type\":\"run_failed\",\"source\":\"scheduler\","
      "\"run_id\":\"run_view_agent_failure\","
      "\"payload\":{\"reason\":\"agent failed\"}}\n";
  char *out = NULL;
  char *argv[] = { "-h", "-d", "run_view_agent_failure" };
  int exit_code = 0;
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= test_mkdir_p("workspace/runs/run_view_agent_failure/agent_outputs");
  rc |= test_write_file("workspace/runs/run_view_agent_failure/event_log.jsonl",
                        events);
  rc |= test_write_file(
      "workspace/runs/run_view_agent_failure/agent_outputs/003_manage.stderr",
      "FIRST_ATTEMPT_STDERR_MARKER\n");
  rc |= test_write_file(
      "workspace/runs/run_view_agent_failure/agent_outputs/005_manage.stderr",
      "SECOND_ATTEMPT_STDERR_MARKER\n");
  rc |= test_write_file(
      "workspace/runs/run_view_agent_failure/agent_outputs/004_other.stderr",
      "UNREFERENCED_AGENT_STDERR_MUST_NOT_RENDER\n");
  rc |= expect(harness_cli_view(3, argv, &out, &exit_code) == 0,
               "capture view -d agent failure evidence");
  rc |= expect(exit_code == 0, "view -d exits successfully");
  rc |= expect(out && test_count_substr(out, "FIRST_ATTEMPT_STDERR_MARKER") == 1,
               "first agent error renders exactly its own stderr");
  rc |= expect(out && test_count_substr(out, "SECOND_ATTEMPT_STDERR_MARKER") == 1,
               "second agent error renders exactly its own stderr");
  rc |= expect_no_substr(out, "UNREFERENCED_AGENT_STDERR_MUST_NOT_RENDER",
                         "view -d never dumps unreferenced agent stderr");
  free(out);
  rc |= test_reset_workspace();
  return rc;
}
