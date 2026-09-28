# FloofClaw philosophy

FloofClaw is a small event-sourced interpreter for agent processes. It
composes focused agents and subprocess workers while keeping every decision,
effect, and result inspectable. The runtime supplies mechanics; a floop
supplies product behavior.

This is the canonical design contract. Other documents describe specific
interfaces and implementations but do not override it.

## The boundary test

The kernel is a walker, not a judge. It loads a profile, advances declared
steps, validates envelopes, runs jobs, appends events, and stops when no
runnable consequence remains. It does not decide whether work was useful,
whether a reply was interesting, or which capability should be tried next.

Put a rule in the narrowest owner:

| Rule concerns | Owner |
|---|---|
| transport decoding and rendering | channel adapter |
| bus envelopes and delivery routing | bus and ports |
| run and job progression | scheduler and runners |
| durable meaning of an event | the owning reducer |
| provider wire formats | `runtime/llm/` |
| action use and argument meaning | `action.json` |
| product roles, sequencing, and voice | floop and agent packets |

If a runtime change needs to recognize a product role, action name, or desired
conversation outcome, the boundary is wrong. Repair the contract instead.

## Truth and evidence

- Inputs become events; events create or wake runs.
- `event_log.jsonl` is behavioral truth. `state.json` and durable stores are
  reducer-owned projections.
- Reducers apply events at append time. Live execution never rereads the event
  log as a queue.
- Trace records explain decisions but are never truth.
- Raw provider and subprocess artifacts are retained. Diagnose from the exact
  run, request, and artifact rather than from a prompt or summary.
- A run may contain several provider calls. Count provider-call artifacts or
  usage records, not run directories.

LLMs author intent, not authority. The runtime owns IDs, timestamps, source,
append order, exact task bindings, revisions, and lifecycle transitions.

## Floops own behavior

A floop is a declarative composition of focused roles. It may route several
event kinds to one agent or split them among many. The kernel does neither on
its behalf.

Focused multi-step composition is intentional. For admitted work, FloofClaw's
product contract is:

```text
chat -> durable work -> bound controller -> action consequence
                              ^                    |
                              +--------------------+
                         complete or block
                                  |
                                  v
                         result message
```

Once work is admitted, every step produces exactly one successor until the
user receives a visible terminal message. The bound controller advances,
completes, or blocks the exact revision. Success, rejection, timeout, and
failure are evidence for that controller; they are not scheduler judgments.

## Actions own capability contracts

Agents can call only allowlisted actions. An action's manifest defines when it
is useful, its argument schema, timeout, execution contract, and effect class.
If a model calls an action incorrectly, improve `action.json`, especially its
top-level and property descriptions. Do not copy action-specific argument
rules into prompts.

Data-returning actions are managed operations. They terminalize once and
publish a correlated `operation_result`; a floop decides which agent handles
it. Fire-and-forget actions report their own terminal action event. No result
travels through a hidden same-run channel.

Outside-world effects are conservative under crash recovery. A request with a
durable pre-effect start but no provable terminal result is blocked as
ambiguous and is never replayed merely because retrying looks convenient.

## Durable state has one owner

The generic run projector owns only run-local fields. Memory, tasks, affairs,
operations, and bound-work lineage each have their own reducer and store.
Apps inspect those artifacts; they never become authorities.

Stable IDs travel end to end. Never infer “current” when an explicit ID,
context, task revision, or request binding exists. Extend an envelope with an
optional field rather than changing the meaning of an existing field.

## Reactor and resource discipline

The gateway is one cooperative reactor:

```text
collect_fds -> poll -> dispatch on_fd -> tick modules
```

Adapters, bus intake, runtime, jobrunner, status, watchers, and janitors use
the same module interface. Callbacks do not block. Provider calls, actions,
DNS resolution, and other long work run in supervised children or explicit
nonblocking state machines.

Queues and buffers have named caps. Overflow is loud and durable; silent
truncation is a defect. Prefer fixed ownership, bounded scratch, cached
startup configuration, and obvious code over dynamic machinery on the hot
path. The reactor sleeps until a real deadline and never spins to look busy.

## Safety and observability

- Secrets appear by reference and resolve only at the request boundary.
- Provider authorization headers are never written to artifacts.
- Channel adapters do not know agents or scheduler internals.
- Discord is a human channel. Automated deployed-bot probes use an authorized
  IRC test deployment, never Discord.
- Experiments run in disposable copies, not an operator's live workspace.
- Do not partially wipe a workspace; use the supported clear command only in
  an explicitly disposable deployment.

## Stack and change discipline

New runtime code is C11. New tests and developer tooling are bash. Existing
app generators and the IRC probe are grandfathered; actions remain separate
process boundaries and may use what their own contracts require. OpenSSL and
libcurl are the only optional native runtime dependencies.

Prefer deletion and a narrow contract over another subsystem, prompt clause,
watcher, or compatibility mode. Preserve raw evidence. Make safe boundaries
forgiving of near-misses while keeping durable event validation strict.

Every behavior change needs a concrete regression. `make test` is the routine
offline gate. Provider/startup or agent-prompt changes get the opt-in live
smoke only with credentials and explicit authority. Major releases and deep
engine surgery use `make robustness`.

The design goal is not an intelligent kernel. It is a boring, inspectable
kernel that lets well-composed agents be effective without hiding what they
did.
