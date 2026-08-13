# Runtime principles

This page is the distilled set of rules the runtime is being built to.
The hardening refactors were instances of these rules. Read it
before adding anything to the hot path.

## 1. Reactor stays generic

The main loop is exactly four phases per pass:

```
collect_fds  →  poll  →  dispatch on_fd  →  tick modules
```

No subsystem-specific phase. No "if this is the runtime module, do X."
The reactor doesn't know what any module does — it asks each one for
fds, polls, dispatches readied fds back to their owner, and ticks
everyone in registration order.

If you find yourself writing a new branch *inside* the reactor, the
right answer is a new module, not a new phase.

## 2. Everything is a module

Channel adapters (IRC, Discord, WS), the jobrunner, the bus intake,
the run scheduler, even the foreground logo animator — all of them
implement the same `FcReactorModule` shape. Same `init / collect_fds /
on_fd / tick / shutdown / next_deadline_ms`. There is no privileged
subsystem.

This is what made it possible to drop the old `FcChannelAdapter` /
`bus_drainer` distinction. Anything that wants to participate in the
reactor pass becomes a module.

## 3. Callbacks must not block

Hard rule. `collect_fds`, `on_fd`, `tick`, `next_deadline_ms` all
return promptly. No `read()` that might wait, no `system()`, no
`llm_call`, no `kill -0` loops. Blocking outbound `getaddrinfo` is the
single documented exception: failure enters channel backoff, while a hung
resolver may stall the reactor for the platform timeout because the bot is
already down without DNS.

Long-running work runs in a child process whose stdout/stderr pipes
are observed via the jobrunner module. The reactor only ever calls
`waitpid(WNOHANG)`.

## 4. Event log is the trace, not the input queue

`event_log.jsonl` is durable. It exists for replay, debugging, and
inspection. **Live state never reads it.**

Events become run state through `apply_*_event` functions called from
`rt_append_event` *at append time*. The in-memory `RtContext` is
always current. Re-reading `event_log.jsonl` to recompute state is the
slow path, used by cold-start recovery for nonterminal runs and by `fclaw
replay` against a frozen log — never as the live per-tick input queue.

Practically: every time you write an event, the reducer projection
runs once, in memory. There is no "after the run, read everything
back and figure out what happened."

## 5. Run-state projector vs durable subsystems

Different concerns, different modules.

- The **run-state projector** (`runtime/state.c::rt_apply_event_to_context`)
  updates per-run fields: `latest_action`, `latest_result_text`,
  `latest_message`, `user_text`. Pure in-memory, no file I/O.

- **Durable subsystems** own files under `workspace/memory/`. Memory records
  and control, tasks, affairs, and managed operations each have a reducer;
  `rt_append_event` routes matching event types to the owning reducer.

The generic projector does **not** call into the durable subsystem.
That separation prevents one from accidentally double-applying or
silently dropping a side effect.

## 6. Static shape on the hot path

The per-event hot path has a **measured allocation budget** rather than an
unbounded heap shape. The native no-op boundary test permits at most 40
`malloc` calls per event and requires zero `realloc` and zero `strdup` calls.
The runtime remains firmware-style where ownership is structural: fixed pools,
fixed scratch buffers, and explicit bounds remove allocator churn from queues
and scheduler state.

Concrete instances landed:

- `RtRun` slots: pool of `RT_MAX_ACTIVE_RUNS` embedded directly in
  `RtScheduler`. Allocated once. Slots are reused; the 210,688-byte struct
  never goes through `malloc`/`free`.
- `RtPendingAction[RT_MAX_PENDING_ACTIONS]` array embedded in `RtRun`.
  No growable heap-backed queue.
- Each nonempty pending `args_json` is one bounded-lifetime heap-owned compact
  string, freed when the request terminates or the run slot resets.
- A text-only run keeps its generic ingress-media pointer `NULL`. A
  media-bearing run allocates only the compact manifest reference; attachment
  descriptors, files, and the provider body belong to the isolated LLM child.
- `RtProfile` cached once at scheduler init; runs borrow a pointer.
- Agent metadata (`executor`, `model.ref`) preloaded at scheduler init,
  one read per agent used by the loop.
- `inbox_scan_sorted` uses a stack `char names[1024][64]`. No `strdup`
  per readdir entry.

When you're tempted to add persistent heap growth mid-run, the answer is
usually "declare a hard bound, embed the buffer, fail loud at the cap."
When a measured transient allocation is simpler and bounded, keep it inside
the tested budget and make its ownership explicit.

## 7. Bound the queue. Fail loud at the cap.

Static buffers need an explicit size. When that size is exceeded:

- emit an `error` event with the cap, the offending value, and enough
  context to understand what was dropped, **and**
