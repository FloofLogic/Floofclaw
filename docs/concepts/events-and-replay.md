# Events and Replay

The central rule:

```text
event_log.jsonl is the source of truth.
```

State files are materialized views. Trace files are diagnostics. Agent
outputs and action call files are artifacts. None of those are believed
unless their effects are events in the event log.

But — and this is the operational subtlety — **the runtime does not
read `event_log.jsonl` to compute live state.** The event log is the
durable trace, not the hot-path input queue. Live state is in
`RtContext`, kept current by an `apply_*_event` projector that runs
once per event at append time. See "Apply at append" below.

## Where events live

Per run:

```text
workspace/runs/<run_id>/event_log.jsonl
```

Globally for the bus:

```text
workspace/logs/bus.jsonl
```

Generic inbound attachment descriptors live in private, content-addressed
manifests:

```text
workspace/media/inbound/<sha256>.json
```

The bus and event log carry only the manifest digest, relative path, item
count, and declared byte total. They never carry attachment bytes, base64, or
signed transport URLs.

## Minimum event shape

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

`event_id` is monotonic per run. `ts` is UTC ISO-8601. `source` names
the emitter (`"user"`, `"kernel"`, `"action_runner"`, `"<agent_name>"`, …).
`payload` is type-specific JSON.

## Event types

The kernel and action_runner emit:

- `user_message` — initial event for a run started from a real user
  turn; payload includes `text`, `origin_event_id`, `channel`,
  `adapter_id`, optional reply-routing `ref`, and optional generic `media`
  manifest reference. `text` may be empty only when `media` is present.
- `affair_review` — initial event for a run woken by the affair
  watcher or the `fclaw affair review` CLI; payload includes
  `affair_id`, `text` (a short diagnostic label), `origin_event_id`,
  `channel`, `adapter_id`. Distinct from `user_message` because the
  system is talking to itself — no `user` memory record is written
  for this turn.
- `action_request` — durable runtime request to run an action; payload
  `{request_id, action, args}`. Agents emit calls in `{"calls":[...]}`; after
  allowlist/schema validation, the runtime turns accepted call intent into
  this event and stamps the runtime-owned `request_id`.
- `action_rejected` — runtime refused a valid `action_request` before
  execution, with `reason`, `message`, and `errors`.
- `action_started` — checked durable pre-effect claim for an executed action.
- `action_succeeded` — canonical successful outcome `{request_id, action,
  result, delivery?}`.
- `action_failed` — canonical failed outcome `{request_id, action, reason,
  result, error}`.
- `run_started` / `run_done` / `run_failed` / `run_canceled` —
  one start plus exactly one terminal event per run (idempotent through
  `apply_run_terminal_event`). Graceful gateway shutdown cancels active runs.
- `error` — any kernel-detected failure that doesn't end the run.

The memory agent emits:

- `memory_recalled` — a read-only observation containing the recent tail
  of `workspace/memory/memory.jsonl` and derived `workspace/memory/memory.json`
  state.
- `user` — memory record for retained user text.
- `asst` — memory record for retained assistant text.
- `fact` — memory record for durable conclusions, corrections,
  preferences, or open loops.
- `summ` — append-only summary record emitted by the memory compactor, with `text` and a `through` cutoff identifying the window it summarized.
- `memory_compaction_requested` — runtime-created request with the selected
  context, messages, and cutoff for the gated `memory.compact` phase.
- `memory_compacted` — validated compactor result; updates
  `workspace/memory/state/summary_state.json`.

`user`, `asst`, `fact`, `summ`, and memory compaction events are memory-domain
event types. They are not channel-message types; external input remains
`user_message`, and visible assistant output is represented by the `message`
action's `action_succeeded` delivery.

Affair-domain events (see [Architecture: Affair Watcher](../architecture.md)):

- `affair_patch` — create / link / update / close an affair. Payload
  `{action, affair_id, context_id, manage, note_text, status,
  next_review_at_ms?}`. The reducer projects this into
  `workspace/memory/state/affairs.json`. The optional
  `next_review_at_ms` field lets callers set when the watcher should
  fire next — the review manager calls `defer` with the duration in
  `args.value`, and `fclaw affair schedule -h` does the same from the CLI.
