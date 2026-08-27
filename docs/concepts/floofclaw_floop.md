# The floofclaw floop

The floofclaw floop is the full shipped floop for durable, background concerns: the
bot accepts long-running requests, keeps working while the user is
gone, and speaks up on its own when something matters. A fresh clone selects
the smaller `hello` mock floop by default; select `floofclaw` when these
durable manager behaviors are wanted.

Two canonical conversations define it:

```text
you:  check for Kill Tony tickets and let me know when they're available.
bot:  I'll keep an eye on it.
[an affair is created; every day a review checks; nothing changed -> silence]
bot:  Tickets are on sale — you should grab one now.
```

```text
you:  put this YouTube playlist on my Spotify.
bot:  I'll work on that.
[you disconnect; a detached worker runs; the runtime waits silently
 for its completion claim — no polling, no runs, no token spend]
bot:  I can't continue: SPOTIFY_API_KEY is unavailable. Put it in the
      FloofClaw environment and tell me when it's ready.
```

## The Three Durable Things

1. **Affair** — a standing concern with one scheduling field,
   `next_review_at_ms`. The gateway's affair watcher publishes an
   `affair_review` bus envelope when it is due (carrying the affair's
   full context route, including adapter). An active affair with no
   armed review is dormant.
2. **Task** — bounded durable work intent. Immutable root; mutable
   `state.work` / `state.done_when`; a reducer-owned `state.work_rev`
   revision counter that increments on every accepted work-mutating
   update.
3. **Attempt** — one detached external execution of a task at a
   specific `work_rev`, tracked by an opaque adapter-owned operation
   handle in the durable operation store
   (`workspace/memory/state/operations.json`).

## The Loop

```text
memory.before -> chat -> review -> admission.dispatch
             -> work.select -> work.dispatch
             -> result -> result.dispatch
             -> memory.after -> memory.compact
```

- `chat_manager` (LLM, gated `event_kind:user_message`) handles ordinary chat,
  replies, opens concerns with `affair_open`, and queues bounded `work`.
- `review_manager` (LLM, gated `event_kind:affair_review`) decides
  whether to fetch a recorded source, add a note, defer, message, or close.
  Data-returning fetches complete through a later `operation_result` run;
  silence is the correct response to "nothing changed".
- `work_manager` (LLM, gated `work_consequence`, bound via
  `bind_task: "open_work"`) advances the open work task one exact
  revision at a time. The runtime, not the model, chooses the task and
  revision; the manager selects one action from its ordered catalog
  (including `manage_codex` / `manage_claude` / `manage_hermes`) and
  ends a revision only with an explicit `work_complete` or
  `work_blocked`. A managed operation it starts detaches immediately —
  the loop sleeps until the worker's completion claim (or the
  deadline's timeout claim) re-enters as its own `operation_result`
  run.
- `result_manager` (LLM, gated `terminal_work_outcome`) routes terminal
  work outcomes — completed or blocked, with the worker evidence behind
  them — to the user, task, or affair note that owns them. It does not
  close affairs: closure stays on a later `affair_review`, whose review
  manager has the active lifecycle context.
- The dispatch steps are the ordinary `action_runner` builtin; every
  manager's calls flow through one.

A custom floop may instead bind the native `workers` agent, which
applies the mechanical attempt rule against the durable operation
store:

```text
no attempt recorded                        -> start at current work_rev
attempt running                            -> wait (completion arrives
                                              as its own bus-driven
                                              result run)
attempt terminal, rev == current work_rev  -> stop; do nothing
attempt terminal, rev <  current work_rev  -> start one attempt at the
                                              current rev, fed the prior
                                              attempt's result
```

Either way, a task left open after a terminal attempt never restarts by
itself — only a newer revision makes it eligible again. That is the
no-blocker-retry-loop invariant, enforced with integer comparison, not
prose interpretation.

## Profile options it uses

Declared in `loop.json`, all generic:

| option | effect |
| --- | --- |
| `one_pass` | one event → one run → one complete profile pass; never rewinds to phase zero or changes task state |
| `serialize_contexts` | same-context runs advance in intake order; other contexts advance concurrently |
| `retry_attempts` | bounded per-event retry after a failed phase job; committed calls are memoized by effect signature so retries never re-deliver or re-execute |
| `operation_timeout_ms` | optional floop-level shortening of managed-operation completion deadlines; never extends past an action's declared maximum |
| per-step `non_critical` | maintenance failure (memory phases) never fails the processed event |
| per-step `gate: "event_kind:a,b"` | step runs only for the listed triggering-event kinds (opaque tokens) |

## The Managed-Operation Contract

An action declares `"contract": "managed_operation"` plus its deadline
policy (`default_timeout_ms`, optional `max_timeout_ms`) in its
`action.json`:

