# Task feature

FloofClaw tasks are compact durable work contracts. They record the request,
the work to perform, how completion is judged, and reducer-owned progress.
Tasks are state, not an execution engine: agents propose calls and the runtime
commits task events.

## Envelope

```json
{
  "task_id": "task_run_042_000008",
  "context_id": "chat:irc:probe",
  "kind": "work",
  "input": null,
  "created_event_id": "evt_run_042_000008",
  "created_ms": 1778348502000,
  "state": {
    "status": "working",
    "work": "write a poem about the Seven Wonders",
    "done_when": "wonders.txt contains the complete poem",
    "work_rev": 1,
    "working_memory": "The source list uses the ancient canonical seven.",
    "affair_id": "",
    "delegate": "",
    "external": null,
    "artifacts": [],
    "updated_ms": 1778348510000
  }
}
```

`affair_id` is stamped with the originating affair when work is admitted
from an affair review; a later `operation_result` turn correlates by this
exact id (an empty value means the task belongs to no affair).

Root identity is immutable after `task_created`. Mutable fields live under
`state`. `work_rev` is reducer-owned: it starts at one when work exists and
increments whenever accepted work or completion criteria change. Managed
operations compare this number to the attempt revision, never prose, so a
terminal attempt cannot restart until the task is explicitly revised.

A revision created by an external request begins a new work lineage. A
controller-selected `work` update is marked with trusted parent
task/revision/request/action/trigger identity and inherits the existing
lineage. It cannot manufacture a fresh budget merely by revising its own
task.

`working_memory` is one opaque, unstructured string shared by agents handling
the task. New and legacy tasks project it as an empty string until something
is appended. It belongs to the task rather than a work revision, so revisions
preserve it. The runtime does not interpret its contents.

## Lifecycle

Stored statuses are `open`, `working`, `blocked`, `completed`, `failed`, and
`canceled`. `completed`, `failed`, and `canceled` are terminal. The current
runtime does not model task dependency graphs; readiness is based on the task's
own lifecycle and operation state.

Run-owned state changes are event-sourced through `task_created`,
`task_updated`, `task_working_memory_appended`, `task_completed`,
`task_failed`, and `task_canceled`. The operator-only
`fclaw task cancel <task_id>` control path applies the same canonical
`task_canceled` transition directly through the reducer because no run owns
that command; it also writes a narration record. The reducer validates task
identity, accepted fields, capacity, and transitions before atomically
rewriting `workspace/memory/state/tasks.json`.

### Who closes an input task

Every `user_message` creates one durable `input` task before any agent runs:
the record "the user asked this, and it is not yet handled". Creating it is
the kernel's job. Closing it is deliberately not automatic, because a reply
is not necessarily an answer — "On it, checking…" leaves the ask open on
purpose — and a run reaching `run_done` never mutates task lifecycle. An
input task closes in exactly four ways:

1. **The agent says so.** `task.update` with `state: "completed"` for the
   exact `task_id`. This is the rule for an agent that does the work itself
   across several event runs (`openclaw`, `truly_openclaw`): it keeps the
   ask open while it emits managed actions and closes it when its response
   is final. The agent's prompt owns that decision; the kernel does not judge
   whether a valid set of calls looks like a continuation or a final answer.

   A structurally malformed LLM decision is repaired, bounded by the
   top-level `repair_attempts` in its `agent.json` (default 2). A response the
   normalizer rejects is never executed or delivered, so it cannot close the
   task by accident.
2. **The agent's `agent.json` sets `autocomplete_message_task_on_send: true`.**
   A successful `message` send completes the bound input task. This is the
   rule for an agent whose reply *is* the answer and whose work, if any,
   lives in its own `work` task with its own lifecycle (`floofclaw`'s
   `chat_manager`, `hello`, `companion`). Review turns in `floofclaw` use
   rule 1 instead: the review prompt ends every turn with `task.update`.
3. **The run fails or is canceled.** The engine stamps `task_failed` /
   `task_canceled` on the open message task the run was responsible for:
   the one born in that run, and — for a continuation run started by a
   runtime-owned event such as `operation_result`, which carries `task_id`
   explicitly — the ask it was handling. When such a run dies nothing else
   is scheduled for the ask and the human has already seen the failure
   narration; an `open` record would claim work in flight that no longer
   exists. Only reserved event kinds bind this way, and only within the
   run's own context; a `task_id` key in a product-owned typed event is
   producer data.
4. **The operator cancels the exact open task.**
   `fclaw task cancel <task_id>` applies `task_canceled` through the task
   reducer and narrates the control action. It refuses non-open tasks and is
   intended for inert records; it does not stop a live run or external worker.

