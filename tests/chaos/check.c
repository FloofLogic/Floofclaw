#include "../../runtime/runtime.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/heap_guard.h"
#include "../../runtime/support/json.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK_MAX_RECORDS 2048

static char g_origins[CHECK_MAX_RECORDS][RT_SMALL];
static char g_deliveries[CHECK_MAX_RECORDS][RT_SMALL];
static size_t g_delivery_count;
static int g_done_count;
static int g_failed_count;
static int g_failures;

/* state.c narrates only through the non-report replay wrapper. The checker
 * calls the report API, but the linker still needs the runtime symbol. */
int rt_narrate(const char *fmt, ...) {
  (void)fmt;
  return 0;
}

static void failf(const char *fmt, ...) {
  va_list ap;
  g_failures++;
  fputs("chaos-check: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static int has_suffix(const char *text, const char *suffix) {
  size_t text_len = strlen(text ? text : "");
  size_t suffix_len = strlen(suffix ? suffix : "");
  return text_len >= suffix_len &&
         strcmp(text + text_len - suffix_len, suffix) == 0;
}

static void check_json_file(const char *path) {
  char *text = NULL;
  JsonRef root;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
    failf("cannot read JSON file %s", path);
    return;
  }
  if (json_ref_top_object(text, &root) != 0)
    failf("state/artifact JSON does not parse: %s", path);
  fc_xfree(text);
}

static void check_json_tree(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  if (!dir) {
    if (errno != ENOENT) failf("cannot open JSON tree %s: %s", path, strerror(errno));
    return;
  }
  while ((entry = readdir(dir)) != NULL) {
    char child[PATH_MAX];
    struct stat st;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name)
        >= (int)sizeof(child)) {
      failf("path too long below %s", path);
      continue;
    }
    if (lstat(child, &st) != 0) {
      failf("cannot stat %s", child);
      continue;
    }
    if (S_ISDIR(st.st_mode)) check_json_tree(child);
    else if (S_ISREG(st.st_mode) && has_suffix(entry->d_name, ".json"))
      check_json_file(child);
  }
  (void)closedir(dir);
}

static int count_regular_files(const char *path) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  int count = 0;
  if (!dir) return errno == ENOENT ? 0 : -1;
  while ((entry = readdir(dir)) != NULL) {
    char child[PATH_MAX];
    struct stat st;
    if (entry->d_name[0] == '.') continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name)
        >= (int)sizeof(child)) continue;
    if (stat(child, &st) == 0 && S_ISREG(st.st_mode)) count++;
  }
  (void)closedir(dir);
  return count;
}

static void check_run(const char *run_name) {
  char path[PATH_MAX];
  char *events = NULL;
  char *runstate = NULL;
  char state[RT_XL];
  RtReplayReport report;
  JsonRef root;
  char status[RT_SMALL] = "";
  if (snprintf(path, sizeof(path), "workspace/runs/%s/event_log.jsonl",
               run_name) >= (int)sizeof(path)) {
    failf("run path too long: %s", run_name);
    return;
  }
  if (fs_read_text(path, &events, FS_READ_TEXT_DEFAULT_CAP) != 0 || !events) {
    failf("missing event log for %s", run_name);
    return;
  }
  if (rt_reduce_event_log_text_report(events, state, sizeof(state), &report) != 0 ||
      json_ref_top_object(state, &root) != 0) {
    failf("replay failed for %s", run_name);
  } else if (report.malformed_lines != 0) {
    failf("replay saw %zu malformed line(s) in %s",
          report.malformed_lines, run_name);
  }
  fc_xfree(events);
  snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json", run_name);
  if (fs_read_text(path, &runstate, FS_READ_TEXT_DEFAULT_CAP) != 0 || !runstate ||
      json_ref_top_object(runstate, &root) != 0 ||
      json_ref_object_get_string(&root, "status", status, sizeof(status)) != 0) {
    failf("runstate does not parse for %s", run_name);
  } else if (strcmp(status, "done") == 0) {
    g_done_count++;
  } else if (strcmp(status, "failed") == 0) {
    g_failed_count++;
  } else {
    failf("run %s is nonterminal (status=%s)", run_name, status);
  }
  fc_xfree(runstate);
}

