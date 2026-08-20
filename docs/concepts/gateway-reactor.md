# Gateway Reactor

The gateway is a **single-process, single-thread** reactor over `poll(2)`.
It's the piece that runs while you're online — what lets IRC pings, WS
clients, and a 30-second LLM call coexist without anything blocking.

There are **no pthreads** and no long-running adapter subprocesses. LLM/script
agents and subprocess actions run as short-lived jobs watched through
`waitpid(WNOHANG)`. Log rotation may also launch a short-lived `gzip` helper;
daemon startup forks once to create the long-lived gateway process.

## Main loop

The loop in `runtime/gateway/reactor.c` is structurally:

```c
while (!stop) {
  fc_pollset_clear(&ps);
  for each module: m->collect_fds(m, &ps);

  earliest = now + default_timeout;
  for each module: earliest = min(earliest, m->next_deadline_ms(m, now));
  timeout = max(0, earliest - now);

  poll(pfds, ps.count, timeout);

  for each ready fd: ps.entries[i].owner->on_fd(...);

  for each module: m->tick(m, now);
}
```

Four phases per pass — `collect_fds → poll → dispatch on_fd → tick`.
Nothing else. No subsystem-specific phase names live in the loop.

## Modules

Every reactor participant implements `FcReactorModule` in
`runtime/gateway/reactor.h`:

```c
struct FcReactorModule {
  const char *name;
  const char *module_id;
  void *state;

  int  (*init)(struct FcReactorModule *m);
  int  (*collect_fds)(struct FcReactorModule *m, FcPollSet *ps);
  int  (*on_fd)(struct FcReactorModule *m, int fd, int events, void *tag);
  int  (*tick)(struct FcReactorModule *m, uint64_t now_ms);
  void (*shutdown)(struct FcReactorModule *m);
  uint64_t (*next_deadline_ms)(struct FcReactorModule *m, uint64_t now_ms);
};
```

The reactor doesn't know what any of them do. It just calls these
callbacks in order.

Hard rule: **no callback may block.** `collect_fds`, `on_fd`, `tick`
all return promptly. Long-running work goes into a child whose
stdout/stderr pipes are observed via the jobrunner module.

## Registered modules (in tick order)

Order matters when modules share state.

1. **`bus_wake`** (`runtime/gateway/wake_module.c`) — Unix datagram socket that lets
   external processes (CLI, bus publish) kick the reactor out of `poll(2)`
   immediately instead of waiting for the timeout
2. **`jobrunner`** (`runtime/gateway/jobrunner_module.c`)
   - `collect_fds` — every running job's stdout + stderr pipe read end
   - `on_fd` — drain bytes from ready pipes nonblocking, tee into out_path / err_path
   - `tick` — `waitpid(WNOHANG)` for each running pid; mark `JOB_FINISHED` only when `exited && stdout_eof && stderr_eof`
   - `next_deadline_ms` — min over jobs of `deadline_ms` and `sigterm_at_ms + kill_grace_ms`
3. **`bus_intake`** (`runtime/gateway/bus_intake_module.c`)
   - `tick` — pop up to `RT_INTAKE_PER_TICK` envelopes from `workspace/bus/inbox/`, parse, create an `RtRun` per accepted envelope
   - `next_deadline_ms` — `now` if `bus_inbox_has_pending()` else `FC_NO_DEADLINE`
4. **`runtime`** (`runtime/gateway/runtime_module.c`)
   - `tick` — pump finished jobs back into runs, wake action-slot waiters, step ready runs (bounded), retire terminal runs
   - `shutdown` — `rt_scheduler_shutdown()` cancels in-flight runs, drains the runner with grace, reaps everything
   - `next_deadline_ms` — `now` if any `RT_RUN_READY` or any job in `JOB_FINISHED`/`FAILED`