- `affair_note_added` — append a short observation note to an existing
  affair. Payload `{affair_id, text, ts_ms?}`.
- `affair_counter_bumped` — deterministically change one of an affair's
  at-most-16 named signed 64-bit counters. Payload contains
  `{affair_id, name, delta}` or `{affair_id, name, set}`. The reducer rejects
  overflow and a 17th distinct name; counters are projected into affair input
  alongside notes.
- `affair_task_linked` — link a work task to an affair. Emitted by
  the runtime when a work task is created during an `affair_review`
  run. Payload `{affair_id, task_id}`.
- `affair_reviewed` — emitted by the watcher after firing. Payload
  `{affair_id, ms}`. The reducer clears `next_review_at_ms` to 0,
  which means the affair goes dormant until something re-arms it
  (an agent via `defer`, or `fclaw affair schedule -h`). No backoff
  multiplier — the floop decides when to be checked again, not a
  hardcoded interval.

Managed-operation events:

- `operation_started` — allocated BEFORE the action executes: records the
  runtime-owned operation id with its task/context/work-revision correlation
  and completion deadline in the durable operation store.
- `operation_result` — the one canonical terminal per operation (status
  `succeeded`, `failed`, or `timed_out`), routed through normal intake to the
  floop's configured result turn. For a bus-submitted completion claim
  (`operation_completed` ingress envelopes — not themselves behavioral
  events), the admitted claim IS the result run's first event; duplicates and
  forged claims are rejected before any run exists.

Task-domain state events, when a profile includes them:

- `task_created`
- `task_updated`
- `task_completed`
- `task_failed`
- `task_canceled`

The v1 memory feature uses memory record events rather than
`memory_patch` as its canonical write path.

Diagnostics live in `trace.jsonl`, never in the event log.

## Action request identity

Every `action_request` carries a stable `request_id`. Any subsequent
`action_rejected`, `action_started`, `action_succeeded`, or `action_failed`
for that request carries the same ID. A request is **pending** only if there is no
terminal `action_rejected`, `action_succeeded`, or `action_failed` event
with the same `request_id`. This prevents duplicate action runs across
replays or scheduler ticks.

Agents and models are not allowed to provide request ids or other durable
identity fields. If a duplicate already-terminal `request_id` appears from a
trusted runtime path, the runtime appends a visible `action_rejected` with
`reason: "duplicate_terminal_request_id"` instead of silently dropping it. For
runtime-stamped fresh IDs, the runtime also rejects duplicate equivalent
non-message action calls in the same run so repeated intent does not repeat side
effects forever.

In the current gateway, the pending-action queue is maintained **in memory**
by `rt_action_runner_note_*` (see `runtime/action_runner.c`).
It is **not** rebuilt by rescanning the event log on every tick. Cold-start
recovery is the deliberate exception: it replays nonterminal run truth to
rehydrate pending work after a hard crash. A request without a committed
`action_started` claim may resume under its stable ID. Once an outside-world
request has a durable start but no provable terminal, recovery never replays
it or invents success; it records `ambiguous_completion` for the bound work.
Declared local actions may replay, so their manifests must classify
`outside_world` honestly and duplicate outcomes remain audit evidence.

## Reducer + state agent

The reducer is deterministic runtime code. `runtime/state.c` applies known event
types to per-run `state.json`; durable memory records live in
`workspace/memory/memory.jsonl` and are applied by `runtime/memory.c`; durable
task records live in `workspace/memory/state/tasks.json`, completed-task
archives live in `workspace/memory/state/tasks_archive.jsonl`, and both are
handled by `runtime/task_state.c` / `runtime/task_projection.c`.

State-oriented agents read recent state + events and may emit task-specific
events or other reducer-owned state intent events. They **do not** write durable
state directly; only reducers apply validated changes.

```text
agents propose state intent  →  events record proposals  →  reducers apply
```

## Apply at append (the live path)

The reducer doesn't run as a periodic pass. It runs **once per event
append**, in memory.

