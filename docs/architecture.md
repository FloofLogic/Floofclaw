# FloofClaw Architecture

FloofClaw is a **transparent agent orchestration runtime**. It composes
multiple focused LLM agents together with heavy-lifting workers (Codex,
Claude Code, and other subprocess research tools) into products, and every step
of that composition is inspectable.

The layer above orchestrates. The layer below (the workers) does the
heavy work. FloofClaw is the layer between them and the outside world
(Discord, IRC, WebSocket, CLI). For the positioning, see
[what_is_floofclaw.md](what_is_floofclaw.md). For the operational tour,
see [comparison.md](comparison.md). This document is the technical
architecture — kernel shape, event model, ownership boundaries.

## The runtime kernel

The runtime kernel does not know whether it is running a chat loop, a
research loop, a memory workflow, or a future scheduler loop. A loop
profile (a `floop`) defines ordered steps. Steps run agents, builtins,
or actions. Agents emit call intent, the runtime normalizes accepted
calls into runtime-owned events, and reducers apply those events to
state.

Every event kind can gate a step. That is how the runtime dispatches
different behaviors to different agents without any subsystem-specific
kernel code. Adding a new event kind (or a new floop that reacts to
one) is a config change, not a kernel change.

The central rule is:

```text
event_log.jsonl is the source of truth.
```

Nothing is considered true unless it is represented as an event in the run log. State files are materialized views. Trace files are diagnostics. Agent outputs and action call files are artifacts. They help explain what happened, but they are not behavioral truth until their effects become events.

## Goals

FloofClaw optimizes for:

- **Observability of compute** — every runtime run is inspectable, and every
  provider call within it has its own request/response artifacts. A run can
  contain retries or memory compaction, so provider-call artifacts and usage
  records — not run-directory counts — are the authority for call cost.
- **Floop-owned composition** — a product may split event kinds across focused
  agents or deliberately preserve one continuing agent across several kinds.
  The loop profile and agent packets make that choice; the kernel does not.
- **Uniform result flow** — every ordinary data-returning action is a
  managed-operation and returns via `operation_result`; a bound controller
  additionally receives exact correlated step consequences. No result reaches
  an agent through hidden same-run artifact pickup. See
  [action_result_flow.md](concepts/action_result_flow.md).
- **Durable perspective** — affairs, tasks, memory persist across
  process restarts and across days. Agents come back knowing what
  they were watching.
- **Explicit ownership** — the runtime owns durable IDs, timestamps,
  event append order, lifecycle transitions. Agents own intent.
  Actions own outside-world effects.
- **Hard separation of concerns** — adapters don't know about agents;
  agents don't know about transport; the kernel doesn't know about
  product behavior.
- **Replayable truth** — the event log is the source of truth. State
  files are materialized views. Replay works from the log alone.
- **Tiny, auditable runtime** — one C binary and one long-lived gateway
  process, with bounded short-lived child jobs and a static scheduler shape.
  Small enough to hold in a human's head or a coding agent's context.
- **Strict action contracts** — `action.json` defines args, contract,
  and exec. Model output that doesn't fit is rejected, not
  interpreted.

The system should be small enough for a human or coding agent to
inspect quickly, but strict enough that model output cannot silently
become runtime truth.

## Stable Envelope Contracts

FloofClaw's high-level envelopes are intended to be stable architecture
contracts, not prompt conventions. Normal feature work should add event types,
reducers, actions, or optional fields instead of changing existing field meaning.
Breaking an envelope requires a versioned spec and tests in the same change.

Canonical contract references:

- [Floof Struct Definitions](concepts/floof-struct-definitions.md) — runtime,
  job, task, artifact, event, agent, model, and action envelopes.
- [Task Feature](concepts/task-feature.md) — task state, task events, artifact
  parts, and A2A mapping notes.
- [Runstate v1.3.8](concepts/runstate-v1.3.8.md) — durable run scheduling
  envelope.

## Separation Rule

Every change must preserve hard separation of concerns.

Do not fix a local bug by making another layer know about it. Do not teach
channel adapters about terminal rendering, UI code about runtime internals,
actions about loop scheduling, agents about transport protocols, or the kernel
about product behavior. That kind of shortcut is a boundary leak even when the
patch is small.

FloofClaw follows Demeter's Law:

- a module may talk to its own state
- a module may talk to explicit arguments it was handed
- a module may call the narrow interfaces it owns or imports by contract
- a module must not reach through another module to its internals

If a fix appears to require cross-layer knowledge, the right move is to add or
adjust a boundary contract. The wrong move is to make unrelated components
coordinate by convention.

## Runtime Shape

Current top-level layout:

```text
floops/      shippable loop+agent bundles
actions/      declared capabilities and effect boundaries
runtime/     tiny C loop kernel
workspace/   event logs, state snapshots, run artifacts
tests/       fast runtime tests and smoke tests for the new architecture
```

Scratch trees may exist under `tmp/`, but they are not the architectural
center for the current runtime. Current behavior lives under `runtime/`,
`floops/`, `actions/`, and `workspace/`.

## Kernel

The kernel is generic:

```c
run_loop(profile) {
  ingest_events();
  reduce_pending_events();

  for step in profile.steps {
    input = build_processor_input(step, event_projection);
    result = run_step(step, input);
    validate_call_intent(step, result.calls);
    append_events(normalize_calls(result.calls));
    reduce_pending_events();
  }
}
```

The kernel owns loop execution only:

- load and validate loop profiles
- create run directories
- append initial events
- run steps in profile order
- validate agent call-output envelopes
- normalize accepted calls into emitted runtime events
- call the reducer after each step
- write minimal trace diagnostics

The kernel does not own product behavior. It does not know what the assistant should care about, which actions are useful, how to speak to the user, or how to prioritize work. Those decisions belong to configured loop phases.

## What the scheduler is not

The scheduler executes a profile and reacts to events. It is not a
judge of whether agents emitted *enough* during an advance. The
contract is narrow:

- run a step
- collect what it emitted
- validate envelope shape and authority
- if it started a job, wait for the result
- otherwise advance to the next step
- when the profile ends with no pending jobs, the run is **done**

There is no semantic check buried inside the scheduler that says "no
message was delivered, so this run failed" or "the kind of event I
personally consider important didn't fire, so this run failed."
End-of-profile is itself a terminal condition — the scheduler walked
the recipe to the bottom, so the run is finished. Whether the
conversation produced a reply is the *agent and profile's* concern,
not the kernel's.

The runtime keeps exactly one loop guard: a full-profile pass cap
(`RT_MAX_PROFILE_PASSES`). If a run keeps cycling — a pass produces
accepted work, the profile rewinds to phase zero, and that never
quiesces — it is failed with `max_profile_passes_exceeded`. That is
not a verdict on agent quality. It is a guard against literal infinite
loops.

