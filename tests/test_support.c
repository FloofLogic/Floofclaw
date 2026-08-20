#include "test_support.h"
#include "../runtime/support/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int expect(int cond, const char *msg) {
  if (!cond) fprintf(stderr, "EXPECT FAILED: %s\n", msg ? msg : "(no message)");
  return cond ? 0 : -1;
}

int expect_not(int cond, const char *msg) { return expect(!cond, msg); }

int expect_substr(const char *haystack, const char *needle, const char *msg) {
  if (!haystack || !needle || !strstr(haystack, needle)) {
    fprintf(stderr, "EXPECT FAILED: %s\nneedle: %s\n", msg ? msg : "substring missing", needle ? needle : "(null)");
    if (haystack) fprintf(stderr, "haystack:\n%.2000s\n", haystack);
    return -1;
  }
  return 0;
}

int expect_no_substr(const char *haystack, const char *needle, const char *msg) {
  if (haystack && needle && strstr(haystack, needle)) {
    fprintf(stderr, "EXPECT FAILED: %s\nunwanted: %s\n", msg ? msg : "substring present", needle);
    return -1;
  }
  return 0;
}

int expect_file_exists(const char *path) {
  struct stat st;
  return expect(path && stat(path, &st) == 0, path ? path : "file exists");
}

int expect_file_not_exists(const char *path) {
  struct stat st;
  return expect(path && stat(path, &st) != 0, path ? path : "file absent");
}

/* test_capture_command and test_run_fclaw used to popen the shell;
 * both are gone. Tests that need to drive the runtime use the harness
 * (tests/harness.h). Tests that need a CLI subcommand call
 * harness_cli_view / harness_cli_cacheview / harness_cli_tools etc. */

/* Recursive rm -rf, native. ENOENT is success. */
int test_remove_path(const char *path) {
  if (!path) return -1;
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d))) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (test_remove_path(child) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -1;
  }
  return unlink(path) == 0 ? 0 : -1;
}

/* mkdir -p, native. */
int test_mkdir_p(const char *path) {
  if (!path || !*path) return -1;
  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *p = buf + 1; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

int test_write_file(const char *path, const char *text) {
  FILE *f;
  char tmp[PATH_MAX];
  const char *slash;
  if (!path) return -1;
  snprintf(tmp, sizeof(tmp), "%s", path);
  slash = strrchr(tmp, '/');
  if (slash) {
    tmp[slash - tmp] = '\0';
    if (test_mkdir_p(tmp) != 0) return -1;
  }
  f = fopen(path, "w");
  if (!f) return -1;
  if (text && fputs(text, f) < 0) { fclose(f); return -1; }
  return fclose(f) == 0 ? 0 : -1;
}

int test_read_file(const char *path, char **out) {
  FILE *f;
  long n;
  char *buf;
  if (out) *out = NULL;
  if (!path || !out) return -1;
  f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  n = ftell(f);
  if (n < 0) { fclose(f); return -1; }
  rewind(f);
  buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return -1; }
  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return -1; }
  buf[n] = '\0';
  fclose(f);
  *out = buf;
  return 0;
}

static int agent_output_name_matches(const char *name, const char *phase,
                                     const char *suffix) {
  char exact[PATH_MAX];
  size_t n, suffix_len, phase_len;
  if (!name || !phase || !suffix) return 0;
  snprintf(exact, sizeof(exact), "%s%s", phase, suffix);
  if (strcmp(name, exact) == 0) return 1;
  n = strlen(name);
  suffix_len = strlen(suffix);
  phase_len = strlen(phase);
  if (n != 4 + phase_len + suffix_len) return 0;
  if (!isdigit((unsigned char)name[0]) ||
      !isdigit((unsigned char)name[1]) ||
      !isdigit((unsigned char)name[2]) ||
      name[3] != '_') return 0;
  return strncmp(name + 4, phase, phase_len) == 0 &&
         strcmp(name + 4 + phase_len, suffix) == 0;
}

