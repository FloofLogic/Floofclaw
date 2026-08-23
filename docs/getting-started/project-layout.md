# Project Layout (current runtime)

**Preconditions:** none. Paths are relative to the repository root; run
`make -j4` first only when you want the generated `bin/` artifacts.

## Top-level directories

- `runtime/` — the current C runtime: kernel, scheduler, gateway reactor, modules, support utilities. **One binary**: `bin/fclaw`.
- `floops/` — shippable floop bundles. Each `floops/<name>/` contains `loop.json` plus the agents used by that loop.
- `loops/`, `agents/` — test fixture fallback paths populated by tests, not production configuration in a fresh checkout.
- `actions/` — outside-world capabilities. Every directory holding an `action.json` is discovered recursively at startup. Bundled shared capabilities live under `actions/common/` and runtime primitives under `actions/core/`; other top-level action areas are local installation surface and ignored by Git. Subprocess actions also have `run.sh`. There is no registration list — copying a directory in installs it, and a `_` prefix (`actions/_github`) unloads one without deleting it. `web_read/_pluck` is build source, and its leading underscore keeps the scanner out.
- `adapters/` — channel adapters, one self-contained source directory each (`adapters/discord/`, `adapters/irc/`, `adapters/ws/`). Each exports a `const FcAdapter fc_adapter_<name>` (`runtime/gateway/adapter.h`); the build discovers the directories and generates the registry, so adding one is: copy the directory, `make`, restart. Statically linked into the same single binary — nothing is dynamically loaded.
- `tests/` — fast C invariant tests plus bash-driven smokes. `make unit` runs the C suite; `make test` runs the full suite.
- `workspace/` — runtime artifacts (event logs, runs, bus, deliveries). Gitignored.
- `docs/` — documentation for the current runtime (this tree).
- `app/` — optional local inspection UIs such as `runtime_flow`; not on the runtime hot path.
- `tmp/` — ignored local scratch when present. It is not shipped or
  authoritative for runtime behavior.

## Adapter source layout

Each directory is self-contained and statically linked. The build generates
`build/adapter_registry.gen.c` from this listing, so nothing here is named in
the engine.

```text
adapters/
├── discord/
│   ├── discord_adapter.c        — descriptor + reactor callbacks
│   ├── discord_gateway.c        — websocket gateway protocol
│   ├── discord_rest.c           — REST API client
│   ├── discord_outbox.c         — outbound message queue
│   └── discord_internal.h       — adapter-local shared types
├── irc/irc_adapter.c            — nonblocking IRC client adapter
└── ws/ws_adapter.c              — nonblocking WS server adapter
```

## Runtime source layout

The runtime remains C-only and is split by responsibility. This is a selected
ownership map rather than an exhaustive file inventory; current measured
footprint details live in [Footprint](../performance/footprint.md).

