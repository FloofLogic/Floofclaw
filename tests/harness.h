#ifndef FCLAW_TEST_HARNESS_H
#define FCLAW_TEST_HARNESS_H

/* In-process test harness. Drives the runtime kernel directly in the
 * test process — no shell, no popen, no fork+exec of fclaw.
 *
 * Why this exists. Forking ./bin/fclaw for every test costs ~50ms of
 * process startup (fork+exec+library load+profile parse), repeated
 * dozens of times per test. The runtime is already a library; tests
 * should call its functions directly. The harness wraps the same
 * functions `runtime/main.c` calls when you type `fclaw run`.
 *
 * What's "natural" about in-process. The harness drives the same
 * `rt_scheduler_init/_tick/_advance_and_retire` your gateway runs.
 * Same agent_runner, same action_runner, same event log on disk, same
 * runstate.json, same action subprocesses (when a test exercises a
 * subprocess action — the *outer* fork is gone, but action bash/etc.
 * still fork inside the scheduler exactly like production).
 *
 * How to use it. Three layers, pick the lowest one your test needs.
 *
 *   harness_run(loop, text, ...)
 *       — equivalent of `fclaw run --floop X --text Y`. Synchronous,
 *         returns 0/1/2 just like the CLI exit code. Use this for
 *         tests that send one user_message and check what came out.
 *
 *   HarnessGateway *g = harness_gateway_init(loop);
 *   harness_gateway_publish(g, "tests", "...");
 *   harness_gateway_drive_until_completed(g, n_expected, timeout_ms);
 *   harness_gateway_close(g);
 *       — gateway emulation. Drives the scheduler tick by tick from
 *         the test process. No daemon, no socket. Use when the test
 *         publishes multiple events, checks concurrent state, or
 *         calls the status API.
 *
 *   harness_cli_view(argc, argv, &out, &rc)
 *   harness_cli_cacheview(argc, argv, &out, &rc)
 *   harness_cli_tools(argc, argv, &out, &rc)
 *   harness_cli_channel(argc, argv, &out, &rc)
 *   harness_cli_usage(argc, argv, &out, &rc)
 *   harness_cli_task(argc, argv, &out, &rc)
 *   harness_cli_replay(event_log_path, &state_out)
 *       — run an fclaw subcommand in-process, capture stdout, get its
 *         exit code. Replaces test_run_fclaw("view ...").
 *
 * What stays as fork. Action subprocesses (bash, web_fetch,
 * the script-executor agent's run.sh, the LLM child) still fork —
 * because they're subprocesses, that's what they are. The point is:
 * a test that wants to verify "action X ran with this output" does NOT
 * fork the test process; only the runtime forks the action, just like
 * production. You're paying for the fork you're testing, not for the
 * fork that gets you to the runtime.
 *
 * One static-state caveat. A handful of runtime modules cache state
 * (e.g., the agent capability cache in event_log.c). This survives
 * between tests in the same binary. Today no test relies on stale
 * cache state; if a future test needs a clean module, add a
 * harness_reset_runtime_caches() and call it at init.
 */

#include "../runtime/runtime_kernel.h"

#include <stddef.h>
#include <stdint.h>

/* ===== high-level: synchronous run ============================== */

/* Equivalent of `fclaw run --floop <loop> --text <user_text>`.
 * Returns:
 *   0 -> run_done       response_out holds the reply
 *   2 -> run_failed     response_out holds "Run failed: ..."
 *   1 -> setup error / no terminal delivery
 * `channel` defaults to "tests" if NULL. run_id_out can be NULL. */
int harness_run(const char *channel,
                const char *loop,
                const char *user_text,
                char *response_out, size_t response_len,
                char *run_id_out, size_t run_id_len);

/* ===== mid-level: gateway emulation ============================= */

typedef struct HarnessGateway HarnessGateway;

