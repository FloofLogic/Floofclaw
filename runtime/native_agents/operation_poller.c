/* operation_poller — deterministic continuation for detached operations.
 *
 * The inbound operation_poll envelope contributes exactly one trusted
 * selector: its opaque handle. Action, task, work revision, context, and
 * terminal state come from the durable operation store. The current action
 * registry supplies the poll argument contract. No model and no controller
 * catalog participate in an already-authorized continuation. */

#include "../agents.h"
#include "../runtime.h"

#include <stdio.h>
#include <string.h>

static int emit_no_calls(char *out, size_t cap) {
  int n = snprintf(out, cap, "{\"calls\":[]}\n");
  return (n < 0 || (size_t)n >= cap) ? RT_ERR_OUTPUT_TOO_LARGE : 0;
}

static int fail_and_block(const RtContext *ctx, const RtOperationSnapshot *op,
                          const char *reason, const char *needed,
                          char *stdout_buf, size_t stdout_cap,
                          char *stderr_buf, size_t stderr_cap) {
  if (stderr_buf && stderr_cap)
    snprintf(stderr_buf, stderr_cap, "%s; %s\n", reason, needed);
  (void)rt_emit_error((RtContext *)ctx, "operation_poller",
                      "%s; %s", reason, needed);
  (void)rt_task_block_unpollable_operation(ctx, op, reason, needed);
  return emit_no_calls(stdout_buf, stdout_cap);
}

int rt_agent_operation_poller_run(const RtContext *ctx, const RtStep *step,
                                  const RtAgentMeta *meta,
                                  const RtActionRegistry *actions,
                                  const char *input_json,
                                  char *stdout_buf, size_t stdout_cap,
                                  char *stderr_buf, size_t stderr_cap) {
  RtOperationSnapshot op;
  const RtActionDef *def;
  JsonRef payload;
  char inbound_handle[RT_SMALL] = "";
  char eaction[RT_MED], etask[RT_MED], earg[RT_MED], ehandle[RT_MED];
  char reason[RT_LARGE], needed[RT_LARGE];
  int n, lookup;
  (void)step;
  (void)meta;
  (void)input_json;
  if (stderr_buf && stderr_cap) stderr_buf[0] = '\0';
  if (!ctx || !stdout_buf || stdout_cap == 0 || !actions) return 1;
  if (strcmp(ctx->event_kind, "operation_poll") != 0)
    return emit_no_calls(stdout_buf, stdout_cap);
  if (json_ref_from_text(ctx->event_payload_json, &payload) != 0 ||
      payload.type != JSON_REF_OBJECT ||
      json_ref_object_get_string(&payload, "handle", inbound_handle,
                                 sizeof(inbound_handle)) != 0 ||
      !inbound_handle[0])
    return emit_no_calls(stdout_buf, stdout_cap);

  lookup = rt_operation_snapshot(inbound_handle, &op);
  if (lookup == 1) return emit_no_calls(stdout_buf, stdout_cap);
  if (lookup != 0) {
    if (stderr_buf && stderr_cap)
      snprintf(stderr_buf, stderr_cap,
               "operation_poller could not read durable operation state\n");
    return 1;
  }
  if (strcmp(op.status, "running") != 0 ||
      strcmp(op.context_id, ctx->context_id) != 0)
    return emit_no_calls(stdout_buf, stdout_cap);
  if (op.work_rev > 0) {
    char task_json[RT_XL];
    if (!op.task_id[0] ||
        rt_task_work_binding_json(op.context_id, op.task_id, op.work_rev, 0,
                                  task_json, sizeof(task_json)) != 0)
      return emit_no_calls(stdout_buf, stdout_cap);
  } else {
    char input_text[RT_LARGE];
    /* Ordinary OpenClaw actions bind to the exact input-message task and
     * deliberately carry work_rev=0. That is a valid durable continuation,
     * not an absent controller binding. */
    if (!op.task_id[0] ||
        !rt_task_reference_is_ready_for_context(op.context_id, op.task_id) ||
        rt_task_input_text(op.context_id, op.task_id,
                           input_text, sizeof(input_text)) != 0)
      return emit_no_calls(stdout_buf, stdout_cap);
  }

  def = rt_action_registry_find(actions, op.action);
  if (!def || !def->managed_operation) {
    snprintf(reason, sizeof(reason),
             "running operation %s references unregistered managed action %s",
             op.handle, op.action[0] ? op.action : "(missing)");
    snprintf(needed, sizeof(needed),
             "fix: restore a registered contract:\"managed_operation\" "
             "action.json for %s before polling",
             op.action[0] ? op.action : "the stored action");
    return fail_and_block(ctx, &op, reason, needed,
                          stdout_buf, stdout_cap,
                          stderr_buf, stderr_cap);
  }
  if (!def->poll_handle_arg[0]) {
    snprintf(reason, sizeof(reason),
             "running operation %s cannot poll %s because %s omits "
             "poll_handle_arg",
             op.handle, op.action, def->manifest_path);
    snprintf(needed, sizeof(needed),
             "fix: add poll_handle_arg naming the action's string poll "
             "handle property to %s",
             def->manifest_path);
    return fail_and_block(ctx, &op, reason, needed,
                          stdout_buf, stdout_cap,
                          stderr_buf, stderr_cap);
  }
  if (json_escape(op.action, eaction, sizeof(eaction)) != 0 ||
      json_escape(op.task_id, etask, sizeof(etask)) != 0 ||
      json_escape(def->poll_handle_arg, earg, sizeof(earg)) != 0 ||
      json_escape(op.handle, ehandle, sizeof(ehandle)) != 0)
    return 1;
  n = snprintf(stdout_buf, stdout_cap,
               "{\"calls\":[{\"name\":\"%s\",\"args\":{\"task_id\":\"%s\","
               "\"op\":\"poll\",\"%s\":\"%s\"}}]}\n",
               eaction, etask, earg, ehandle);
  return (n < 0 || (size_t)n >= stdout_cap)
             ? RT_ERR_OUTPUT_TOO_LARGE
             : 0;
}