```text
runtime allocates operation_id + completion token + deadline  (before exec)
start(args) -> running(worker_handle) | finished(result)
worker      -> operation_completed claim through the bus
deadline    -> the runtime's own idempotent timeout claim
either      -> one operation_result -> one result run
```

The **operation driver** (kernel service, enabled by the contract
declaration itself) reacts to terminals of contract-declaring actions,
dispatching on the declared contract — never on action names, and
never reading meaning out of result text:

- **record**: identity is allocated durably BEFORE the action executes
  (`operation_started` with full correlation and the deadline); a
  `running(worker_handle)` result binds the worker's own identity as
  kill/query metadata and projects the attempt into `state.external`
  with the `work_rev` it started from.
- **admit**: a worker's `operation_completed` claim (operation id +
  secret token + bounded result) is validated against the durable store
  BEFORE any run exists; the accepted claim becomes the one result
  run's first canonical `operation_result`. Duplicates, forgeries, and
  late claims are rejected inspectably.
- **bound**: the deadline enters the reactor's poll timeout; expiry
  submits the runtime's idempotent timeout claim through the same
  admission path (`status:"timed_out"`); completion and timeout compete
  through one reducer-owned terminal gate.
- **publish**: an immediate `finished(result)` appends exactly one
  `operation_result` and publishes it as a correlated bus envelope; it
  re-enters the floop through normal intake as a new run.

`manage_codex` and `manage_claude` implement the contract (their
legacy `op_id`/`state` fields ride alongside `handle`/`status`).
Handler substitution is proven by tests: rebinding the workers phase
from one fake adapter to another changes zero engine code.

## Actions it owns

- `work` — mechanical create-or-revise of the durable work task.
  Single-work-task scope: revises the open work task when one exists,
  creates otherwise. Asynchronous by construction.
- `affair_open` — records a standing concern with an optional first
  `review_in` duration.
- `note_add` — appends an observation to an affair: from a review, or
  mid-chat with an explicit `affair_id` (user adds a URL to a watched
  concern).
- `defer` / `affair_close` — re-arm or end a concern from its review path;
  result turns record evidence but do not own lifecycle closure.
- `web_read` — pluck-backed clean-text page fetch (raw `web_fetch`
  stays off the manager's menu).
- `message` — ordinary delivery; replies retain their inbound route, while a
  ref-less proactive delivery uses the selected adapter's configured proactive
  destination (IRC's first configured `join` channel, or Discord
  `home_channel_id` / the first allowed channel). IRC `ops_channel` is for
  narration, not normal proactive message delivery.

## Listen blocks

The three managers each declare `listen: ["event", "memory", "affairs",
"tasks", "usage"]`:

- `event` — the triggering envelope: kind, text, opaque payload.
- `usage` — LLM calls/tokens and dated USD estimates per agent plus
  worker-operation counts, so cost questions use measured usage and state
  clearly when a provider/model has no configured price.
- `memory` recall is byte-bounded (clipped texts, oldest dropped) so a
  chatty tail can never blow the event budget.

## Conversation memory and legacy contexts

Current channel memory is keyed by the complete conversation route: adapter,
channel or DM, and thread where the adapter supplies one. Compaction only
selects contexts reached through that current route key.

Installations upgraded from before v0.28 may still contain the old shared
context key (for example `chat:discord:discord-main`). It is inert historical
data: no current conversation selects it, it is never sent to a model, and it
cannot compact automatically. Its bytes still appear in aggregate
`memory_bytes` until an operator deliberately archives or removes that legacy
record during an installation-specific migration. FloofClaw leaves it intact
by default rather than guessing which newer conversation should inherit a
formerly shared history. New and active per-conversation contexts compact
normally.

## Watching it run

- The channel configured by `channels.irc.ops_channel`: the runtime
  narrates reviews fired, workers started/finished, notes, defers,
  concerns opened/closed, and `RUN FAILED` reasons as one-liners
  (`workspace/logs/narration.jsonl`, mirrored by the IRC adapter).
- `app/pulse` — live board: concerns with review countdowns, work and
  attempts with completion deadlines, recent runs with failures
  highlighted, and the message log with proactive messages starred.

## Prompt lessons that are now load-bearing

Learned live, encoded across the manager prompts:

- Small models need the literal call shape (`{"name","args"}`) with
  worked examples, or they invent their own envelope.
- "Are there any X?" is a question (queue `work`, answer in minutes);
  "let me know when X" is a concern (`affair_open`). The contrast must
  be explicit or everything becomes a watch.
- Capability honesty must be stated ("you cannot browse; only fetch a
  given URL") or the model claims fictional abilities.
- A topic ban ("never mention the backend") reads as "deflect
  questions about it"; ban the decision, not the honesty.
- Chat memory recall can out-vote instructions: a bad answer teaches
  future turns the bad answer. Correcting stored memory matters.