/* Set up an in-process scheduler for `loop_name`. Returns NULL on
 * profile-load failure. Caller calls harness_gateway_close. */
HarnessGateway *harness_gateway_init(const char *loop_name);

/* Write a user_message envelope to workspace/bus/inbox/. Returns 0
 * on success. The next harness_gateway_drive_* call picks it up. */
int harness_gateway_publish(HarnessGateway *g,
                            const char *channel, const char *text);
/* Publish an already-formed payload object through the same bus boundary.
 * Media contract tests use this to preserve their compact manifest ref. */
int harness_gateway_publish_payload(HarnessGateway *g,
                                    const char *channel,
                                    const char *payload_json);

/* Tick the scheduler once. Used when the test wants explicit step
 * control (e.g., "publish, tick, assert run_001 is RUNNING"). */
void harness_gateway_drive_one_tick(HarnessGateway *g);

/* Tick repeatedly (1ms naps between ticks) until at least
 * `expected_completed` runs have retired (DONE or FAILED), or
 * timeout. Returns 0 on success, -1 on timeout. */
int harness_gateway_drive_until_completed(HarnessGateway *g,
                                          int expected_completed,
                                          uint64_t timeout_ms);

/* Same shape but waits until a specific run reaches RT_RUN_DONE or
 * RT_RUN_FAILED. */
int harness_gateway_drive_until_run_done(HarnessGateway *g,
                                         const char *run_id,
                                         uint64_t timeout_ms);

/* Direct status JSON via the same builder the daemon serves over its
 * unix socket. Caller frees. */
char *harness_gateway_status_json(const HarnessGateway *g);

/* Look up a run by id; NULL if not in the active pool. */
const RtRun *harness_gateway_find_run(const HarnessGateway *g, const char *run_id);

/* Build the gateway-shape stats line ("gateway: heap mallocs=… runs_completed=…")
 * that `make test`'s budget tests grep. Caller frees. */
char *harness_gateway_stats_line(const HarnessGateway *g);

/* Drive `events` user_message bursts through the fast floop, return when
 * all complete. Used by the heap-budget tests. Caller frees out_log. */
int harness_gateway_fast_loop(int events, char **out_log);

void harness_gateway_close(HarnessGateway *g);

/* ===== CLI subcommands in-process =============================== */

/* Each captures stdout into *stdout_out (caller frees) and exit code
 * into *exit_code. argv elements are passed straight to the rt_*_main
 * function (no leading "fclaw" or subcommand prefix). */
int harness_cli_gateway  (int argc, char **argv, char **stdout_out, int *exit_code);
int harness_cli_bus      (int argc, char **argv, char **stdout_out, int *exit_code);
int harness_cli_view     (int argc, char **argv, char **stdout_out, int *exit_code);
int harness_cli_cacheview(int argc, char **argv, char **stdout_out, int *exit_code);
int harness_cli_tools    (int argc, char **argv, char **stdout_out, int *exit_code);
int harness_cli_channel  (int argc, char **argv, char **stdout_out, int *exit_code);
int harness_cli_usage    (int argc, char **argv, char **stdout_out, int *exit_code);
int harness_cli_task     (int argc, char **argv, char **stdout_out, int *exit_code);

/* `fclaw replay <event-log-path>` — runs `rt_reduce_event_log_text`
 * over the file contents and returns the resulting state.json blob.
 * Returns 0 on success, 1 on read/reduce failure. Caller frees. */
int harness_cli_replay(const char *event_log_path, char **state_out);

/* `fclaw bus publish --channel C --text T` — synchronous helper. */
int harness_cli_bus_publish(const char *channel, const char *text);

/* ===== artifacts ================================================ */

/* Read workspace/runs/<run_id>/event_log.jsonl. Caller frees. */
char *harness_read_event_log(const char *run_id);

/* Read workspace/runs/<run_id>/runstate.json. Caller frees. */
char *harness_read_runstate(const char *run_id);

#endif