In particular:
- a missing message is not a runtime failure by itself
- a failed task-bound action can retain narrow diagnostic context on its task
- an agent emitting nothing is not automatically an error
- end-of-profile with no jobs pending and no accepted work in the pass is **terminal `done`**, never failure

Anything that reads "the runtime should know that X is important and
should fire if X doesn't happen" belongs in the agent, the profile,
or a dedicated reducer rule — never in scheduler control flow. The
scheduler walks; it does not decide.

This is the canonical example of a category of bug we don't want
again: pulling a small piece of product knowledge into a place that
shouldn't have it. When it's tempting, **the boundary contract is
the right place to fix the problem**, not a smarter check in the
kernel.

## Loop Profiles

A floop profile is JSON under `floops/<name>/loop.json`.

Profiles are declarative. They say what steps run and in what order. They do not embed runtime policy.

### `floofclaw`

`floops/floofclaw/loop.json` is the full durable-manager event-gated loop.
It is selectable, while a fresh clone uses the smaller `hello` mock floop by
default:

```text
memory.before -> chat_manager -> review_manager -> result_manager -> dispatch -> workers -> memory.after -> memory.compact
```

One triggering event creates one run and one profile pass. The event kind
selects the responsible manager declaratively:

- `chat_manager` handles `user_message`, replies, opens affairs, and queues
  bounded work.
- `review_manager` handles `affair_review`, records useful observations,
  closes resolved affairs, or defers the next check.
- `result_manager` handles `operation_result` and routes fresh worker or
  tool evidence to the user or an affair.
- `workers` is native and starts the managed-operation handler named in its
  agent configuration; completion returns later through the bus.
- `memory.before`, `memory.after`, and `memory.compact` keep conversation
  memory explicit and inspectable.

Chat input tasks use the retained successful-message autocomplete feature.
Review turns close their bounded input explicitly with `task.update`, including
when the same accepted batch first admits background work; that prior work
intent remains executable after the input transition.

Same-context runs are serialized, failed phases have bounded event retries,
and committed action calls are memoized by signature. Data-returning actions
finish through `operation_result`; no agent role gets a hidden same-run
result channel.

### Affair Watcher

The reactor's affair watcher scans active affairs by
`next_review_at_ms`. When one is due it publishes an `affair_review` bus
envelope, not a synthetic user message. The event carries the affair ID and
routes to `review_manager`; chat memory therefore does not gain fake user or
assistant records.

An `affair_reviewed` transition clears the schedule before dispatch. The
review manager may re-arm it with `defer`, close it with `affair_close`, or
leave it dormant. The watcher skips contexts with an active run. Cadence uses
monotonic time; persisted epoch deadlines are compared through the process's
fixed wall/monotonic anchor.

CLI:

```text
fclaw affair create [-a|-h] --context <id> --manage <text> [--in <duration>]
fclaw affair list [-a|-h] [--context <id>]
fclaw affair pause|resume|close [-a|-h] <affair_id>
fclaw affair review [-a|-h] <affair_id>
fclaw affair schedule [-a|-h] <affair_id> --in <duration>
```

Duration suffixes are `ms | s | m | h | d`. Without `--in`, a new affair
is manual-only.
### `openclaw`

`floops/openclaw/loop.json` is the event-driven chat/tool simulation with
basic memory:

```text
memory.before -> main_claw -> dispatch -> memory.after -> memory.compact
```

`main_claw` is gated to `user_message`, `operation_result`, and
`affair_review`. It may emit one visible progress `message` plus one
substantive action. `dispatch` runs accepted calls, and the operation driver publishes a
terminal consequence as a later `operation_result` bus event. A later run
routes that event back to the same `main_claw`, which may choose another tool,
retry, explain a block, or emit the final user-visible `message`. Detached
operations complete through the bus completion contract — no polling step
exists. There is no same-run action-result pickup phase and no scheduler
judgment about whether the task is done.

The original input task remains open across those event runs. That same task's
input and `working_memory` are projected into each result turn. A continuing
decision emits a substantive action and leaves the task open. A final decision
emits the user-visible `message` plus the existing validated `task.update` call
with `state: "completed"`. A message by itself changes no task state, and
`one_pass` only ends the run created for the current event. The older
`autocomplete_message_task_on_send` feature remains independent for floops
that deliberately want a successful message delivery to complete its input
task.

Main Claw declares the generic agent-boundary
`output_contract.kind: "task_action_or_finalize"`. Before any call is
normalized, appended, dispatched, or delivered, that contract accepts exactly
one of two task-bearing decisions: one substantive action with optional
progress message/working-memory sidecar, or one final message plus the exact
completed `task.update`. A rejected decision remains only in provider and
agent-output artifacts. The same phase is re-run as another ordinary LLM child
job with the exact rejected response and a specific correction; Main Claw
configures two such repairs. Exhaustion fails with
`agent_decision_contract_exhausted`. The scheduler still only walks the floop;
it does not infer whether prose sounds finished.

`memory.before` recalls runtime-owned memory projection:
`conversation_summary`, `recent_summary`, and the canonical `recent` raw
message tail. Model-facing processors receive only the listen-projected state
blocks declared by their `agent.json`.
`memory.after` appends user text, committed non-message tool activity, and the
final assistant reply to
`workspace/memory/memory.jsonl`. It does not decide compaction. After
`memory.after` commits, the runtime checks the configured memory summary
policy and creates `memory_compaction_requested` only when due. The gated
`memory.compact` phase invokes the LLM compactor and the runtime validates
the returned cutoff before atomically updating
`workspace/memory/state/summary_state.json`.

## Managed Operations

Actions may declare `"contract": "managed_operation"` in their
`action.json`: `start(args) -> running(worker_handle)|finished(result)`,
plus a declared completion-deadline policy (`default_timeout_ms`,
optional `max_timeout_ms`). Before the action executes, the runtime
allocates the operation's identity durably: a runtime-owned operation id
(`op_<request_id>`), a secret completion token, and an absolute
deadline, committed as an `operation_started` record with full
correlation and projected into the task's `state.external` with the
starting `work_rev`. `worker_handle` from a running result is recorded
as kill/query metadata only — never identity.

