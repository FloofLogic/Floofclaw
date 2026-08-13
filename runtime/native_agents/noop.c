/* runtime/native_agents/noop.c
 *
 * Perf baseline agent. Discards input, emits one message action_request
 * intent with text "ok". Used by the kernel/scheduler stress harness; zero
 * allocations on the hot path. */

#include "../agents.h"
#include "../runtime.h"

#include <stdio.h>
#include <string.h>

int rt_agent_noop_run(const RtContext *ctx, const RtStep *step,
                      const RtAgentMeta *meta,
                      const RtActionRegistry *actions,
                      const char *input_json,
                      char *stdout_buf, size_t stdout_cap,
                      char *stderr_buf, size_t stderr_cap) {
  int n;
  (void)ctx; (void)step; (void)meta; (void)actions;
  (void)input_json; (void)stderr_buf; (void)stderr_cap;
  n = snprintf(stdout_buf, stdout_cap,
               "{\"calls\":[{\"name\":\"message.send\","
               "\"args\":{\"message\":\"ok\"}}],"
               "\"notes\":[],\"error\":null}\n");
  return (n < 0 || (size_t)n >= stdout_cap) ? RT_ERR_OUTPUT_TOO_LARGE : 0;
}
