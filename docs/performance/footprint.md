# FloofClaw runtime footprint

Measurements snapshot. For the heap-budget / per-event allocation
discipline, see [Heap budget](heap-budget.md).

The current reproducible source and binary measurements are emitted by
`make metrics`, including date, platform, compiler, and revision. The detailed
tables below preserve the 2026-07-20 macOS/arm64 hardening snapshot and must
not be read as current release values.

The 2026-08-25 arm64 macOS measurement (Apple clang 17) reports 42,665 runtime
C/header lines and a 557,192-byte stripped `release-small` executable carrying
all four shipped adapters. Repeated isolated samples put the mock-backed
`hello` around 0.2 seconds and idle gateway RSS around 8–10 MiB. The older 667,304-byte
number was the default `-O2`, WS-only build and is not the distributed-binary
claim. `./scripts/metrics.sh` includes the exact profile, adapters, and revision;
RSS and wall time are naturally variable samples, not the Mach
`physical_footprint` stress metric described below.

Idle RSS fell from ~11,840 KiB when the scheduler stopped being memset at
startup. The struct is ~4.8 MB, nearly all of it the fixed run pool; zeroing
it faulted every page before a single event arrived. `fc_xcalloc` hands back
lazily-faulted zero pages instead, and each pool slot is cleared as it is
taken, so only slots actually in use become resident.

## LOC

At the historical snapshot, total source under `runtime/` (`.c` + `.h`) was
**26,919 lines** across 137 files (`0.22.0-stable-floofclaw`).

Top files:

| file | lines |
|---|---:|
| `runtime/action_runtime.c` | 1,883 |
| `adapters/irc/irc_adapter.c` | 927 |
| `runtime/affair_state.c` | 887 |
| `runtime/task_state.c` | 884 |
| `runtime/gateway/janitor_module.c` | 775 |
| `adapters/ws/ws_adapter.c` | 648 |
| `runtime/support/json.c` | 638 |
| `runtime/memory.c` | 633 |
| `runtime/view.c` | 600 |
| `runtime/gateway/gateway.c` | 591 |

## Binary

Historical sizes on macOS arm64:

| Build target          | `ls -la` size | Make target     |
|-----------------------|---------------|-----------------|
| Dev (unstripped)      | 460,104 bytes | `make`          |
| Historical release (stripped) | 432,160 bytes | `make release` |
| Current all-adapter small release | 557,192 bytes | `make release-small` |

The dev build keeps symbols so backtraces from crashes are readable.

`size` reports 376,832 bytes of `__TEXT` for this build. BSD `size` also
reports a roughly 4 GiB `others` value for page-zero virtual address space;
that is not on-disk content. Use the file size above for storage comparisons.

## Memory

**Trust `physical_footprint` (Mach), not `ps RSS`.** On macOS, `ps`
counts file-cache pages mapped through the unified buffer cache,
which inflates dramatically for I/O-heavy processes (the gateway
under load writes thousands of small files; `ps` reports those as
the gateway's working set).

The cap-saturation thrash driver reports both `peak_rss_kb` and
`peak_footprint_kb` in its JSON output. The footprint number is what matches
Activity Monitor's "Memory" column.

When a dedicated stress sweep is run (`fast` floop + native `noop_agent`), keep
the resulting local baseline artifact as the current measured source for wall
time, throughput, p99, RSS, and Mach `physical_footprint`; do not copy numbers
here unless they are freshly re-measured in the same change.

## Caps and pool sizes

Reported at gateway startup:

```
gateway: rtrun_size=226048 max_active=16 pool_bytes=3616768
```

Measured at 0.28.0 on arm64 macOS:

| | size |
|---|---:|
| `RT_MAX_ACTIVE_RUNS` | 16 |
| `sizeof(RtRun)` | 226,048 bytes (~221 KiB) |
| `RtScheduler.pool` | 3,616,768 bytes (~3.45 MiB), faulted per used slot |
| `RT_MAX_PENDING_ACTIONS` per run | 16 |
| `sizeof(RtPendingAction)` | 1,368 bytes |
| `RT_MAX_COMPLETED_ACTIONS` per run | 32 |
| `sizeof(RtContext)` | 175,512 bytes |
| `sizeof(RtActionRegistry)` | 898,576 bytes |
| `sizeof(RtProfile)` | 20,760 bytes |
| `sizeof(RtScheduler)` | 4,849,632 bytes (~4.62 MiB) |
| `sizeof(RtJob)` | 12,216 bytes |
| `JOB_OUTPUT_CAP_STDOUT_DEFAULT` | 8 MiB per child |
| `JOB_OUTPUT_CAP_STDERR_DEFAULT` | 1 MiB per child |
| `RT_INTAKE_PER_TICK` | 4 envelopes/tick |

Historical throughput sweeps peaked at 10–14 active runs. The current thrash
gate deliberately fills all 16 slots, verifies that saturation rejects loudly,
and proves a released slot is reusable; excess inbox files remain queued for a
later drain.

## What scales with N

The on-disk inbox at `workspace/bus/inbox/<id>.json` scales temporarily with
publish rate. The scheduler holds 16 active runs in memory at peak; the rest
queue as files until slots free up. The fixed scheduler pool does not grow with
N, while transient queues and files are controlled by explicit caps and
retention work.

`workspace/runs/<id>/` accumulates one subdirectory per processed run. The
default configuration asks the janitor to retain 500 terminal runs and check
every five minutes. JSONL rotation defaults to a 64 MiB active-file cap with
five rotated files. Local config remains the authority for enabled cleanup
tasks and their limits. The retained count is a floor, not a ceiling: only
terminal, unpinned runs are eligible, so a run that never terminates — a
crash, a hang, a run still pinned by an unreconciled publication — is kept
deliberately, which is the correct forensic outcome.

## See also

- [Heap budget](heap-budget.md) — per-event allocation budget and
  measurement caveats
- [Architecture](../architecture.md) §Memory And Hot Paths
- [Principles](../concepts/principles.md) §6 Static shape on the hot path