```text
runtime/
├── main.c                       — argv dispatch
├── runtime.h                    — shared types, RT_SMALL/MED/LARGE/XL constants
├── version.h
│
├── runtime_kernel.h             — RtRun, RtRunState, RtPendingAction, RtScheduler, RT_* caps
├── run_internal.h               — cross-file private decls (scheduler/run/agent/action/ports)
├── scheduler.c                  — pool, init, reactor pass, finished-job pump, shutdown
├── run.c                        — per-run lifecycle, advance accounting, step machine
├── recovery.c                   — interrupted-run discovery and recovery
├── agent_runner.c               — native/llm/script agent paths, agent meta cache
├── agent_exec.c                 — agent metadata, prompt rendering, invocation artifacts
├── agent_meta.c                 — agent.json parser + capability cache
├── agent_model.c                — listen-projected processor input builder
├── agent_selected.c             — narrow conversational/affair/memory output normalization
├── agent_errors.c               — classified agent error events
├── action_internal.h            — private action dispatch/validation contract
├── action_runner.c              — pending-action queue, action launch, terminal events
├── action_runtime.c              — runtime-intrinsic actions (message, write_file, read_file, manage_codex, manage_claude)
├── action_registry.c             — action.json registry, schemas, prompt docs
├── action_subprocess.c           — subprocess action launcher
├── action_events.c               — action lifecycle event emission
├── action_output.c               — compact action stdout extraction
├── action_coerce.c               — type-sloppy arg coercion before validation
├── task_state.c                 — task reducer, lifecycle state
├── task_artifacts.c             — task artifact normalization
├── task_projection.c            — task projection for processor input
├── affair_state.c               — affair reducer + watcher helpers (load/write/list_due)
├── affair_cli.c                 — `fclaw affair create|list|pause|resume|close|review|schedule`
├── operation_state.c            — durable managed-operation state (ids, completion tokens, deadlines)
├── operation_driver.c           — completion-contract driver: preallocation, claim terminalization, result publication
├── operation_cli.c              — `fclaw operation complete` worker-side claim submission
├── ports.c                      — bus envelope intake, completion-claim admission, deliveries.jsonl writes
├── ports.h                      — ports public API
├── run_state.c                  — runstate envelope persistence
├── internal_cli.c               — `fclaw internal agent-llm` child entrypoint, `fclaw internal detach`
│
├── profile.c                    — loop profile loader
├── floop.c                      — floop bundle resolver
├── event_log.c                  — append + validate agent intent, stamp runtime fields
├── state.c                      — reducer (events → state.json + ctx projection)
├── trace.c                      — trace.jsonl writer
├── view.c                       — `fclaw view` CLI (human/agent rendering)
│
├── usage_cli.c                  — `fclaw usage`
├── qol_cli.c                    — clear / actions / adapters / floops / setup / version
├── auth_cli.c                   — `fclaw auth …`
├── bus_cli.c                    — `fclaw bus publish|reserve|log`
├── gateway_cli.c                — `fclaw gateway start|run|status|stop|reload`
├── channel_cli.c                — `fclaw channel cli|ws|irc`
├── cacheview.c                  — `fclaw cacheview` raw-request diff
│
├── memory.c                     — durable memory reducer + compaction policy
├── json.c                       — small json helpers (rt_json_*)
│
├── native_agents/
│   ├── memory.c                 — native memory.before/after agent
│   ├── workers.c                — native worker dispatch agent
│   └── noop.c                   — perf-test fixture agent
│
├── bus/
│   └── bus.c                    — workspace/bus/inbox enqueue + log
│
├── gateway/
│   ├── reactor.c                — collect_fds → poll → dispatch on_fd → tick
│   ├── reactor.h                — FcReactorModule contract, FcPollSet
│   ├── adapter.h                — FcAdapter contract; fc_adapters[] is generated
│   ├── gateway.c                — start/stop/status, daemon lifecycle
│   ├── jobrunner.c              — pipe-backed child job table
│   ├── jobrunner_module.c       — reactor module wrapping the runner
│   ├── bus_intake_module.c      — imports bus envelopes per tick
│   ├── wake_module.c            — Unix datagram bus-wake socket
│   ├── runtime_module.c         — pumps finished jobs, steps ready runs
│   ├── status_module.c          — gateway status socket
│   ├── janitor_module.c         — appliance cleanup and log rotation
│   └── affair_watcher_module.c  — fires affair_review envelopes for due affairs
│
├── channel/
│   └── channel.c                — synchronous channel helpers shared by adapters
│
├── llm/
│   ├── call.c                   — provider dispatch (mock vs http)
│   ├── http.c                   — libcurl easy mode (blocking, runs inside child)
│   ├── mock.c                   — deterministic fixtures
│   ├── normalize.c              — strict event normalization around raw model text
│   ├── profile.c                — provider/profile resolution
│   ├── request.c                — provider request construction
│   ├── provider_limit.c         — local RPM/daily caps
│   └── usage.c                  — token + latency tracking
│
├── fjson_repair/
│   └── repair.c                 — fixed-buffer syntax repair for LLM JSON candidates only
│
├── secure/
│   ├── auth.c                   — endpoint header resolution
│   ├── auth_targets.c           — auth route table
│   └── secret_store.c           — mode-restricted file-backed secret store
│
└── support/
    ├── json.c                   — span-based JSON parser (no allocation)
    ├── fsutil.c                 — fs_read_text / fs_append_text / fs_mkdir_p
    ├── net.c / net_tls.c        — nonblocking socket + OpenSSL helpers
    ├── heap_guard.c             — per-event allocation counter for budget tests
    ├── duration.c / log_rotation.c / reconnect_backoff.c / scratch_guard.c
    ├── color.c / logo.c / timing.c
    └── stringhelp.h
```

## Runtime paths

- `.fclaw/run/` — transient instance-control state: gateway pid and loop name,
  the runtime-owner lock, gateway status socket, and bus wake socket.
- `workspace/bus/inbox/` — pending bus envelopes (one JSON file each).
- `workspace/bus/processed/` — consumed envelopes after `bus_pop_oldest`.
- `workspace/media/inbound/` — private content-addressed manifests for
  inbound attachments; signed source URLs exist only here.
