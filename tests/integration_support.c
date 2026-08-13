/* Integration-only test helpers. These call into the harness which
 * links the runtime kernel; the unit binary doesn't link the runtime
 * so it doesn't compile this file. */

#include "test_support.h"
#include "harness.h"
#include "../runtime/support/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int test_run_with_text(const char *loop, const char *text,
                       char *run_id, size_t run_id_len, char **out) {
  /* In-process via the harness — same code path as `fclaw run`, no
   * fork. Synthesizes the same default JSON stdout shape the CLI produces
   * so existing tests that grep `out`
   * keep working. Returns 0 for run_done, -1 otherwise. Tests that
   * need to distinguish run_failed (rc==2) from setup error (rc==1)
   * call harness_run directly. */
  char response[8192];
  char run_id_local[64];
  char *p_run_id = (run_id && run_id_len) ? run_id : run_id_local;
  size_t p_run_id_len = (run_id && run_id_len) ? run_id_len : sizeof(run_id_local);
  int rc = harness_run("tests", loop ? loop : "fast", text ? text : "",
                       response, sizeof(response), p_run_id, p_run_id_len);
  if (out) {
    char escaped_run_id[256], escaped_response[32768];
    char buf[32768 + 512];
    if (json_escape(p_run_id, escaped_run_id, sizeof(escaped_run_id)) != 0 ||
        json_escape(response[0] ? response : "", escaped_response,
                    sizeof(escaped_response)) != 0)
      return -1;
    snprintf(buf, sizeof(buf),
             "{\"run_id\":\"%s\",\"status\":\"%s\",\"result\":\"%s\"}\n",
             escaped_run_id, rc == 0 ? "done" : "failed", escaped_response);
    *out = strdup(buf);
  }
  return rc == 0 ? 0 : -1;
}

int test_gateway_fast_loop(int events, char **out_log) {
  /* In-process gateway emulation. Bursts N user_messages onto the
   * fast floop, drives the scheduler in tick-by-tick mode until all
   * have completed, and returns a synthesized "gateway log" string
   * with the heap stats and completion counters that budget tests
   * grep for. */
  return harness_gateway_fast_loop(events, out_log);
}
