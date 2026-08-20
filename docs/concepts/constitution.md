# The FloofClaw Constitution

**v2 — 2026-07-20**, rewritten at the close of the hardening pass.
The v1 text remains in private development git history.

This document is **law, not argument**. An entry belongs here only if
it is an invariant the engine makes unbypassable — and a test in this
repository proves it. Behavior — chat, memory, routing, prompts,
provider shaping — is untrusted data on the engine, never an article.
If this document grows like an app, it has already failed.

> FloofClaw is a small event-sourced interpreter for constrained,
> LLM-authorable agent processes. The core does not try to be
> intelligent. It makes certain lies and escapes impossible.

---

## Article I — Truth is the log

**Invariant.** Every meaningful transition is an event. State is a
pure reduction of the log. If the reducer accepts a log, the state it
produces parses — always.

**Mechanism.** Append-only JSONL with newline-as-commit; atomic
snapshot writes (temp → fsync → rename); a torn tail is repaired only
at the uncommitted final record, never by reinterpreting a committed
one.

**Check.** The chaos checker replays every committed log through the
production reducer; `reducer_output_parses_with_oversized_escaped_fields`;
`gateway_restart_recommits_torn_inbound_event`.

*Lesson learned:* the fuzz gate proved "reducer accepts ⇒ state
parses" can silently break at a truncation boundary
(`json_escape`, fixed `97e8b63`). The invariant is a test now, not a
belief.

## Article II — Kill it anywhere; it comes back honest

**Invariant.** SIGKILL, power loss, disk-full, or a read-only
filesystem at any instant loses at most the uncommitted tail. Entries in the
runtime delivery ledger are exactly-once. Action recovery follows the durable
pre-effect start claim: an unstarted request may resume under its stable ID,
while a started outside-world request with no provable terminal is blocked as
`ambiguous_completion` and is never replayed or inferred successful. Declared
local actions may still replay, and every duplicate outcome is audited.

External channel sends are not a distributed transaction: an adapter or
remote service can fail after accepting bytes but before acknowledging them.
The engine's exactly-once claim ends at its durable delivery ledger.

**Mechanism.** Delivery ledger; durable intake claim before the first
behavioral append; checked `action_started` append before effects;
scheduler-owned cold-start recovery; conservative outside-world blocking;
`memoized_duplicate` terminals; a reader-side commit boundary
(`bus_<digits>.json`) matching the atomic publisher; deferred
narration retry so the operator diagnostic survives the outage that
caused it.

**Check.** 500-kill randomized chaos plus targeted torn-write kills
(`make chaos`); real ENOSPC and read-only remount drills
(`make disk-full`); `action_start_claim_is_sync_and_pre_effect`;
`started_outside_action_blocks_ambiguous_completion_without_replay`; and
permanent regressions for the other recovery paths.

## Article III — Bounded and loud

**Invariant.** Every queue and buffer has a compiled cap. Overflow is
a durable, named rejection — never a silent truncation. The
steady-state hot path stays inside a measured allocation budget.

**Mechanism.** Caps are named boundary contracts in headers, not
private array trivia; heap-guard counters; static scratch instead of
giant stack frames; truncating library routines terminate cleanly and
report.

**Check.** Every cap has a boundary test that walks off the cliff on
purpose; `make thrash` holds a live gateway at the active-run ceiling
with durable backpressure; the native no-op budget test permits at most 40
mallocs per event and requires zero reallocs and zero strdups.

*Lesson learned:* a cap that rejects only in memory is not loud —
the rejection must survive a crash (waiting-jobs overflow, fixed
`4d088f4`).

## Article IV — The engine is a walker, not a judge

**Invariant.** The kernel knows no product. Floops, agents, prompts,
and configs are untrusted data. IDs, timestamps, and lifecycle
transitions are runtime-owned end to end. An agent may use only the
actions on its declared allowlist. Which provider, model, and effort a
call resolves to is declared data, never core behavior.

