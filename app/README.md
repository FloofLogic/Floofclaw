Optional local apps that inspect or exercise FloofClaw live here.

These apps are not on the runtime hot path. Treat the C runtime, loop
profiles, agents, actions, and run artifacts as the source of truth.
JSON view models are generated snapshots, not current project-status
documents — scripts/up.sh regenerates them on a loop from the selected
workspace. Never point a documentation refresh at a live installation.

- `pulse/` shows live affairs, work, runs, and messages on port 8003.
- `runtime_flow/` explains configured and observed runtime flow on port 8002.
- `tokenwatch/` shows per-agent usage, cache evidence, and estimated spend on
  port 8004.
