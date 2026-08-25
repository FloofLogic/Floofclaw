#include "janitor_module.h"

#include "gateway.h"    /* GW_LOG_PATH + gateway_is_daemonized_stdout() */
#include "../publication_outbox.h"
#include "../run_state.h"
#include "../runtime.h"
#include "../support/fsutil.h"
#include "../support/json.h"
#include "../support/log_rotation.h"
#include "../support/scratch_guard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Janitor tick — the module wakes up every JANITOR_TICK_MS to check
 * whether any of its tasks are due. Task cadences are configured
 * per-task; the module tick is just the "check the schedule" heartbeat
 * and can be much coarser than any single task's interval since each
 * task carries its own next_run_ms. 30s is fine — nothing here is
 * time-critical, most tasks run every ~5 minutes or hourly. */
#define JANITOR_TICK_MS   30000ULL

/* Safe defaults for every task. Overridden by config/floofclaw_config.json
 * under the "appliance" block. Keep them conservative — a real appliance
 * install with no config still ends up bounded. */
#define DEFAULT_RUNS_KEEP                 500
#define DEFAULT_RUNS_INTERVAL_MS          300000ULL   /* 5 min  */
#define DEFAULT_WORKER_STORE_KEEP         200
#define DEFAULT_WORKER_STORE_INTERVAL_MS  300000ULL
#define DEFAULT_BUS_PROCESSED_KEEP        10000
#define DEFAULT_BUS_PROCESSED_INTERVAL_MS 300000ULL
#define DEFAULT_TASKS_ARCHIVE_KEEP_DAYS   90
#define DEFAULT_TASKS_ARCHIVE_INTERVAL_MS 3600000ULL  /* 1 hour */

/* JSONL log rotation defaults. Cadence is a single interval for the
 * whole rotator (all targets checked in the same tick — cheap: stat
 * per target). Per-target settings live in config. */
#define DEFAULT_LOGS_INTERVAL_MS          60000ULL    /* 1 min */
#define DEFAULT_LOGS_MAX_BYTES            67108864ULL /* 64 MB */
#define DEFAULT_LOGS_KEEP                 5
#define DEFAULT_LOGS_COMPRESS             "gzip"      /* "gzip" or "none" */

typedef struct JanitorTask JanitorTask;

/* Each task carries its own cadence. next_run_ms = 0 makes it fire on
 * the first tick after startup — good for surfacing configuration
 * problems immediately, bad for busy startup. We compromise: seed
 * next_run_ms with a small stagger per task at init so they don't all
 * fire in the same tick. */
struct JanitorTask {
  const char *name;
  int         enabled;
  uint64_t    interval_ms;
  uint64_t    next_run_ms;
  int       (*run)(JanitorTask *t);
  /* per-task fields */
  char        path[256];
  int         keep;
  int         keep_days;
  /* Managed-worker stores: the entry-name prefix to strip before
   * "op_" reconstructs the runtime-owned operation id. Empty means
   * the entry name is already the operation id. */
  char        op_prefix[16];
  /* for JSONL rotation tasks */
  uint64_t    max_bytes;
  int         compress;   /* 1 = gzip subprocess, 0 = none */
  /* If set, after successful rotation reopen fd 1 and fd 2 to the
   * freshly-created path via dup2. Used exclusively for gateway.log
   * because the daemon holds fd 1/2 pointing at it — a plain rename
   * would leave subsequent daemon writes vanishing into the moved
   * file. Guarded by gateway_is_daemonized_stdout() at runtime so
   * foreground `gateway run` (fd 1 = terminal) isn't clobbered. */
  int         reopen_stdio;
};

typedef struct {
  uint64_t     next_tick_ms;
  JanitorTask  tasks[20];  /* room to grow — janitor tasks are cheap */
  size_t       task_count;
} JanitorState;

/* ------------------------------------------------------------------
 * Config reading — one shot at init. Returns 0 on success, non-zero
 * doesn't fail the module (we just use defaults).
 * ------------------------------------------------------------------ */

static int read_appliance_block(char **out_text, JsonRef *out_appliance) {
  char *text = NULL;
  JsonRef root;
  if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  if (json_ref_first_object(text, &root) != 0) { free(text); return -1; }
  if (json_ref_object_get_object(&root, "appliance", out_appliance) != 0) {
    free(text); return -1;
  }
  *out_text = text;  /* caller frees */
  return 0;
}

static int cfg_task_int(const JsonRef *appliance, const char *task,
                        const char *key, long long *out) {
  JsonRef sub;
  if (json_ref_object_get_object(appliance, task, &sub) != 0) return -1;
  return json_ref_object_get_long(&sub, key, out);
}

/* ------------------------------------------------------------------
 * Helpers — recursive delete, listing, etc. Kept small; if a task
 * needs something more elaborate it can grow its own helper below.
 * ------------------------------------------------------------------ */

/* Recursive rmdir via `rm -rf` fork/exec. Runs synchronously but a
 * single run directory is small (~130 KB), so this is fine at 5-min
 * cadence. If perf ever bites, swap to non-blocking posix_spawn. */
