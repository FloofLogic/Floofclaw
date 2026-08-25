# What is FloofClaw

**FloofClaw is a transparent agent orchestration runtime.** It composes
multiple focused LLM agents together with heavy-lifting workers (Codex,
Claude Code, and other subprocess research tools) into working products, and
every step of that composition is inspectable.

It is not another coding assistant. It is the layer *above* coding
assistants — the thing that decides which agent handles what, dispatches
work to the workers, keeps durable state, and shows you every LLM call
it made along the way as a separate provider-call artifact.

## The one-line pitch

> A tiny C runtime that composes focused LLM agents into products,
> drives coding-agent workers underneath, keeps durable state across
> days, and makes every compute cost visible in inspectable runs and
> provider-call artifacts.

## What that means concretely

### Multiple focused agents, not one big one

Most agentic tools give you one LLM with tools. Complex behavior means
a fatter prompt. FloofClaw splits behavior across small agents, each
handling one event kind:

- `chat_manager` — user says something
- `review_manager` — a scheduled affair check-in fires
- `result_manager` — a worker (Codex, web fetch, and others) returns

The floop (`floops/<name>/loop.json`) dispatches events to agents by
kind. Each agent's prompt describes its role, not a case tree covering every
situation. Adding a new event kind requires an explicit floop gate and step;
the floop may route it to a new specialist or an existing agent. The kernel
does not decide that product policy.

### Workers below, agents above

Coding assistants (Codex and Claude Code) do the heavy lifting
— multi-file edits, shell commands, actual research. FloofClaw drives
them as *workers*. A FloofClaw agent decides "this needs Codex" and
dispatches `manage_codex` with a task description. Codex runs. Its
result comes back as an event. Another FloofClaw agent digests the
result and replies to the user.

The agents are thin classifiers and composers. The workers are the
heavy lifters. FloofClaw is the orchestration layer between them and
the outside world (Discord, IRC, WebSocket, CLI).

### Every run and provider call is inspectable

Under most agentic runtimes, one user message produces one visible
"turn" that might have made 4 LLM calls internally. You can't tell
without inspecting the runtime's private state.

Under FloofClaw, every triggering envelope becomes a run in
`workspace/runs/`. A run can contain more than one LLM invocation when
an agent retries or a later gated LLM phase runs. Every invocation gets
separate files under `provider_calls/`, while
`./bin/fclaw view -a <run_id>` shows the run's complete event truth.

This is a first-order product virtue: **the runtime does not hide
compute cost from you.** Count `provider_calls/` artifacts or
`workspace/logs/llm_usage.jsonl` records to measure model calls; replay
the run's event log to verify behavioral state.

### Durable perspective

FloofClaw agents have durable state that persists across days:

- **Affairs** — ongoing concerns. "Watch for Kill Tony tickets." The
  affair store is a JSON file. The affair watcher reactor module
  publishes review events when concerns come due. Reviews can trigger
  work, add notes, defer.
- **Tasks** — bounded units of work with completion criteria and
  external state (which worker is running, which handle, revision).
- **Memory** — per-context conversation history. The memory compactor
  agent produces distilled summaries so context stays useful without
  growing forever.

An agent has a perspective — "here's what I'm watching, here's what
I've noted, and here's what the workers I dispatched came back with."
That perspective is real data in real files, not an in-memory
conversation that vanishes when the process dies.

### Event-driven, uniform, replayable

Every accepted action intent becomes an event. Events become state through
reducers at append time. `workspace/runs/<run_id>/event_log.jsonl`
is the durable trace of one run; `workspace/logs/bus.jsonl` is every
envelope that crossed the bus.

Data-returning actions (`web_read`, `read_file`, `bash`, and others) are
**managed operations**. The `work` action is fire-and-forget task admission;
its configured worker runs as a managed operation. When managed operations
finish, they publish an `operation_result` bus event. Another run picks up that
event and routes it to the agent configured for operation results. That may be
a separate result role (`floofclaw`) or the same continuing main agent
(`openclaw`). Every result crosses an event boundary; exact durable task and
request correlation replaces hidden same-run coupling.

This uniformity makes the runtime observable, debuggable, and
replayable. It also means adding a new tool is mechanical: declare it
managed-op, and configured result turns receive it through the existing path.

### One binary, one long-lived process, zero magic

FloofClaw is one C binary (667,304 bytes stripped in the current macOS arm64
source-release measurement; `make metrics` reproduces it) and one
long-lived, single-threaded gateway process with no pthreads on the hot
path. LLM agents and subprocess actions appear as bounded, short-lived
child jobs while work is active. Runtime state is under `workspace/`;
configuration lives under `config/`, `floops/`, and `actions/`. The
runtime and current harnesses are C and bash; a few existing app/test
utilities in Python are explicitly grandfathered. A coding agent can
hold the runtime in its head; a human can audit it in an afternoon.

## What FloofClaw is not

- **Not a coding assistant.** It drives coding assistants. Codex and
  Claude Code are workers, not competitors.
- **Not a chat framework.** It's an event-driven runtime that happens
  to have chat as one kind of I/O among many.
- **Not agentless.** The runtime is not just a pipe between the user
  and a worker — it has agents with their own values, memory, and
  perspective. A product floop can present a companion, not just a UI.
- **Not an in-process library.** It's a runtime process. State is on
  disk. You interact with it via bus events and channels, not by
  calling C functions.

## The differentiator, said differently

Codex and Claude Code are like individual strong programmers.
FloofClaw is like a project manager who keeps those programmers busy,
remembers what everyone is working on, notices
when a concern hasn't been checked in a while, tells the user when
something matters, and shows the manager's own thinking as an
inspectable trail. The programmers do the code work. The manager
composes them.

## Where to go from here

- **Read the architecture** — [architecture.md](architecture.md) for
  the technical shape, kernel, reducers, boundaries.
- **Choose a product loop** — [the floop guide](getting-started/floops.md)
  covers the fresh-clone `hello` default, `companion`, the durable
  [floofclaw floop](concepts/floofclaw_floop.md), and `openclaw`.
- **Understand the unified result flow** —
  [action_result_flow.md](concepts/action_result_flow.md) explains
  why every data-returning action goes through the event bus.
- **First-time build** —
  [installation-and-build.md](getting-started/installation-and-build.md)
  → [first-run.md](getting-started/first-run.md).
