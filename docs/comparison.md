# Runtime orientation

A one-page tour of FloofClaw's shape. For the positioning, see
[what_is_floofclaw.md](what_is_floofclaw.md). For boundary contracts,
see [architecture.md](architecture.md). For reactor module details,
see [gateway-reactor.md](concepts/gateway-reactor.md).

## Current shape

| concern | current runtime |
|---|---|
| binary | one `bin/fclaw` binary |
| implementation | C runtime + C/bash harnesses and actions; a few existing Python app/test utilities are grandfathered |
| default floop | `default_floop` in `config/floofclaw_config.json`; the shipped neutral config selects `hello` |
| product floops | `hello`, `companion`, `floofclaw`, `openclaw` |
| test/perf floops | `fast`, `thrash_cap` |
| run owner | scheduler / runtime-owner path only |
| event truth | per-run `workspace/runs/<run_id>/event_log.jsonl` |
| durable state | materialized from events into JSON under `workspace/memory/state/` |
| bus | file-backed inbox → processed under `workspace/bus/` |
| gateway | one long-lived, single-thread reactor process with pluggable modules and short-lived job children |
| channels | reactor modules: IRC, WS, Discord, Telegram, chosen at build time with `FCLAW_ADAPTERS`; CLI publishes via bus events |
| media | Discord attachments (10 × 25 MiB, 50 MiB per message) and Telegram photos/documents (20 MiB per Bot API file) ride `user_message` behind a private content-addressed manifest; the bounded LLM child downloads only from the authenticated channel file host; Gemini receives images, PDF, audio, video, and OpenAI-style profiles receive images |
| affairs | durable concerns reduced from `affair_*` events; affair watcher module fires review envelopes |
| tasks | durable task state reduced from task-specific events |
| workers | common `manage_codex`, `manage_claude`, and `manage_hermes`, driven as managed operations |
| result flow | uniform: every data-returning action is managed-op → `operation_result` → a later configured agent turn |
| user-visible output | `message` action → `action_succeeded.delivery` → channel adapter tail → out |
| appliance mode | janitor module rotates JSONL logs and bounds terminal-run, processed-bus, task-archive, and every managed-worker store |

## Shipped floops

`hello` is the fresh-clone, mock-backed first run. `companion` is a
conversational product with memory and durable-concern extraction.
`floofclaw` is the full background-work manager, and `openclaw` is the
coding/tool loop. `fast` and `thrash_cap` exist for proof harnesses, not as
user products. See [Choosing a floop](getting-started/floops.md) for
requirements and intended use.

The two general-purpose event-driven examples are detailed below.

### `floofclaw` — friendly agent that watches things

Three agents, one per event kind:

```text
memory.before          (agent: native memory)
chat                   (agent: chat_manager   — gate: event_kind:user_message)
review                 (agent: review_manager — gate: event_kind:affair_review)
result                 (agent: result_manager — gate: event_kind:operation_result)
dispatch               (builtin: action_runner)
workers                (agent: workers — starts detached managed attempts)
memory.after           (agent: native memory, non_critical)
memory.compact         (agent: memory_compactor, gate: memory_compaction_due)
```

Each event kind routes to exactly one agent. `chat_manager` handles
what the user just said; `review_manager` handles scheduled affair
check-ins; `result_manager` digests worker results. Every data-returning
action is a managed operation, so worker and optional `web_read` results
all flow back through `operation_result` → `result_manager`.

This is the full friendly background-manager shape.

### `openclaw` — coding assistant

One main agent across durable event-kind turns:

```text
memory.before          (agent: native memory)
main                   (agent: main_claw — gate: event_kind:user_message,operation_result,affair_review)
dispatch               (builtin: action_runner)
memory.after           (agent: native memory, non_critical)
memory.compact         (agent: memory_compactor — gate: memory_compaction_due)
```

Data-returning actions are managed operations, but their results return to the
same `main_claw` through a later `operation_result` run. The original input
task and its `working_memory` remain the request context until Main Claw
explicitly completes it. `main_claw` may choose another tool after each result
and may send a progress message alongside one action; a final message is paired
with the existing validated `task.update` call. The prompt owns that product
behavior; the kernel normalizes individual calls without judging their overall
combination. Structurally malformed output gets bounded, artifact-backed
feedback. Its bootstrap catalog is the single `all_actions` entry, expanded in
place to the complete loaded registry with every authoritative description and
schema. The same main claw also owns affair-review choices.

## Agents

Three executor types:

| executor | current use |
|---|---|
| `native` | deterministic runtime-owned agents such as `memory`, `noop_agent`, `workers` |
| `llm` | prompt-backed agents in `hello`, `companion`, `floofclaw`, and `openclaw` |
| `script` | external-process agents supplied by fixture or deployment floops |

Ordinary script and LLM agents emit an intent-only calls envelope:
`{"calls":[]}`. The small `hello` and `companion` conversational agents use
the retained conversational-output adapter, which the runtime maps back through
the normal `message` action. Agents describe message intent, tool intent, args,
and task update intent. The runtime owns durable IDs, timestamps, event append
order, request IDs, job IDs, run IDs, and task lifecycle transitions. Raw
model/provider output stays in artifacts, not truth.

## Actions

