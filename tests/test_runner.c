#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "test_support.h"

static long long now_ms(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0) return 0;
  return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

typedef int (*TestFn)(void);

typedef enum {
  TEST_GROUP_CONTRACT,
  TEST_GROUP_CONFIG,
  TEST_GROUP_EXECUTOR,
  TEST_GROUP_BUS,
  TEST_GROUP_SCHEDULER,
  TEST_GROUP_ACTIONS,
  TEST_GROUP_ARTIFACTS,
  TEST_GROUP_REPLAY,
  TEST_GROUP_BUDGET,
  TEST_GROUP_ARCHITECTURE,
  TEST_GROUP_MEMORY
} TestGroup;

typedef struct {
  const char *name;
  TestFn fn;
  TestGroup group;
} TestCase;

/* TEST_REGISTRY_INC names the registry to compile against — passed via
 * -DTEST_REGISTRY_INC=... so the same runner core builds two binaries
 * (fclaw_unit_tests + fclaw_integration_tests). Default to the unit
 * registry when the macro is unset (safer than crashing). */
#ifndef TEST_REGISTRY_INC
#define TEST_REGISTRY_INC "test_registry_unit.inc"
#endif

#define TEST_ENTRY(name, fn, group) int fn(void);
#include TEST_REGISTRY_INC
#undef TEST_ENTRY

#define TEST_ENTRY(name, fn, group) { name, fn, group },
static const TestCase tests[] = {
#include TEST_REGISTRY_INC
};
#undef TEST_ENTRY

/* Per-test wall-clock budget (ms). Tests over budget fail with a
 * clear message — catches the next "silent slowdown" before it
 * accumulates. The default budget is set per binary at compile time
 * (DEFAULT_BUDGET_MS); tests that legitimately need more time list
 * an override here. Each entry should have a one-line reason so the
 * exception is auditable. */
#ifndef DEFAULT_BUDGET_MS
#define DEFAULT_BUDGET_MS 250
#endif

