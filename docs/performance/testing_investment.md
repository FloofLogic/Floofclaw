# Testing investment

FloofClaw is not a one-shot script. It is an engine that has been
continuously worked, tested, and hardened since its first commit.
This file records how much of that work went into proving the engine,
with the methodology stated so every number can be recomputed from
the repository itself.

Historical snapshot measured 2026-07-20 at hardening-close commit `13fe4b3`.
The history counts below are intentionally pinned to that commit so later
documentation and onboarding work does not rewrite the record.

## The raw history

| measure | value | command |
|---|---|---|
| first commit | 2026-03-04 | `git log --reverse --format=%ad \| head -1` |
| calendar span | 138 days | — |
| distinct active days (≥1 commit) | 78 | `git log --format=%ad --date=short \| sort -u \| wc -l` |
| total commits | 938 | `git rev-list --count 13fe4b3` |
| commits touching test code | 339 (36%) | `git log --oneline 13fe4b3 -- tests/ \| wc -l` |
| active days including test work | 66 of 78 (85%) | `git log --format=%ad --date=short 13fe4b3 -- tests/ \| sort -u \| wc -l` |

## Engineering hours (estimate, method stated)

Assuming an average of 4 working hours per calendar day across the
project's 138-day span — a deliberate operator estimate, not a
timesheet — the engine represents roughly **550 hours of engineering
time**. On the more conservative basis of counting only the 78 days
that produced commits, the floor is **~310 hours**.

Commit share is not a timesheet. If the 36% test-touching share is used only as
a rough allocation proxy, it suggests about **200 hours** of the calendar-span
estimate or **112 hours** of the active-day floor involved testing/hardening.
The stronger evidence is the recorded machine campaigns below.

## Machine proof-hours (recorded, not estimated)

These are actual recorded campaigns, each with a dated evidence note
(the `dev_research/` notes were retired in the 2026-07-20 cleanup and
remain retrievable from git history):

- **Fuzzing:** one-hour libFuzzer qualification per parser (three
  parsers) under ASan, plus the permanent ten-minute regression
  window in every `make robustness` run. Millions of executions,
  zero findings outstanding.
- **Crash chaos:** more than 1,100 recorded SIGKILL crash-recovery
  cycles (500 optimized + 500 sanitized + targeted torn-write kills
  + independent review reruns on fresh seeds), each proving replay,
  no duplicate runtime-ledger delivery, and torn-tail repair.
- **Load thrash:** two full 10-minute campaigns at 60 events/s
  across 64 contexts — 72,000 delivered events with flat memory
  (peak physical footprint 8.0 MiB) — plus every compiled cap walked
  off its cliff on purpose.
- **Filesystem drills:** real ENOSPC on a filled ram-disk and a
  genuine read-only remount, both survived by a live gateway that
  narrated the failure and recovered without losing a delivery.
- **The suite itself:** at the pinned hardening-close commit, 20 unit + 80
  integration tests + one isolated, mock-provider-backed smoke. The current
  0.25.0 release has 32 unit + 139 integration + the same representative
  smoke, now hermetic through a committed local lookup fixture and a
  network-command guard.
  The project does not claim that every historical commit was green; the
  required policy is that changes leave the current branch green before work
  moves on.

## Why this is on record

The hardening pass (2026-07-16 → 2026-07-20) alone found and fixed,
with permanent regressions: a SIGPIPE process-killer, wall-clock
deadline drift, unbounded file reads, ~890 KB stack frames, unbounded
error-path recursion, torn-tail event-log corruption, a restart that
stranded live runs, an inbox race with atomic publishes, silently
lost failure narration during disk-full, and a nondurable cap
rejection. Every one was caught by a harness in this repository, not
by a user.

"Hardened" here is not an adjective. It is the recorded, repeatable
output of `make robustness`, backed by dated evidence notes in git
history and rerunnable on a macOS checkout with `make robustness`.