`runtime/event_log.c::rt_append_event` is the single funnel for every
event the runtime emits. After it writes the event line to
`event_log.jsonl`, it does two things:

1. Calls `rt_apply_event_to_context(ctx, type, payload)` —
   `runtime/state.c`. This is the *run-state projector*. Pure
   in-memory: updates `ctx->user_text`, `ctx->latest_action`,
   `ctx->latest_result_text`, `ctx->latest_message`. No file I/O.
2. If the event type matches a durable subsystem, routes there:
   - `user` / `asst` / `fact` / `summ` → `rt_memory_apply(type, payload, ts, run_id)`
   - memory compaction/control events → `rt_memory_apply_control(type, payload)`
   - `task_created` / `task_updated` / `task_completed` / `task_failed` /
     `task_canceled` → `rt_task_apply_event(type, payload)`
   - affair-domain events → `rt_affair_apply_event(type, payload)`
   - managed-operation events → `rt_operation_apply_event(type, payload)`

Skipped on `replay_mode` so a frozen log doesn't re-mutate disk.

Two consequences:

- **Live state never re-reads `event_log.jsonl`.** `RtContext` is
  always current because the projector ran when the event was
  appended.
- **Run-state vs durable state are separate concerns.** The generic
  projector (state.c) doesn't touch `workspace/memory/`. The memory
  subsystem (memory.c) doesn't touch run-context fields. Each owns
  its half.

`rt_write_state_snapshot(ctx)` writes `state.json` from `ctx` fields
directly — no walk, no parse. It's called at step boundaries by the
scheduler and the sync CLI loop.

## Replay

Given just the event log:

```bash
./bin/fclaw replay -h workspace/runs/run_001/event_log.jsonl
```

…the runtime should reconstruct the same `state.json`. Trace files,
agent artifacts, and action call files are **not** required for replay;
they explain how the log was produced.

This is the correctness check. If a feature can't be replayed from
events, that feature is leaking truth into a non-event surface.

For a media turn, the event's hashed manifest reference is behavioral truth
about the model input. Replaying the run-state projection still needs only the
event log; reissuing the historical provider call additionally requires the
verified manifest and its downloaded run artifact. Provider calls are effects
and are not automatically repeated by `replay`.

## What is and isn't an event

Behavioral truth (events):

- the `user_message` that started the run
- each action lifecycle event that occurred: `action_request`, optional
  pre-execution `action_rejected`/`action_failed`, checked `action_started`,
  and terminal `action_succeeded`/`action_failed`
- lifecycle transitions (`run_*`)
- task or reducer-owned events that change durable state
- `error` records that influence the run

Not events:

- raw agent stdout (artifact under `agent_outputs/`)
- raw action stdout/stderr (artifact under `action_calls/`)
- LLM provider responses (artifact)
- downloaded attachment bytes and raw provider base64 (verified input/call
  artifacts referenced by event truth)
- `state.json` (materialized view)
- `trace.jsonl` (diagnostic)

The strict action-output rule is:
action stdout is **never** parsed back into events. The canonical
action terminal event is the only thing the runtime trusts.

Large action outputs stay in artifacts. An action may write large payloads
under `action_calls/`, and its canonical `action_succeeded.result` should
carry artifact paths, byte counts, status fields, and short excerpts. If an
ordinary result does not fit, the runtime fails it with `result_too_large`,
an empty result, and error metadata pointing to the preserved artifact. For a
managed operation, it first tries to salvage `status` and `handle`, retaining
bounded `text` when present; successful salvage produces a minimal
`_salvaged:true` success. A result is
never silently truncated into a successful `{}`.

## Run-side artifacts

```text
workspace/runs/<run_id>/
  event_log.jsonl    behavioral truth
  runstate.json      durable scheduler/recovery envelope
  state.json         materialized state view
  trace.jsonl        diagnostics
  agent_outputs/     raw agent i/o (stdin / stdout / stderr per step)
  action_calls/       raw action i/o (stdin / stdout / stderr per call)
  provider_calls/     numbered LLM traffic/metadata, when an LLM phase ran
```

This layout is intentionally agent-readable. Ordinary debugging uses file
inspection and `jq`, not a debugger.
