# FloofClaw — tiny code. sharp claws.

<p align="center">
  <img alt="Language: C11" src="https://img.shields.io/badge/C-C11-A8B9CC?style=for-the-badge&amp;logo=c&amp;logoColor=white">
  <img alt="Platforms: macOS, Linux, and Android ARM64" src="https://img.shields.io/badge/Platforms-macOS%20%7C%20Linux%20%7C%20Android%20ARM64-2563EB?style=for-the-badge">
  <a href="LICENSE"><img alt="License: Apache 2.0" src="https://img.shields.io/badge/License-Apache%202.0-D22128?style=for-the-badge&amp;logo=apache&amp;logoColor=white"></a>
</p>

<p align="center">
  <a href="https://floofclaw.com/"><img alt="FloofClaw website" src="https://img.shields.io/badge/Website-floofclaw.com-0EA5E9?style=for-the-badge&amp;logo=googlechrome&amp;logoColor=white"></a>
  <a href="docs/index.md"><img alt="Official documentation" src="https://img.shields.io/badge/Docs-Official-2563EB?style=for-the-badge&amp;logo=readthedocs&amp;logoColor=white"></a>
</p>

**FloofClaw is not another agent. It is the engine agents run inside
when their work needs to stay durable, economical, observable, and
accountable over time.**

Some projects perform intelligent work. Some give an agent tools.
FloofClaw is the runtime that decides *when* intelligence is needed —
and handles everything else without pretending it needs an LLM. A
durable control plane for agentic systems: deterministic where
possible, intelligent where necessary.

**License:** FloofClaw is open source under the
[Apache License 2.0](LICENSE). Commercial and noncommercial use,
modification, and distribution are permitted under its terms.

## Quickstart

The offline/mock path needs a C11 compiler, `make`, and a POSIX userland.
libcurl enables real HTTP providers; OpenSSL 3 enables TLS channels. Some
optional actions and local inspection apps also use `python3`, `jq`, or
`curl` at runtime.

```bash
make -j4                           # build bin/fclaw
./bin/fclaw run -h --text "hello"  # zero-account reply from the mock model
./bin/fclaw gateway start -h       # start the local always-on runtime
./bin/fclaw -i -h                  # talk through the running gateway
```

