# Pulse

Pulse is a lightweight live-state board for the current FloofClaw
workspace: what the bot is watching, what it is working on, what its
recent runs did, and what was said. Same shape as `app/runtime_flow` —
a stdlib-only generator writes one JSON view model, a static page
renders it.

It reads the selected workspace's truth only:

- `workspace/memory/state/affairs.json` — standing concerns, next
  review times, notes; flags dormant active affairs
- `workspace/memory/state/tasks.json` (+ archive count) — work tasks,
  revisions, attempt state
- `workspace/memory/state/operations.json` — detached worker
  operations and their completion deadlines
- `workspace/logs/bus.jsonl` + `workspace/logs/deliveries.jsonl` — the
  interleaved message log, with proactive messages (reviews and worker
  results speaking up on their own) highlighted
- `workspace/runs/run_*/event_log.jsonl` — recent runs, their origin
  kinds, action calls, and outcomes (failures glow)
- `workspace/logs/gateway.log` — last started gateway pid/floop, with a
  real liveness check

It does not call the gateway or mutate runtime state. It does write the
requested JSON view model.

> **Snapshot warning:** generated `pulse.json` files are local evidence, not a
> statement of the repository's current version, branch, gateway liveness, or
> product status. Public source releases omit the snapshot. Regenerate only
> against a scratch or explicitly authorized local workspace; never read or
> publish a live user's conversation merely to refresh documentation.

## Generate

From the repo root:

```bash
python3 app/pulse/generate.py --repo-root .
```

## Serve

```bash
cd app/pulse
python3 -m http.server 8003
```

Open `http://127.0.0.1:8003/`. The page is fully static: it re-reads
`pulse.json` every 8 seconds (and on the Refresh button). Freshness
comes from re-running `generate.py` — `scripts/up.sh` does that on a
loop; standalone, rerun it by hand whenever you want a new snapshot.
