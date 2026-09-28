# Architecture

This is the current ownership map. Read [Philosophy](PHILOSOPHY.md) for the
design rules and follow the linked concept pages for interface details.

## System shape

```text
channel / CLI
      |
      v
bus envelope -> run -> floop steps -> agent call intent -> action
                    |                    |                 |
                    v                    v                 v
                event log          normalized events   terminal event
                    |                                      |
                    +------------ reducers <---------------+
                                      |
                           projections / later bus wake
```

The repository has four architectural inputs and one runtime-owned output:

| Path | Owns |
|---|---|
| `floops/` | profiles, agent packets, prompts, model references |
| `actions/` | capability manifests and subprocess implementations |
| `config/` | installation, provider, channel, and action configuration |
| `runtime/` | generic interpreter, reducers, transports, and CLI |
| `workspace/` | runs, event logs, projections, bus records, and diagnostics |

`apps/` reads artifacts and projections. It is not an authority. `agents/`
and `loops/` are test-fixture fallbacks; shipped bundles live in `floops/`.

## Ownership boundaries

| Layer | Owns | Must not own |
|---|---|---|
| adapters | transport decode/render, connection state | agents, tasks, scheduler policy |
| bus and ports | envelope publication, intake, delivery routing | product semantics |
| scheduler and runners | profile walking, jobs, lifecycle | whether work is good or important |
| event log | validation and durable append | projection policy |
| reducers | meaning of accepted events and durable stores | transport or prompting |
| LLM transport | profile resolution and provider wire formats | product roles |
| action registry/runner | capability contracts and effects | conversation policy |
| floops and agents | roles, routing, decisions, voice | runtime-owned identity |

Dependencies point through narrow interfaces. If a change needs knowledge
from two distant layers, change the boundary contract rather than reaching
through module internals.

## Gateway and reactor

The gateway is a single cooperative `poll(2)` reactor. Every participant uses
`FcReactorModule`:

```text
collect_fds -> poll -> on_fd -> tick
```

Core modules are bus wake, jobrunner, bus intake, runtime, and status.
Optional modules include the affair watcher, janitor, and compiled channel
adapters. Callbacks return promptly; provider requests and subprocess work run
under the jobrunner, and network connections use nonblocking state machines.

The gateway owns the selected floop. Clients that omit `--floop` follow the
owner; a conflicting explicit value is rejected before bus publication. See
[Gateway Reactor](concepts/gateway-reactor.md).

## Intake and runs

Adapters and CLI clients publish the same bus envelope shape. Bus intake
claims an envelope durably, derives the conversation context, and creates one
run under `workspace/runs/<run_id>/`. A triggering event creates one run, not
necessarily one model call.

Important per-run artifacts:

```text
event_log.jsonl       append-only behavioral truth
runstate.json         durable scheduler and recovery envelope
state.json            materialized run view
trace.jsonl           diagnostic decisions
provider_calls/       numbered raw request, response, and metadata
agent_outputs/        numbered raw agent output/stderr
action_calls/         numbered action input/output/stderr
```

The scheduler walks the selected profile in order. It validates step gates,
starts the declared executor, normalizes accepted intent into events, invokes
reducers, waits for jobs when necessary, and retires when no consequence is
runnable. Its only semantic-free loop guard is the bounded profile-pass cap.

## Floops and agents

A floop is a directory containing `loop.json` and agent packets. Profiles
declare ordering and gates; `agent.json` declares executor, model reference,
inputs, allowlisted actions, and optional structural output contracts.

The shipped `floofclaw` profile is:

```text
memory.before -> chat -> review -> admission.dispatch
              -> work.select -> work.dispatch
              -> result -> result.dispatch
              -> memory.after -> memory.compact
```

Event gates assign focused roles:

- `chat_manager` handles user messages, replies, affairs, and work admission.
- `review_manager` handles scheduled affair review.
- `work_manager` owns the exact bound work revision and its next decision.
- `result_manager` receives correlated terminal work outcomes and must send
  one user-facing message.
- native phases project memory and select/dispatch trusted work.

Executors are `native`, `llm`, and `script`. Deterministic shipped phases are
native; model decisions are LLM-backed; script is an external escape hatch.
See [Floops](concepts/floops.md) and [Executors](concepts/executors.md).

## Admitted work

`work` creates or revises a durable work task. The runtime binds
`work_manager` to the exact context, task, and `work_rev`; the model never
chooses those identifiers.

