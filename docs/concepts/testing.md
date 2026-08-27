# Testing

`make test` has these layers:

| layer | binary/script | purpose |
|---|---|---|
| **Unit** | `bin/fclaw_unit_tests` | 41 fast C checks for budgets and boundary contracts |
| **Integration** | `bin/fclaw_integration_tests` | 191 in-process scheduler/runtime checks across four isolated workers |
| **Isolation** | `tests/test_fixture_isolation.sh` | harness-control refusal and killed-fixture checkout preservation |
| **Public contracts** | `tests/test_cli_output_modes.sh`, `tests/test_version_contract.sh`, `tests/test_action_cli.sh` | uniform output modes, release versioning, and the managed-worker action bridge |
| **Local client** | `tests/test_local_client_api.sh` | authenticated fixed HTTP/WS routes, streaming lifecycle, failure, fallback, and bounds |
| **Actions and apps** | `tests/test_web_read_action.sh`, `tests/test_tokenwatch_app.sh` | bundled extraction and inspection-view contracts |
| **Recovery** | `tests/test_managed_handle_restart.sh` | restart recovery |
| **Smoke** | `tests/smoke_lookup_and_poem.sh` | one isolated, mock-provider-backed scenario through `openclaw` and one through `floofclaw` |

Independent shell contracts also run in four fixed workers. The slower
rebuild-class contracts—mock-only, adapter selection, and Android ARM64 build
shape—belong to `make test-full`, not the developer loop. Every worker still
runs in its own disposable repository copy. The shell runner asserts that
every `tests/test_*.sh` is registered in either the routine or rebuild gate.
The C runner rejects malformed shard selectors, and the integration driver
fails if a spawned child leaves an AddressSanitizer or UBSan diagnostic in a
shard log even when an enclosing test missed the child's exit status.

This is deliberate. The suite protects runtime budgets and public contracts,
with a small amount of end-to-end confidence. It does not freeze prompts,
documentation wording, or every intermediate fixture shape.

## Make targets

```sh
make unit
make integration
make test
make test-full
make asan
make ubsan
make test-small-stack
make fuzz
make chaos
make disk-full
make thrash
make robustness
FCLAW_LIVE_SMOKE=1 make live-smoke
```

`make test-full` runs `make test`, then the rebuild/feature-selection
contracts. `make release-check` runs it against the assembled public tree;
`make robustness` includes those rebuild contracts before its deep gates.

`make asan` and `make ubsan` are distinct clean builds of the full routine
suite. ASan instruments address and lifetime errors. UBSan instruments
undefined behavior with recovery disabled, so the first diagnostic aborts the
test and fails the gate. Both use debug symbols, `-O1`, and preserved frame
pointers; findings are bugs to fix, not warnings to suppress. Instrumented
builds apply one four-times wall-clock budget scale to every test while leaving
the ordinary suite's measured budgets unchanged. A malformed scale is a test
runner error, and a failed sanitizer gate cleans its instrumented binaries.

## Harness note

Integration tests still run in-process through `tests/harness.h`, because they
are budget checks against the real scheduler path. The retained smoke uses the
production `bin/fclaw` binary with mock model responses. Each layer runs in an
isolated temporary copy.

The lookup-and-poem path reads a committed local `example.com` fixture and
installs a failing `curl` shim during the run, so accidental network use makes
the smoke fail. The complete `make test` target is offline and hermetic.
`make live-smoke` is the explicit manual provider check: it
requires `FCLAW_LIVE_SMOKE=1`, uses the configured model profile, verifies
provider artifacts were written, and fails on `agent_output_invalid`.

## Two-tier test policy

There are exactly two required tiers:

| tier | command | when to run |
|---|---|---|
| Every change and every routine release | `make test` | The only routine gate: under 60 seconds on the reference development machine, hermetic, isolated, and offline. |
| Major version releases only | `make robustness` | Run once before a major version tag, or after deep engine surgery (new subsystem, durability/recovery changes). About 20–23 minutes — deliberately too expensive to be routine. Never blocks docs, config, or ordinary feature work. |

`make robustness` fails fast in this order: rebuild/feature-selection
contracts (`test-build-contracts`), ASan, UBSan, small-stack tests, fuzzing,
500-iteration crash chaos, disk-full/read-only drills, and the ten-minute
cap-saturation thrash. Its default fuzz window is ten minutes for each of the
three concurrent fuzzers, followed by the ten-minute thrash; the shorter build
and proof gates add modest overhead. On success it clean-builds normal
optimized binaries before printing its PASS summary.

Run the gauntlet on a clean archive of the head being released, on a machine
with **no live gateway running** (a deployed bot such as Mox or Truly sharing
the host): chaos and thrash are wall-clock contracts, and a live gateway's
turns have produced a spurious `delivery timeout` at a random kill (seen at
the v0.23.0 seal and again on 2026-08-25). The crash chaos also asserts that
every `.json` under `workspace/runs` and `workspace/memory/state` parses after
each of its 500 random SIGKILLs and three targeted mid-write kills
(`state-write`, `event-append`, `agent-output-write`): run artifacts are
replaced by rename, never written in place, and a subprocess's drained stdout
lives in a `.partial` sibling until the job finalizes.

The composed target currently requires macOS: the disk-full/read-only gate is
built on `hdiutil`, `diskutil`, HFS+, and `/Volumes`. Linux supports the normal
suite and the applicable individual platform-neutral proof targets, but cannot
claim a full `make robustness` PASS until that filesystem harness gains a
Linux implementation.

The standalone `make fuzz` target keeps its full one-hour (`3600` second)
default. Only `make robustness` overrides that window to `600` seconds so the
three fuzzers and the thrash each occupy one ten-minute campaign.

`FCLAW_LIVE_SMOKE=1 make live-smoke` is not a suite tier — it sends one
cheap real provider request and must never be confused with the mock-provider
routine gate. It stays **mandatory for any change touching
`runtime/llm/`, `runtime/ports.c`, `runtime/agent_*.c`, or the agent
prompts**: a green `make test` has NOT exercised a live provider.

There is no formal soak gate. Daily use supplies natural soak;
`scripts/rss_soak.sh` is available only for ad-hoc observation.

## Workspace tenancy

The unit and integration targets run through
`tests/run_in_isolated_copy.sh`, and the smoke creates its own fresh copy, so
`make test` does not touch the repository workspace and needs no cleanup
between runs. The C runners validate the wrapper's physical scratch root and
sentinel before any test or teardown; invoking a raw test binary in the
checkout refuses with the exact wrapper command. `make integration` also
SIGKILLs a paused, real fixture writer and byte-compares Git status before and
after, so abnormal termination is part of the isolation contract. Manual
testing against a single `workspace/` directory requires care: see
[Project Layout — Starting fresh](../getting-started/project-layout.md#starting-fresh)
for the workspace lifecycle contract and the
`workspace_task_id_collision` refusal path.
