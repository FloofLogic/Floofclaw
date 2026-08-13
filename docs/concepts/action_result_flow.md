# Action result flow — the unified pattern

FloofClaw has one general architectural rule for how action results reach LLM
agents: **results return as durable, correlated consequences, never through
hidden same-run artifact pickup.** Ordinary data-returning actions use the
managed-operation contract and return through `operation_result`. A
revision-bound work controller receives each selected action's terminal
result, rejection, or failure as `work_step_result`; managed operations still
return their final value through `operation_result`.

This has been the hard rule since `v0.21.0-stable-floofclaw`. The alternative
"sync-artifact same-run pickup" pattern is fully retired. Completion claims
now travel through durable result and work-outcome events with the exact
correlation that joins them to their evidence.

## Two categories of actions

**Fire-and-forget actions.** The caller doesn't need to reason about
the return value. Success/failure surfaces through `action_failed` if
it breaks; the content of a successful result is irrelevant to the
agent's next decision:

- `message` — reply to the user (`message.send` remains a normalization alias)
- `note_add` — attach a note to an affair
- `affair_open` / `affair_close` — affair lifecycle
- `defer` — schedule the next review
- `task.update` — mark a task state

These execute inline and the caller moves on.

`work` is the fire-and-forget task-admission action. It creates or revises
durable work; the configured handler later invokes a managed worker such as
`manage_codex` or `manage_claude`.

**Data-returning actions.** The caller (or another configured agent) needs
the return value to make a decision:

- `manage_codex` / `manage_claude` — detached research / code execution
- `web_fetch` / `web_read` / `http_call` — fetch or call a URL
- `read_file` — read file content
- `read_dir` — list a directory
- `write_file` — writes are fire-and-forget-shaped semantically, but
  the `text` field ("wrote <path>") is used by later agent turns to
  confirm what happened; treat as data-returning
- `apply_patch` — same shape as write_file
- `bash` — run a shell command; stdout is the data
- Future lookups follow the same pattern

**All data-returning actions declare `contract: "managed_operation"`
in their `action.json`.** Their result flows through the same pipe.

## The event flow

```
User: "read foo.py"
  ↓
Event: user_message
  ↓ (dispatched to chat_manager / main_claw / etc.)
LLM turn #1 (agent configured for user_message):
  Emits: read_file(op:"start", path:"foo.py")
  ↓ (runtime dispatches)
Intrinsic executes → returns { path, text, status:"finished", handle:"<request_id>" }
  ↓ (operation_driver publishes)
Event: operation_result carrying payload.text = file content
  ↓ (dispatched to result_manager / main_claw / etc.)
LLM turn #2 (agent configured for operation_result; it may be the same agent):
  Composes reply: "Here's foo.py: <content>"
  Emits: message + task.update(state:"completed")
  ↓
User sees the reply
```

Two events normally produce two model turns and one message to the user.
From the runtime's perspective this is a clean pipeline. Runs remain the unit
of scheduling and event history; provider calls are recorded separately
inside each run.

Run completion and task completion are separate. A one-pass profile yields
after one event-driven profile pass; it does not silently complete the input
task. OpenClaw keeps the task open while emitting another managed action and
closes it explicitly with the existing `task.update` call when Main Claw
decides its response is final. The legacy
`autocomplete_message_task_on_send` behavior remains an opt-in feature for
other conversational floops.

OpenClaw also configures a pre-commit output contract around that choice. A
task-bearing Main Claw response must contain either the next substantive action
or the final message/completed-task pair. A message-only promise is withheld,
then repaired by the same agent with the exact rejected response and explicit
feedback; no rejected call reaches the event log or delivery path.

A bound work controller may take several such turns. The runtime binds it to
the exact same-context task and `work_rev`, persists each semantic selection
before dispatch, and carries the originating `request_id`, action, and trigger
into the result turn. Intermediate `work_step_result` and `operation_result`
events may request the next selection. Only `work_completed` or
`work_blocked` is terminal for that revision.

## Debuggability as a first-order virtue

Under sync-artifact, one user message could produce one visible "run"
that internally spun through 2-3 LLM calls. You couldn't tell how
expensive a turn was without opening the run's `provider_calls/`
directory.

Under the event-driven pattern, the initial event and a later
`operation_result` are separate runs. That exposes the architectural turns,
but a run can still contain retries or a memory-compaction provider call. For
exact per-run call cost, inspect `provider_calls/`; for the current usage-log
segment aggregate, use `fclaw usage -a`. Do not count run directories.

This is a product virtue: **the runtime doesn't hide compute cost from
you.** Every provider invocation has inspectable request/response artifacts
and usage accounting, while every scheduling turn has an inspectable run.

## The auto-wrap layer

Intrinsic actions in `runtime/action_runtime.c` and subprocess actions
in `actions/*/run.sh` used to write their result JSON without any
managed-op fields. To make the pattern work uniformly, the runtime has
an auto-wrap step:

- `rt_action_wrap_finished_result(result, len, request_id)` in
  `runtime/action_output.c` injects `"status":"finished"` and uses the
  already-stamped durable `request_id` as the handle.