Every LLM agent that answers user messages must do 1 or 2. **If it does
neither, each successful turn leaves one open task behind.** The active
store holds `TASK_MAX_ITEMS` (32) tasks and eviction reclaims only terminal
ones — a full store of open tasks is treated as a real backlog, never as
debris — so a floop that never closes its input tasks fills the store in
days. From then on `task_created` is refused at intake, no task exists for
an agent call to bind to, and every call that requires a binding is
rejected as `invalid_agent_call: action call requires existing task_id`
while `message`-only replies still succeed: a bot that can chat but can
never act, with `agent_output_invalid` as the only visible symptom. Look in
`workspace/logs/rejected_events.jsonl` first; the diagnosis is
`workspace/memory/state/tasks.json` holding 32 `open` `input` tasks.

The store is a bounded projection, not the record of truth. When every
entry is provably debris (all `input`, `work: null`), deleting `tasks.json`
with the gateway stopped is a valid reset. The usual cause is a fork of a
shipped floop that lost one of these rules in a later shipped-floop change;
the shipped floop of the same shape is the reference for which rule to use.

`working_memory_append` adds one unstructured contribution through the normal
action path. Contributions remain in event order with one plain newline
between nonempty values. There is no replace, edit, delete, clear, merge, or
structured-note operation. The fixed value limit is 4095 bytes; an append
that would exceed it fails without committing the append event or changing
the existing value.

## Authority

The runtime owns task IDs, context IDs, timestamps, source, event ordering,
work revisions, and lifecycle transitions. Agents own intent such as `work`
and `done_when`; actions own only result and error bodies. Agents may refer to
runtime-supplied IDs but may not mint or overwrite them.

## Work controllers and managed operations

The `work` action creates or revises the context's work task. A fixed native
workers phase may start a configured managed operation at the current
`work_rev` and project the attempt under `state.external`; the detached
worker's completion claim — or the deadline's timeout claim — then produces
exactly one `operation_result` when terminal. Nothing polls in between.

A generic controller may instead declare `bind_task: "open_work"` and a
`work_consequence` gate. The runtime, not the model, chooses the exact
same-context task and revision. Each selected action is synchronously
committed before dispatch and its terminal action event produces a correlated
`work_step_result`; a managed operation's final data still arrives through
`operation_result`. These are intermediate consequences. The controller ends
the revision only with `work_completed` or `work_blocked`, routed through the
work-outcome path.

A bound controller may include one `working_memory_append` sidecar alongside
its one ordinary execution or terminal call. The full response is validated
first, then the sidecar is dispatched first. It carries the exact task binding
but no work revision or consequence trigger, so it consumes no semantic step,
repair attempt, or extra model turn. A managed worker launched by the ordinary
call therefore receives the newly appended value. Two sidecars or two
ordinary controller calls are rejected.

One external lineage permits eight semantic selections. After a rejection or
failure, another repair selection is allowed while the controller agent's
`work_policy.max_repair_attempts` budget remains. The setting defaults to
`1`, accepts `0` through `8`, and is mechanically bounded by the engine safety
ceiling. Controller-selected `work` revisions inherit both counters. A blocked
revision stays blocked until a newer external revision supplies new work; a
controller-created revision does not reset it. The reducer-owned projection is
`workspace/memory/state/work_steps.json` and is rebuildable from event logs.

Successful action data is not passed to a same-run reviewer through task
artifacts. The former sync-artifact result mechanism is retired. Failed-action
error/reason context may still be attached to the task as diagnostic evidence;
that narrow path is not a general result channel.

## Projection

Agents that declare `listen: ["tasks"]` receive a bounded active-task
projection. Archived terminal tasks are not model-facing. Task presentation is
derived from durable state; adapters and prompts do not reach into reducer
implementation details.

Task-aware action callers may append to an exact `task_id` present in that
projection. Managed Codex and Claude workers receive the same working-memory
value in their assigned-task prompt, inherit the exact task route in
`fclaw action exec -a`, and use the same `working_memory_append` action rather
than writing projection files.

A bound work controller additionally receives only the bounded consequence
for its trusted task/revision, including the originating request, selected
action, trigger, outcome, and remaining semantic/repair state. It does not
search task text to rediscover correlation.

The durable task snapshot retains its compact artifact array. Agent-facing
task projections preserve every retained artifact but partition that array
structurally:

```json
{
  "artifacts": {
    "current": [{"artifact_id":"art_run_042_000009_001","work_rev":2,"parts":[]}],
    "historical": [{"artifact_id":"art_run_041_000014_001","work_rev":1,"parts":[]}]
  }
}
```

The reducer stamps new artifacts with the bound `work_rev`; the projection
places equal revisions in `current` and older revisions in `historical`.
This is a mechanical correlation rule, not an agent judgment. It keeps prior
ideas and diagnostics visible without presenting them as evidence produced by
the revision currently being advanced.

## Compatibility note

Older task examples included dependency arrays and a selected-task reviewer
pipeline. Those fields and execution modes were retired with the legacy floop
in the 2026-07 hardening pass. Git history preserves that contract; this file
describes only the current runtime.