static int rm_rf(const char *path) {
  pid_t pid;
  int status;
  if (!path || !*path) return -1;
  /* Extra paranoia: never accept a path with "..". The janitor should
   * only ever be handed workspace-relative paths from config; a config
   * accident is still cheap to guard against. */
  if (strstr(path, "..")) return -1;
  pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execlp("rm", "rm", "-rf", "--", path, (char *)NULL);
    _exit(127);
  }
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Extract the trailing numeric suffix from a directory name like
 * "run_00123" or "bus_000045.json". Returns -1 if no digits. */
static long long parse_trailing_number(const char *name) {
  size_t len = strlen(name);
  size_t i = len;
  size_t start;
  long long out = 0;
  int found = 0;
  if (len > 5 && strcmp(name + len - 5, ".json") == 0) {
    len -= 5;
    i = len;
  }
  while (i > 0 && name[i-1] >= '0' && name[i-1] <= '9') { --i; found = 1; }
  if (!found) return -1;
  start = i;
  /* skip leading zeros implicitly by parsing */
  for (; start < len; ++start) {
    out = out * 10 + (name[start] - '0');
    if (out > 100000000LL) return -1;  /* overflow guard */
  }
  return out;
}

typedef struct { long long num; char name[256]; } DirEntry;

#define JANITOR_RUNS_SCAN_CAP      2048
#define JANITOR_WORKER_STORE_SCAN_CAP 4096
#define JANITOR_BUS_SCAN_CAP      12288

typedef struct {
  DirEntry entries[JANITOR_BUS_SCAN_CAP];
} JanitorScratch;

static JanitorScratch g_janitor_scratch;
static FcScratchGuard g_janitor_scratch_guard =
    FC_SCRATCH_GUARD_INIT("janitor");

static DirEntry *janitor_entries_borrow(void) {
  if (fc_scratch_guard_acquire(&g_janitor_scratch_guard) != 0) return NULL;
  return g_janitor_scratch.entries;
}

static int janitor_entries_release_result(int result) {
  fc_scratch_guard_release(&g_janitor_scratch_guard);
  return result;
}

int fc_janitor_test_scratch_reentry_guard(void) {
  DirEntry *outer = janitor_entries_borrow();
  DirEntry *nested;
  int result = -1;
  if (!outer) return -1;
  nested = janitor_entries_borrow();
  if (!nested) result = 0;
  return janitor_entries_release_result(result);
}

static int by_num_asc(const void *a, const void *b) {
  long long x = ((const DirEntry *)a)->num;
  long long y = ((const DirEntry *)b)->num;
  return (x > y) - (x < y);
}

/* Max-heap on the trailing number, used to hold the `cap` OLDEST entries
 * while the scan runs. Filling in readdir order and truncating whatever
 * arrives last lets the filesystem decide which runs survive a cap
 * overflow; every caller here prunes from the old end, so overflow must
 * drop the NEWEST. Selecting with a heap keeps that O(log cap) per entry
 * — a sorted-insert would memmove up to 12288 entries per overflow. */
static void heap_swap(DirEntry *a, DirEntry *b) {
  DirEntry tmp = *a;
  *a = *b;
  *b = tmp;
}

static void heap_sift_up(DirEntry *heap, int index) {
  while (index > 0) {
    int parent = (index - 1) / 2;
    if (heap[parent].num >= heap[index].num) return;
    heap_swap(&heap[parent], &heap[index]);
    index = parent;
  }
}

static void heap_sift_down(DirEntry *heap, int n, int index) {
  for (;;) {
    int left = 2 * index + 1;
    int right = left + 1;
    int largest = index;
    if (left < n && heap[left].num > heap[largest].num) largest = left;
    if (right < n && heap[right].num > heap[largest].num) largest = right;
    if (largest == index) return;
    heap_swap(&heap[index], &heap[largest]);
    index = largest;
  }
}

/* List every entry in dir matching prefix, extract trailing number,
 * sort ascending. Returns count or -1 on error. Caller supplies fixed
 * capacity; overflow drops the newest entries, because every caller
 * deletes from the oldest end. */
static int list_sorted(const char *dir, const char *prefix,
                       DirEntry *out, int cap) {
  DIR *d = opendir(dir);
  struct dirent *e;
  int n = 0;
  int saved_errno;
  if (!d) {
    if (errno == ENOENT) return 0;
    fprintf(stderr, "janitor: opendir failed for %s: %s\n",
            dir, strerror(errno));
    return -1;
  }
  errno = 0;
  while ((e = fs_readdir_checked(d)) != NULL) {
    long long num;
    if (e->d_name[0] == '.') continue;
    if (prefix && *prefix && strncmp(e->d_name, prefix, strlen(prefix)) != 0)
      continue;
    num = parse_trailing_number(e->d_name);
    if (num < 0) continue;
    if (n < cap) {
      out[n].num = num;
      snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
      heap_sift_up(out, n);
      n++;
    } else if (cap > 0 && num < out[0].num) {
      /* Newest kept entry loses its slot to this older one. */
      out[0].num = num;
      snprintf(out[0].name, sizeof(out[0].name), "%s", e->d_name);
      heap_sift_down(out, n, 0);
    }
  }
  saved_errno = errno;
  if (closedir(d) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    fprintf(stderr, "janitor: readdir failed for %s: %s\n",
            dir, strerror(saved_errno));
    errno = saved_errno;
    return -1;
  }
  qsort(out, (size_t)n, sizeof(*out), by_num_asc);
  return n;
}