typedef struct { const char *name; int budget_ms; } BudgetOverride;
static const BudgetOverride kBudgetOverrides[] = {
  /* Heap-budget tests run a 20-event gateway sample; each event walks
   * the noop_agent + message intrinsic — measured ~120ms in practice. */
  { "native_noop_heap_budget_stays_under_limit", 3000 },
  { "native_noop_has_zero_reallocs",             3000 },
  { "native_noop_has_zero_strdups",              3000 },
  /* Two complete turns, including one durable failed-run retirement. APFS
   * state fsyncs under sanitizer instrumentation have measured 289-836ms. */
  { "failed_turn_does_not_poison_next_conversational_turn", 1500 },
  /* Atomic durable-state replacement fsyncs each state transition. Under
   * sanitizer instrumentation these four multi-phase flows measure
   * 1046-1103ms; keep explicit headroom without relaxing global budgets. */
  { "codex_missing_deliverable_reports_honest_result", 2500 },
  { "manage_codex_project_root_is_explicit_cached_and_contained", 8000 },
  { "claude_handler_delivers_modern_operation_result", 2500 },
  { "concrete_request_queues_managed_work", 2500 },
  { "manual_affair_review_with_note_add_records_note", 1000 },
  { "manual_affair_review_with_close_marks_affair_closed", 1000 },
  /* Two real split-role manager turns prove result rejection followed by
   * review closure; child launches and durable state fsyncs dominate. */
  { "result_manager_close_is_rejected_and_review_can_close", 3000 },
  { "manual_affair_review_status_only_does_not_add_note", 1000 },
  /* Builds a registry-max catalog, captures two mock provider requests,
   * and exercises duplicate/unknown/overflow startup boundaries. */
  { "configured_action_catalog_is_ordered_faithful_growable_and_loud", 5000 },
  { "force_disabled_action_is_masked_rejected_and_still_declared", 5000 },
  { "deterministic_work_controller_binding_and_terminal_controls", 5000 },
  { "deterministic_completion_claims_use_durable_operation_contract", 20000 },
  { "operation_completion_deadline_and_recovery_contract", 20000 },
  { "default_floofclaw_admitted_work_reaches_result_manager_reply", 20000 },
  /* Each drives the shipped floop through one failed subprocess attempt and
   * a later semantic controller turn using hermetic provider responses. */
  { "work_manager_tries_justified_alternative_after_failure", 20000 },
  { "work_manager_blocks_when_no_justified_alternative_remains", 20000 },
  { "work_consequence_routes_intermediate_and_terminal_results", 20000 },
  { "work_controller_keeps_managed_actions_ordinary_and_stale_results_inert", 5000 },
  /* Starts two loopback HTTP fixtures to prove exact media transfer,
   * cache reuse, truncation rejection, and atomic partial cleanup. */
  { "llm_media_downloader_is_bounded_atomic_and_cacheable", 3000 },
  { "work_publication_recovers_once_after_source_append_crash", 15000 },
  { "work_publication_pending_claim_resumes_same_run", 15000 },
  { "publication_outbox_rejects_conflicting_sources_and_enforces_cap", 30000 },
  /* Drive a real gateway and fork a parking subprocess to prove pass
   * semantics; process launch dominates wall time. */
  { "one_pass_floop_resumes_instead_of_rewinding_on_a_parked_subprocess", 5000 },
  { "looping_floop_still_rewinds_on_a_parked_subprocess", 5000 },
  { "generic_calls_large_write_file_payload_executes", 1000 },
  /* Typed ingress: each drives real script agents and a subprocess action
   * through complete runs, and the context test drives five. Process
   * launch and durable-state fsyncs dominate. */
  { "typed_event_gates_replies_and_stays_inspectable", 5000 },
  { "typed_event_adds_no_chat_memory_or_correlation", 3000 },
  { "typed_event_context_namespace_is_the_callers_choice", 8000 },
  { "typed_event_survives_a_restart_before_intake", 3000 },
  { "typed_publication_is_stable_and_conflicts_loudly", 3000 },
  { "typed_ingress_rejects_reserved_and_malformed_publications", 2000 },
  { "ungated_typed_event_completes_as_a_noop_run", 1000 },
  { "legacy_user_message_ingress_is_unchanged", 1000 },
  { "memory_after_appends_only_user_asst_and_compact_skip_is_traced", 1000 },
  /* Builds a script-backed floop and drives its real subprocess through a
   * complete durable run; APFS fsync and process launch dominate wall time
   * (128-1459ms measured under normal/sanitizer/post-chaos load). */
  { "counter_bump_intrinsic_emits_and_reduces_event", 3000 },
  { "counter_bump_is_affair_scoped_without_task_binding", 3000 },
  { "core_web_fetch_terminal_publishes_one_correlated_operation_result", 5000 },
  /* Starts three managed operations and waits for all three correlated
   * result publications. APFS durable writes measured 374-384ms after the
   * task action catalog gained the working-memory sidecar. */
  { "immediate_operations_forward_bounded_result_text", 1000 },
  /* Each drives the shipped OpenClaw profile through multiple durable runs.
   * Mock provider calls and APFS fsyncs dominate their wall time. */
  { "openclaw_same_main_claw_completes_multi_tool_chain", 5000 },
  { "openclaw_action_list_discovers_wildcard_grant", 5000 },
  { "one_pass_run_does_not_complete_input_task", 3000 },
  { "openclaw_progress_and_detached_completion_reach_final_reply", 10000 },
  { "openclaw_long_operation_result_reaches_same_main_claw", 3000 },
  { "openclaw_managed_failure_and_rejection_return_to_main_claw", 5000 },
  { "openclaw_main_claw_owns_affair_review", 3000 },
  /* Creates 12,287 real directory entries to prove the janitor's cap-minus-one
   * scan boundary, then sorts and prunes them to the configured keep count. */
  { "right_sized_capacity_boundaries_are_loud", 5000 },
  { "operation_capacity_boundary_is_loud",      5000 },
  /* Forks two publishers and performs 1,000 lock-protected durable bus
   * writes. ASan plus APFS fsync load has measured 457ms. */
  { "concurrent_bus_publish_mints_unique_envelope_ids", 1000 },
  /* Managed-operation lifecycle fixtures poll real detached fakes on
   * 120-200ms timers; the blocker end-to-end drives two attempts with
   * deliberate worker sleeps. */
  { "opgeneric_fake_one_full_lifecycle",               8000 },
  { "opgeneric_rebind_fake_two_config_only",           8000 },
  { "opgeneric_stale_claim_after_terminal_is_noop",     10000 },
  { "opgeneric_terminal_attempt_never_restarts_itself", 10000 },
  { "opgeneric_retry_reuses_committed_calls",          8000 },
  { "opgeneric_serialize_contexts_orders_runs",        10000 },
  { "opgeneric_non_critical_step_failure_keeps_run_done", 8000 },
  { "floofclaw_background_blocker_end_to_end",       25000 },
  { "floofclaw_manager_retry_no_duplicate_effects",  10000 },
  { "floofclaw_standing_concern_review_lifecycle",   15000 },
  /* Two affair-open turns, one review-dispatched work turn, and a full
   * gateway restart — same class as the lifecycle test above. */
  { "review_dispatched_work_carries_origin_affair_id", 15000 },
  { "floofclaw_review_queues_background_work",       15000 },
};