There is no polling. A detached worker reports completion through the
ordinary bus: it publishes a narrow `operation_completed` claim
(operation id + token + bounded result) — via `fclaw operation
complete`, whose capability arrives through the trusted child
environment — and the wake socket knocks a live gateway immediately,
while the durable inbox preserves the claim across a stopped gateway.
Intake validates the claim against the durable operation store BEFORE
any run exists (unknown ids, wrong tokens, duplicates, and stale claims
are rejected inspectably), then admits it as the one result run whose
first behavioral event is the runtime-stamped, self-sourced
`operation_result`. The deadline enters the reactor's poll timeout; on
expiry the runtime submits its own idempotent timeout claim through the
same admission path, producing one `operation_result` with
`status:"timed_out"`. Completion, timeout, and the immediate
finished(result) path all compete through the single reducer-owned
terminal gate in the operation store: at-least-once delivery,
idempotent admission, exactly one authoritative terminal transition.
The detached external process is never a waiting job owned by any run.

## Event Model

Each run writes:

```text
workspace/runs/run_001/event_log.jsonl
```

Minimum event shape:

```json
{
  "event_id": "evt_run_001_000001",
  "ts": "2026-04-30T00:00:00Z",
  "type": "user_message",
  "source": "user",
  "run_id": "run_001",
  "payload": {}
}
```

Current behavioral event families include:

- `user_message`
- `memory_recalled`
- `user`
- `asst`
- `fact`
- `summ`
- `action_request`
- `action_rejected`
- `action_started`
- `action_succeeded`
- `action_failed`
- `task_created`
- `task_updated`
- `task_working_memory_appended`
- `task_completed`
- `task_failed`
- `task_canceled`
- `affair_patch`
- `affair_note_added`
- `affair_counter_bumped`
- `affair_task_linked`
- `affair_reviewed`
- `operation_started`
- `operation_result`
- `work_step_result`
- `work_outcome`
- `work_completed`
- `work_blocked`
- `memory_compaction_requested`
- `memory_compacted`
- `run_started`
- `run_done`
- `run_failed`
- `run_canceled`
- `error`

`user`, `asst`, `fact`, and `summ` are memory record event types. They
are intentionally short, but they do not mean channel-level user or
assistant messages. Channel input remains `user_message`; visible output
is a `message` action success delivery.

Raw agent output is not a behavioral event by default. It is stored under `agent_outputs/`. State snapshots are not behavioral events. They are materialized views.

## Durable Run State

A run is durable runtime work-in-progress. It can advance multiple times through
the same profile. The scheduler uses `workspace/runs/<run_id>/runstate.json` as
the inspectable control record for that work.

The canonical envelope is defined in the official immutable
[Runstate Spec v1.3.8](concepts/runstate-v1.3.8.md).

The envelope/checksum remains canonical. Its checksum-pinned OpenClaw
simulator walkthrough is a historical example of the loop at publication time,
not the current event-driven OpenClaw pipeline described above. Envelope shape:

```json
{
  "run_id": "run_004",
  "context_id": "chat:cli",
  "profile": "openclaw",
  "status": "ready",
  "advance": 0,
  "created_from": { "event_id": "bus_000004", "type": "user_message" },
  "waiting": { "jobs": [], "events": [], "deadline": null },
  "cursors": { "phase_index": 0, "last_advanced_event_seq": null },
  "last_error": null
}
```

V1 scheduling is job-correlation only:

```text
run starts job
-> run.status = waiting_jobs
-> job result includes run_id/job_id/request_id
-> runtime wakes the same run
-> same run advances again from its runtime-owned phase_index cursor
```

Do not introduce fake continuation events, reply modes, task fields inside
runstate, run-control events, global event sequence numbers, or event-window
cursors for this milestone. `cursors.phase_index` is the runtime-owned next
phase to execute; `cursors.last_advanced_event_seq` is reserved and always
`null`. The v1.3.8 runstate spec is checksum-pinned by the
architecture tests; changing it requires explicit user approval.

### Action Request Identity

LLM agents do not emit durable action requests directly. They emit call intent:

```json
{
  "calls": [
    {
      "name": "write_file",
      "args": {
        "task_id": "task_run_042_000002",
        "op": "start",
        "path": "note.txt",
        "content": "hello"
      }
    }
  ]
}
```

The runtime validates the existing task reference when present and stamps the
resulting `action_request` with stable, runtime-owned identity:

```json
{
  "type": "action_request",
  "payload": {
    "request_id": "actionreq_run_042_000005",
    "task_id": "task_run_042_000002",
    "action": "write_file",
    "args": {
      "op": "start",
      "path": "note.txt",
      "content": "hello"
    }
  }
}
```

Runtime action lifecycle events include the same `request_id`:

```json
{
  "type": "action_succeeded",
  "payload": {
    "request_id": "actionreq_run_042_000005",
    "action": "write_file",
    "result": {
      "status": "finished",
      "handle": "actionreq_run_042_000005",
      "path": "note.txt",
      "text": "wrote note.txt"
    }
  }
}
```

Canonical `action_succeeded` / `action_failed` events must stay compact. Large
action payloads belong in `action_calls/` artifacts. If an ordinary action's
`result` object cannot fit in the bounded runtime event buffer, execution fails
loudly with `result_too_large`, an empty result object, and error metadata that
points to the preserved artifact. A managed-operation result gets one narrow
recovery attempt first: if `status` and `handle` can be extracted, it retains
those plus bounded `text` when present and marks the result `_salvaged:true`; the
full output remains in the artifact. If salvage cannot establish the contract,
the same loud failure path applies. No oversized payload is silently accepted.

An action request is pending only if there is no terminal `action_rejected`,
`action_succeeded`, or `action_failed` event with the same `request_id`.
For a revision-bound controller selection, a correlated `work_completed` or
`work_blocked` is also durable terminal proof for that selected request.

This prevents repeated `action_runner` passes from rerunning old work. If a
duplicate already-terminal `request_id` is seen, the runtime appends
`action_rejected.reason = "duplicate_terminal_request_id"` instead of silently
dropping it.

Fresh runtime ids are not a license to repeat side effects. Within one run,
the runtime rejects an equivalent non-message call while its first request is
still in flight. Once the first request is terminal, the runtime memoizes that
recorded outcome under the new request id instead of executing it again.

## Reducer And State-Oriented Agents

The event log is behavioral truth. Reducers own durable projections for runs,
tasks, affairs, operations, work steps, and memory. Agents emit intent; they
never stamp runtime identity or mutate projection files directly.

Task records contain runtime-owned identity, context, kind, input, status,
work, completion criteria, one opaque append-only `working_memory` string,
artifacts, correlation fields, and a monotonic `work_rev`. A work task in
`open` or `working` state is actionable.
Managed-operation attempts compare `work_rev` with the latest attempt so an
unchanged revision is not started twice.

Revision-bound controller selections are projected into
`workspace/memory/state/work_steps.json`. The projection retains the external
lineage, semantic-step and repair counts, and exact task/revision/request/
action/trigger correlation. It is rebuildable from committed event logs; it
is not an independent authority.