/* Is workspace/runs/<name>/runstate.json marked terminal (done/failed)?
 * If we can't read the file, treat as non-terminal — safer to keep
 * than to nuke an in-flight run.
 *
 * The status is read through the runtime's own runstate reader, not by
 * naming the key here. This module spelled it "state" while the
 * serializer wrote "status", so every run looked non-terminal and nothing
 * was ever pruned — silently, because "can't read → keep" is also the
 * fail-safe. One owner for the key makes that drift impossible. */
static int run_is_terminal(const char *runs_dir, const char *name) {
  char path[512];
  char *text = NULL;
  char status[32] = "";
  int terminal;
  if (snprintf(path, sizeof(path), "%s/%s/runstate.json", runs_dir, name)
      >= (int)sizeof(path))
    return 0;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  (void)rt_run_state_status_from_json(text, status, sizeof(status));
  free(text);
  terminal = rt_run_status_is_terminal(status);
  return terminal;
}

/* ------------------------------------------------------------------
 * Task: log_rotate — rotate append-only JSONL logs when they exceed
 * a size threshold. Each configured target has its own path, cap,
 * keep count, and compression choice.
 *
 * Rotation ladder:
 *   bus.jsonl.<keep-1>.gz  →  bus.jsonl.<keep>.gz   (delete if past keep)
 *   ...
 *   bus.jsonl.1{,.gz}      →  bus.jsonl.2{,.gz}
 *   bus.jsonl              →  bus.jsonl.1          (live becomes newest)
 *
 * Compression happens in a non-blocking fork+exec `gzip` on the
 * freshly-rotated .1. If gzip isn't on PATH we log a narration
 * warning and keep .1 uncompressed — no crash.
 *
 * File shift handles both `.N` and `.N.gz` at each slot: whichever
 * exists gets renamed one slot older. That keeps the code simple
 * across cold-restart edge cases where a previous run left a
 * mix of compressed and uncompressed archives.
 *
 * Adapters that tail these files (deliveries.jsonl cursor in
 * discord/irc; narration.jsonl cursor in irc ops channel)
 * detect rotation by "current file size < remembered cursor" and
 * reset the cursor to 0 of the new file. See channel adapter
 * changes in the same commit.
 * ------------------------------------------------------------------ */

