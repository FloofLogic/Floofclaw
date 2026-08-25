#ifndef FC_JANITOR_MODULE_H
#define FC_JANITOR_MODULE_H

#include "reactor.h"

/* Appliance-mode housekeeping module. Ticks on its own schedule and
 * runs each configured cleanup task at its own cadence. Every cleanup
 * task is a pure MECHANISM — the config supplies "which directory,
 * how many to keep, how often to check"; the module never encodes
 * those choices in C. Design rationale in the retired ENGINE_TODO (git history).
 *
 * Current tasks (as of first landing):
 *
 *   runs_prune            workspace/runs/                 (§1)
 *   codex_ops_prune       workspace/logs/codex_ops/       (§1 same shape)
 *   claude_ops_prune      workspace/logs/claude_ops/      (§1 same shape)
 *   hermes_ops_prune      workspace/logs/hermes_ops/      (§1 same shape)
 *   bus_processed_prune   workspace/bus/processed/        (§5)
 *   tasks_archive_prune   workspace/memory/state/         (§7)
 *
 * The three managed-worker stores share one task implementation and one
 * config shape (appliance.<store>.keep / .check_interval_ms). A worker
 * added later that writes one entry per operation belongs in that table
 * rather than in a task of its own. They also share the one exception to
 * count-only retention: an entry whose operation is still running in
 * workspace/memory/state/operations.json is never removed.
 *
 * Later commits add JSONL rotation and gateway.log SIGHUP rotation
 * to the same module (§§2, 4).
 *
 * Configuration reads from config/floofclaw_config.json under the
 * top-level "appliance" block. A missing block or missing target
 * uses safe defaults defined in janitor_module.c. Setting
 * check_interval_ms: 0 or keep: 0 disables that specific task. */
FcReactorModule *fc_janitor_module_create(void);
void fc_janitor_module_destroy(FcReactorModule *m);

/* Regression hook: deliberately attempts a nested scratch borrow. Returns
 * zero only when the guard rejects re-entry and releases cleanly. */
int fc_janitor_test_scratch_reentry_guard(void);
int fc_janitor_test_runs_prune(const char *path, int keep);
int fc_janitor_test_bus_processed_prune(const char *path, int keep);
/* Run one managed-worker store's retention directly. `op_prefix` is the
 * entry-name prefix stripped before "op_" rebuilds the operation id
 * ("" when the entry name is already the id). */
int fc_janitor_test_worker_store_prune(const char *path, const char *op_prefix,
                                       int keep);
int fc_janitor_test_bus_scan_cap(void);
/* Run the bounded oldest-first selection over `dir` with an explicit cap
 * and report the chosen names, comma-joined, in the order the pruner walks
 * them. Returns the count selected, or -1. */
int fc_janitor_test_list_oldest(const char *dir, const char *prefix, int cap,
                                char *out_csv, size_t out_len);

#endif