Task creation and update calls cross the same validation boundary as every
other action. Unknown task references, runtime-owned identity fields, and
invalid state transitions are rejected before durable append. Completed tasks
are archived only after context quiescence.
## Durable Memory

Canonical durable memory lives in one append-only file:

```text
workspace/memory/memory.jsonl
```

Each line is a flat AI-readable JSON object:

```json
{"type":"fact","text":"The user prefers concise replies.","ts":"2026-05-04T00:00:00.000Z","run_id":"run_001"}
```

Allowed memory record types:

- `user`: user text worth retaining as conversation memory
- `asst`: assistant text worth retaining as conversation memory
- `fact`: durable conclusion, correction, preference, or open loop
- `summ`: append-only summary record emitted by the memory compactor, with `text` and a `through` cutoff identifying the window it summarized

Memory-control event types are separate from the append-only records:

- `memory_compaction_requested`: runtime-created request containing the exact
  compaction window and cutoff selected from memory policy
- `memory_compacted`: validated compactor result; reducer updates
  `workspace/memory/state/summary_state.json`

In profiles with memory phases, only an agent granted the `memory` capability
may emit memory records. The shipped OpenClaw and FloofClaw profiles grant it
to their `memory` agent. Runtime enforces this in
`floops/<floop>/agents/<id>/agent.json`; prompts are not trusted for memory
authority. The fresh-clone `hello` loop intentionally has no memory phase.

`memory.before` reads `summary_state.json` plus a complete-record recent tail
of `memory.jsonl`, bounded to 1 MiB, and emits a `memory_recalled` event for the
current run.
`memory.after` emits only the existing `user` and `asst` record types. A
committed non-message tool terminal is rendered as an `asst` transcript line,
so it follows the same projection and compaction path without another store or
role schema.
The runtime then checks `compact_after_messages` and `compact_after_tokens`.
If due, it selects the compaction window, emits `memory_compaction_requested`,
and lets the visible gated `memory.compact` phase summarize that exact range.

## Agents

Agents are executable loop phases under:

```text
floops/<floop>/agents/<id>/agent.json
floops/<floop>/agents/<id>/prompt.md
floops/<floop>/agents/<id>/run.sh
```

Current shipped roles include native memory and worker phases, the
`chat_manager`, `review_manager`, and `result_manager` LLM phases,
OpenClaw's single `main_claw` phase, and the native fast-loop fixture.

Every ordinary script or LLM agent receives only the state blocks declared in
its `listen` list and emits one call-intent envelope:

```json
{"calls":[{"name":"work","args":{"work":"write GREATS.md","done_when":"GREATS.md exists"}}]}
```

The runtime validates the allowlist and action schema, then stamps event,
request, run, task, and source identity. Agents cannot supply those fields.
A configured `handler` on a native worker agent names the managed-operation
action it dispatches; the kernel treats the binding as opaque configuration.

At scheduler startup, every loaded registry action receives a process-local
numeric slot and each agent's configured `actions` array resolves to a 64-bit
authorization mask plus an exact bootstrap presentation order. Explicit
action ids are both allowed and presented. The `all_actions` sentinel fills
the mask and expands in place to every loaded authoritative action contract.
Expansion is deliberately ordered and non-deduplicating, so explicit entries
overlapping `all_actions` remain visibly duplicated until the agent
configuration is corrected. The ordinary `action_list` action may list only
ids in the resolved mask and return one selected authoritative manifest.
Durable records keep string action ids because numeric slots may change after
restart. The engine does not rank or automatically select actions.

An agent configured with `bind_task: "open_work"` receives one exact
same-context open task and revision selected by the runtime. With a
`work_consequence` gate, it is invoked only for a correlated work consequence
and may not replace the trusted task binding. A trigger-bearing selection is
committed synchronously before dispatch. Each external work lineage permits
eight semantic selections. The controller's agent packet declares
`work_policy.max_repair_attempts` to choose how many repair selections may
follow rejected or failed actions; omission defaults to one, and the engine
enforces an absolute ceiling of eight. A controller-selected `work` revision
inherits those counters; a new external revision starts a new lineage.
Exceeding either the semantic budget or the configured repair budget appends a
correlated `work_blocked` without another model call.

The retained `conversational_payload_only` mode serves the companion floop.
It projects a small conversation request and accepts `{"message":...}`; the
runtime maps the reply back through the normal message action. All modern
event-gated managers use the ordinary `{"calls":[]}` envelope.

LLM response artifacts keep provider bytes and normalized runtime events
separate:

```json
{
  "runtime_meta": {
    "agent": "chat_manager",
    "step": "chat",
    "normalized_runtime_events": []
  },
  "provider_response": {},
  "model_text": {}
}
```

Executors are `native`, `llm`, and `script`. Hidden side effects are
forbidden for all three; only appended runtime events become behavioral truth.
## Actions

Actions are the runtime's capability and effect boundary. Most agents request
them by emitting tool calls in the `{"calls":[]}` output envelope. A retained selected-task
compatibility processor emits the smaller `{"tool":...}` envelope and the
runtime wraps it into the same internal call path; current shipped floops do
not depend on it. The runtime maps accepted calls into durable
`action_request` events.
The runtime-owned
`action_runner` resolves the action,
launches it, records subprocess call artifacts, and appends canonical `action_started` and
`action_succeeded` / `action_failed` events. Pre-execution refusals append
`action_rejected`.

For a revision-bound controller, an action result, rejection, or failure is an
intermediate consequence: the runtime publishes a correlated
`work_step_result` (or `operation_result` for a managed operation) that may
wake the controller again. `work_completed` and `work_blocked` are terminal
decisions and route through the work-outcome path instead. Exact
`request_id`, task, revision, action, and trigger correlation—not prose or
artifact discovery—joins these turns.

Successful data-returning actions do not create a hidden task-artifact result
channel. Managed-operation success returns through a later
`operation_result` run. Ordinary non-controller managed-action failures and
rejections use the same correlated continuation so the calling agent can
retry, choose another action, or explain the block. A failed task-bound action
instead follows the controller consequence path and may append one narrow
runtime-owned diagnostic artifact (tool, reason, and error message) so the
failure remains inspectable; it is not model-facing success data.

The active structure is:

```text
actions/<area>/<id>/action.json
actions/<area>/<id>/run.sh   # subprocess actions only
```

`actions/` is scanned recursively at startup; every directory holding an
`action.json` is loaded. There is no registration list, so copying a directory
in installs it. `action.json`'s `id` is the identity and must be unique across
the tree — a collision fails startup naming both manifests.

See [Actions](concepts/actions.md) for the practical "how to make an action"
guide and the current `action.json` contract.

### `message` is an action

User-visible output is not a special runtime escape hatch.