static int task_log_rotate(JanitorTask *t) {
  struct stat st;
  char err[128];
  if (!t->enabled || t->max_bytes == 0 || t->keep <= 0) return 0;
  if (stat(t->path, &st) != 0) return 0;
  if ((uint64_t)st.st_size < t->max_bytes) return 0;
  if (fc_rotate_file(t->path, t->keep, t->compress, err, sizeof(err)) != 0) {
    (void)rt_narrate("janitor: rotate failed for %s — %s", t->path, err);
    return -1;
  }
  /* fc_rotate_file already renamed live to .1 and (if compress) spawned
   * gzip. For gateway.log-shaped tasks, retarget the daemon's fd 1/2 to
   * the freshly-created path so subsequent daemon writes land there and
   * not in the now-moved .1. Skip in foreground mode — fd 1 is the
   * terminal there. */
  if (t->reopen_stdio && gateway_is_daemonized_stdout()) {
    int nfd = open(t->path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (nfd >= 0) {
      (void)dup2(nfd, 1);
      (void)dup2(nfd, 2);
      close(nfd);
    }
  }
  (void)rt_narrate("janitor: rotated %s (was %lld bytes, keep=%d, compress=%s%s)",
                   t->path, (long long)st.st_size, t->keep,
                   t->compress ? "gzip" : "none",
                   t->reopen_stdio ? ", stdio reopened" : "");
  return 0;
}

/* ------------------------------------------------------------------
 * Task: runs_prune — workspace/runs/run_NNN/
 * ------------------------------------------------------------------ */

static int task_runs_prune(JanitorTask *t) {
  DirEntry *entries;
  int n, keep, to_drop, dropped = 0;
  int i;
  int result = -1;
  if (!t->enabled || t->keep <= 0) return 0;
  entries = janitor_entries_borrow();
  if (!entries) return -1;
  if ((n = list_sorted(t->path, "run_", entries,
                       JANITOR_RUNS_SCAN_CAP)) < 0)
    goto cleanup;
  keep = t->keep;
  if (n <= keep) { result = 0; goto cleanup; }
  to_drop = n - keep;
  /* Only drop terminals; walk the oldest end and skip anything still
   * running. A run that never terminates (crash, hang) accumulates,
   * but that's the correct forensic outcome. */
  for (i = 0; i < n && dropped < to_drop; ++i) {
    char full[512];
    int pinned = rt_publication_outbox_run_pending(entries[i].name);
    /* An unreadable publication store is not permission to discard the
     * source truth. Keep the run and let reconciliation narrate the fault. */
    if (pinned != 0) continue;
    if (!run_is_terminal(t->path, entries[i].name)) continue;
    if (snprintf(full, sizeof(full), "%s/%s", t->path, entries[i].name)
        >= (int)sizeof(full)) continue;
    if (rm_rf(full) == 0) dropped++;
  }
  if (dropped > 0)
    (void)rt_narrate("janitor: pruned %d runs from %s (kept %d)",
                     dropped, t->path, keep);
  result = 0;

cleanup:
  return janitor_entries_release_result(result);
}

int fc_janitor_test_runs_prune(const char *path, int keep) {
  JanitorTask task;
  if (!path || !*path || keep <= 0) return -1;
  memset(&task, 0, sizeof(task));
  task.name = "runs_prune_test";
  task.enabled = 1;
  task.keep = keep;
  snprintf(task.path, sizeof(task.path), "%s", path);
  return task_runs_prune(&task);
}

int fc_janitor_test_list_oldest(const char *dir, const char *prefix, int cap,
                                char *out_csv, size_t out_len) {
  DirEntry *entries;
  int n, i;
  size_t pos = 0;
  if (!out_csv || out_len == 0) return -1;
  out_csv[0] = '\0';
  if (cap <= 0 || cap > JANITOR_BUS_SCAN_CAP) return -1;
  entries = janitor_entries_borrow();
  if (!entries) return -1;
  n = list_sorted(dir, prefix, entries, cap);
  for (i = 0; i < n; ++i) {
    int written = snprintf(out_csv + pos, out_len - pos, "%s%s",
                           i ? "," : "", entries[i].name);
    if (written < 0 || (size_t)written >= out_len - pos) {
      return janitor_entries_release_result(-1);
    }
    pos += (size_t)written;
  }
  return janitor_entries_release_result(n);
}

/* ------------------------------------------------------------------
 * Task: worker_store_prune — one entry per managed-worker operation
 * under workspace/logs/<store>/. Every entry is written once when the
 * operation starts and never revisited after it finishes, so the rule
 * is count-only: keep the newest N by trailing number.
 *
 * The one exception is a running operation. Its worker still owns the
 * entry — a Codex or Claude child writes into its directory until it
 * terminalizes — so a live operation's entry is never removed, however
 * old its number makes it look. `rt_operation_status` is the authority;
 * an entry the store has forgotten is ordinary debris.
 *
 * Stores are directories for the process workers and single JSON files
 * for Hermes handles. rm_rf takes both, and parse_trailing_number
 * already ignores a .json suffix.
 * ------------------------------------------------------------------ */

/* Is the operation that owns this entry still running? Unknown ids are
 * not protected: the operation store keeps a bounded window of finished
 * records, so "never heard of it" is the normal state of old debris. */
static int worker_store_entry_is_live(const JanitorTask *t, const char *name) {
  char op_id[RT_SMALL];
  char status[RT_SMALL];
  const char *base = name;
  size_t len, prefix_len;
  if (!t || !name || !*name) return 0;
  prefix_len = strlen(t->op_prefix);
  if (prefix_len > 0 && strncmp(name, t->op_prefix, prefix_len) == 0)
    base = name + prefix_len;
  len = strlen(base);
  if (len > 5 && strcmp(base + len - 5, ".json") == 0) len -= 5;
  if (len == 0 || len >= sizeof(op_id)) return 0;
  if (prefix_len > 0) {
    if ((size_t)snprintf(op_id, sizeof(op_id), "op_%.*s", (int)len, base) >=
        sizeof(op_id))
      return 0;
  } else {
    snprintf(op_id, sizeof(op_id), "%.*s", (int)len, base);
  }
  if (rt_operation_status(op_id, status, sizeof(status)) != 0) return 0;
  return strcmp(status, "running") == 0;
}

static int task_worker_store_prune(JanitorTask *t) {
  DIR *d;
  struct dirent *e;
  DirEntry *entries;
  int n = 0, keep, to_drop, dropped = 0, kept_live = 0;
  int any_running;
  int saved_errno;
  int i;
  int result = -1;
  if (!t->enabled || t->keep <= 0) return 0;
  entries = janitor_entries_borrow();
  if (!entries) return -1;
  d = opendir(t->path);
  if (!d) {
    if (errno == ENOENT) { result = 0; goto cleanup; }
    fprintf(stderr, "janitor: opendir failed for %s: %s\n",
            t->path, strerror(errno));
    goto cleanup;
  }
  errno = 0;
  while ((e = fs_readdir_checked(d)) != NULL) {
    long long num;
    if (e->d_name[0] == '.') continue;
    num = parse_trailing_number(e->d_name);
    if (num < 0) continue;
    if (n < JANITOR_WORKER_STORE_SCAN_CAP) {
      entries[n].num = num;
      snprintf(entries[n].name, sizeof(entries[n].name), "%s", e->d_name);
      n++;
    }
  }
  saved_errno = errno;
  if (closedir(d) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    fprintf(stderr, "janitor: readdir failed for %s: %s\n",
            t->path, strerror(saved_errno));
    errno = saved_errno;
    goto cleanup;
  }
  qsort(entries, (size_t)n, sizeof(*entries), by_num_asc);
  keep = t->keep;
  if (n <= keep) { result = 0; goto cleanup; }
  to_drop = n - keep;
  /* One store read answers the common case. Only when something is
   * actually in flight is it worth asking per candidate. */
  any_running = rt_operation_any_running();
  for (i = 0; i < to_drop; ++i) {
    char full[512];
    if (any_running && worker_store_entry_is_live(t, entries[i].name)) {
      kept_live++;
      continue;
    }
    if (snprintf(full, sizeof(full), "%s/%s", t->path, entries[i].name)
        >= (int)sizeof(full)) continue;
    if (rm_rf(full) == 0) dropped++;
  }
  if (dropped > 0 || kept_live > 0)
    (void)rt_narrate(
        "janitor: pruned %d from %s (kept %d, %d still running)",
        dropped, t->path, keep, kept_live);
  result = 0;

cleanup:
  return janitor_entries_release_result(result);
}

/* ------------------------------------------------------------------
 * Task: bus_processed_prune — workspace/bus/processed/bus_NNNNNN.json
 * Small files, monotonic ids. Same shape as runs pruning but files
 * not dirs.
 * ------------------------------------------------------------------ */

static int task_bus_processed_prune(JanitorTask *t) {
  DirEntry *entries;
  int n, keep, to_drop, dropped = 0;
  int i;
  int result = -1;
  if (!t->enabled || t->keep <= 0) return 0;
  entries = janitor_entries_borrow();
  if (!entries) return -1;
  if ((n = list_sorted(t->path, "bus_", entries,
                       JANITOR_BUS_SCAN_CAP)) < 0)
    goto cleanup;
  keep = t->keep;
  if (n <= keep) { result = 0; goto cleanup; }
  to_drop = n - keep;
  for (i = 0; i < to_drop; ++i) {
    char full[512];
    char wake_id[RT_SMALL];
    size_t name_len = strlen(entries[i].name);
    int pinned;
    if (name_len <= 5U || name_len - 5U >= sizeof(wake_id) ||
        strcmp(entries[i].name + name_len - 5U, ".json") != 0)
      continue;
    memcpy(wake_id, entries[i].name, name_len - 5U);
    wake_id[name_len - 5U] = '\0';
    pinned = rt_publication_outbox_wake_pending(wake_id);
    /* A pending exact claim still needs its processed envelope for same-run
     * recommit. Unreadable outbox state is conservatively pinned too. */
    if (pinned != 0) continue;
    /* A completion claim whose operation is still running is pending
     * evidence: its claim run may recommit from it, or the reconciler
     * will requeue it. */
    if (rt_ports_completion_claim_pinned(wake_id) != 0) continue;
    if (snprintf(full, sizeof(full), "%s/%s", t->path, entries[i].name)
        >= (int)sizeof(full)) continue;
    if (unlink(full) == 0) dropped++;
  }
  if (dropped > 0)
    (void)rt_narrate("janitor: pruned %d bus/processed entries (kept %d)",
                     dropped, keep);
  result = 0;

cleanup:
  return janitor_entries_release_result(result);
}

int fc_janitor_test_worker_store_prune(const char *path, const char *op_prefix,
                                       int keep) {
  JanitorTask task;
  if (!path || !*path || keep <= 0) return -1;
  memset(&task, 0, sizeof(task));
  task.name = "worker_store_prune_test";
  task.enabled = 1;
  task.keep = keep;
  snprintf(task.path, sizeof(task.path), "%s", path);
  snprintf(task.op_prefix, sizeof(task.op_prefix), "%s",
           op_prefix ? op_prefix : "");
  return task_worker_store_prune(&task);
}

int fc_janitor_test_bus_processed_prune(const char *path, int keep) {
  JanitorTask task;
  if (!path || !*path || keep <= 0) return -1;
  memset(&task, 0, sizeof(task));
  task.name = "bus_processed_prune_test";
  task.enabled = 1;
  task.keep = keep;
  snprintf(task.path, sizeof(task.path), "%s", path);
  return task_bus_processed_prune(&task);
}

int fc_janitor_test_bus_scan_cap(void) {
  return JANITOR_BUS_SCAN_CAP;
}

/* ------------------------------------------------------------------
 * Task: tasks_archive_prune — workspace/memory/state/tasks_archive.jsonl
 * Rewrite the file keeping only lines whose completed_ms is younger
 * than keep_days. Atomic rename via .tmp swap.
 * ------------------------------------------------------------------ */

static int task_tasks_archive_prune(JanitorTask *t) {
  char *text = NULL;
  char tmp[512];
  FILE *fp;
  long long now_ms;
  long long cutoff_ms;
  int survived = 0, dropped = 0;
  const char *cursor;
  if (!t->enabled || t->keep_days <= 0) return 0;
  if (fs_read_text(t->path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  now_ms = (long long)time(NULL) * 1000LL;
  cutoff_ms = now_ms - (long long)t->keep_days * 86400000LL;
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", t->path) >= (int)sizeof(tmp)) {
    free(text); return -1;
  }
  fp = fopen(tmp, "w");
  if (!fp) { free(text); return -1; }
  cursor = text;
  while (*cursor) {
    const char *line_start = cursor;
    const char *nl = strchr(cursor, '\n');
    size_t len = nl ? (size_t)(nl - cursor) : strlen(cursor);
    JsonRef root;
    long long ms = 0;
    char buf[8192];
    if (len == 0) { cursor = nl ? nl + 1 : cursor + strlen(cursor); continue; }
    if (len >= sizeof(buf)) {
      /* line too long — preserve verbatim rather than lose it */
      fwrite(line_start, 1, len, fp);
      fputc('\n', fp);
      survived++;
      cursor = nl ? nl + 1 : cursor + strlen(cursor);
      continue;
    }
    memcpy(buf, line_start, len);
    buf[len] = '\0';
    if (json_ref_first_object(buf, &root) == 0 &&
        (json_ref_object_get_long(&root, "completed_ms", &ms) == 0 ||
         json_ref_object_get_long(&root, "updated_ms", &ms) == 0)) {
      if (ms >= cutoff_ms) {
        fwrite(buf, 1, len, fp);
        fputc('\n', fp);
        survived++;
      } else {
        dropped++;
      }
    } else {
      /* no timestamp we can compare — keep it (safer) */
      fwrite(buf, 1, len, fp);
      fputc('\n', fp);
      survived++;
    }
    cursor = nl ? nl + 1 : cursor + strlen(cursor);
  }
  {
    int write_failed = 0;
    int saved_errno = 0;
    if (fflush(fp) != 0 || ferror(fp) || fsync(fileno(fp)) != 0) {
      write_failed = 1;
      saved_errno = errno;
    }
    if (fclose(fp) != 0) {
      write_failed = 1;
      if (!saved_errno) saved_errno = errno;
    }
    fp = NULL;
    if (write_failed) {
      free(text);
      unlink(tmp);
      errno = saved_errno ? saved_errno : EIO;
      return -1;
    }
  }
  free(text);
  if (dropped > 0) {
    if (rename(tmp, t->path) != 0) { unlink(tmp); return -1; }
    (void)rt_narrate("janitor: pruned %d tasks_archive entries older than %d days (kept %d)",
                     dropped, t->keep_days, survived);
  } else {
    unlink(tmp);
  }
  return 0;
}

/* ------------------------------------------------------------------
 * Reactor module callbacks
 * ------------------------------------------------------------------ */

static JanitorTask *add_task(JanitorState *s, const char *name, int enabled,
                             uint64_t interval_ms, int (*run)(JanitorTask *),
                             const char *path, int keep, int keep_days,
                             uint64_t stagger) {
  JanitorTask *t;
  if (s->task_count >= sizeof(s->tasks) / sizeof(s->tasks[0])) return NULL;
  t = &s->tasks[s->task_count++];
  memset(t, 0, sizeof(*t));
  t->name = name;
  t->enabled = enabled;
  t->interval_ms = interval_ms;
  t->next_run_ms = stagger;  /* absolute ms; 0 = fire on first tick */
  t->run = run;
  t->keep = keep;
  t->keep_days = keep_days;
  if (path) snprintf(t->path, sizeof(t->path), "%s", path);
  return t;
}

/* Every managed worker that writes one entry per operation. A worker
 * added later belongs in this table, not in a task of its own: the
 * retention rule is the same and so is the config shape. */
static const struct {
  const char *task_name;    /* janitor task id */
  const char *config_key;   /* appliance.<key>.keep / .check_interval_ms */
  const char *path;
  const char *op_prefix;    /* stripped before "op_" rebuilds the op id */
  uint64_t    stagger_ms;
} kWorkerStores[] = {
  { "codex_ops_prune",  "codex_ops",  "workspace/logs/codex_ops",  "",        10000 },
  { "claude_ops_prune", "claude_ops", "workspace/logs/claude_ops", "",        11000 },
  { "hermes_ops_prune", "hermes_ops", "workspace/logs/hermes_ops", "hermes_", 12000 },
};

/* Register a JSONL rotation task. The name maps to a hardcoded engine
 * log path (bus → workspace/logs/bus.jsonl, etc.) — engine-owned
 * operating output, not a user-configurable path. Config supplies
 * cap, keep, compress. Missing entry uses defaults. */
static void add_rotation_task(JanitorState *s, const char *name,
                              const char *path, uint64_t interval_ms,
                              const JsonRef *logs_targets, int have_targets,
                              int gzip_ok, int reopen_stdio, uint64_t stagger) {
  JanitorTask *t;
  JsonRef entry;
  long long v;
  char compress_str[16] = "";
  uint64_t max_bytes = DEFAULT_LOGS_MAX_BYTES;
  int keep = DEFAULT_LOGS_KEEP;
  int compress = strcmp(DEFAULT_LOGS_COMPRESS, "gzip") == 0 ? 1 : 0;
  if (s->task_count >= sizeof(s->tasks) / sizeof(s->tasks[0])) return;
  if (have_targets && json_ref_object_get_object(logs_targets, name, &entry) == 0) {
    if (json_ref_object_get_long(&entry, "max_size_bytes", &v) == 0 && v > 0)
      max_bytes = (uint64_t)v;
    if (json_ref_object_get_long(&entry, "keep", &v) == 0 && v >= 0)
      keep = (int)v;
    if (json_ref_object_get_string(&entry, "compress", compress_str,
                                    sizeof(compress_str)) == 0 && compress_str[0])
      compress = (strcmp(compress_str, "gzip") == 0);
  }
  if (compress && !gzip_ok) compress = 0;  /* gzip missing at init; keep uncompressed */
  t = &s->tasks[s->task_count++];
  memset(t, 0, sizeof(*t));
  t->name = name;
  t->enabled = (keep > 0 && max_bytes > 0 && interval_ms > 0);
  t->interval_ms = interval_ms;
  t->next_run_ms = stagger;
  t->run = task_log_rotate;
  snprintf(t->path, sizeof(t->path), "%s", path);
  t->keep = keep;
  t->max_bytes = max_bytes;
  t->compress = compress;
  t->reopen_stdio = reopen_stdio;
}

static int janitor_init(FcReactorModule *m) {
  JanitorState *s = m ? (JanitorState *)m->state : NULL;
  char *text = NULL;
  JsonRef appliance;
  int have_cfg;
  long long v;
  /* Config-driven overrides. Absent block or absent task uses defaults. */
  int    runs_keep         = DEFAULT_RUNS_KEEP;
  uint64_t runs_iv         = DEFAULT_RUNS_INTERVAL_MS;
  size_t  store_i;
  int    bus_keep          = DEFAULT_BUS_PROCESSED_KEEP;
  uint64_t bus_iv          = DEFAULT_BUS_PROCESSED_INTERVAL_MS;
  int    ta_keep_days      = DEFAULT_TASKS_ARCHIVE_KEEP_DAYS;
  uint64_t ta_iv           = DEFAULT_TASKS_ARCHIVE_INTERVAL_MS;
  uint64_t logs_iv         = DEFAULT_LOGS_INTERVAL_MS;
  JsonRef logs_block, logs_targets;
  int have_logs_targets = 0;
  int gzip_ok;
  if (!s) return -1;
  s->next_tick_ms = 0;

  have_cfg = (read_appliance_block(&text, &appliance) == 0);
  if (have_cfg) {
    if (cfg_task_int(&appliance, "runs", "keep", &v) == 0) runs_keep = (int)v;
    if (cfg_task_int(&appliance, "runs", "check_interval_ms", &v) == 0) runs_iv = (uint64_t)v;
    if (cfg_task_int(&appliance, "bus_processed", "keep", &v) == 0) bus_keep = (int)v;
    if (cfg_task_int(&appliance, "bus_processed", "check_interval_ms", &v) == 0) bus_iv = (uint64_t)v;
    if (cfg_task_int(&appliance, "tasks_archive", "keep_days", &v) == 0) ta_keep_days = (int)v;
    if (cfg_task_int(&appliance, "tasks_archive", "check_interval_ms", &v) == 0) ta_iv = (uint64_t)v;
    if (json_ref_object_get_object(&appliance, "logs", &logs_block) == 0) {
      if (json_ref_object_get_long(&logs_block, "check_interval_ms", &v) == 0 && v > 0)
        logs_iv = (uint64_t)v;
      if (json_ref_object_get_object(&logs_block, "targets", &logs_targets) == 0)
        have_logs_targets = 1;
    }
  }
  gzip_ok = fc_rotation_gzip_available();
  if (!gzip_ok)
    (void)rt_narrate("janitor: gzip not on PATH — rotated JSONL logs will be kept uncompressed");
  if (bus_keep >= JANITOR_BUS_SCAN_CAP) {
    int requested_keep = bus_keep;
    bus_keep = JANITOR_BUS_SCAN_CAP - 1;
    (void)rt_narrate(
        "janitor: bus_processed keep %d reaches scan cap %d; clamped to %d",
        requested_keep, JANITOR_BUS_SCAN_CAP, bus_keep);
  }

  /* Stagger the first-fire so all tasks don't hit disk in the same
   * tick. Interval-relative: task N runs at now + N * stagger_slot. */
  add_task(s, "runs_prune",          runs_keep > 0 && runs_iv > 0,
           runs_iv, task_runs_prune, "workspace/runs", runs_keep, 0, 5000);
  /* One retention task per managed-worker store. Config lives under
   * appliance.<store>.keep / .check_interval_ms, so appliance.codex_ops
   * keeps meaning exactly what it meant. */
  for (store_i = 0; store_i < sizeof(kWorkerStores) / sizeof(kWorkerStores[0]);
       ++store_i) {
    int      keep = DEFAULT_WORKER_STORE_KEEP;
    uint64_t iv   = DEFAULT_WORKER_STORE_INTERVAL_MS;
    JanitorTask *t;
    if (have_cfg) {
      if (cfg_task_int(&appliance, kWorkerStores[store_i].config_key,
                       "keep", &v) == 0) keep = (int)v;
      if (cfg_task_int(&appliance, kWorkerStores[store_i].config_key,
                       "check_interval_ms", &v) == 0) iv = (uint64_t)v;
    }
    t = add_task(s, kWorkerStores[store_i].task_name, keep > 0 && iv > 0,
                 iv, task_worker_store_prune, kWorkerStores[store_i].path,
                 keep, 0, kWorkerStores[store_i].stagger_ms);
    if (t) snprintf(t->op_prefix, sizeof(t->op_prefix), "%s",
                    kWorkerStores[store_i].op_prefix);
  }

  add_task(s, "bus_processed_prune", bus_keep > 0 && bus_iv > 0,
           bus_iv, task_bus_processed_prune, "workspace/bus/processed",
           bus_keep, 0, 15000);
  add_task(s, "tasks_archive_prune", ta_keep_days > 0 && ta_iv > 0,
           ta_iv, task_tasks_archive_prune,
           "workspace/memory/state/tasks_archive.jsonl", 0, ta_keep_days,
           20000);

  /* JSONL rotation targets. Each target's config is under
   * appliance.logs.targets.<name>; the name→path mapping is
   * engine-owned (these are the engine's own logs). */
  add_rotation_task(s, "bus",             "workspace/logs/bus.jsonl",
                    logs_iv, &logs_targets, have_logs_targets, gzip_ok, 0, 25000);
  add_rotation_task(s, "deliveries",      "workspace/logs/deliveries.jsonl",
                    logs_iv, &logs_targets, have_logs_targets, gzip_ok, 0, 30000);
  add_rotation_task(s, "narration",       "workspace/logs/narration.jsonl",
                    logs_iv, &logs_targets, have_logs_targets, gzip_ok, 0, 35000);
  add_rotation_task(s, "llm_usage",       "workspace/logs/llm_usage.jsonl",
                    logs_iv, &logs_targets, have_logs_targets, gzip_ok, 0, 40000);
  add_rotation_task(s, "rejected_events", "workspace/logs/rejected_events.jsonl",
                    logs_iv, &logs_targets, have_logs_targets, gzip_ok, 0, 45000);
  /* gateway.log uses the same default cap/retention as the other logs. The
   * reopen_stdio flag makes the daemon's fd 1/2 follow the rotation. */
  add_rotation_task(s, "gateway",         GW_LOG_PATH,
                    logs_iv, &logs_targets, have_logs_targets, gzip_ok, 1, 50000);

  if (text) free(text);
  return 0;
}

static int janitor_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  (void)m; (void)ps;
  return 0;
}

static int janitor_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  (void)m; (void)fd; (void)events; (void)tag;
  return 0;
}

