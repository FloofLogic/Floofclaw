#ifndef FCLAW_GATEWAY_H
#define FCLAW_GATEWAY_H

#include <stddef.h>

#define GW_PID_PATH       ".fclaw/run/gateway.pid"
#define GW_LOOP_PATH      ".fclaw/run/gateway.loop"
#define GW_RUNTIME_OWNER_LOCK_PATH ".fclaw/run/runtime_owner.lock"
#define GW_STATUS_SOCK_PATH ".fclaw/run/gateway.status.sock"
#define GW_LOG_PATH       "workspace/logs/gateway.log"
#define GW_DEFAULT_POLL_MS 5000

int gateway_start(const char *loop_name, int human);
int gateway_status(int human);
int gateway_stop(int human);
int gateway_reload(const char *loop_name, int human);
int gateway_run_foreground(const char *loop_name, int human);
int gateway_running_pid(void);

/* Read the floop owned by the live gateway without exposing its owner-file
 * representation to clients. Returns 0 with `out` populated, 1 when no
 * gateway is live, and -1 when a live gateway's floop cannot be read. */
int gateway_running_floop(char *out, size_t out_len);

/* Install process-wide signal behavior required before any gateway socket
 * I/O. Exported so the closed-peer regression can exercise the real policy. */
void gateway_install_signal_policy(void);

/* 1 if the daemon has redirected fd 1/2 to GW_LOG_PATH via dup2;
 * 0 otherwise (foreground run, before daemonization, tests). The
 * janitor's gateway.log rotation task reads this to know whether
 * it's safe to reopen fd 1 after rotating — foreground fd 1 is the
 * terminal and must not be clobbered. */
int gateway_is_daemonized_stdout(void);

#endif
