# Runstate Spec v1.3.8

Status: official immutable runtime contract.

This document defines the v1.3.8 durable runstate envelope used by FloofClaw's scheduler. A change to this document is a runtime contract change. Do not edit this v1.3.8 spec without explicit user approval. Approved changes must either create a new versioned spec or update the checksum guard in the architecture tests in the same reviewed change.

The envelope shape is intentionally stable. New runtime behavior should usually
be represented by events, reducers, or separate durable envelopes rather than by
changing `runstate.json`. Breaking this shape requires a new versioned spec.

## Definition

A **Run** is durable runtime work-in-progress.

A run can be advanced multiple times. Do not create a new run just because a job finished. Do not create fake continuation events. Job completion wakes the same run through explicit correlation.

## Canonical Envelope

```json
{
  "run_id": "run_004",
  "context_id": "chat:discord:adam",
  "profile": "openclaw",
  "status": "ready",
  "advance": 0,
  "created_from": {
    "event_id": "bus_000004",
    "type": "user_message"
  },
  "waiting": {
    "jobs": [],
    "events": [],
    "deadline": null
  },
  "cursors": {
    "phase_index": 0,
    "last_advanced_event_seq": null
  },
  "last_error": null
}
```

## Fields

`run_id` is the stable id for one durable run. It stays the same across all advances.

`context_id` is the lane this run belongs to, such as `chat:cli`, `chat:discord:<adapter>`, `affair:<id>`, or a future scheduler lane.

`profile` is the loop profile used when this run advances.

`status` is the scheduling state. Allowed v1.3.8 values are:

- `ready`: scheduler may advance this run
- `running`: currently being advanced
- `waiting_jobs`: waiting for one or more jobrunner jobs
- `waiting_event`: serialized for future real event/deadline waits and for the current internal action-slot wait state
- `done`: finished successfully
- `failed`: finished with an error

`advance` increments once per profile advance attempt.

`created_from` records the real event that created the run.

`waiting.jobs` contains job IDs that must finish before the run can advance again.

`waiting.events` is reserved for future real event waits. Leave it empty in v1.3.8.

`waiting.deadline` is reserved for future deadline wakes. Use `null` in v1.3.8.

`cursors.phase_index` is the runtime-owned next profile phase to execute. Before
the runtime waits on an async job, this cursor is already set to where execution
should continue when the wait clears. Agents, actions, adapters, and UI code must
not write it.

`cursors.last_advanced_event_seq` is reserved for future event-window behavior.
In v1.3.8 it is always written as `null` and is not read for scheduling.

`last_error` is either `null` or an object such as:

```json
{
  "class": 50,
  "code": "max_profile_passes_exceeded",
  "message": "profile pass cap reached without quiescing"
}
```

`class` is a two-digit common runtime bucket. `code` remains the
domain-specific error string, such as `llm_provider_connection_failed`.

## Runtime Loop

```text
poll inputs/jobs
append real events
reduce events
router creates/wakes runs
advance ready runs
persist updated run state
repeat
```

## Job-Correlation Wakeup

Every jobrunner job carries:

```json
{
  "job_id": "job_001",
  "run_id": "run_004",
  "request_id": "actionreq_000001"
}
```

`action_started` and terminal action events (`action_succeeded` or
`action_failed`) preserve that correlation. Pre-execution refusal uses
`action_rejected` and does not create a job. When a job finishes, runtime
removes `job_id` from the owning run's `waiting.jobs`. If no waiting jobs
remain, the same run becomes `ready`.

No router guessing is needed for job completion.

## OpenClaw Simulator Target

```text
run_004 created from user message

advance 1:
  profile starts from step zero
  tool_calls requests web_fetch
  action_runner starts job_001
  runtime persists waiting_jobs with waiting.jobs = [job_001]

job_001 finishes:
  action_succeeded includes run_id/job_id/request_id
  runtime removes job_001
  runtime persists ready

advance 2:
  same run_004 starts profile from step zero
  tool_calls sees fetch result
  action_runner starts write_file job_002
  runtime persists waiting_jobs

job_002 finishes:
  same run_004 becomes ready

advance 3:
  same run_004 starts profile from step zero
  no more tool jobs are needed
  conversation emits final message
  emit_outputs delivers it
  runtime persists done
```

## Non-Goals For v1.3.8

- no task fields inside runstate; tasks live in `workspace/memory/state/tasks.json`
  and are reduced from task-specific events
- no fake continuation events
- no `run_control` events
- no reply modes
- no global event sequence numbers
- no lane event windows
- no cursor-based event visibility

Control is native runtime logic that updates `run.status`.