The controller must emit exactly one ordinary decision:

```text
action -> correlated work_step_result/operation_result -> same controller
work_complete -----------------------------------------> result_manager
work_blocked ------------------------------------------> result_manager
```

One `working_memory_append` sidecar is allowed but is not a decision. Action
success, failure, rejection, and timeout all return as evidence. Once work is
admitted, there is no valid silent exit: the controller advances, completes,
or blocks; a terminal result requires one message. Invalid model output uses
the declared bounded repair budget, then the existing run-failure delivery.
On completion, the runtime carries the exact selected consequence text beside
the controller's concise terminal summary, so the result manager receives the
answer rather than a claim that an answer exists.

This is floop behavior enforced through generic declared contracts. The
scheduler never decides whether work succeeded. See
[Task Feature](concepts/task-feature.md).

## Actions and managed operations

The registry recursively discovers `actions/**/action.json`. Agents see only
their allowlist (or an explicit all-actions grant). The manifest owns calling
guidance, argument schema, timeout, effect class, and execution contract.

Calls normalize into runtime-owned `action_request` events. Intrinsics execute
inside the runtime; subprocess actions receive one JSON object on stdin. Both
produce the same start and terminal lifecycle.

Data-returning actions declare `contract: "managed_operation"`. Their
operation state is durable and their one terminal result is published as a
correlated `operation_result` bus event. The floop decides which agent handles
that event. There is no hidden same-run result channel. See
[Actions](concepts/actions.md).

## Events and projections

`rt_append_event` validates and appends the event, then routes it to the
owning reducer. The generic run projector updates only run-local fields.
Separate reducers own:

- conversation memory;
- tasks and archived tasks;
- affairs and review schedules;
- managed operations;
- bound work lineage and budgets.

Their stores live under `workspace/memory/`. Replay rebuilds from the event
log; live execution does not reread the log. New behavior normally adds an
optional field, event type, reducer transition, action, or floop gate rather
than changing existing field meaning. See [Events and Replay](concepts/events-and-replay.md).

## Contexts, ordering, and recovery

Conversation contexts serialize runs. Different contexts may advance
concurrently within fixed scheduler/job bounds. Context IDs, task IDs,
request IDs, revisions, and trigger IDs remain explicit across every wake.

Run control and projections use atomic replacement. JSONL newline is the
commit boundary; recovery may discard only an uncommitted torn tail. A
request is durably marked started before an outside-world effect. If recovery
cannot prove the terminal outcome, it records `ambiguous_completion` and does
not replay the effect.

Publication outbox records bridge a committed source event to one bus wake.
They bind to the claiming run at intake and release after that run is durably
terminal. Delivery records are exactly-once inside FloofClaw; external
services remain outside that transaction boundary.

## LLM boundary

Model profiles choose provider, model, effort, output ceiling, and limits.
`runtime/llm/` serializes provider-specific requests, performs transport,
extracts text/usage/stop reason, and retains raw artifacts. Agent execution
normalizes untrusted text into call intent. Only normalized events affect
durable state.

Secrets are referenced by name in config and resolve only at the request
boundary. Authorization values are not written to provider artifacts.

## Entry points

| Question | Start here |
|---|---|
| profile loading and steps | `runtime/profile.c`, `runtime/floop.c` |
| scheduling and recovery | `runtime/scheduler.c`, `runtime/recovery.c` |
| agent normalization | `runtime/agent_exec.c`, `runtime/agent_runner.c` |
| action lifecycle | `runtime/action_runner.c`, `runtime/action_events.c` |
| bus routing | `runtime/bus/`, `runtime/ports.c` |
| event truth and projections | `runtime/event_log.c`, `runtime/*_state.c` |
| provider transport | `runtime/llm/` |
| gateway modules | `runtime/gateway/`, `runtime/channel/` |

For debugging, begin with `fclaw view -a <run_id>` and the raw per-run
artifacts. For public command shapes see [CLI Reference](reference/cli.md).

## Deliberate non-goals

The kernel does not contain conversation policy, semantic success checks,
action-specific advice, a fallback supervisor, or a second truth database.
Adapters do not call agents. Apps do not mutate runtime authority. Floops do
not mint IDs. Prompts do not duplicate action schemas.

When two designs satisfy the contract, choose the one with fewer owners,
states, documents, and moving parts.