Actions have `action.json` contracts under `actions/<area>/<id>/`:

- `id`
- `description`
- `outside_world`
- `args_schema`
- `exec` (`["bash", "run.sh"]` for subprocess, `["runtime", "fc_x()"]`
  for intrinsic)
- `timeout_ms`
- `contract: "managed_operation"` for data-returning actions

Two action-result shapes:

- **Immediate/runtime-reduced**: `message`, `note_add`, `defer`,
  `affair_close`, `affair_open`, `counter_bump`, `rotate_file`, and `work`.
  `work` creates or revises a durable task; the floop's native workers phase
  then dispatches its configured managed worker. `task.update` is a validated
  runtime call, not a registry action.
- **Managed operation**: common `manage_codex`, `manage_claude`,
  `manage_hermes`, `web_read`, and `http_call`, plus core `read_file`,
  `read_dir`, `write_file`, `apply_patch`, `bash`, and `web_fetch`. Results
  flow via `operation_result` to a later configured agent turn; OpenClaw
  routes that turn back to the same `main_claw`.

Action lifecycle:

```text
action_request → action_rejected
action_request → action_started → action_succeeded
action_request → action_started → action_failed
```

For managed-op actions, `action_succeeded` also triggers the operation
driver to publish an `operation_result` envelope. See
[action_result_flow.md](concepts/action_result_flow.md).

## Tasks and artifacts

Tasks are durable units of work. They live in
`workspace/memory/state/tasks.json` and are updated only by
task-specific events reduced by `runtime/task_state.c`. Stored
lifecycle: `open | working | blocked | completed | failed | canceled`.
Readiness is derived from state, never stored.

Artifacts are typed output bundles attached to a task. The sync-artifact
same-run result path is fully retired; managed-operation plus
`operation_result` is the current data-return path. Artifacts remain for
correlation state and narrow failure diagnostics, not for handing successful
action data to a same-run agent.

## Affairs

Durable concerns — distinct from tasks. An affair has:

- `manage` — what to keep an eye on
- `next_review_at_ms` — single scheduling field (`0` = manual only)
- `notes[]` — a short log
- `task_ids[]` — work tasks spawned by review actions
- `status` — `active | paused | closed`

The affair watcher reactor module walks the store each tick and
publishes an `affair_review` bus envelope for any active affair whose
`next_review_at_ms` has passed. After firing, the field is cleared to
`0`; the affair stays dormant until something re-arms it (an agent
via `defer`, or the CLI via `fclaw affair schedule -h`).

See [the floofclaw floop concept](concepts/floofclaw_floop.md).

## Gateway and channels

One long-lived gateway process, with cooperative reactor modules and bounded
short-lived agent/action child jobs:

- `bus_wake` — Unix datagram wake socket for inbox signals
- `jobrunner` — pid/pipe-backed agent/action child table
- `bus_intake` — drains `workspace/bus/inbox/`
- `runtime` — pumps finished jobs, steps ready runs
- `status` — gateway status socket
- `affair_watcher` — fires `affair_review` envelopes for due affairs
- `janitor` — appliance-mode log rotation and bounded cleanup for terminal
  runs, processed bus records, task archives, and every managed-worker store
- `irc` / `ws` / `discord` / `telegram` — channel adapters, chosen at build time with `FCLAW_ADAPTERS`
- attachments never touch the reactor: an adapter parses them into a media
  manifest at publish time, and the LLM child fetches within its own bound
  (`runtime/bus/media_manifest.c`, `runtime/llm/media.c`)

`gateway start` daemonizes. `gateway run` runs the same code path in
the foreground so Ctrl-C stops it cleanly. Both write pid/loop state
to `.fclaw/run/`.

## Testing

Programmatic testing of agent behavior goes through IRC (see
[Discord and IRC convention](discord_and_irc.md) and
[IRC probing](testing/irc_probing.md)). Discord is the human channel;
it's not for automated probes.

Current test surfaces and required gates:

| layer | command | purpose |
|---|---|---|
| unit | `make unit` | 49 fast C boundary/budget checks |
| integration | `make integration` | 200 in-process runtime checks across four isolated workers |
| routine gate | `make test` | unit + integration + public-contract probes + hermetic smoke in isolated workspaces; offline |
| release/build contracts | `make test-full` | routine gate plus mock-only, adapter-selection, and Android ARM64 rebuilds |
| major/deep-engine gate | `make robustness` (currently macOS) | ASan, UBSan, small-stack, fuzz, chaos, disk-full, and thrash; roughly an hour on the reference Mac |
| live provider | `FCLAW_LIVE_SMOKE=1 make live-smoke` | opt-in single real-provider path check |
| IRC baseline | `scripts/irc_probe.py` per flow | end-to-end agent behavior on the shipped floops |

There is no formal soak gate. `scripts/rss_soak.sh` is an ad-hoc sampler only.

## Where to go next

- [Architecture](architecture.md) — kernel, reducers, boundaries
- [Action Result Flow](concepts/action_result_flow.md) — the unified
  managed-op pattern
- [Principles](concepts/principles.md) — rules the runtime is built to
- [Floof Struct Definitions](concepts/floof-struct-definitions.md) —
  runtime/job/task/artifact/event envelope contracts
- [CLI Reference](reference/cli.md)