static int janitor_tick(FcReactorModule *m, uint64_t now_ms) {
  JanitorState *s = m ? (JanitorState *)m->state : NULL;
  size_t i;
  if (!s) return -1;
  if (now_ms < s->next_tick_ms) return 0;
  s->next_tick_ms = now_ms + JANITOR_TICK_MS;
  for (i = 0; i < s->task_count; ++i) {
    JanitorTask *t = &s->tasks[i];
    if (!t->enabled) continue;
    if (now_ms < t->next_run_ms) continue;
    if (t->run(t) != 0) {
      fprintf(stderr, "janitor: task %s failed\n", t->name);
      (void)rt_narrate("janitor: task %s failed", t->name);
    }
    t->next_run_ms = now_ms + t->interval_ms;
  }
  return 0;
}

static uint64_t janitor_next_deadline_ms(FcReactorModule *m, uint64_t now_ms) {
  JanitorState *s = m ? (JanitorState *)m->state : NULL;
  uint64_t soonest;
  size_t i;
  (void)now_ms;
  if (!s) return FC_NO_DEADLINE;
  soonest = s->next_tick_ms;
  for (i = 0; i < s->task_count; ++i) {
    if (s->tasks[i].enabled && s->tasks[i].next_run_ms &&
        s->tasks[i].next_run_ms < soonest)
      soonest = s->tasks[i].next_run_ms;
  }
  return soonest;
}

static void janitor_shutdown(FcReactorModule *m) {
  (void)m;
}

FcReactorModule *fc_janitor_module_create(void) {
  FcReactorModule *m = (FcReactorModule *)calloc(1, sizeof(*m));
  JanitorState *s = (JanitorState *)calloc(1, sizeof(*s));
  if (!m || !s) { free(m); free(s); return NULL; }
  m->name = "janitor";
  m->module_id = "janitor-main";
  m->state = s;
  m->init = janitor_init;
  m->collect_fds = janitor_collect_fds;
  m->on_fd = janitor_on_fd;
  m->tick = janitor_tick;
  m->next_deadline_ms = janitor_next_deadline_ms;
  m->shutdown = janitor_shutdown;
  return m;
}

void fc_janitor_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m->state);
  free(m);
}
