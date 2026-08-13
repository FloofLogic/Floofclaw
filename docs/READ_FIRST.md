# Read this first

FloofClaw is a **transparent agent orchestration runtime**. It composes
LLM agents and drives workers (Codex and Claude Code) underneath. Every LLM
call is a first-class inspectable run. For the full framing see
[what_is_floofclaw.md](what_is_floofclaw.md).

The load-bearing rules any change must obey:

**The event model.**

- Inputs become events.
- Events open or wake frames (runs).
- Frames are advanced by the loop.
- The loop is the only place time moves.
- The reducer is the only place state changes.
- Actions are only attempted through explicit queues.
- Outcomes are not assumed; they return as events.
- The system stops advancing when there is no runnable consequence left.

**The data model.**

- Affairs are not tasks. Affairs are durable *concerns* an agent
  watches over time. Tasks are bounded units of *work* with completion
  criteria.
- LLMs are untrusted fuzzy transforms. Their raw output stays in
  artifacts; only the reducer-produced events are truth.
- Chat is just I/O. Discord, IRC, WebSocket, CLI — all publish
  `user_message` events on the same bus.
- Workers (Codex and Claude Code) are subprocess actions. They report back
  via `operation_result` events; they don't own state.

**The composition model.**

- Every data-returning action is a managed operation. Its result flows
  through the bus as an `operation_result` event → a later configured agent
  turn handles it. The old same-run task-artifact pickup path is removed.
- Each event kind can gate a step. A floop may route kinds to separate agents
  or intentionally route several kinds to the same agent; the kernel does not
  choose that product policy.
- Adding a new event kind requires an explicit gate/step contract. Whether it
  uses a new agent or an existing one belongs to the floop.

**The observability model.**

- `event_log.jsonl` is the source of truth.
- Every triggering envelope gets an inspectable run in `workspace/runs/`.
  A run may make more than one provider call (for example, a retry or a
  second gated LLM phase); each call is captured separately under that
  run's `provider_calls/` directory and in the usage ledger.
- Nothing is hidden inside a "turn." Count provider-call artifacts or
  `workspace/logs/llm_usage.jsonl` records to count model invocations;
  do not treat the number of run directories as the call count.

Nothing else in the docs overrides this.