**Mechanism.** Action registry allowlists; model profiles referenced
by id; provider transports translate declared data (e.g. `effort` →
Gemini `thinkingLevel`, OpenAI Responses `reasoning.effort`) without
whitelisting it — the provider is the authority and rejects loudly.

**Check.** `kernel_modules_contain_no_product_identifiers` scans the
kernel sources in every suite run; contract tests own the ID and
lifecycle shapes.

## Article V — Secrets by name, debugging by evidence

**Invariant.** Runtime configuration, action intent, event logs, and provider
artifacts carry secret *references*, never secret values. Every run leaves
inspectable artifacts on disk; failures are debugged from recorded evidence,
never from guesses.

**Mechanism.** Provider auth resolves a named environment variable or a
mode-restricted local secret-store entry only at the request boundary; headers
are not written to provider artifacts. Use `fclaw auth set-stdin -h` to populate
the store. The retained `auth set <endpoint> <value>` form exposes its value in
argv and shell history and is therefore not an approved operational path.

**Check.** Provider/auth tests prove environment precedence and store fallback;
startup tests prove missing references fail with a named fix. A failure with no
artifact trail is itself a bug.

## Article VI — Proof is the release

**Invariant.** "Hardened," "stable," and "fast" are recorded,
repeatable runs — never adjectives. The suite must stay green under its
documented preconditions; a permanently red test is a bug we fix or a test we
delete.

**Mechanism.** Two tiers: `make test` (tens of seconds, hermetic, offline) is
the gate for every change and every routine release;
`make robustness` (asan, ubsan, small-stack, fuzz, chaos, disk-full, thrash —
fail-fast, about 20–23 min) runs before a major version tag or after deep
engine surgery, and never blocks routine work. Its composed filesystem proof
currently makes the full robustness target macOS-only.
Live-smoke stays mandatory for provider-path changes.

**Check.** The robustness gate found a production-reachable engine
bug on its very first run. When verifying a proof, capture the real
exit code — a status-masking pipe once hid a genuine failure.

*Lesson learned:* main sat committed-red for four days without anyone
noticing, because every eye was on a worktree. Green means the
default branch, verified clean.

## Article VII — The stack is the constitution's enforcement

**Invariant.** New core/runtime code is C11; new tests and tooling are bash.
Existing app-local Python/JavaScript and the IRC probe are explicitly
grandfathered, not precedents for new code. Two optional native runtime
dependencies (OpenSSL and libcurl), each earned in writing. The binary stays small because the kernel
refuses domain behavior — size growth demands the same written case a
dependency does.

**Check.** A clone builds and runs the hermetic routine gate with `make test`;
the smoke blocks accidental `curl` use and reads only committed local fixtures.
`make robustness` is the major-release/deep-surgery gate; `make metrics`
reproduces the current source, binary, timing, and idle-memory measurements.

---

## Deferred — reserved from v1, not law today

These v1 keystones are not implemented; they stay recorded so a
future version adds them as new events under an unchanged grammar,
never a migration:

- **Verifier-bound completion.** Reducer-owned "done" gated on a
  verify event whose class (`mechanical | structural | judgment |
  user-confirmed`) is fixed at admission. Died with the legacy
  reviewer; rebuild event-driven on `operation_result` if wanted.
- **One compiled admission gate** through which every floop —
  hand-written or generated — passes identically, with checkable
  rejection reasons.
- **A full capability broker** (filesystem scope, network, exec,
  budget) beyond today's action allowlists.

## Anti-scope — the standing rejection list

The core must refuse to absorb: affair extraction or intelligence;
smart routing; chat, memory, or speaker behavior; fleet scheduling;
per-provider protocol *shaping* beyond the one normalized transport;
case-tree prompts; engine-side judgments of whether work was good.

> Test for any proposed core addition: *"Is this an invariant the
> untrusted layer must not be able to violate?"* If it is merely
> useful behavior, it is data, not core.

---

*Give truth to the reducer. Make every crash survivable and every
overflow loud. Keep the kernel ignorant of the product, the secrets
out of the log, and the proof in a command anyone can rerun.*
