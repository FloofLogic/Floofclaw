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
[you disconnect; a detached worker runs; polls happen in the background]
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
memory.before -> chat_manager -> review_manager -> result_manager -> dispatch -> workers -> memory.after -> memory.compact
```

- `chat_manager` (LLM, gated `event_kind:user_message`) handles ordinary chat,
  replies, opens concerns with `affair_open`, and queues bounded `work`.
- `review_manager` (LLM, gated `event_kind:affair_review`) decides
  whether to fetch a recorded source, add a note, defer, message, or close.
  Data-returning fetches complete through a later `operation_result` run;
  silence is the correct response to "nothing changed".
- `result_manager` (LLM, gated `event_kind:operation_result`) routes fresh
  worker or tool evidence to the user, task, or affair note that owns it. It
  does not close affairs: closure stays on a later `affair_review`, whose
  review manager has the active lifecycle context.
- `dispatch` is the ordinary `action_runner` builtin.
- `workers` (native, gated in for user/review/poll/result events)
  applies the attempt rule through its configured `handler` binding
  (an action implementing the managed-operation contract):

```text
no attempt recorded                        -> start at current work_rev
attempt running                            -> poll (on poll events only)
attempt terminal, rev == current work_rev  -> stop; do nothing
attempt terminal, rev <  current work_rev  -> start one attempt at the
                                              current rev, fed the prior
                                              attempt's result
```

A task left open after a terminal attempt never restarts by itself —
only a newer revision makes it eligible again. That is the
no-blocker-retry-loop invariant, enforced with integer comparison, not
prose interpretation.

## Profile options it uses

Declared in `loop.json`, all generic:

| option | effect |
| --- | --- |
| `one_pass` | one event → one run → one complete profile pass; never rewinds to phase zero or changes task state |
| `serialize_contexts` | same-context runs advance in intake order; other contexts advance concurrently |
| `retry_attempts` | bounded per-event retry after a failed phase job; committed calls are memoized by effect signature so retries never re-deliver or re-execute |
| `poll_interval_ms` | managed-operation poll cadence; also the switch that enables the operation driver for the floop |
| per-step `non_critical` | maintenance failure (memory phases) never fails the processed event |
| per-step `gate: "event_kind:a,b"` | step runs only for the listed triggering-event kinds (opaque tokens) |

## The Managed-Operation Contract

An action declares `"contract": "managed_operation"` in its
`action.json`:

```text
start(args)  -> running(handle) | finished(result)
poll(handle) -> running         | finished(result)
```

The **operation driver** (kernel service, enabled by
`poll_interval_ms`) reacts to terminals of contract-declaring actions,
dispatching on the declared contract — never on action names, and
never reading meaning out of result text:

- **record**: first `running(handle)` appends `operation_started` with
  full correlation (task, context, delivery route) and projects the
  attempt into `state.external` with the `work_rev` it started from.
- **schedule**: at most one poll timer per handle; a timer that fires
  after terminal is a no-op; a failed adapter call re-arms the timer so
  a transient fault cannot strand a running operation.
- **publish**: `finished(result)` appends exactly one
  `operation_result` per handle and publishes it as a correlated bus
  envelope; it re-enters the floop through normal intake as a new run.

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
- `usage` — LLM calls/tokens per agent plus worker-operation counts,
  so cost questions are answered with real numbers.
- `memory` recall is byte-bounded (clipped texts, oldest dropped) so a
  chatty tail can never blow the event budget.

## Watching it run

- The channel configured by `channels.irc.ops_channel`: the runtime
  narrates reviews fired, workers started/finished, notes, defers,
  concerns opened/closed, and `RUN FAILED` reasons as one-liners
  (`workspace/logs/narration.jsonl`, mirrored by the IRC adapter).
- `app/pulse` — live board: concerns with review countdowns, work and
  attempts with poll timers, recent runs with failures highlighted,
  and the message log with proactive messages starred.

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
