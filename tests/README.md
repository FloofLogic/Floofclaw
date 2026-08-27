# tests

The routine suite is hermetic, offline, and intentionally contract-focused:

- `bin/fclaw_unit_tests`: 41 fast C checks for parser, provider, filesystem,
  reconnect, TLS, and allocation-boundary contracts.
- `bin/fclaw_integration_tests`: 193 in-process checks across the scheduler,
  reducers, recovery, channels, provider limits, durable operations, and hard
  capacity boundaries.
- `tests/test_local_client_api.sh`: an isolated loopback lifecycle probe for
  authenticated HTTP/WebSocket coexistence, streaming, resume, cancellation,
  fallback, failure reconciliation, caps, and bounded progress overflow.
- `tests/smoke_lookup_and_poem.sh`: one hermetic smoke script that exercises
  both `openclaw` and `floofclaw` with mock model responses and a local
  `example.com` fixture.

Run the only every-change and routine-release gate with:

```sh
make test
```

The unit and integration suites can be run separately with `make unit` and
`make integration`. For a focused binary check, keep the wrapper:

```sh
make -j4 bin/fclaw_integration_tests
./tests/run_in_isolated_copy.sh ./bin/fclaw_integration_tests <name-filter>
```

The raw C runners refuse to start in the checkout. `make integration` also
SIGKILLs a real fixture-writing test after it has changed the copied registry
and created copied action/floop fixtures, then proves the scratch root is gone
and the checkout's byte-for-byte Git status is unchanged. It also rejects
malformed shard controls and child sanitizer diagnostics. The shell-contract
driver refuses any unregistered `tests/test_*.sh`. The Makefile also exposes
focused sanitizer, small-stack, fuzz, chaos, disk-full, and thrash targets.

`make robustness` composes those expensive proofs on macOS and takes roughly
20–23 minutes. It is required only before a major version release or after
deep engine surgery, never for ordinary docs, config, or feature work. There
is no formal soak gate; `scripts/rss_soak.sh` is available for ad-hoc
observation.

## Fuzz corpus policy

Every seed under `tests/fuzz/corpus/` must be deliberately synthetic or
explicitly scrubbed. Never copy a record, conversation, context identifier,
account, host, path, or provider profile from a live workspace into the
corpus. Preserve useful parser structure by replacing content fields with
obvious `synthetic ...` values; keep intentionally malformed seeds small and
hand-authored. `make public-check` rejects production-shaped chat context IDs,
contact addresses, and absolute user-home paths in the corpus.

`make test`, `make unit`, and `make integration` run their C suites in
isolated temporary copies; direct C-runner execution is rejected before any
test teardown or fixture write. A manual gateway or channel test uses the
current directory's `workspace/`, so perform it only in an explicitly
selected scratch checkout. Automated behavior probes use IRC and never
Discord.

See [the testing concept](../docs/concepts/testing.md) for the complete policy,
including when the opt-in live-provider smoke is mandatory.
