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

State changes are event-sourced through `task_created`, `task_updated`,
`task_working_memory_appended`, `task_completed`, `task_failed`, and
`task_canceled`. The reducer validates task identity, accepted fields,
capacity, and transitions before atomically rewriting
`workspace/memory/state/tasks.json`.

A run reaching `run_done` does not mutate task lifecycle. Agents that own a
multi-event request may close it explicitly through the validated
`task.update` call with `state: "completed"`; conversational agents that opt
into `autocomplete_message_task_on_send` retain that separate successful-send
behavior.

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
