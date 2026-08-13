/* workers — native attempt-scheduling phase for managed background work.
 *
 * Applies the attempt rule for the context's single open work task by
 * comparing integers, never by reading prose:
 *
 *   no attempt recorded                       -> start at current work_rev
 *   attempt running                           -> poll (only on a poll event)
 *   attempt terminal, rev == current work_rev -> stop; do nothing
 *   attempt terminal, rev <  current work_rev -> start one attempt at the
 *                                                current work_rev, feeding
 *                                                the prior attempt's result
 *
 * A task left open after a terminal attempt never restarts by itself:
 * only a newer revision (a new user instruction through the manager)
 * makes it eligible again. That is the no-blocker-retry-loop invariant.
 *
 * The backend is the configured `handler` in this agent's agent.json —
 * an ordinary registered action implementing the managed-operation
 * contract. This phase emits ordinary call intent; the calls flow
 * through the normal action_request -> action_runner path, so every
 * start and poll is visible in the event log like any other call. */

#include "../agents.h"
#include "../runtime.h"

#include <stdio.h>
#include <string.h>

static int emit_calls(char *out, size_t cap, const char *calls_body) {
  int n = snprintf(out, cap, "{\"calls\":[%s]}\n", calls_body ? calls_body : "");
  return (n < 0 || (size_t)n >= cap) ? RT_ERR_OUTPUT_TOO_LARGE : 0;
}

int rt_agent_workers_run(const RtContext *ctx, const RtStep *step,
                         const RtAgentMeta *meta,
                         const RtActionRegistry *actions,
                         const char *input_json,
                         char *stdout_buf, size_t stdout_cap,
                         char *stderr_buf, size_t stderr_cap) {
  char task_id[RT_SMALL] = "", work[RT_LARGE] = "", done[RT_LARGE] = "";
  char external[RT_LARGE] = "";
  long long work_rev = 0;
  (void)step; (void)actions; (void)input_json;
  if (stderr_buf && stderr_cap) stderr_buf[0] = '\0';
  if (!ctx || !stdout_buf || stdout_cap == 0) return 1;
  if (!meta || !meta->handler[0]) {
    if (stderr_buf)
      snprintf(stderr_buf, stderr_cap,
               "workers agent requires a configured handler in agent.json");
    return 1;
  }
  if (rt_task_open_work_snapshot(ctx->context_id,
                                 task_id, sizeof(task_id),
                                 work, sizeof(work),
                                 done, sizeof(done),
                                 &work_rev,
                                 external, sizeof(external)) != 0) {
    return emit_calls(stdout_buf, stdout_cap, "");
  }
  {
    JsonRef ext;
    char handle[RT_SMALL] = "", status[RT_SMALL] = "", excerpt[RT_MED] = "";
    long long attempt_rev = 0;
    int has_attempt = 0;
    int poll_event = strcmp(ctx->event_kind, "operation_poll") == 0;
    if (external[0] && strcmp(external, "null") != 0 &&
        json_ref_from_text(external, &ext) == 0 && ext.type == JSON_REF_OBJECT &&
        json_ref_object_get_string(&ext, "handle", handle, sizeof(handle)) == 0 &&
        handle[0]) {
      has_attempt = 1;
      (void)json_ref_object_get_string(&ext, "status", status, sizeof(status));
      (void)json_ref_object_get_long(&ext, "work_rev", &attempt_rev);
      (void)json_ref_object_get_string(&ext, "result_excerpt", excerpt, sizeof(excerpt));
    }
    if (has_attempt && strcmp(status, "running") == 0) {
      if (!poll_event) return emit_calls(stdout_buf, stdout_cap, "");
      {
        char etask[RT_MED], ehandle[RT_MED], call[RT_LARGE];
        if (json_escape(task_id, etask, sizeof(etask)) != 0 ||
            json_escape(handle, ehandle, sizeof(ehandle)) != 0) return 1;
        snprintf(call, sizeof(call),
                 "{\"name\":\"%s\",\"args\":{\"task_id\":\"%s\",\"op\":\"poll\","
                 "\"op_id\":\"%s\"}}",
                 meta->handler, etask, ehandle);
        return emit_calls(stdout_buf, stdout_cap, call);
      }
    }
    if (has_attempt && attempt_rev >= work_rev) {
      /* Terminal attempt for the current revision: quiescent. The
       * manager decides what the returned text means — answered
       * (complete the task) or blocked (leave it open); the handler
       * never restarts and never judges. */
      return emit_calls(stdout_buf, stdout_cap, "");
    }
    if (!has_attempt && poll_event) {
      /* Stale poll with no recorded attempt: nothing to do. */
      return emit_calls(stdout_buf, stdout_cap, "");
    }
    {
      /* Start one attempt at the current revision. Mechanical
       * concatenation only: complete latest work + completion intent +
       * the prior attempt's recorded result so a follow-up attempt
       * continues instead of starting blind. */
      char text[RT_LARGE * 2 + RT_MED + 128];
      char etext[sizeof(text) * 2], etask[RT_MED], call[sizeof(etext) + RT_MED + 128];
      int n;
      if (excerpt[0])
        n = snprintf(text, sizeof(text),
                     "%s\n\nDone when: %s\n\nPrior attempt result:\n%s",
                     work, done, excerpt);
      else
        n = snprintf(text, sizeof(text), "%s\n\nDone when: %s", work, done);
      if (n < 0 || (size_t)n >= sizeof(text)) return 1;
      if (json_escape(text, etext, sizeof(etext)) != 0 ||
          json_escape(task_id, etask, sizeof(etask)) != 0) return 1;
      n = snprintf(call, sizeof(call),
                   "{\"name\":\"%s\",\"args\":{\"task_id\":\"%s\",\"op\":\"start\","
                   "\"task\":\"%s\"}}",
                   meta->handler, etask, etext);
      if (n < 0 || (size_t)n >= sizeof(call)) return 1;
      return emit_calls(stdout_buf, stdout_cap, call);
    }
  }
}