5. **`status`** (`runtime/gateway/status_module.c`) — serves the gateway status socket (`.fclaw/run/gateway.status.sock`) for `fclaw gateway status` and external inspectors
6. **`affair_watcher`** (`runtime/gateway/affair_watcher_module.c`)
   - `tick` (every ~5s) — walks `workspace/memory/state/affairs.json`; for each active affair whose `next_review_at_ms` has passed (i.e. `now ≥ next_review_at_ms`), publishes an `affair_review` bus envelope and stamps `affair_reviewed` (which clears `next_review_at_ms` to 0). Re-arming after that is the review manager's `defer` call with `args.value` set to a duration (or the CLI's `affair schedule -h`); without re-arming, the affair stays dormant. Skips affairs whose `context_id` already has an active run, and serializes multiple due affairs within the same context across ticks
   - `next_deadline_ms` — `now + (next_tick_ms - now)` so the reactor wakes at the next 5s tick boundary
7. **`janitor`** (`runtime/gateway/janitor_module.c`) — periodically rotates
   JSONL logs and prunes processed bus records, archived tasks, and Codex
   worker directories. Terminal-run retention is configured here but is
   temporarily ineffective because the janitor reads the wrong lifecycle
   field from `runstate.json`; see the
   [known limitations](../../CHANGELOG.md#known-limitations)
8. **`logo_anim`** when foreground output is a TTY — optional display module
9. **`irc`** (`adapters/irc/irc_adapter.c`) — connect/PING/PRIVMSG state machine; tails `workspace/logs/deliveries.jsonl` for outbound `message` deliveries matching its `module_id`
10. **`ws`** (`adapters/ws/ws_adapter.c`) — listens, completes RFC-6455 handshake, routes inbound text frames to bus, delivers outbound messages to the matching connection
11. **`discord`** (`adapters/discord/discord_adapter.c`) — Discord Gateway WSS client + REST poster; publishes attended `MESSAGE_CREATE` events to the bus and consumes `message` deliveries addressed to `discord-main`

Channel adapters (9-11) are not named anywhere in the gateway. Each is a
self-contained source directory under `adapters/` exporting a
`const FcAdapter` (`runtime/gateway/adapter.h`); the build generates
`fc_adapters[]` from the directory listing and the gateway loops over it. To
add one: create `adapters/<name>/`, export `fc_adapter_<name>`, build.

Configuration can omit optional channel modules, but the relative order of
registered modules remains the order above: adapters always register after
every internal module, because order is semantic where modules share state.
Order *among* adapters is not semantic — each owns its own socket and delivery
cursor — so they register in the generated registry's sorted order.

## Job model

Every loop step that needs external work becomes a job:

```c
typedef enum { JOB_AGENT, JOB_ACTION } JobKind;
typedef enum { JOB_RUNNING, JOB_FINISHED, JOB_FAILED } JobState;
```

Two job kinds:

- **`JOB_AGENT`** — runs `floops/<floop>/agents/<id>/run.sh` (script agent) or
  `fclaw internal agent-llm` (executor=llm agent). Agents can run
  **concurrently**; multiple `user_messages` arriving at the same time
  each progress in their own run.
- **`JOB_ACTION`** — runs a registered `actions/<area>/<id>/run.sh`. Actions are
  **serialized**: one action at a time across the entire gateway. A run
  whose next step is an action request and the slot is busy enters
  `RT_RUN_WAITING_ACTION_SLOT`; the runtime tick wakes it when
  `rt_jobrunner_count_active(JOB_ACTION) == 0`.

This is the v1 conservative rule that prevents two actions from racing
on `workspace/memory/`, file writes, or external state. Future
`action.json` lock scopes are deferred until the single-action bottleneck
becomes a measured problem.

## Pipe handling

The runtime now uses **pipes** for child stdout/stderr, not redirected
files. The parent owns the pipe read ends, drains nonblocking on `POLLIN`,
and tees the bytes into `out_path` / `err_path` so callers reading the
artifacts see the same files as before.

A job reaches `JOB_FINISHED` / `JOB_FAILED` only when:

```text
exited (waitpid returned)  &&  stdout_eof  &&  stderr_eof
```

This means we read every byte before declaring the job done. Output
caps (`stdout_cap=8 MiB`, `stderr_cap=1 MiB` defaults) are enforced as
bytes drain; over-cap triggers `SIGTERM` with `killed_for_output_limit`
set on the job.

## Per-run state machine

`runtime/runtime_kernel.h`:

```c
RT_RUN_READY                 // can advance synchronously
RT_RUN_RUNNING               // currently advancing through profile steps
RT_RUN_WAITING_JOBS          // agent or action subprocess jobs in flight
RT_RUN_WAITING_EVENT         // claimed inbound (outbox wake / completion claim)
                             // whose first behavioral append is not yet proven
RT_RUN_WAITING_ACTION_SLOT    // pending action request, slot held by another run
RT_RUN_DONE
RT_RUN_FAILED
```

`rt_run_step_ready` advances exactly one phase (or, if waiting, returns
without launching). It inspects in-memory state only — no event log
scanning, no JSON reparse — and returns one of:

```text
RT_STEP_ADVANCED
RT_STEP_WAITING_JOB
RT_STEP_WAITING_ACTION_SLOT
RT_STEP_DONE
RT_STEP_FAILED
```

`RtRun.phase_index` is the single in-memory phase cursor. It always means
"next profile phase to execute." Before an agent or action wait, the runtime
sets `phase_index` to where execution should continue when the wait clears.
Implicit post-agent effects therefore resume at the next phase naturally.
Explicit `action_runner` phases that intentionally restart the profile set
`phase_index` to `0` before waiting on the action job.

Runstate v1.3.8 serializes both `RT_RUN_WAITING_EVENT` and
`RT_RUN_WAITING_ACTION_SLOT` as `status: "waiting_event"`. `waiting.events`
remains reserved and empty.

Pending actions are tracked in a per-run **fixed-size embedded array**
(`RtPendingAction[RT_MAX_PENDING_ACTIONS]`) maintained by
`rt_action_runner_note_request`, `rt_action_runner_note_started`, and
`rt_action_runner_note_terminal` as events are appended. The array itself is
static; each nonempty `args_json` is a bounded-lifetime heap-owned compact
string. Hitting the cap emits a loud `error` event—never silent truncation.

## Static-shape ownership

The scheduler's persistent shape is static. Transient parsing and file work
stay inside the measured per-event heap budget: at most 40 mallocs/event and
zero reallocs/strdups in the native no-op boundary test.

```text
RtScheduler
├── runner             ← jobrunner state
├── pool[16] of RtRun  ← run slots, never malloc'd
│   ├── ctx            ← per-run context (event log path, etc.)
│   ├── profile        ← borrowed pointer to scheduler.profile
│   ├── scheduler      ← borrowed back-pointer for cache lookups
│   ├── pending_actions[16]   ← embedded array; args_json payloads heap-owned
│   └── …
├── used[16]           ← slot occupancy bitmap
├── runs[16]           ← compacted active list (pointers into pool)
├── profile            ← RtProfile loaded once at init
└── agent_meta[…]       ← executor + model_ref preloaded per agent
```

Sizes printed at gateway start:

```text
gateway: rtrun_size=210688 max_active=16 pool_bytes=3371008
```

`sizeof(RtScheduler)` is 4,837,856 bytes, `sizeof(RtProfile)` is 20,760,
`sizeof(RtPendingAction)` is 1,176, and `sizeof(RtJob)` is 12,216 in this
measurement. Across a long event stream, each run goes through
`scheduler_alloc_run` → `scheduler_free_run` (mark-used / mark-free, no
per-run pool allocation). The 210,688-byte struct is reused; the allocation
budget covers bounded transient work rather than persistent growth.
See [Heap budget](../performance/heap-budget.md) for the per-event
allocation budget and measurement story.

## Dynamic poll timeout

The poll timeout is no longer hardcoded. Each module's
`next_deadline_ms` returns the absolute time it wants its next tick by:

- jobrunner: min over running jobs of `deadline_ms` / `sigterm_at_ms + kill_grace`
- bus_intake: `now` if inbox is non-empty
- runtime: `now` if any run is `RT_RUN_READY` or any job is
  `JOB_FINISHED`/`JOB_FAILED`; otherwise the earliest managed-operation
  completion deadline, so a silent worker's timeout fires without any
  periodic wakeup
- irc: `next_reconnect_ms` while `IRC_DISCONNECTED`

The reactor takes the min across modules, clamps to `default_timeout_ms`,
and uses `max(0, earliest - now)` as the `poll(2)` timeout. So
"immediate work" never waits for a tick boundary, and pending timers are
never overslept.

## Channel routing

Inbound (`channel → bus`):

- adapter parses transport (IRC PRIVMSG, WS frame, Discord MESSAGE_CREATE, …)
- emits a `user_message` envelope to `workspace/bus/inbox/` via
  `bus_publish`
- payload includes `adapter_id`, optional reply-routing `ref`, `text`, and,
  for attachment-capable transports, an optional compact generic `media`
  manifest reference
- attachment descriptors are allocated only for media-bearing messages;
  bytes and signed source URLs never enter the bus envelope or reactor-owned
  scheduler structs

Outbound (`runtime → channel`):

- an LLM agent emits model call intent: `{"calls":[{"name":"message","args":{"message":"..."}}]}`
- the runtime stamps that into a `message` `action_request` with a runtime-owned
  `request_id`
- the action runner dispatches the registered action through its runtime or
  subprocess implementation, then emits `action_succeeded` or `action_failed`
- if the run originated on a channel, the kernel attaches a `delivery`
  block: `{delivery_id, origin_event_id, channel, adapter_id, ref?, text}`
- the kernel also appends a line to `workspace/logs/deliveries.jsonl`
- channel adapters tail `deliveries.jsonl` from their init-time cursor;
  each adapter consumes only deliveries whose `delivery.channel` and
  `delivery.adapter_id` match its own (`channel` + `module_id`)

## Replay protection

Adapters that tail `workspace/logs/deliveries.jsonl` (and IRC's optional
`narration.jsonl` feed) initialize their cursor at the current EOF and act only
on later bytes. This prevents a restart from replaying old outbound log lines.
Those JSONL files are evidence/hand-off logs, not a general-purpose outbound
queue; runtime recovery and delivery IDs own the durable execution contract.

## Shutdown protocol

On `SIGINT`/`SIGTERM` the reactor loop first stops, so no module receives more
network or intake callbacks. Shutdown hooks then run in registration order.
The runtime module calls `rt_scheduler_shutdown(grace_ms = 3000)`, which:
   - SIGTERMs every running job;
   - waits up to 3 s, ticking the runner so jobs reap as they exit;
   - SIGKILLs and reaps survivors; and
   - appends `run_canceled` with `gateway_shutdown` for each remaining run.
Channel hooks then send IRC `QUIT` / close frames, status and wake sockets are
closed, owner files are removed, and the gateway exits.

That is the graceful-shutdown path: active runs receive a durable
`run_canceled` terminal. A hard crash follows a different path. On cold start,
the recovery pass repairs a torn final event record, rehydrates nonterminal
runs, recommits claimed inbound work when needed, and reconciles unfinished
action effects. A request with no committed `action_started` claim may resume
under its stable request ID. A started outside-world request with no provable
terminal is never replayed or inferred successful; recovery records a
correlated `ambiguous_completion` blocker. Declared local actions may replay,
with duplicate outcomes remaining visible. Committed runtime-delivery ledger
entries remain exactly-once; remote channel transport itself is not
transactional.

## DNS

Outbound adapters call blocking `getaddrinfo` on connection attempts. A failed
lookup is narrated and enters the shared reconnect backoff; a hung resolver
can stall the reactor for the platform resolver timeout, a known and accepted
tradeoff because a bot without DNS is down anyway. No resolver subsystem is
planned.
