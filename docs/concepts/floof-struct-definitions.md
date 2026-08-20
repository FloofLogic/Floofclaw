# Floof struct definitions

This page is the short index of active FloofClaw envelopes. Detailed behavior
lives at the owning boundary; examples here must not invent authority.

## Authority model

| owner | fields |
| --- | --- |
| runtime | durable IDs, timestamps, trusted source, event order, run/job/task lifecycle |
| agent/model | message and call intent, action arguments, task work and completion intent |
| action | result and error bodies |
| reducer/projection | materialized task, affair, operation, memory, and run views |

Agents may reference trusted IDs supplied in input, but may not mint or replace
runtime-owned identity.

## Media reference and manifest

A media-bearing `user_message` adds one compact, generic reference:

```json
{
  "media": {
    "version": 1,
    "manifest": "media/inbound/8f…c2.json",
    "sha256": "8f…c2",
    "count": 1,
    "total_bytes": 8411
  }
}
```

The private manifest contains ordered descriptors with `id`, `filename`,
`media_type`, `size_bytes`, and the authenticated transport source. Attachment
bytes are downloaded to bounded run-local files by the isolated LLM child.
Channel adapters validate their own source policy; intake validates the
content-addressed manifest; provider serializers map verified local files to
their native image, file, audio, or video parts. Unsupported provider/MIME
pairs fail rather than dropping the part.

Text-only runs allocate no descriptor state. A media-bearing `RtContext` owns
one optional heap pointer to the compact reference, and the LLM child owns the
short-lived descriptor array and request body.

## Runstate

`runtime/run_state.c` persists one scheduler envelope per run:

```json
{
  "run_id":"run_004",
  "context_id":"chat:irc:probe",
  "profile":"floofclaw",
  "status":"waiting_jobs",
  "advance":2,
  "created_from":{"event_id":"bus_000004","type":"user_message"},
  "waiting":{"jobs":["job_001"],"events":[],"deadline":null},
  "cursors":{"phase_index":3,"last_advanced_event_seq":null},
  "last_error":null
}
```

The runtime owns the whole envelope. See [runstate-v1.3.8](runstate-v1.3.8.md).

## Job

`RtJob` in `runtime/gateway/jobrunner.h` tracks one LLM-agent, script-agent, or
subprocess-action child: runtime IDs, pid, launch/deadline time, artifact
paths, pipe drain counts, exit state, and timeout/output-limit flags. Native
agents execute inline and do not create an `RtJob`. It is an in-memory reactor
contract; agents and actions do not mutate it.

## Task

Tasks have immutable root identity and mutable reducer-owned state:

```json
{
  "task_id":"task_run_004_000002",
  "context_id":"chat:irc:probe",
  "kind":"work",
  "input":null,
  "created_event_id":"evt_run_004_000002",
  "created_ms":1778348502000,
  "state":{
    "status":"working",
    "work":"write the requested file",
    "done_when":"the file exists",
    "work_rev":1,
    "affair_id":"",
    "delegate":"",
    "external":null,
    "artifacts":[],
    "updated_ms":1778348510000
  }
}
```

See [Task feature](task-feature.md). The runtime has no dependency-graph or
selected-task reviewer envelope.

## Agent processor input and output

An agent sees only the blocks declared in its `listen` list: `event`,
`memory`, `tasks`, `affairs`, and `usage`. Ordinary output is call intent:

```json
{"calls":[{"name":"message","args":{"message":"Done."}}]}
```

The runtime validates the allowlist and schema, stamps request/event identity,
and appends lifecycle events. Narrow conversational, affair-extraction, and
memory-compaction shapes remain for the flows that declare them.

## Action request and terminal events

An accepted call becomes a runtime-owned `action_request`. It may terminate
before execution as `action_rejected` or `action_failed`; otherwise the
runtime durably commits `action_started` before the effect and later appends
exactly one `action_succeeded` or `action_failed`. On cold recovery, an
unstarted nonterminal request may resume under its stable request ID. A
started outside-world request with no provable terminal is never replayed or
inferred successful; bound work ends with `ambiguous_completion` instead.
Data-returning actions declare the managed-operation contract; their
successful result is published once as a correlated `operation_result` for a
later run.

## Affair and operation state

Affairs are durable concerns with status, notes, context route, and
`next_review_at_ms`. Operations record opaque handles, task/context
correlation, work revision, completion deadline, and terminal result. These stores
are reducer-owned and written atomically under `workspace/memory/state/`.

## Durable truth

Per-run `event_log.jsonl` is the replay source. A terminating newline is the
record commit marker; replay ignores a torn final record and reports malformed
interior records. Materialized JSON is a projection, never a second source of
truth.
