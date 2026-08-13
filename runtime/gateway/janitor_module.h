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
 *   bus_processed_prune   workspace/bus/processed/        (§5)
 *   tasks_archive_prune   workspace/memory/state/         (§7)
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
int fc_janitor_test_bus_scan_cap(void);

#endif
