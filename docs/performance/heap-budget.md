# Heap budget

The runtime uses firmware-style static ownership for persistent scheduler
state and a measured budget for transient work. The native no-op boundary test
permits **at most 40 mallocs per event** and requires **zero reallocs and zero
strdups**. Pools and fixed buffers keep queues and long-lived state bounded;
the remaining parsing and file-read allocations are explicit and measured.

## Why this matters

"No leaks" is not the same as "constant memory."

`leaks(1)` proving every alloc has a matching free only tells you
the program eventually returns memory. It says nothing about whether
the allocator is being hit hard during steady-state operation.
Allocator hot paths cause:

- RSS bloat — the allocator keeps freed pages mapped into the process
  rather than returning them to the kernel. macOS in particular
  attributes file-cache pages to the process via `ps`, masking what
  actually came from heap churn vs file I/O.
- Throughput stalls — allocator bookkeeping can block the hot path under
  bursty load and create a cliff with no obvious code-level bottleneck.
- Latency variance — small-block allocator paths add workload- and
  platform-dependent jitter to otherwise deterministic scheduling.

A bounded program with bounded queues is the cleanest answer:
predictable RSS, predictable latency, predictable throughput.

## Measurement

Two pieces of instrumentation, both committed:

### Heap guard (`runtime/support/heap_guard.{h,c}`)

Explicit allocation-counting wrappers:

```c
void *fc_xmalloc (size_t);
void *fc_xcalloc (size_t, size_t);
void *fc_xrealloc(void *, size_t);
char *fc_xstrdup (const char *);
void  fc_xfree   (void *);
```

Each increments an atomic counter, then forwards to libc. Scheduler-path
allocations covered by the permanent native no-op budget go through these; the
counter does not claim to interpose on libc, third-party libraries, or
child-process/provider allocations. A baseline snapshot is taken after gateway
initialization, and the shutdown delta supplies the measurement used by the
budget tests.

Why explicit wrappers and not `DYLD_INTERPOSE`: tried it; macOS's
main-executable interposition doesn't engage on macOS 15. Explicit
wrappers also have the benefit that "what counts" is exactly "calls
we deliberately routed through the wrappers" — no ambiguity about
whether libc's internal mallocs are in the budget.

The gateway prints the delta at shutdown:

```
gateway: heap mallocs=N callocs=N reallocs=N strdups=N frees=N
```

### Mach physical_footprint, not `ps RSS`

The cap-saturation thrash driver records both:

- `peak_rss_kb` — sample of `ps -o rss=`. Kept for diff against old
  numbers, but documented as **unreliable** on macOS for I/O-heavy
  processes. It includes pages from the unified buffer cache.
- `peak_footprint_kb` — kernel-tracked peak `physical_footprint`,
  read once at shutdown via `vmmap --summary`. This is the same
  number Activity Monitor's "Memory" column shows. **Trust this.**

A real example from the investigation:

```
Physical footprint:         7,217 KB
Physical footprint (peak):  40,144 KB
ps -o rss=:                 704,000 KB     ← ignore this
```

`ps RSS` reported 700 MB on a workload whose actual peak working
set was 40 MB. The 660 MB delta was kernel file cache from writing
~150 k small files during the burst. Not a leak. Not a regression.

## Per-event budget (current)

Run `make integration` to exercise the permanent budget checks. The gateway's
heap-guard counters at shutdown provide the underlying measurements:

- mallocs ≤ events × 40
- reallocs = 0
- strdups = 0

Do not treat an old counter sample as current unless it came from the build
being investigated. The bound is deliberately a ceiling, not a promise that
every event performs exactly the same number of allocations.

## Structural allocation already eliminated

| concern | how |
|---|---|
| `RtRun` struct churn | static pool of `RT_MAX_ACTIVE_RUNS` slots in `RtScheduler`; never `malloc`'d |
| `RtPendingAction` queue | embedded `[RT_MAX_PENDING_ACTIONS]` array per run |
| pending-action queue shape | embedded 16-entry array; each nonempty `args_json` is one heap-owned compact string, freed when the call completes or the run slot resets |
| inbound media on text-only runs | one optional pointer remains `NULL`; descriptors, downloaded bytes, and the provider body are created only in a media-bearing LLM child |
| inbox dirent names | stack `char names[1024][64]` — no per-name `strdup` |
| `RtProfile` | loaded once at scheduler init, borrowed by all runs |
| Agent metadata (executor, model.ref) | preloaded at scheduler init by walking the profile |

These remove persistent allocator growth and queue churn. Further reductions
are optimizations only when measurement justifies them; zero mallocs is not a
current engine invariant.

## Pool / static-shape sizes

Counts that the gateway reports at startup and that you should know:

```
gateway: rtrun_size=226048 max_active=16 pool_bytes=3616768
```

Measured at 0.28.0 on arm64 macOS:

| | size |
|---|---:|
| `sizeof(RtRun)` | 226,048 bytes (~221 KiB) |
| `RT_MAX_ACTIVE_RUNS` | 16 |
| `RtScheduler.pool` | 3,616,768 bytes (~3.45 MiB) — resident per used slot |
| `sizeof(RtScheduler)` | 4,849,632 bytes (~4.62 MiB) |
| `sizeof(RtProfile)` | 20,760 bytes (cached once, borrowed) |
| `RT_MAX_PENDING_ACTIONS` per run | 16 entries × 1,368 bytes each = 21,888 bytes |
| `RT_MAX_COMPLETED_ACTIONS` per run | 32 IDs/signatures/origin IDs |
| `sizeof(RtJob)` | 12,216 bytes |

Historical 100–10k event sweeps observed `max_runs` peaks of 10–14. The current
thrash gate deliberately fills all 16 slots. A boundary test verifies that the
seventeenth is rejected and that a released slot is reusable.

## Stress harness

```bash
make integration   # permanent per-event heap-budget checks
make thrash        # active-cap and sustained-load proof
```

The retired pre-thrash performance campaign historically swept
`N ∈ {100, 500, 1000, 5000, 10000}` and produced the older observations cited
above; there is no current `perf` target. `make thrash` is the maintained load
harness and reports throughput, active-run saturation, RSS, and Mach
`physical_footprint` against the current binary.

## See also

- [Footprint](footprint.md) — the LOC / binary / RSS numbers
- [Principles](../concepts/principles.md) §6 — "Static shape on the hot path"