Public source releases contain the pinned `pluck` source as ordinary files; no
submodule checkout or private repository access is needed. The extractor's
canonical public upstream is
[FloofLogic/pluck](https://github.com/FloofLogic/pluck).
`make` compiles the C runtime. The one-shot command then proves the complete
path with the mock-backed `hello` floop and creates only local artifacts. The
gateway command keeps that configured floop running locally, and the final
command opens an interactive terminal client for it. Stop the daemon later
with `./bin/fclaw gateway stop -h`. Development checkouts that want `web_read`
initialize its public source with
`git submodule update --init actions/common/web_read/_pluck` and then run
`bash actions/common/web_read/build.sh`.

The CLI is agent-first: every public command defaults to compact JSON/JSONL,
exactly as if `-a` were supplied. Terminal examples use `-h` for formatted,
colored output. See the [CLI output contract](docs/reference/cli.md#output-contract).

For a real model, store a key without placing it in shell history—for example,
`./bin/fclaw auth set-stdin -h gemini_key`—then select a provider-backed floop and
profile using the [provider guide](docs/getting-started/providers.md).

Next: [providers](docs/getting-started/providers.md),
[Discord](docs/getting-started/discord.md),
[floops](docs/getting-started/floops.md),
[extensions](docs/concepts/extensions.md),
[architecture](docs/architecture.md), and the
[constitution](docs/concepts/constitution.md).

## Extending it

FloofClaw has three extension surfaces, and you add to each by copying a
directory into it. No registration list, no core file to edit.

```text
action:          copy into actions/   → restart          → fclaw actions -h
native adapter:  copy into adapters/  → make → restart   → fclaw adapters -h
floop:           copy into floops/    → select and run   → fclaw floops -h
```

Only the adapter rebuilds — it is C, statically linked into the same single
binary. See [extensions](docs/concepts/extensions.md).

## The numbers

Measurements refreshed 2026-08-13 on arm64 macOS with Apple clang 17 from the
0.24.1 source release; run `make metrics` to reproduce them, including the
exact measured revision:

| metric | value |
| --- | --- |
| stripped binary | **650,664 bytes (~635 KiB)** |
| engine source | **39,420 lines of runtime C/header source** |
| optional native libraries | libcurl (HTTP providers), OpenSSL 3 (TLS channels) |
| isolated mock-backed `hello` wall time | **0.124 s** |
| idle isolated gateway RSS | **11,840 KiB (~11.6 MiB)** |
| LLM calls for a quiet supervision review | **exactly 1** |

Every model call leaves numbered request, response, and metadata artifacts
inside its run. The event log is the source of truth; state is a materialized
view; any run can be replayed. A run may contain more than one model call, so
count `provider_calls/*_meta.json` for exact per-run accounting or use
`fclaw usage -a` for the compact current-segment aggregate (`fclaw usage -h` for
the terminal table).

For a local visual breakdown, `./scripts/up.sh` also serves Tokenwatch at
`http://127.0.0.1:8004/`, alongside Pulse and Runtime Flow. Tokenwatch keeps
pricing and cache/heatmap calculations in the app while the runtime only
streams recorded usage and provider-call evidence.

## How it works

The kernel does not know what kind of loop it is running. A loop
profile (a "floop") defines ordered steps. Steps run agents,
builtins, or actions. Agents and actions emit events. The
deterministic reducer applies those events to state.

## Build

```bash
make -j4
```

Android ARM64 is a first-class cross-build of the same CLI entry point. It
produces a normal executable named `fclaw`, not an APK library or
application-specific wrapper:

```bash
ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/27.0.12077973" \
FCLAW_ANDROID_DEPS_ROOT=/path/to/android-arm64-static-deps \
make android-arm64

file dist/android-arm64/fclaw
```

The dependency root contains `curl/` and `openssl/` ARM64 static installations.
See [Android ARM64 build](docs/getting-started/android-arm64.md) for the exact
layout, individual overrides, API selection, and device smoke command.

## Run

```bash
./bin/fclaw run -h --text "hello"
./bin/fclaw run -h --floop openclaw --text "hello"   # needs its configured provider
./bin/fclaw run -h --floop floofclaw --text "hello"  # needs its configured provider
./bin/fclaw replay -h workspace/runs/<run_id>/event_log.jsonl
```

Default floop:

```text
hello
```

The durable-concern manager loop (standing affairs with scheduled
reviews, detached background workers, proactive messages) is
`floofclaw` — see `docs/concepts/floofclaw_floop.md`:

```bash
./bin/fclaw gateway start -h --floop floofclaw
```

Watch it run: `app/pulse` (live web board) or the IRC ops channel
(`channels.irc.ops_channel`), where the runtime narrates reviews,
workers, and failures as they happen.

## Runtime Shape

```text
floops/     shippable loop+agent bundles
actions/    declared capabilities and tool boundaries
runtime/    tiny C loop kernel
workspace/  event logs, state snapshots, run artifacts
tests/      fast runtime checks and hermetic smoke coverage
```

Each run writes:

```text
workspace/runs/<run_id>/
  event_log.jsonl
  runstate.json
  state.json
  trace.jsonl
  agent_outputs/
  action_calls/
  provider_calls/      # present when an LLM-backed phase ran
```

`event_log.jsonl` is behavioral truth. `trace.jsonl` is diagnostics. `state.json` is a materialized view.

## Floops

See [Choosing a floop](docs/getting-started/floops.md) for the complete
shipped set, model requirements, and newcomer guidance.

`floofclaw` is the modern event-gated concern-management loop:

```text
memory.before -> chat_manager -> review_manager -> result_manager -> dispatch -> workers -> memory.after -> memory.compact
```

Each event kind routes to one manager: chat handles user messages, review handles
scheduled affair checks, and result handles managed-operation results. The
native workers phase starts or polls the configured managed-operation handler.
Data-returning actions publish `operation_result`; no agent gets a hidden
same-run result channel.
After each accepted agent output, the runtime validates and stamps events,
then executes accepted action requests. Each action invocation completes
within the current run. A data-returning action also records a managed
operation whose result re-enters the floop through a durable
`operation_result` event and a later run.

`openclaw` is the event-driven coding/tool loop with basic
memory:

```text
memory.before -> main_claw -> operation.poll -> dispatch -> memory.after -> memory.compact
```

The same `main_claw` handles `user_message`, `operation_result`, and
`affair_review` events. It may request one managed action, observe its durable
result in a later run, request another action, and repeat until it emits the
user-visible `message` or is blocked. The original input task and its existing
`working_memory` preserve the active request across those runs; affairs remain
decisions made by `main_claw`, not scheduler policy.
`memory.before` recalls summaries plus the recent message tail into step input;
`memory.after` appends user, committed tool-activity, and assistant records,
and the gated `memory.compact` phase runs only when the runtime has selected a
compaction window.

Both loops use the same runtime-owned action execution path. LLM agents emit
intent only; the runtime assigns request ids, appends durable `action_request`
events, and the `message` action ends in `action_succeeded` like any other
successful action.

## Tests

```bash
make test
```

Current smoke tests:

```text
tests/smoke_lookup_and_poem.sh
```

`make test` runs 32 unit tests, 139 integration tests, and the hermetic
auxiliary gates and smoke in isolated workspaces with mock provider responses.
It is offline and remains the gate for everyday development. Public release
candidates additionally run `make release-check`, which assembles and tests the
allowlisted public tree. Before a
raw C test can start, it must see the isolation wrapper's physical scratch
root; direct execution in the checkout is rejected. The integration gate also
SIGKILLs a real fixture writer and proves Git status is unchanged. For a
**major version** release (or after deep engine surgery), run `make robustness`
once; its ASan, UBSan, small-stack, fuzz, chaos, disk-full/read-only, and thrash
gates take about 20 minutes by design—which is exactly why it is not a routine
gate. The composed target is currently macOS-only because the disk drill uses
`hdiutil`/`diskutil`; Linux developers can run its platform-neutral component
targets separately. To exercise the configured live-provider path, run
`FCLAW_LIVE_SMOKE=1 make live-smoke`; that credentialed, opt-in target sends
one cheap real request through the `responses` profile by default and is
required after LLM/provider/startup-path or agent-prompt changes. Set
`FCLAW_LIVE_PROFILE_REF=<profile>` to exercise another configured profile. See
[Testing — Two-tier test policy](docs/concepts/testing.md#two-tier-test-policy).

This engine's testing is an investment, not an afterthought: at the current
history snapshot, 341 of 953 commits (about 36%) touch test code, with
hundreds of engineering hours and recorded
machine proof-campaigns (fuzz, 1,100+ chaos kills, 72,000-event thrash,
real disk-full drills) behind the word "hardened." The numbers and the
commands that reproduce them: [Testing investment](docs/performance/testing_investment.md).

## Releases

Active engine development lands on private `main`. The release tooling
assembles an allowlisted, scrubbed public tree and creates a root public-history
commit on `release`; annotated `vX.Y.Z` tags identify source releases. Private
development and deployment ancestry is never a public release source.

## License

FloofClaw is licensed under the
[Apache License 2.0](LICENSE), an OSI-approved open-source license that permits
commercial and noncommercial use, modification, and distribution under its
terms.