When an agent wants to speak, it emits:

```json
{"calls":[{"name":"message","args":{"message":"Done."}}]}
```

The runtime stamps that intent into a durable `message` action request; the
normal `action_runner` executes it and records an `action_succeeded` event.
Channels and CLIs should deliver visible output from message deliveries, not
from raw agent text.

## Builtins

Initial builtin:

- `action_runner`

The action runner:

- tracks pending requests in the run's embedded pending-action queue
- picks the next not-yet-started `action_request`
- validates the request against the loaded `action.json` contract and agent allowlist
- invokes the registered action subprocess or runtime function
- writes call artifacts under `action_calls/` for subprocess actions
- synchronously appends `action_started` before an effect may begin
- appends a compact canonical `action_succeeded` or `action_failed`
- records narrow task-bound failure diagnostics when an action fails
- reconciles committed controller selections and terminal work outcomes after
  restart

Agents do not directly invoke actions. They request actions by emitting calls.

## Run Artifacts

Each run writes:

```text
workspace/runs/<run_id>/
  event_log.jsonl
  runstate.json
  state.json
  trace.jsonl
  agent_outputs/
  action_calls/
  provider_calls/    # only when an LLM-backed phase ran
  media/             # verified local inputs for a selected media call
```

Meaning:

- `event_log.jsonl`: behavioral truth
- `runstate.json`: durable scheduler/recovery envelope
- `state.json`: materialized state view
- `trace.jsonl`: diagnostics
- `agent_outputs/`: raw sequence-prefixed agent inputs/outputs/stderr
- `action_calls/`: raw action inputs/outputs/stderr
- `provider_calls/`: numbered provider request/response/meta artifacts when present
- `media/`: bounded private attachment files materialized by the isolated LLM
  child; absent from text-only runs

This layout is intentionally AI-readable. A coding agent should be able to inspect a failed run by opening files, not by attaching a debugger or guessing from prompt prose.

Workspace ownership is intentionally narrow. `workspace/runs/` and
`workspace/memory/state/tasks*.json*` must describe the same workspace
generation. If run directories are reset while task state remains, the next
run id can collide with existing `task_<run_id>_*` records. Synchronous input
now refuses before publishing when the next run id would collide and tells the
operator to run `./bin/fclaw clear -h --yes` or restore the matching run
directories. The reducer is not expected to repair mixed-tenancy state.

## Replay

Replay is the main correctness test for the architecture.

Given:

```text
workspace/runs/run_001/event_log.jsonl
```

the runtime should reconstruct the same final reduced state.

Trace is not required for truth. Agent artifacts are not required for truth. Action call artifacts are not required for truth. They explain how the log was produced, but the event log is enough to replay behavior.

## Ownership

Mutation rights are explicit:

- kernel: run creation, profile execution, event append, step sequencing
- reducers: per-run state reduction, durable memory logs, durable task state
- agents: propose calls only
- actions: perform outside-world effects and return structured results
- action runner: execute pending action requests and record invocation lifecycle
- trace: diagnostics only
- profiles: ordered step configuration only

If a layer mutates another layer's state directly, that is a boundary leak.

## Programming Style

FloofClaw should stay small by making state ownership obvious, not by hiding complexity behind abstractions.

Project style:

- prefer boring code over clever architecture
- avoid framework dependencies unless the standard library cannot reasonably do the job
- avoid a generic abstraction until there are at least two real callers
- prefer small files with one job
- prefer small, single-purpose functions over large procedural blocks
- keep feature toggles explicit and localized
- delete compatibility glue quickly once the new path owns the behavior
- do not pass broad module-wide structs when a narrower DTO or function argument set is enough

Generalization rule:

1. first implementation: direct
2. second implementation: copy if the shared shape is still unclear
3. third implementation: extract the shared shape

LOC is controlled by ownership and boundaries, not by compressing logic.

## Memory And Hot Paths

The runtime should use C like a small systems glue language.

Rules:

- pre-allocate hot-path memory pools at startup where the lifetime and capacity are known
- never allocate in critical frame, update, or hot loops without a measured reason
- use fixed-capacity queues and buffers for hot paths where feasible
- never let unbounded caches grow without an explicit eviction or replacement policy
- separate persistent state from per-step scratch memory
- clear or reset scratch memory by scope, such as `begin_step_scope` and `end_step_scope`, not piecemeal throughout hot code
- keep allocation ownership visible at the function boundary

If a hot path introduces heap allocation, document why and add coverage that exercises the bounded case.

### What's already enforced (firmware-style discipline)

The runtime has a measured per-event allocation budget: the native no-op
boundary test permits at most 40 mallocs per event and requires zero reallocs
and zero strdups. Persistent scheduler structures still use static ownership:

- `RtRun` slots: pool of `RT_MAX_ACTIVE_RUNS` (16) embedded in
  `RtScheduler`. Reused across the gateway lifetime; the ~206 KiB
  struct never goes through the allocator.
- `RtPendingAction` queue: embedded `[RT_MAX_PENDING_ACTIONS]` array
  per run. No heap-backed growable queue. Hitting the cap emits a
  loud `error` event — never silent truncation.
- Pending-action `args_json`: one heap-owned compact string per nonempty
  request, freed at terminal handling or run-slot reset. The embedded queue
  bounds the number of live payloads; this allocation is explicit and measured.
- `RtProfile`: loaded once at scheduler init, runs borrow a pointer.
- Agent metadata (`executor`, `model.ref`): preloaded by walking the
  loop's profile at init. No per-launch read of `agent.json`.
- Inbox dirent names: stack `char names[1024][64]` in
  `bus_drain_inbox`. No `strdup` per readdir entry.

What's measured: `runtime/support/heap_guard.{h,c}` provides explicit
counter wrappers (`fc_xmalloc`, `fc_xstrdup`, …); the gateway prints
the per-N delta at shutdown and integration tests enforce the budget. See
[Heap budget](performance/heap-budget.md) for the per-event budget,
the measurement caveats (`ps RSS` is unreliable on macOS — use Mach
`physical_footprint`), and the current static-shape measurements.

### Boring beats clever

When two designs work, the simpler one wins:

- A fixed array beats a growable queue if the cap is known.
- A pointer to scheduler-owned config beats a per-run copy.
- A loud error on overflow beats flexible-but-mysterious truncation.
- An apply-at-append projector beats a periodic event-log scanner.
- A new reactor module beats a new branch in the reactor.

If a design can't be explained as one of those, look harder before
committing to the cleverness.

See [Principles](concepts/principles.md) for the full set of rules
that the rest of this document is an instance of.

## Dependencies

Dependencies are one-way:

```text
orchestration -> services -> support
```

Rules:

