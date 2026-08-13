# FloofClaw runtime footprint

Measurements snapshot. For the heap-budget / per-event allocation
discipline, see [Heap budget](heap-budget.md).

The current reproducible source and binary measurements are emitted by
`make metrics`, including date, platform, compiler, and revision. The detailed
tables below preserve the 2026-07-20 macOS/arm64 hardening snapshot and must
not be read as current 0.24.1 values.

The 2026-08-13 arm64 macOS 0.24.1 source-release measurement (Apple clang 17)
reports 39,420 runtime C/header lines, a 650,664-byte stripped executable, a
0.124-second isolated mock-backed `hello`, and 11,840 KiB RSS for an idle
isolated gateway. `make metrics` includes the exact measured revision. RSS is
a simple, naturally variable `ps` sample for this reproducible idle scenario,
not the Mach `physical_footprint` stress metric described below.

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
| Release (stripped)    | 432,160 bytes | `make release`  |

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
gateway: rtrun_size=210688 max_active=16 pool_bytes=3371008
```

| | size |
|---|---:|
| `RT_MAX_ACTIVE_RUNS` | 16 |
| `sizeof(RtRun)` | 210,688 bytes (~206 KiB) |
| `RtScheduler.pool` | 3,371,008 bytes (~3.21 MiB) always-resident |
| `RT_MAX_PENDING_ACTIONS` per run | 16 |
| `sizeof(RtPendingAction)` | 1,176 bytes |
| `RT_MAX_COMPLETED_ACTIONS` per run | 32 |
| `sizeof(RtProfile)` | 20,760 bytes |
| `sizeof(RtScheduler)` | 4,837,856 bytes (~4.61 MiB) |
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
every five minutes, but that pruning is temporarily ineffective because of a
known `runstate.json` field mismatch. JSONL rotation still works and defaults
to a 64 MiB active-file cap with five rotated files. Local config remains the
authority for enabled cleanup tasks and their limits; until the pruning defect
is fixed, do not assume the run-directory count is bounded.

## See also

- [Heap budget](heap-budget.md) — per-event allocation budget and
  measurement caveats
- [Architecture](../architecture.md) §Memory And Hot Paths
- [Principles](../concepts/principles.md) §6 Static shape on the hot path
