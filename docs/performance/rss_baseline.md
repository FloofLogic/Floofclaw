# RSS + disk baseline

**Status: ad-hoc sampler; no formal soak gate.**

The instrumentation is in `scripts/rss_soak.sh`. It writes one JSON
line per hour to `workspace/logs/rss_soak.jsonl` recording:

- RSS in KB (via `ps -o rss=`)
- Total `workspace/` disk use in bytes (derived from `du -sk`)
- Run directory count
- Line counts for each rotated JSONL log (bus, deliveries, narration,
  llm_usage)

The engine gets natural soak from daily use. This sampler is retained for a
specific investigation or optional observation window; no fixed-duration run
is required before release.

## How to run an ad-hoc observation

```bash
# Start the gateway, then in another shell:
./scripts/rss_soak.sh &
# Let it cover the window you want to inspect. Kill with `kill <pid>` when done.
```

## What "healthy" looks like

Use these trend-based expectations as investigation prompts, not as a release
gate. There is no universal RSS or disk-size band: enabled adapters, traffic,
retention overrides, toolchain, and platform all change the absolute number.

| Metric | Expected steady-state | Threshold for concern |
|---|---|---|
| RSS | Settles after startup and ordinary traffic bursts | Sustained linear growth; on macOS confirm with Mach `physical_footprint` because `ps` includes file-cache pages |
| workspace/ disk | Log growth settles within configured rotation bounds; run directories may continue growing while the known pruning defect remains open | Unexpected growth outside accumulated runs or configured log history |
| runs_count | Currently grows with processed runs because terminal-run pruning is ineffective | Use the trend as evidence for the known defect; do not treat `appliance.runs.keep` as an enforced bound yet |
| bus.jsonl lines | Resets after active file reaches 64 MiB | No reset across repeated janitor cadences |
| deliveries.jsonl lines | Resets after active file reaches 64 MiB | Same |
| narration.jsonl lines | Resets after active file reaches 64 MiB | Same |
| llm_usage.jsonl lines | Resets after active file reaches 64 MiB | Same |

Sustained growth beyond the configured queue and log-rotation bounds is a
defect to investigate. Short-window growth can be normal while an inbox drains
or before the janitor's next rotation cadence. Run-directory growth is a
separate known defect until terminal-run pruning is repaired.

The scheduler's fixed pool bounds its persistent run storage, but BSS is
faulted lazily and file I/O can inflate `ps` RSS. Actual observations from the
build and workload being investigated—not an old absolute estimate—are the
authority for steady-state memory.

## Recording the actual baseline

When an ad-hoc observation is worth retaining, record:

- Peak, mean, and p95 RSS across the window
- Mean disk usage after rotation kicked in
- Any anomalies observed (spikes, restarts, external interruptions)
- Commit hash the observation ran against
- Which loop (the selected floop)
- Approximate traffic level (turns/hour, worker ops/hour)

Retained observations can be compared over time when investigating a suspected
regression. They are not a required release artifact.

## Not a substitute for

- **Leak detection tools.** If RSS drifts up, `leaks` (macOS) or
  `valgrind --tool=massif` (Linux) tell you *where*.
- **Load testing.** This measures steady-state behavior at whatever
  real traffic exists. Deliberate load-testing is a separate exercise.
- **Per-adapter cost analysis.** RSS is a single number; if it's high
  we need to look at `size bin/fclaw` module contributions, per-run pool
  sizes, etc. Baseline first, decompose after.