- skip the operation cleanly (don't truncate silently).

Never truncate without saying so. A pending-action overflow that
silently drops a request is worse than one that fails the run.

## 8. Cache config at scheduler init, not per event

Files that don't change for the gateway's lifetime:

- the floop profile (`floops/<name>/loop.json`)
- per-agent `floops/<name>/agents/<id>/agent.json`
- the action registry and per-agent metadata/allowlists cached by the scheduler

Reading these on every event was unjustified allocator pressure that
made gateway hot-path malloc count grow with N. Load once at init,
borrow by pointer.

## 9. Agent executors and actions

Agents and actions are not the same thing.

| executor | use for | mechanism |
|---|---|---|
| `native` | runtime-owned pure phases | C function in `runtime/agents.c`, called inline |
| `llm` | model-backed phases | runtime renders prompt + action docs, then starts `fclaw internal agent-llm` as a child job |
| `script` | dev / experiments / external adapters | fork+exec `floops/<floop>/agents/<id>/run.sh` |
| `action` | outside-world side effects | isolated subprocess with output cap + timeout |

Anything FloofClaw ships inside a floop and can be deterministic should be
`native`. Model-backed phases use `llm`. `script` is the escape hatch, not the
default.

## 10. Actions can be runtime intrinsics

`message` is a "tool" at the model/contract layer (agents emit a
`action_request` intent for `message`) but a runtime intrinsic at the
execution layer (no fork+exec, runtime emits `action_started` +
`action_succeeded` / `action_failed` directly).

The agent never knows the difference. The model sees the same tool
schema. Channels see the same delivery records. The runtime saves a
fork+exec.

## 11. The reactor never spins

The poll timeout is dynamic: `min(default_timeout, min over modules
of next_deadline_ms)`. If any module reports immediate work
(`<= now_ms`), poll returns 0. If no one has anything to do, we sleep
to the default ceiling.

Equally important: the reactor doesn't busy-loop when there's pending
work. The dynamic timeout ensures we wake exactly when needed.

## 12. Replay protection at adapter init

When an outbound adapter starts, it records the current EOF offset of the
logs it actually tails (`workspace/logs/deliveries.jsonl`, plus
`narration.jsonl` for IRC ops narration). It only acts on later bytes, so
startup does not replay historical deliveries. This is restart-spam
protection, not transactional exactly-once delivery to an external network.

## 13. Stable IDs end to end

Every envelope, every event, every delivery has an explicit ID:

- `event_id` on bus envelopes and event log entries
- `origin_event_id` on user_messages so the run can refer back to the
  thing that caused it
- `run_id`, `request_id`, `delivery_id` follow the same rule

There is never an "implied current" — refer to things by ID.
LLMs do not author these IDs; they describe intent and the runtime assigns
durable identity.

## 14. Measurement discipline

Two measurement rules:

- **`ps RSS` is unreliable on macOS** for I/O-heavy processes. It
  includes pages from the unified buffer cache (file pages mapped
  back through the kernel). Use Mach `physical_footprint` (Activity
  Monitor's "Memory" column) for real working-set measurements.
- **`alloc count == free count` is necessary but not sufficient.** No
  leaks ≠ constant memory. The heap guard
  (`runtime/support/heap_guard.{h,c}`) provides the counters; the integration
  allocation-budget tests enforce at most 40 mallocs/event with zero
  reallocs/strdups, and `make thrash` checks bounded behavior under load.

## 15. Boring and obvious

When two designs work, the simpler one wins. Concretely:

- A fixed array beats a growable queue if the cap is known.
- A pointer to scheduler-owned config beats a per-run copy.
- A loud error on overflow beats a flexible-but-mysterious truncation.
- An apply-at-append projector beats a periodic event-log scanner.
- A new reactor module beats a new branch in the reactor.

If you can't explain a design as one of the rules above, look harder
before committing to the cleverness.

## 16. The kernel is a walker, not a judge

The runtime kernel walks profiles, runs steps, validates envelopes,
appends events, waits on jobs. That is its entire job description.
It does **not** decide whether the work an agent did was sufficient,
appropriate, or interesting. It does not check for the presence of
"important" event types. It does not know what a good run looks like.

If a check inside the scheduler reads as **"this advance didn't
produce X, so the run failed"** — that check is wrong. Move it. The
right place is one of:

- the **agent** that owns the behavior (the conversation/chat agent decides
  whether to speak, not the kernel)
- the **profile** that owns the recipe (gate steps, branch loops,
  pick which agents run when)
- the **reducer** for a specific durable concern (memory authority,
  task events), where the rule is about what an event *means* on
  disk, not about whether enough events fired

The kernel's only loop guard is a full-profile pass cap
(`RT_MAX_PROFILE_PASSES`): if a run keeps cycling without quiescing,
fail it. That is a literal infinite-loop guard, not a verdict on
agent quality. End-of-profile with no pending jobs and no accepted
work in the pass is itself a terminal condition; the kernel finishes
the run.

The same boundary applies to bound-work persistence. A controller agent's
`work_policy.max_repair_attempts` decides how many repair turns its product
wants. The runtime enforces that declared value mechanically and retains an
absolute compiled ceiling as a runaway-cost guard; it does not hard-code the
product's normal repair count.

This rule generalizes: **whenever a fix asks the runtime to know
something product-shaped, the contract is wrong, not the runtime.**
Find the boundary that should have caught it and fix that. The
kernel is allowed to be dumb. That is its strength.

(See "What the scheduler is not" in `docs/architecture.md` for the
specific case that produced this rule.)

## 17. Stable envelopes

Runtime envelopes are contracts, not prompt conventions. The runtime owns
durable IDs, trusted source, timestamps, append order, and lifecycle
transitions. Agents emit intent only. Actions own only result/error body
content. Reducers own their durable projections.

When a feature needs more behavior, prefer adding an event type, reducer, action,
or optional field over changing existing field meaning. If an envelope shape
must break, version it and update the checksum/source-scan tests in the same
change.

## 18. Stack discipline: new core code is C and bash

New runtime code is **C** and new tests, harnesses, and developer tooling are
**bash**. Going forward, do not introduce
Python, Rust, JavaScript, Go, Lua, or any other language into this
repository. The runtime is C because performance and predictability
are first-class. Tests, harnesses, and developer tooling are bash
because the runtime they exercise is bash-shaped — agents are scripts,
actions are scripts, the CLI is text, the bus is files.

Why this rule:

- A second language brings a second toolchain, packaging story, and
  dependency surface. `make metrics` records the current stripped binary size;
  keeping that artifact small depends on resisting extra toolchains.
- Performance work expects to read C. A perf bug hidden in Python
  test infra is invisible until it isn't.
- Reproducibility: bash + a C compiler are present on every dev
  machine. Nothing to install, nothing to lock, no `requirements.txt`
  drift, no `Cargo.lock` arguments.

Existing app-local Python/JavaScript (including the visual app generators) and
the existing `scripts/irc_probe.py` probe are
**grandfathered**. Do not add more non-C/non-bash code to the runtime, tests,
or production tooling. When grandfathered scripts are touched substantially,
prefer rewriting them to bash + jq + a small C helper if needed.

External binaries the bash scripts shell out to are **fine** when they
are pre-installed and pinned by the OS toolchain (`make`, `awk`, `sed`,
`find`, `nc`, `curl`, `jq`). They aren't this repo's own code.

The runtime supports exactly two optional third-party libraries: OpenSSL and
libcurl. A build may omit either for the mock/offline path; adding a third
native dependency needs a real case.

This rule applies to *this repo's own code*. Actions are scripts and
can be written in any language; that's a property of the action area,
not this rule. What's banned is FloofClaw itself growing a second
language.

## 19. A bad tool call is the action's bug, not the prompt's

When an agent calls an action with wrong or missing arguments, the fix goes
in **`action.json`** — the top-level `description` and the per-property
`description`s in `args_schema`. It does not go in the calling agent's
prompt.

Agent prompts stay **generic**. They reference the injected action catalog
and never enumerate per-action argument rules. The registry renders both the
description and the schema to every agent that may call the action
(`rt_action_render_available`), so one edit to the manifest reaches every
caller, deterministically, forever.

Patch a prompt instead and you have fixed it for exactly one agent, in one
floop, until someone writes another. The next agent to call that action
makes the same bad call, and the guidance now lives in two places that will
drift.

This is principle 16 pointed one layer up. There, a fix that asks the
*runtime* to know something product-shaped means the contract is wrong.
Here, a fix that asks a *prompt* to know something action-shaped means the
action's contract is wrong. In both cases: find the boundary that should
have carried the knowledge, and fix that.

Concretely, if a model calls an action wrongly, ask what the manifest failed
to say:

- format not stated, or stated without an example — say `RFC3339 dateTime
  like 2026-08-01T15:00:00-05:00`, not `RFC3339`
- relative values not ruled out — say `an absolute timestamp; never a
  relative word like "today"`
- required combinations not stated — say which fields each `kind` needs
- when to reach for the action at all — that belongs in the top-level
  `description`

The engine side of this is already right and needs no change: the catalog
surfaces `description` and `args_schema` to every allowlisted agent. These
fixes are **data-only edits to `action.json`**.

One hard constraint: the top-level `description` must fit `RT_MED` (512
bytes, `RtActionDef.description`). A longer one fails manifest load with
`invalid action contract` and the gateway will not boot. Keep it
usage-focused; `args_schema` has `RT_LARGE` (4096) for per-field detail,
which is where format rules belong anyway.

*(This rule was learned twice from the same defect: a work manager calling
`gcal` with `time_min: "todayT18:00:00-00:00"`. `time_min` said only
"RFC3339" while the neighbouring `start` gave a concrete example. Both times
the first instinct was to patch the work manager's prompt. Both times that
was wrong — the schema field was.)*