- `workspace/runs/<run_id>/` — per-run artifacts:
  - `event_log.jsonl` — behavioral truth
  - `state.json` — materialized state (reducer output)
  - `runstate.json` — durable scheduler/recovery envelope
  - `trace.jsonl` — diagnostics
  - `agent_outputs/` — raw agent inputs/outputs/stderr
  - `action_calls/` — raw action inputs/outputs/stderr
  - `provider_calls/` — request, response, metadata, and raw wire artifacts
    when the run invokes an LLM; raw requests are private files because a
    media call contains base64 input bytes
  - `media/` — bounded, verified run-local attachment files for a media call
- `workspace/logs/`
  - `bus.jsonl` — append-only bus envelope log
  - `gateway.log` — gateway lifecycle
  - `narration.jsonl` — operator-facing runtime narration
  - `llm_usage.jsonl` — provider usage and latency records
  - `deliveries.jsonl` — channel delivery records
  - `rejected_events.jsonl` — envelopes rejected at intake
  - `provider_limits/<provider>.json` — local RPM/daily limit state
  - `channels/<channel>.log` — per-channel transcript
- `workspace/memory/memory.jsonl` — durable append-only memory records
- `workspace/memory/memory.json` — derived recent memory state for listen-based agent input
- `workspace/memory/state/tasks.json` — durable task reducer output
- `workspace/memory/state/tasks_archive.jsonl` — append-only archive of completed tasks after context quiescence
- `workspace/memory/state/affairs.json` — durable affair reducer output (used by the affair watcher)
- `workspace/memory/state/operations.json` — durable managed-operation state
- `workspace/memory/state/summary_state.json` — memory compaction state
- `workspace/logs/codex_ops/<op_id>/` — managed Codex operation state, stdout, stderr, final.txt
- `workspace/logs/claude_ops/<op_id>/` — same shape for managed Claude Code operations
- `config/floofclaw_config.json` — gateway + channel config
- `config/llm_registry.json`, `config/model_profiles.json` — LLM config

## Starting fresh

The workspace has multiple coupled stores: `workspace/runs/`, task and task
archive state, affair state, managed-operation state, memory, the bus, and
logs. They're written by different events but reference each other by ID, so
they need to be wiped together.

### The collision contract

Run IDs are monotonic per workspace, starting one higher than the greatest
existing `workspace/runs/run_<N>/` directory. Task IDs are
`task_<run_id>_<event_seq>`. If you wipe `workspace/runs/` without
wiping the rest, the next `fclaw run` reuses old run IDs (run_001
becomes free again) but the task store still has the old tasks.
The runtime detects this and refuses to start with
`workspace_task_id_collision`, pointing at this section.

The same refusal fires when stale tasks live in
`workspace/memory/state/tasks_archive.jsonl` — the archive is part of
the same naming scheme.

### To start fresh

```bash
./bin/fclaw clear -h --yes     # wipes runs, memory, media, bus, logs, .fclaw
```

`fclaw clear -h --yes` is the documented human-facing path. It preserves
`workspace/*.json` config you've written by hand (the smoke fixtures
read those) and removes only the runtime-managed subtrees. See
[CLI Reference: `fclaw clear`](../reference/cli.md#fclaw-clear--actions--adapters--floops--setup----version)
for the exact target list.

### Why not auto-clean

The runtime could auto-wipe when it sees a collision, but that
hides the contract. The refusal is loud, the message names the
exact fix, and the rare case it fires is usually "I accidentally
ran `rm -rf workspace/runs/`" — the user wants to know.

`tests/run_in_isolated_copy.sh` is the testing path that side-steps
this entirely by syncing the repo (minus `workspace/`) to a temp
dir per run. The C test binaries require that wrapper's physical-root marker
and refuse before teardown or fixture writes when invoked directly in the
checkout. The integration gate includes a SIGKILL regression proving a test
killed after writing copied actions, a copied floop, and the copied shipping
registry leaves Git status unchanged.

## Key properties

- `make test` runs 32 unit cases, 139 integration cases, fixture-isolation and
  public-interface contract probes, and one hermetic, mock-provider-backed
  smoke using local fixtures only.
- `FCLAW_LIVE_SMOKE=1 make live-smoke` checks the opt-in real-provider path
  through the `responses` profile by default; set `FCLAW_LIVE_PROFILE_REF` to
  select another profile.
- The gateway is a single binary, thread, and process at idle.

See also:

- [Architecture](../architecture.md)
- [Footprint](../performance/footprint.md)
- [CLI Reference](../reference/cli.md)