- orchestration may depend on services
- services must not depend on orchestration internals
- support utilities must remain leaf code
- tools, renderers, backends, transports, and IO layers must not mutate engine state directly
- parsing and validation should be centralized at the module boundary rather than duplicated across callers

Lower layers can return facts, results, errors, or events. They cannot advance runtime policy.

## No Fuzzy Handoffs

Every boundary uses a structured contract:

- profile step -> typed step object
- kernel -> listen-projected processor input JSON such as `{ "tasks": { "active": [] } }`
- script/LLM processor -> `{ "calls": [...] }`
- processor call intent -> normalized appended event log entry
- action call intent -> validated existing task reference when task-bound, `action`, `args`
- runtime action request -> `request_id`, `action`, `args`
- task event -> task reducer state update
- subprocess action exit status plus trusted stdout body fields `result` and
  `error`, mapped by the runtime to `action_succeeded` or `action_failed`
- reducer -> state snapshot

Free-form text may exist inside payloads, prompts, summaries, messages, or artifacts. It must not be the control surface between runtime layers.

## Transition Events

Every major state transition should produce a canonical event or trace diagnostic.

Behavioral state transitions belong in `event_log.jsonl`. Diagnostics belong in `trace.jsonl`.

Current behavioral examples include `run_started`, `action_request`,
`action_started`, `action_succeeded`, `action_failed`, and exactly one terminal
`run_done` / `run_failed` / `run_canceled`. Phase entry, timing, and other
explanatory details are trace records rather than behavioral events. The
important rule is that truth-changing transitions are visible, ordered, and
replay-compatible without turning diagnostics into state.

## Boundary Preservation

At module boundaries:

- preserve raw arguments before validation or normalization
- record parsed or normalized forms separately when useful
- classify failures before recovery or retry
- never swallow tool, IO, backend, or transport failures
- keep real tool/action errors distinct from runtime plumbing errors
- reject malformed contracts loudly unless a documented compatibility rule says otherwise

The runtime may recover, but recovery must not erase the original failure.

## Retry And Timeout Policy

Retries must be bounded.

Rules:

- every retry loop has a configured max attempt count
- hard failures stop after the limit
- async operations have timeout and latency budgets
- retry or alternate-path behavior is explicit and traceable
- retry state is visible through trace or events

No background path should be able to wait forever without an observable timeout state.

## Concurrency

Default assumption: single-thread ownership.

Rules:

- one owning module writes each mutable data structure
- shared mutable state requires synchronization or a documented single-thread ownership rule
- side effects must be gated behind explicit phase checks, for example `state == RUNNING`
- lower layers do not mutate shared runtime state directly

If concurrency is added, the ownership model must be documented before the code lands.

## LLM Integration Direction

The current runtime does not require every agent to hit an LLM, and agents that
*don't* need a model should not pay subprocess overhead just to participate
in the loop.

The executor model — see [Executors](concepts/executors.md) for the
full version:

| executor | use for | mechanism |
|---|---|---|
| `native` | runtime-owned pure phases | C function in `runtime/agents.c`, called inline, no fork+exec |
| `llm` | prompt-backed model phases | `fclaw internal agent-llm` child job; runtime renders prompt + generated action docs |
| `script` | dev / experiments / external adapters | fork+exec `floops/<floop>/agents/<id>/run.sh` |
| `action` | outside-world side effects (not an agent executor) | isolated subprocess with caps + timeout, or runtime intrinsic for trusted ones like `message` |

Anything FloofClaw ships inside a floop and can be deterministic should be
`native`. Model-backed phases use `llm`. `script` is the escape hatch.

Whichever normal executor runs the processor, the output is still a JSON object
with `calls`:

```json
{ "calls": [] }
```

Prompt-backed LLM agents may use the tiny runtime-owned prompt variable forms
`@{bot_name}`, `@{now}`, and `@{user}`. The runtime reads
`floops/<floop>/agents/<id>/prompt.md`, renders only those allowlisted
variables — `@{bot_name}` from `config/floofclaw_config.json`, `@{now}` as the
local wall clock, `@{user}` as the operator-curated standing user facts in
`config/user.md` (absent file renders empty; both files are reread per run, so
edits land on the next run without a restart) — then appends the
listen-projected processor input JSON. Unknown variables, malformed `@{...}`
references, or
oversized rendered prompts fail the agent before any provider call. This is a
fixed substitution pass, not a template language.

A media-bearing `user_message` follows the same selected-input rule. The
channel publishes a compact reference to a private, content-addressed generic
manifest; only an LLM agent that listens to that triggering event receives the
explicit manifest path. Its child downloads bounded inputs to run-local files
under one aggregate 30-second deadline and passes a provider-neutral
`LlmRequest` containing text plus ordered local file descriptors. The channel
adapter owns its transport-specific source authorization; the generic
manifest/materializer boundary knows only canonical HTTPS and never reaches
back into Discord. Provider serializers own their image/file/audio/video wire
parts and reject unsupported MIME pairs. Gemini inline serialization also
rejects a body at or above 20 MB before provider I/O, leaving Files API upload
as a later implementation behind the same local-file descriptor. The
scheduler never owns attachment bytes or base64. This request contract is
independent of the HTTP response mode, so later response streaming does not
change event selection or media ownership.

The provider response (for LLM agents) is not truth. It becomes truth only
after the runtime validates the model call intent, assigns durable runtime ids,
and appends normalized events to `event_log.jsonl`.

An LLM agent may declare this bounded semantic output contract in
`agent.json`:

```json
{
  "output_contract": {
    "kind": "task_action_or_finalize",
    "repair_attempts": 2
  }
}
```

This is pre-commit agent-output validation, not floop retry and not JSON
cleanup. Each repair is a separately recorded provider call under the same run
and uses the normal asynchronous agent-job path and timeout. Run-local
`.rejected.json` evidence states that the rejected response was neither
executed nor delivered.

Bounded syntactic repair for LLM JSON envelopes is a quarantined boundary
cleanup step, not a parser mode and not contract repair. Raw model text first
goes through strict JSON/call validation. If that fails,
`runtime/fjson_repair/` performs fixed-buffer syntax repair for LLM output only:
fences/prose, missing commas, bad closers, trailing commas, Python-ish literals,
comments, raw string controls, and missing final delimiters. The repaired
candidate must still pass the strict parser and model-envelope contract
validator before anything becomes runtime truth. Repair is never used
for user envelopes, profiles, config, raw action stdout, or canonical event logs.

## Gateway, Bus, And Channels Direction

Gateway, bus, and channel support should sit on top of the event model, not beside it.

Target responsibilities:

- bus moves event envelopes and diagnostics
- gateway runs configured loops as a service
- channels convert transport messages into `user_message` events
- channels deliver approved `message` deliveries