static int check_runs(void) {
  DIR *dir = opendir("workspace/runs");
  struct dirent *entry;
  int count = 0;
  if (!dir) {
    failf("workspace/runs is missing");
    return 0;
  }
  while ((entry = readdir(dir)) != NULL) {
    char path[PATH_MAX];
    struct stat st;
    int number;
    if (sscanf(entry->d_name, "run_%d", &number) != 1) continue;
    snprintf(path, sizeof(path), "workspace/runs/%s", entry->d_name);
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    count++;
    check_run(entry->d_name);
  }
  (void)closedir(dir);
  return count;
}

static void check_deliveries(int expected) {
  char *text = NULL;
  char *line;
  if (fs_read_text("workspace/logs/deliveries.jsonl", &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
    failf("delivery ledger is missing");
    return;
  }
  line = text;
  while (*line) {
    char *next = strchr(line, '\n');
    JsonRef root, delivery;
    char origin[RT_SMALL] = "";
    char delivery_id[RT_SMALL] = "";
    if (next) *next = '\0';
    if (json_ref_top_object(line, &root) != 0 ||
        json_ref_object_get_object(&root, "delivery", &delivery) != 0 ||
        json_ref_object_get_string(&delivery, "origin_event_id", origin,
                                   sizeof(origin)) != 0 ||
        json_ref_object_get_string(&delivery, "delivery_id", delivery_id,
                                   sizeof(delivery_id)) != 0) {
      failf("malformed delivery ledger line");
    } else if (g_delivery_count >= CHECK_MAX_RECORDS) {
      failf("delivery checker cap exceeded");
    } else {
      for (size_t i = 0; i < g_delivery_count; ++i) {
        if (strcmp(g_origins[i], origin) == 0)
          failf("duplicate delivery for origin %s", origin);
        if (strcmp(g_deliveries[i], delivery_id) == 0)
          failf("duplicate delivery id %s", delivery_id);
      }
      snprintf(g_origins[g_delivery_count], sizeof(g_origins[0]), "%s", origin);
      snprintf(g_deliveries[g_delivery_count], sizeof(g_deliveries[0]), "%s",
               delivery_id);
      g_delivery_count++;
    }
    if (!next) break;
    line = next + 1;
  }
  fc_xfree(text);
  if ((int)g_delivery_count != expected)
    failf("delivery count=%zu expected=%d", g_delivery_count, expected);
}

int main(int argc, char **argv) {
  int expected;
  int expected_deliveries;
  int expected_failed;
  int runs;
  int processed;
  int inbox;
  if ((argc != 2 && argc != 4) || (expected = atoi(argv[1])) < 1) {
    fprintf(stderr,
            "usage: fclaw_chaos_check <expected-events> "
            "[expected-deliveries expected-failed]\n");
    return 2;
  }
  expected_deliveries = argc == 4 ? atoi(argv[2]) : expected;
  expected_failed = argc == 4 ? atoi(argv[3]) : 0;
  if (expected_deliveries < 0 || expected_deliveries > expected ||
      expected_failed < 0 || expected_failed > expected) {
    fprintf(stderr, "chaos-check: invalid expected counts\n");
    return 2;
  }
  if (expected > CHECK_MAX_RECORDS) {
    fprintf(stderr, "chaos-check: expected count exceeds checker cap\n");
    return 2;
  }
  check_json_tree("workspace/runs");
  check_json_tree("workspace/memory/state");
  runs = check_runs();
  processed = count_regular_files("workspace/bus/processed");
  inbox = count_regular_files("workspace/bus/inbox");
  check_deliveries(expected_deliveries);
  if (runs != expected) failf("run count=%d expected=%d", runs, expected);
  if (processed != expected)
    failf("processed envelope count=%d expected=%d", processed, expected);
  if (inbox != 0) failf("inbox is not empty (count=%d)", inbox);
  if (g_failed_count != expected_failed)
    failf("failed run count=%d expected=%d", g_failed_count, expected_failed);
  if (g_done_count != expected - expected_failed)
    failf("done run count=%d expected=%d", g_done_count,
          expected - expected_failed);
  if (g_failures) {
    fprintf(stderr, "chaos-check: FAILED (%d finding%s)\n", g_failures,
            g_failures == 1 ? "" : "s");
    return 1;
  }
  printf("chaos-check: ok events=%d runs=%d done=%d failed=%d deliveries=%zu\n",
         expected, runs, g_done_count, g_failed_count, g_delivery_count);
  return 0;
}