int test_agent_output_path(const char *run_id, const char *phase,
                           const char *suffix, char *out, size_t out_len) {
  char dir[PATH_MAX];
  DIR *d;
  struct dirent *ent;
  char best[PATH_MAX] = "";
  if (!run_id || !phase || !suffix || !out || out_len == 0) return -1;
  snprintf(dir, sizeof(dir), "workspace/runs/%s/agent_outputs", run_id);
  d = opendir(dir);
  if (!d) return -1;
  while ((ent = readdir(d)) != NULL) {
    if (!agent_output_name_matches(ent->d_name, phase, suffix)) continue;
    if (!best[0] || strcmp(ent->d_name, best) > 0) snprintf(best, sizeof(best), "%s", ent->d_name);
  }
  closedir(d);
  if (!best[0]) return -1;
  if (snprintf(out, out_len, "%s/%s", dir, best) >= (int)out_len) return -1;
  return 0;
}

int test_read_agent_output(const char *run_id, const char *phase,
                           const char *suffix, char **out) {
  char path[PATH_MAX];
  if (test_agent_output_path(run_id, phase, suffix, path, sizeof(path)) != 0) return -1;
  return test_read_file(path, out);
}

int test_agent_output_contains(const char *run_id, const char *phase,
                               const char *suffix, const char *needle) {
  char *text = NULL;
  int ok;
  if (test_read_agent_output(run_id, phase, suffix, &text) != 0) return -1;
  ok = text && needle && strstr(text, needle);
  free(text);
  return ok ? 0 : -1;
}

int expect_agent_output_exists(const char *run_id, const char *phase,
                               const char *suffix) {
  char path[PATH_MAX];
  return expect(test_agent_output_path(run_id, phase, suffix, path, sizeof(path)) == 0,
                phase ? phase : "agent output exists");
}

int expect_agent_output_not_exists(const char *run_id, const char *phase,
                                   const char *suffix) {
  char path[PATH_MAX];
  return expect(test_agent_output_path(run_id, phase, suffix, path, sizeof(path)) != 0,
                phase ? phase : "agent output absent");
}

long test_file_size(const char *path) {
  struct stat st;
  if (!path || stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

int test_stat_mtime_size(const char *path, long *mtime_out, long *size_out) {
  struct stat st;
  if (!path || stat(path, &st) != 0) return -1;
  if (mtime_out) *mtime_out = (long)st.st_mtime;
  if (size_out) *size_out = (long)st.st_size;
  return 0;
}

int test_file_contains(const char *path, const char *needle) {
  char *text = NULL;
  int ok;
  if (test_read_file(path, &text) != 0) return -1;
  ok = text && needle && strstr(text, needle);
  free(text);
  return ok ? 0 : -1;
}

/* Copy regular file src -> dst, preserving mode bits. */
static int copy_file(const char *src, const char *dst, mode_t mode) {
  FILE *in = fopen(src, "rb");
  if (!in) return -1;
  FILE *out = fopen(dst, "wb");
  if (!out) { fclose(in); return -1; }
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
  }
  fclose(in); fclose(out);
  if (chmod(dst, mode) != 0) return -1;
  return 0;
}

/* Strip last path component into out. "agents/foo/bar" -> "agents/foo". */
static int dirname_of(const char *path, char *out, size_t out_len) {
  const char *slash = strrchr(path, '/');
  if (!slash) { snprintf(out, out_len, "."); return 0; }
  size_t n = (size_t)(slash - path);
  if (n + 1 >= out_len) return -1;
  memcpy(out, path, n);
  out[n] = 0;
  return 0;
}

/* Copy directory src -> dst recursively. Files keep their mode (so
 * run.sh stays executable). */
int test_copy_fixture_dir(const char *src, const char *dst) {
  if (!src || !dst) return -1;
  if (test_remove_path(dst) != 0) return -1;
  char parent[PATH_MAX];
  if (dirname_of(dst, parent, sizeof(parent)) != 0) return -1;
  if (test_mkdir_p(parent) != 0) return -1;
  if (mkdir(dst, 0755) != 0) return -1;
  DIR *d = opendir(src);
  if (!d) return -1;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char child_src[PATH_MAX], child_dst[PATH_MAX];
    snprintf(child_src, sizeof(child_src), "%s/%s", src, ent->d_name);
    snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, ent->d_name);
    struct stat st;
    if (lstat(child_src, &st) != 0) { closedir(d); return -1; }
    if (S_ISDIR(st.st_mode)) {
      if (test_copy_fixture_dir(child_src, child_dst) != 0) { closedir(d); return -1; }
    } else if (S_ISREG(st.st_mode)) {
      mode_t mode = st.st_mode;
      /* The original cp -R + find -exec chmod +x ensured run.sh was
       * executable even when the source bit was missing in git. */
      if (strcmp(ent->d_name, "run.sh") == 0) mode |= 0111;
      if (copy_file(child_src, child_dst, mode) != 0) { closedir(d); return -1; }
    }
  }
  closedir(d);
  return 0;
}