Channels must not call agents or actions directly. They should submit events and observe outputs.

### Runtime owner boundary

No command path creates runs directly. Only the active runtime owner creates
or wakes runs.

The owner may be the long-running gateway process, or an embedded
single-process owner temporarily hosted by `fclaw run` when no gateway is
running. That is a process-shape difference only; the semantics are
the same:

```text
adapter / CLI / test
  -> submit event
  -> wait or observe delivery

runtime owner
  -> drains bus events
  -> owns run IDs
  -> owns scheduler/run state
  -> owns job dispatch
  -> emits deliveries
```

The gateway's floop is part of that ownership boundary. A client that omits
`--floop` follows the live owner. A matching explicit value asserts that the
expected gateway is active; a conflicting value is rejected before bus
publication with a `gateway reload -h --floop <name>` fix. Clients never select a
different floop for one event behind the gateway's back.

Forbidden shape:

```text
CLI command -> create run directory -> run loop directly
```

## Gateway Execution Model

The gateway is a **single-process, single-thread reactor**. Long-running channel adapters (IRC, Discord, WebSocket) and internal subsystems (jobrunner, bus_intake, runtime) are all in-process, cooperative, nonblocking state machines registered with the reactor's generic `FcReactorModule` interface. There are **no pthreads** and **no long-running adapter subprocesses**. The reactor `poll(2)`s every module's fds plus its own timers each tick and dispatches `on_fd` / `tick` callbacks.

The PID and floop files under `.fclaw/run/` are advisory control records.
Gateway identity also requires the runtime-owner file lock held for the
process's complete lifetime, so an unclean exit followed by OS PID reuse cannot
make an unrelated process block gateway recovery.

See [Gateway Reactor](concepts/gateway-reactor.md) for the module list, tick order, and the full state-machine details.

### Clock domains

`fc_now_ms()` is process-local monotonic time. Reactor poll deadlines,
channel reconnect/heartbeat timers, job timeouts, SIGTERM-to-SIGKILL grace,
run-duration markers, operation scan cadence, and janitor cadence use that
domain. Wall-clock corrections cannot delay or stampede them.

The two durable schedules, `next_review_at_ms` and the managed-operation
`deadline_ms`, remain Unix-epoch values because they must survive a restart. On first use the
runtime captures one process-wide `{wall, monotonic}` anchor. While that process lives,
durable deadlines are scheduled and compared against wall-shaped time derived
only from monotonic elapsed time. A restart intentionally takes a fresh anchor
from the then-current wall clock.

Actual wall time is reserved for values whose meaning is civil time or age:
event/trace/rejected-envelope/LLM-usage log timestamps, bus/channel log
timestamps, task/affair `created_ms` and `updated_ms`, managed-worker
`last_checked_ms`, provider daily-limit dates, and janitor age-based retention
cutoffs. These fields are persisted for humans or compared to persisted
file/event ages; they are not process deadlines.

The single-thread rule applies to **long-running** adapters. Agents and actions still run as **short-lived subprocess jobs** owned by the gateway's in-flight job table. An agent-job child is allowed to do blocking I/O internally (including blocking libcurl for HTTP LLM calls); the gateway only watches its `(pid, deadline_ms)`.

`ps aux` shows one bot-labelled gateway process at idle, derived from
`bot_name` in `config/floofclaw_config.json`. During active runs, brief
agent/action children appear and exit. There are no separate supervisor or
adapter processes.

### Job model

Each loop step that needs external work becomes a job:

```c
typedef enum { JOB_AGENT, JOB_ACTION } JobKind;
typedef enum { JOB_RUNNING, JOB_FINISHED, JOB_FAILED } JobState;
```

The runtime reactor's ready-run stepping path may only:

1. Advance work that's immediately ready (kernel-internal, in-memory state transitions).
2. Launch external jobs (agent/action subprocesses).
3. Poll in-flight jobs via `waitpid(pid, &status, WNOHANG)`.
4. Drain finished jobs: read captured output, validate, append events, advance the loop.

It must **never** call a blocking LLM function, run an agent/action subprocess synchronously, or open an HTTP socket. There is no `JOB_LLM_HTTP` in v1 — LLM-backed agents run inside the same agent-job child as script agents, and the agent child invokes blocking libcurl easy mode internally.

### Pipe handling

Job stdin is a file. Job stdout/stderr go through **pipes**: the parent owns nonblocking read ends, registers them with the reactor pollset via the jobrunner module, drains them on `POLLIN`/`POLLHUP`, and tees the bytes into `out_path`/`err_path` so callers reading the artifacts see the same files as before. A job is `JOB_FINISHED`/`JOB_FAILED` only when `exited && stdout_eof && stderr_eof` — no byte is left in the kernel buffer at retire time. Output caps (8 MiB stdout / 1 MiB stderr default) are enforced as bytes drain; over-cap triggers `SIGTERM` with `killed_for_output_limit`.

The reactor only ever calls `waitpid(WNOHANG)`. No blocking waits, no `wait()`.

### Shutdown protocol

On `SIGINT`/`SIGTERM`, the reactor loop stops before any further network or
intake callback can run. Shutdown hooks execute in registration order. The
runtime hook asks the jobrunner to terminate and reap active children within a
three-second grace window (SIGTERM, then SIGKILL for survivors), then appends
`run_canceled` for remaining runs and frees their slots. Later channel hooks
send IRC `QUIT` / close frames. Finally the gateway releases sockets and owner
files and exits. There is no dead-letter list or persistent shutdown queue.

In-flight jobs either finish during the grace period or are terminated. Their
runs receive a durable `run_canceled` terminal during graceful shutdown. A
hard crash is different: on cold start the recovery pass repairs a torn final
event record, rehydrates nonterminal runs, recommits claimed inbound work when
needed, and reconciles unfinished effects. Committed entries in the runtime
delivery ledger remain exactly-once. A request with no committed
`action_started` claim remains launchable under its stable request id. A
started outside-world action with no provable terminal outcome is never
replayed and never inferred successful: recovery appends a correlated
`work_blocked` with reason `ambiguous_completion`. Declared local actions may
be replayed; action authors must therefore classify `outside_world`
honestly. A remote channel send is not a transactional exactly-once boundary,
though the committed message-delivery ledger remains the recognized
reconciliation exception.

No parent-death signal is configured for job children. A hard `kill -9` of the
gateway can therefore leave an already-running child alive until that child
finishes or hits its own limits; cold-start recovery reconciles the durable run
and action state, but cannot retroactively enforce the dead parent's deadline.

### Concurrency