- Called from `rt_action_runtime_complete` for intrinsics and
  `rt_action_runner_complete_job` for subprocess actions.
- Idempotent: if the action already emitted a `status` field itself
  (like `web_read` and `manage_codex` do), the wrap is
  a no-op.

The auto-wrap does not mint another identity. Reusing `request_id` keeps the
finished handle stable across gateway restarts and replay, and prevents a
fresh process from colliding with an operation persisted by an earlier one.

So an action author writing a new managed-op has two choices:

1. **Speak the contract explicitly.** Return
   `{"status":"finished","handle":"<yours>","text":"...", ...}`.
   `web_read` and `manage_codex` are the templates.

2. **Let the wrap handle it.** Return
   `{"path":"foo","text":"wrote foo","other":"fields"}` — auto-wrap
   adds status and uses that request's ID as the finished handle. Enough for
   most cases.

Either works. The wrap is smart enough to skip when the author
already spoke the contract.

## Op arg convention

Every managed-op action supports the common `start` and `poll` operations.
Callers pass `op:"start"` on the first call. `poll` exists for contract
completeness (long-running ops need it), while most actions finish
synchronously and never need polling. Delegate schemas may additionally
offer `reply`, `abort`, or `status`.

Fast actions like `read_file` finish on `start` — they publish
`operation_result` immediately. Slow actions like `manage_codex`
return `"running"` on start and then `"finished"` from `poll` calls
the operation driver arms.

## Floops that use this pattern

- **floofclaw** (chat_manager / review_manager / result_manager).
  Each event kind routes to its own agent via `event_kind:` gate. All
  data-returning tools are managed-ops. The result manager routes evidence;
  affair closure remains a review-manager decision on a later
  `affair_review` event.
- **openclaw** (`main_claw`). Same durable result pattern, with a different
  product choice: the same `main_claw` handles `user_message`,
  `operation_result`, and
  `affair_review`. It may select another managed action after each result and
  continues until it replies or is blocked. The original input task remains
  open while its existing `state.external` record proves a continuation is
  pending; task input and `working_memory` restore the request context.

Both floops share one durable consequence pattern. When adding a new floop,
follow the same shape:

- Declare an explicit event-kind gate for every turn you care about. Different
  kinds may use different agents or intentionally return to the same agent.
- Every data-returning action is a managed-op.
- Result flow: an agent dispatches → `operation_result` → a later configured
  agent turn digests it. Never hidden same-run pickup.

A configured work controller is the generic sequential variant of this
pattern:

- `bind_task: "open_work"` gives it the runtime-selected task and revision.
- `work_consequence` wakes it only for a correlated initial trigger or
  intermediate result.
- One external work lineage has a fixed budget of eight semantic selections.
- Rejected or failed selections may receive repairs up to the controller
  agent's `work_policy.max_repair_attempts`; omission keeps the compatibility
  default of one, and the engine retains an absolute safety ceiling of eight.
- A controller-created `work` revision inherits the lineage and its counters.
  Only a newer external revision resets them.

## When to break the rule

Don't reintroduce hidden same-run pickup. If you want to compose N tool results
into one reply, either:

- Use the `work` pattern (delegate to a subprocess like `manage_codex`
  that composes internally, returns one summary result)
- Or use an OpenClaw-shaped sequence where each tool result triggers a durable
  turn of the same agent and only the final turn sends the reply

Do not add general-purpose same-run task-artifact pickup.

## Successful results and failed-action context

Successful ordinary data returns reach agents through `operation_result`.
Ordinary non-controller managed-action failures and rejections use the same
correlated continuation, allowing the configured agent to retry, choose an
alternative, or explain the block.
Bound controller steps additionally receive their exact correlated
`work_step_result`; this is a durable event path, not artifact pickup. Failed
actions also keep a narrow operator-visible diagnostic on the task record via
`rt_task_append_action_failure`: tool, reason, and message context survive
without creating a second result-return architecture.

Recovery treats the durable start claim conservatively. A selected action
with no `action_started` remains launchable under its stable request id. If an
outside-world action has `action_started` but no provable terminal event,
recovery does not replay it or invent success; it terminates the bound
revision with `work_blocked.reason = "ambiguous_completion"`. This prevents a
cold restart from repeating an unprovable side effect.

If a proposed change needs data to flow from an action to an agent
without going through `operation_result`, stop and confirm with the
user. The default answer is: convert the action to managed-op and
route through the standard pipeline. Only bring same-run pickup back after an
explicit architecture review.

Reason for the guardrail: general result pickup is a hidden coupling. It works
when one agent owns multiple phases within a run, but breaks silently under
event-driven dispatch. Durable `operation_result` turns work for both split
roles and a continuing single agent. The v0.21 refactor retired hidden pickup
after fixing several real bugs it caused (the "web_read regression", the "F8
stall", and the openclaw multi-tool question).

## Related docs

- [`docs/concepts/actions.md`](actions.md) — how to author an action
  (action.json, run.sh, intrinsic).
- [`docs/discord_and_irc.md`](../discord_and_irc.md) — testing
  convention (IRC for probes).