int test_copy_fixture_file(const char *src, const char *dst) {
  if (!src || !dst) return -1;
  char parent[PATH_MAX];
  if (dirname_of(dst, parent, sizeof(parent)) != 0) return -1;
  if (test_mkdir_p(parent) != 0) return -1;
  struct stat st;
  if (stat(src, &st) != 0) return -1;
  return copy_file(src, dst, st.st_mode);
}

int test_cleanup_fixtures(void) {
  int rc = 0;
  rc |= test_remove_path("agents/tests_phase_a");
  rc |= test_remove_path("agents/tests_phase_b");
  rc |= test_remove_path("agents/tests_script_echo");
  rc |= test_remove_path("agents/tests_script_missing");
  rc |= test_remove_path("agents/tests_unknown_executor");
  rc |= test_remove_path("agents/tests_unknown_action_agent");
  rc |= test_remove_path("agents/tests_non_object_args_agent");
  rc |= test_remove_path("agents/tests_duplicate_equiv_agent");
  rc |= test_remove_path("agents/tests_provider_connection_fail");
  rc |= test_remove_path("agents/tests_action_contract_agent");
  rc |= test_remove_path("agents/tests_escape_agent");
  rc |= test_remove_path("agents/tests_side_effect_agent");
  rc |= test_remove_path("agents/tests_memory_emit");
  rc |= test_remove_path("agents/tests_memory_unauthorised");
  rc |= test_remove_path("agents/tests_memory_echo");
  rc |= test_remove_path("agents/memory");
  rc |= test_remove_path("agents/noop_agent");
  rc |= test_remove_path("agents/tests_llm_message");
  rc |= test_remove_path("agents/tests_bad_prompt_var");
  rc |= test_remove_path("agents/tests_bad_authority_agent");
  rc |= test_remove_path("agents/tests_multi_tool_agent");
  rc |= test_remove_path("agents/tests_multi_conversation_agent");
  rc |= test_remove_path("agents/tests_empty_agent");
  rc |= test_remove_path("agents/tests_large_result_agent");
  rc |= test_remove_path("agents/tests_task_agent");
  rc |= test_remove_path("agents/tests_task_planner");
  rc |= test_remove_path("agents/tests_task_speaker");
  rc |= test_remove_path("agents/tests_task_worker");
  rc |= test_remove_path("agents/tests_task_reviewer");
  rc |= test_remove_path("agents/tests_stuck_planner");
  rc |= test_remove_path("agents/tests_stuck_speaker");
  rc |= test_remove_path("agents/tests_stuck_worker");
  rc |= test_remove_path("agents/tests_dup_planner");
  rc |= test_remove_path("agents/tests_dup_worker");
  rc |= test_remove_path("actions/core/tests_echo_action");
  rc |= test_remove_path("actions/core/tests_large_result_action");
  rc |= test_remove_path("loops/tests_phase_order_loop.json");
  rc |= test_remove_path("loops/tests_unknown_phase_loop.json");
  rc |= test_remove_path("loops/tests_unknown_agent_loop.json");
  rc |= test_remove_path("loops/tests_script_echo_loop.json");
  rc |= test_remove_path("loops/tests_script_missing_loop.json");
  rc |= test_remove_path("loops/tests_unknown_executor_loop.json");
  rc |= test_remove_path("loops/tests_unknown_action_loop.json");
  rc |= test_remove_path("loops/tests_selected_write_loop.json");
  rc |= test_remove_path("loops/tests_non_object_args_loop.json");
  rc |= test_remove_path("loops/tests_duplicate_equiv_loop.json");
  rc |= test_remove_path("loops/tests_provider_connection_fail_loop.json");
  rc |= test_remove_path("loops/tests_action_contract_loop.json");
  rc |= test_remove_path("loops/tests_escape_loop.json");
  rc |= test_remove_path("loops/tests_side_effect_loop.json");
  rc |= test_remove_path("loops/tests_memory_emit_loop.json");
  rc |= test_remove_path("loops/tests_memory_unauthorised_loop.json");
  rc |= test_remove_path("loops/tests_memory_recall_echo_loop.json");
  rc |= test_remove_path("loops/tests_memory_default_loop.json");
  rc |= test_remove_path("loops/tests_llm_message_loop.json");
  rc |= test_remove_path("loops/tests_bad_prompt_var_loop.json");
  rc |= test_remove_path("loops/tests_bad_authority_loop.json");
  rc |= test_remove_path("loops/tests_multi_advance_loop.json");
  rc |= test_remove_path("loops/tests_empty_advance_loop.json");
  rc |= test_remove_path("loops/tests_large_result_loop.json");
  rc |= test_remove_path("loops/tests_task_contract_loop.json");
  rc |= test_remove_path("loops/tests_agent_only_work_loop.json");
  rc |= test_remove_path("loops/tests_agent_only_endless_loop.json");
  rc |= test_remove_path("loops/tests_duplicate_rejection_loop.json");
  return rc;
}

