#ifndef FCLAW_CHANNEL_H
#define FCLAW_CHANNEL_H

#include <stddef.h>

#define CHANNEL_LOG_DIR "workspace/logs/channels"
#define CHANNEL_FLOOP_CONFLICT 3

/* Synchronous channel helper: publish a user_message envelope and wait for a
 * matching outbound delivery in workspace/logs/deliveries.jsonl. Used by
 * `fclaw run`, non-TTY CLI tests, and simple line-oriented channel stubs.
 * TTY `fclaw -i` is a live UI: it publishes input and tails channel
 * deliveries instead of deciding which delivery is final.
 *
 * Returns the run's terminal status:
 *   0 -> run_done; response_out holds the reply
 *   2 -> run_failed; response_out holds the "Run failed: …" message
 *   1 -> no terminal delivery within timeout, or setup/I-O error
 *   3 -> an explicit client floop conflicts with the live gateway floop
 *
 * Channels do not call agents, actions, or inbox-drain helpers directly. They
 * publish to the bus and receive delivery log entries; the runtime owner is
 * the only runtime driver. */
int channel_synchronous(const char *channel, const char *loop_name,
                        const char *user_text,
                        char *response_out, size_t response_out_len,
                        char *run_id_out, size_t run_id_out_len);

int channel_run_cli_interactive(const char *loop_name, int human);
int channel_run_ws(const char *loop_name, int human);
int channel_run_irc(const char *loop_name, int human);

#endif