static int budget_for(const char *name) {
  for (size_t i = 0; i < sizeof(kBudgetOverrides) / sizeof(kBudgetOverrides[0]); ++i)
    if (strcmp(kBudgetOverrides[i].name, name) == 0) return kBudgetOverrides[i].budget_ms;
  return DEFAULT_BUDGET_MS;
}

static const char *group_name(TestGroup group) {
  switch (group) {
    case TEST_GROUP_CONTRACT: return "contract";
    case TEST_GROUP_CONFIG: return "config";
    case TEST_GROUP_EXECUTOR: return "executor";
    case TEST_GROUP_BUS: return "bus";
    case TEST_GROUP_SCHEDULER: return "scheduler";
    case TEST_GROUP_ACTIONS: return "actions";
    case TEST_GROUP_ARTIFACTS: return "artifacts";
    case TEST_GROUP_REPLAY: return "replay";
    case TEST_GROUP_BUDGET: return "budget";
    case TEST_GROUP_ARCHITECTURE: return "architecture";
    case TEST_GROUP_MEMORY: return "memory";
  }
  return "unknown";
}

static int matches_filter(const TestCase *test, const char *filter) {
  if (!filter || !*filter) return 1;
  return strstr(test->name, filter) != NULL || strstr(group_name(test->group), filter) != NULL;
}

/* FCLAW_TEST_SHARD="I/N" → this worker runs tests where (index % N) == I-1.
 * I is 1-based ("1/4" means worker 1 of 4). Used by tests/run_integration_parallel.sh
 * to split the suite across N workers, each in its own per-worker workspace.
 * Returns 0 if the test does not belong to this shard (skip), 1 if it does. */
static int matches_shard(int index, const char *shard) {
  if (!shard || !*shard) return 1;
  int i = 0, n = 0;
  if (sscanf(shard, "%d/%d", &i, &n) != 2) return 1;
  if (n <= 0 || i < 1 || i > n) return 1;
  return (index % n) == (i - 1);
}

static void on_alarm(int sig) {
  (void)sig;
  fprintf(stderr, "\n[FAIL] test timed out\n");
  _exit(124);
}

static int path_is_at_or_below(const char *root, const char *path) {
  size_t root_len;
  if (!root || !*root || !path || !*path) return 0;
  root_len = strlen(root);
  return strcmp(root, path) == 0 ||
         (strncmp(root, path, root_len) == 0 && path[root_len] == '/');
}