static int test_reset_workspace_once(void);

/* The checkout ships config/floofclaw_config.json with the setup-needing
 * common actions in `force_disable` — deployment truth, not test truth. A
 * test that renders or dispatches one of those actions swaps in a neutral
 * config with an empty mask, and puts the shipped file back before
 * returning because every later test in the process reads the same path. */
int test_config_enable_all(char **saved) {
  if (saved) {
    *saved = NULL;
    (void)test_read_file("config/floofclaw_config.json", saved);
  }
  return test_write_file("config/floofclaw_config.json",
                         "{\"bot_name\":\"FloofClaw\","
                         "\"default_floop\":\"hello\","
                         "\"force_disable\":[]}\n");
}

void test_config_restore(char *saved) {
  if (!saved) return;
  (void)test_write_file("config/floofclaw_config.json", saved);
  free(saved);
}

int test_reset_workspace(void) {
  /* Skip the gateway-stop probe — integration tests run in-process
   * and don't start a daemon. If a stale gateway from an earlier
   * dev session is running, the embedded owner lock will detect it
   * and channel_synchronous will route to the daemon instead.
   *
   * Bounded retry: a prior test's detached worker runner (session-
   * detached by design) may still be writing state or a completion
   * claim while this reset deletes the tree, turning one rm -rf into a
   * transient ENOTEMPTY. Retrying for up to ~2s absorbs the straggler
   * instead of failing the NEXT test silently. */
  int rc = -1;
  for (int attempt = 0; attempt < 20 && rc != 0; ++attempt) {
    if (attempt > 0) usleep(100000);
    rc = test_reset_workspace_once();
  }
  return rc;
}

