/* Native agent registry.
 *
 * Anything in floops/<floop>/agents/<id>/agent.json with executor="native" is
 * dispatched by id to a function in this table. agent.json remains
 * the source of truth for which agents exist and which executor each
 * one uses; this registry is just where the C function lives.
 *
 * Agents that need model-backed reasoning use executor="llm" instead.
 * User-authored experimental agents and test fixtures use
 * executor="script", which keeps fork+exec on the floop-local run.sh.
 *
 * Adding a native agent:
 *   1. write runtime/native_agents/<id>.c with rt_agent_<id>_run
 *      matching RtAgentFn
 *   2. add a forward declaration + entry in g_native_agents below
 *   3. add the file to NATIVE_AGENT_SRC in the Makefile
 *   4. set executor="native" in the floop-local agent.json
 *   5. delete the floop-local run.sh */

#include "agents.h"

#include <string.h>

/* Forward declarations — implementations in runtime/native_agents/. */
int rt_agent_noop_run(const RtContext *ctx, const RtStep *step,
                      const RtAgentMeta *meta,
                      const RtActionRegistry *actions,
                      const char *input_json,
                      char *stdout_buf, size_t stdout_cap,
                      char *stderr_buf, size_t stderr_cap);
int rt_agent_memory_run(const RtContext *ctx, const RtStep *step,
                        const RtAgentMeta *meta,
                        const RtActionRegistry *actions,
                        const char *input_json,
                        char *stdout_buf, size_t stdout_cap,
                        char *stderr_buf, size_t stderr_cap);
int rt_agent_workers_run(const RtContext *ctx, const RtStep *step,
                         const RtAgentMeta *meta,
                         const RtActionRegistry *actions,
                         const char *input_json,
                         char *stdout_buf, size_t stdout_cap,
                         char *stderr_buf, size_t stderr_cap);
int rt_agent_operation_poller_run(const RtContext *ctx, const RtStep *step,
                                  const RtAgentMeta *meta,
                                  const RtActionRegistry *actions,
                                  const char *input_json,
                                  char *stdout_buf, size_t stdout_cap,
                                  char *stderr_buf, size_t stderr_cap);

static const struct {
  const char *id;
  RtAgentFn fn;
} g_native_agents[] = {
  { "noop_agent", rt_agent_noop_run    },
  { "memory",     rt_agent_memory_run  },
  { "workers",    rt_agent_workers_run },
  { "operation_poller", rt_agent_operation_poller_run },
  { NULL, NULL },
};

RtAgentFn rt_agent_lookup(const char *id) {
  if (!id) return NULL;
  for (int i = 0; g_native_agents[i].id; ++i) {
    if (strcmp(g_native_agents[i].id, id) == 0) return g_native_agents[i].fn;
  }
  return NULL;
}
