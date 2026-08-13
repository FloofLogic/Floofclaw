#ifndef FCLAW_AGENTS_H
#define FCLAW_AGENTS_H

#include <stddef.h>

#include "runtime.h"

/* Native agent buffer caps. Sized for realistic agent output:
 *   stdout — agent's calls JSON, capped well above the largest
 *            payload the runtime accepts (RT_LARGE = 4 KB) and the
 *            largest memory_recalled tail we'd surface (≈50 records).
 *   stderr — diagnostic text written when the agent fails or wants
 *            to leave a breadcrumb. Truncated, never reallocated. */
#define RT_NATIVE_STDOUT_MAX (64 * 1024)
#define RT_NATIVE_STDERR_MAX (8 * 1024)

/* Native agent function. Called synchronously inside the launch path:
 * ctx/step are trusted runtime state, actions is the scheduler's read-only
 * registry, and input_json is the same processor input persisted for every
 * agent. The agent writes its calls JSON to stdout_buf and any diagnostic to
 * stderr_buf, both of which are pre-zeroed by the runtime.
 *
 * Returns:
 *   0                          success — runtime parses stdout_buf
 *   RT_ERR_OUTPUT_TOO_LARGE    stdout would have exceeded stdout_cap
 *   any other nonzero          generic agent failure
 *
 * stderr_buf overflow truncates silently (still NUL-terminated). The
 * runtime treats nonzero returns the same way it treats a script
 * agent exiting nonzero — emit a kernel error event, fail the step. */
#define RT_ERR_OUTPUT_TOO_LARGE 0x4f4c  /* 'OL' */

typedef int (*RtAgentFn)(const RtContext *ctx, const RtStep *step,
                         const RtAgentMeta *meta,
                         const RtActionRegistry *actions,
                         const char *input_json,
                         char *stdout_buf, size_t stdout_cap,
                         char *stderr_buf, size_t stderr_cap);

/* Look up a registered native agent by id. Returns NULL if no native
 * agent is registered for that id. The caller (agent_runner) consults
 * this only when the floop-local agent.json declares executor="native";
 * agent.json is the source of truth for which executor an agent
 * uses. */
RtAgentFn rt_agent_lookup(const char *id);

#endif