static int test_reset_workspace_once(void) {
  int rc = 0;
  rc |= test_cleanup_fixtures();
  rc |= test_remove_path("workspace/runs");
  rc |= test_remove_path("workspace/bus");
  rc |= test_remove_path("workspace/logs");
  rc |= test_remove_path("workspace/memory");
  rc |= test_remove_path("workspace/media");
  rc |= test_remove_path("workspace/test_side_effect.txt");
  rc |= test_remove_path("workspace/tests_action.txt");
  rc |= test_remove_path("workspace/dup.txt");
  rc |= test_remove_path("workspace/agent_only.txt");
  rc |= test_remove_path("workspace/fetched.txt");
  rc |= test_remove_path("workspace/multi_done.txt");
  rc |= test_remove_path("workspace/escape.txt");
  rc |= test_remove_path("escape.txt");
  rc |= test_remove_path(".fclaw/run");
  rc |= test_mkdir_p("workspace");
  return rc;
}

int test_install_agent_fixture(const char *id) {
  char src[PATH_MAX], dst[PATH_MAX];
  snprintf(src, sizeof(src), "tests/fixtures/agents/%s", id);
  snprintf(dst, sizeof(dst), "agents/%s", id);
  return test_copy_fixture_dir(src, dst);
}

int test_install_action_fixture(const char *id) {
  char src[PATH_MAX], dst[PATH_MAX];
  snprintf(src, sizeof(src), "tests/fixtures/actions/%s", id);
  snprintf(dst, sizeof(dst), "actions/core/%s", id);
  return test_copy_fixture_dir(src, dst);
}

int test_install_loop_fixture(const char *file_name) {
  char src[PATH_MAX], dst[PATH_MAX];
  snprintf(src, sizeof(src), "tests/fixtures/loops/%s", file_name);
  snprintf(dst, sizeof(dst), "loops/%s", file_name);
  return test_copy_fixture_file(src, dst);
}

int test_extract_run_id(const char *text, char *out, size_t out_len) {
  JsonRef root;
  if (!text || !out || out_len == 0) return -1;
  return json_ref_top_object(text, &root) == 0
             ? json_ref_object_get_string(&root, "run_id", out, out_len)
             : -1;
}

/* test_run_with_text and test_gateway_fast_loop are defined in
 * tests/integration_support.c — they call the harness which links the
 * runtime, and the unit-tests binary does not link those. */

int test_latest_run_id(char *out, size_t out_len) {
  DIR *d = opendir("workspace/runs");
  int max_n = -1;
  struct dirent *e;
  if (!d || !out || out_len == 0) { if (d) closedir(d); return -1; }
  while ((e = readdir(d))) {
    int n = -1;
    if (sscanf(e->d_name, "run_%d", &n) == 1 && n > max_n) max_n = n;
  }
  closedir(d);
  if (max_n < 0) return -1;
  snprintf(out, out_len, "run_%03d", max_n);
  return 0;
}

int test_event_log_path(const char *run_id, char *out, size_t out_len) {
  if (!run_id || !out || out_len == 0) return -1;
  snprintf(out, out_len, "workspace/runs/%s/event_log.jsonl", run_id);
  return 0;
}

int test_read_run_event_log(const char *run_id, char **out) {
  char path[PATH_MAX];
  if (test_event_log_path(run_id, path, sizeof(path)) != 0) return -1;
  return test_read_file(path, out);
}

int test_count_substr(const char *text, const char *needle) {
  int count = 0;
  size_t n;
  const char *p = text;
  if (!text || !needle || !*needle) return 0;
  n = strlen(needle);
  while ((p = strstr(p, needle))) { count++; p += n; }
  return count;
}

int test_count_event_type(const char *run_id, const char *type) {
  char *text = NULL;
  char needle[128];
  int count;
  if (test_read_run_event_log(run_id, &text) != 0) return -1;
  snprintf(needle, sizeof(needle), "\"type\":\"%s\"", type);
  count = test_count_substr(text, needle);
  free(text);
  return count;
}