static int require_isolated_test_root(const char *program) {
  const char *marker = getenv("FCLAW_TEST_ISOLATED");
  const char *source_root = getenv("FCLAW_TEST_SOURCE_ROOT");
  const char *isolated_root = getenv("FCLAW_TEST_ISOLATED_ROOT");
  char cwd[PATH_MAX];
  struct stat st;
  int valid = marker && strcmp(marker, "1") == 0 &&
              source_root && source_root[0] == '/' &&
              isolated_root && isolated_root[0] == '/' &&
              getcwd(cwd, sizeof(cwd)) != NULL &&
              strcmp(cwd, isolated_root) == 0 &&
              !path_is_at_or_below(source_root, cwd) &&
              lstat(".fclaw-test-isolated", &st) == 0 && S_ISREG(st.st_mode);
  if (!valid) {
    fprintf(stderr,
            "test runner: refusing to run outside an isolated copy; fix: "
            "./tests/run_in_isolated_copy.sh %s [filter]\n",
            program && *program ? program : "./bin/fclaw_integration_tests");
    return -1;
  }
  /* An isolated tree must never carry the operator's secret store. The
   * store resolves from FCLAW_HOME, defaulting to the working directory,
   * so a copied secrets/ would be handed to every action the suite
   * launches -- and an action that reads it reaches the live outside
   * world from a test run. Tests needing a secret build their own store
   * under a scratch FCLAW_HOME. */
  if (lstat("secrets", &st) == 0) {
    fprintf(stderr,
            "test runner: refusing to run with a secrets/ directory in the "
            "isolated copy; real credentials must never reach the suite. "
            "Check the rsync excludes in tests/run_in_isolated_copy.sh\n");
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : NULL;
  const char *shard = getenv("FCLAW_TEST_SHARD");
  int failures = 0;
  int ran = 0;
  int total = (int)(sizeof(tests) / sizeof(tests[0]));
  if (require_isolated_test_root(argc > 0 ? argv[0] : NULL) != 0) return 2;
  signal(SIGALRM, on_alarm);
  for (int i = 0; i < total; ++i) {
    if (!matches_filter(&tests[i], filter)) continue;
    if (!matches_shard(i, shard)) continue;
    ran++;
    printf("[RUN ] [%s] %s\n", group_name(tests[i].group), tests[i].name);
    fflush(stdout);
    /* Hard wall-clock backstop: the per-test budget plus slack. Tests
     * with large overrides (gateway lifecycles with real timers) get a
     * proportionally larger alarm instead of a flat 15s. */
    alarm((unsigned)(budget_for(tests[i].name) / 1000 + 10));
    long long t0 = now_ms();
    int rc = tests[i].fn();
    long long elapsed = now_ms() - t0;
    alarm(0);
    int budget = budget_for(tests[i].name);
    int over_budget = (elapsed > budget);
    if (rc == 0 && !over_budget) {
      printf("[PASS] [%s] %s (%lldms)\n",
             group_name(tests[i].group), tests[i].name, elapsed);
    } else {
      failures++;
      if (over_budget) {
        printf("[FAIL] [%s] %s — %lldms exceeds %dms budget\n",
               group_name(tests[i].group), tests[i].name, elapsed, budget);
        fprintf(stderr,
                "  budget exceeded: add an override in tests/test_runner.c "
                "kBudgetOverrides[] if this is intentional\n");
      } else {
        printf("[FAIL] [%s] %s\n",
               group_name(tests[i].group), tests[i].name);
      }
    }
  }
  if (ran == 0) {
    fprintf(stderr, "no tests matched filter: %s\n", filter ? filter : "");
    return 2;
  }
  (void)test_reset_workspace();
  printf("\n%d test(s), %d failure(s)\n", ran, failures);
  return failures ? 1 : 0;
}