- **Agent jobs may run concurrently.** Two channel users firing user_messages in parallel both progress.
- **Action jobs are serialized — one at a time across the entire gateway.** Conservative v1 rule that prevents two actions from racing on `workspace/memory/`, file writes, or external state. Future `action.json` lock scopes are deferred until the single-action bottleneck becomes a measured problem.

### DNS

Outbound adapters call blocking `getaddrinfo` on connection attempts. A failed
lookup is narrated and enters the shared reconnect backoff; a hung resolver
can stall the reactor for the platform resolver timeout, a known and accepted
tradeoff because a bot without DNS is down anyway. No resolver subsystem is
planned.

### Run lifecycle events

The kernel emits four lifecycle event types per run, written to `event_log.jsonl`:

- `run_started` — emitted right after `rt_create_run`, payload `{run_id, origin_event_id}`.
- `run_done` — emitted on normal loop completion.
- `run_failed` — emitted when a step or the reducer errors, payload includes a short `reason`.
- `run_canceled` — emitted for explicit cancellation, including graceful
  gateway shutdown of an active run.

Adapters use these as terminal signals (e.g. typing-indicator stop conditions on Discord).

### Stable IDs and delivery contract

Every bus envelope and event_log entry carries a top-level `event_id`. Inbound
`user_message` events stamp `origin_event_id` (the bus envelope's id),
`channel`, `adapter_id`, optional reply-routing `ref`, and optional generic
`media` manifest reference into their payload so routing and selected model
input survive the run. Signed source URLs and attachment bytes are excluded
from both records. When `action_runner` produces an `action_succeeded` event
for the `message` action on a channel-originated run, it stamps a `delivery`
block onto the success event:

```json
{
  "delivery": {
    "delivery_id": "deliv_<run>_<seq>",
    "origin_event_id": "evt_...",
    "channel": "irc",
    "adapter_id": "irc-main",
    "ref": { ... },
    "text": "..."
  }
}
```

Adapters consume only delivery records whose `delivery.channel` and `delivery.adapter_id` match themselves.

### Replay protection

`workspace/logs/bus.jsonl` and each run's `event_log.jsonl` are durable runtime
records. Outbound delivery and narration tailers initialize their in-memory
cursors at the current end of their respective logs, so adapter startup does
not resend historical lines. New runtime delivery records are deduplicated by
the durable delivery ledger. That ledger is the end of the exactly-once claim:
an adapter's external network send and its local cursor advance are not one
transaction, so a crash across that boundary can still duplicate or lose an
external send.

## Current CLI

Current CLI:

```bash
./bin/fclaw run -h --text "hello"
./bin/fclaw run -h --floop openclaw --text "hello"
./bin/fclaw run -h --floop floofclaw --text "hello"
./bin/fclaw replay -h workspace/runs/run_001/event_log.jsonl
./bin/fclaw gateway start -h --floop floofclaw
./bin/fclaw -i -h  # uses the running gateway's floop
```

Every public command defaults to agent mode: omitted output mode and explicit
`-a` both produce compact JSON or JSONL. `-h` selects formatted, colored human
output. This is a presentation boundary only; it does not change event intake,
runtime ownership, scheduling, or durable truth. The private `internal`
child-process interface is not part of this public CLI contract.

Default floop:

```text
hello
```

The default is read from `default_floop` in
`config/floofclaw_config.json`; a missing, malformed, or empty value uses the
compiled `floofclaw` fallback when a new runtime owner is selected. Once the
gateway is live, clients that omit `--floop` follow its active floop. Passing
the same value explicitly is an assertion; a conflicting value fails before
publication and directs the operator to `gateway reload -h --floop <name>`.

Interactive mode, gateway mode, channel adapters, trace viewing, and
LLM-backed agents are all implemented on the current runtime-owner path.

## Tests

Current architecture gates:

```bash
make test
make robustness                      # major/deep-engine gate; full target currently requires macOS
FCLAW_LIVE_SMOKE=1 make live-smoke   # opt-in live provider path
```

`make test` is the only routine gate. `make robustness` is the fail-fast
ASan → UBSan → small-stack → fuzz → chaos → disk-full → thrash gate used
before a major version tag or after deep engine surgery; see
[Testing](concepts/testing.md) for the binding two-tier policy.

The unit and integration layers run in isolated temporary copies, and their C
runners refuse before teardown or fixture writes unless the wrapper's
physical-root contract is present. The integration gate SIGKILLs a real
fixture writer in its copy and proves the checkout's Git status remains
unchanged. The smoke also uses an isolated copy, mock model responses, and a
committed local `example.com` fixture. The complete `make test` target is
hermetic and offline; it never calls a live model provider or public host.

Current smoke tests:

```text
tests/smoke_lookup_and_poem.sh
```

The tests should preserve these guarantees:

- profiles load and validate
- runs create event logs and artifacts
- repeated action runner passes do not rerun completed action requests
- message output is a normal `action_succeeded` event
- reducer output can be replayed from event truth
- durable memory changes happen through events and reducer output

Test style:

- keep `make test` centered on 32 unit checks, 139 integration checks, and one
  representative hermetic, mock-provider-backed smoke, with focused shell
  probes for public CLI, app, recovery, and portability contracts
- test contracts rather than implementation trivia, using unit/integration
  boundary tests for subsystem happy and failure paths
- add another end-to-end smoke only when it protects a distinct product path;
  do not turn prompts or every fixture shape into smoke contracts
- write new runtime/tests/tooling in C or bash; existing app-local
  Python/JavaScript and the IRC probe are grandfathered
- keep replay tests close to event semantics

## Contributor Rules

Before changing runtime behavior:

1. Identify which boundary owns the behavior.
2. Add or update a unit/integration boundary test; add smoke coverage only for
   a distinct end-to-end product path.
3. Preserve raw artifacts for debugging.
4. Keep event log semantics replayable.
5. Do not make a snapshot or trace file canonical.
6. Do not let agents or actions mutate durable runtime state directly.
7. Keep support utilities leaf-only.
8. Keep the visible invariants section current.
9. Remove patterns that violate these rules instead of documenting around them.

If a change cannot be explained as events flowing through a profile and reducer, it probably does not belong in the kernel.

## Invariants

These invariants should stay visible and current:

- `event_log.jsonl` is behavioral truth
- state snapshots are materialized views, not truth
- trace files are diagnostics, not truth
- agents emit intent only
- actions are the only model-requestable effect boundary
- LLMs describe desired effects as calls; the runtime creates authoritative ids/events
- action execution is request/terminal-outcome paired by `request_id`
- action requests are pending only when no matching terminal action event exists
- reducer applies durable state changes
- agents propose call intent but do not write durable state directly
- `message` is a normal action
- channels submit events and deliver message deliveries; they do not call agents or actions directly
- lower layers do not mutate orchestration state
