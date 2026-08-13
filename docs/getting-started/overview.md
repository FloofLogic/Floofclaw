# Overview

**Preconditions:** none. Read this page for the runtime map; use
[Installation and Build](installation-and-build.md) when you are ready to run
commands.

**FloofClaw is a transparent agent orchestration runtime.** It composes
multiple focused LLM agents together with heavy-lifting workers (Codex,
Claude Code, and other subprocess research tools) into products where every
step is observable.

If you're new, start with **[what_is_floofclaw.md](../what_is_floofclaw.md)**
for the framing. This page is the operational overview.

## The concrete shape

- **One compact runtime binary**: `bin/fclaw`; its exact size varies with the
  compiler, platform, optional libraries, and debug/sanitizer flags.
- **One long-lived process at idle**: the gateway is a single-threaded reactor
  with no pthreads on the hot path. Provider calls and subprocess actions may
  run in supervised child processes.
- **Six runnable floops**: four product/example floops and two harness-only
  floops, inventoried in [Choosing a floop](floops.md). The fresh-clone
  default is the one-agent, mock-backed `hello` floop.
- **Behavior bundles**: JSON and prompt files under `floops/<name>/`, plus
  registered action contracts under `actions/`, define steps, agents, prompts,
  and allowed effects.
- **Installation configuration**: `config/floofclaw_config.json` selects the
  default floop and channels; `config/model_profiles.json` and
  `config/llm_registry.json` select provider transports and models.
- **State**: files under `workspace/`. Event logs, affairs, tasks, managed
  operations, and memory are durable and replayable.

## How it runs

The gateway is a `poll(2)` reactor with pluggable modules — no
subsystem-specific hot-path code, no special-cased subsystems:

- `bus_wake`, `jobrunner`, `bus_intake`, `runtime`, `status` — core
- `affair_watcher` — publishes review events when concerns come due
- `janitor` — log rotation plus bounded cleanup of processed bus records,
  archived tasks, and Codex worker artifacts; configured terminal-run pruning
  is temporarily ineffective because of a known run-state field mismatch
- `irc`, `ws`, `discord` — channel adapters

Inbound events land on the bus (`workspace/bus/inbox/`). The bus
intake module drains them into runs. Each run steps through the
floop's step list, invoking agents (`native` C, `llm`, or `script`)
and running actions (subprocess or runtime intrinsic). A run can contain zero,
one, or multiple provider calls; a provider call is not another name for a
run. Each LLM call keeps request and response artifacts under that run's
`workspace/runs/<run_id>/provider_calls/` directory.

## The uniform result flow (v0.21.0+)

Effects follow one of two result shapes:

- **Fire-and-forget** — effects such as `message`, `note_add`, `defer`, and
  affair lifecycle calls do not feed a successful return value into another
  agent decision. A failure surfaces as `action_failed`.
- **Data-returning (managed operation)** — an action whose manifest declares
  `contract: "managed_operation"` returns its result through an
  `operation_result` bus event, which the floop routes to an agent. This covers
  both quick intrinsic reads and long-lived worker operations.

There is no third pattern. See
[action_result_flow.md](../concepts/action_result_flow.md).

## What makes it different

- **Floop-configured roles.** A floop may split event kinds across small
  agents, as `floofclaw` does, or deliberately route several kinds back to one
  agent, as `openclaw` does. The kernel only walks the declared steps.
- **Workers underneath.** Codex and Claude Code are subprocess workers
  driven via `manage_codex` / `manage_claude`. FloofClaw is above
  them, not next to them.
- **Every LLM call leaves first-class artifacts.** The containing run gives
  request, response, usage, and trace transparency for each call; one run may
  contain more than one provider call.
- **Durable perspective across days.** Affairs, tasks, and memory persist in
  files, so a deployed agent comes back tomorrow knowing what it was watching
  yesterday.
- **Event-driven, replayable.** `event_log.jsonl` is truth. Reducers
  fire at append time. Replay works from the log alone.

## Rules the runtime is built to

Distilled — full list in [Principles](../concepts/principles.md):

- **Reactor stays generic.** `collect_fds → poll → dispatch on_fd →
  tick modules`. No subsystem-specific phase.
- **Everything is a module.** Adapters, jobrunner, bus, runtime,
  status, watcher, janitor — same `FcReactorModule` shape.
- **Callbacks must not block.** Long work goes to child processes
  observed via the jobrunner.
- **Event log is durable truth, not a work queue.** Reducers apply events at
  append time and update the materialized state immediately.
- **Hard separation.** Adapters don't know about agents; agents don't
  know about transport; kernel doesn't know about product behavior.
- **No policy in the engine.** Config decides *when* and *what for*;
  the engine provides mechanisms.
- **One pattern for results.** Data-returning actions are managed-op;
  results flow via `operation_result` to a later configured agent turn.

## Reading order

Start:

1. [What is FloofClaw](../what_is_floofclaw.md) — positioning
2. [Installation and Build](installation-and-build.md)
3. [First Run](first-run.md)
4. [Project Layout](project-layout.md)
5. [Architecture](../architecture.md)

Deep concepts (pick as you need them):

- [Principles](../concepts/principles.md) — the rules above with
  worked examples
- [Action Result Flow](../concepts/action_result_flow.md) — the
  managed-op + operation_result pattern
- [Floops](../concepts/floops.md) — loop profile shape
- [Executors](../concepts/executors.md) — native / llm / script
- [Actions](../concepts/actions.md) — action.json and run.sh
- [Events and Replay](../concepts/events-and-replay.md) — reducers,
  durable trace
- [Gateway Reactor](../concepts/gateway-reactor.md) — modules
- [the floofclaw floop](../concepts/floofclaw_floop.md) — the affair concept
  (durable concerns) and its watcher

Product / testing:

- [Discord and IRC](../discord_and_irc.md) — human vs probe channel
  convention
- [Testing](../concepts/testing.md) — unit / integration / smoke
- [CLI Reference](../reference/cli.md)
