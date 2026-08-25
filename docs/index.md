# Documentation Index

> **[READ_FIRST.md](READ_FIRST.md) is binding. Read it before any other
> doc and before any change. Nothing below overrides it.**

**FloofClaw is a transparent agent orchestration runtime.** It composes
multiple focused LLM agents together with heavy-lifting workers
(Codex, Claude Code, and other subprocess research tools) into products where
every step is inspectable. Triggering envelopes become runs, and every
provider call inside a run has its own request/response artifacts.

For the pitch, read [what_is_floofclaw.md](what_is_floofclaw.md).

## Start here

- [What is FloofClaw](what_is_floofclaw.md) — the positioning
- [Overview](getting-started/overview.md) — the operational shape
- [Installation and Build](getting-started/installation-and-build.md)
- [First Run](getting-started/first-run.md)
- [Project Layout](getting-started/project-layout.md)
- [Adding an action](getting-started/adding-an-action.md) — copy a directory, restart
- [Adding an adapter](getting-started/adding-an-adapter.md) — copy a directory, `make`, restart
- [Choosing a floop](getting-started/floops.md)
- [Providers](getting-started/providers.md)
- [Discord](getting-started/discord.md)
- [Telegram](getting-started/telegram.md)
- [IRC](getting-started/irc.md)
- [MCP servers](getting-started/mcp.md) — point FloofClaw at an MCP server and
  its tools become ordinary actions
- [Android arm64](getting-started/android-arm64.md) — building and running
  under Termux

## Core concepts

- [Constitution](concepts/constitution.md) — the invariants the engine makes unbypassable, each backed by a test
- [Principles](concepts/principles.md) — the rules the runtime is built to
- [Architecture](architecture.md) — kernel, floops, events, ownership boundaries
- [Action Result Flow](concepts/action_result_flow.md) — the unified event-driven pattern: every data-returning action is managed-op; results flow via `operation_result`
- [Extensions](concepts/extensions.md) — the three surfaces, what is discovered when, and why there is no `dlopen`
- [Actions](concepts/actions.md) — how to author an action (action.json, run.sh, intrinsic)
- [Executors](concepts/executors.md) — `native`, `llm`, `script`
- [Events and Replay](concepts/events-and-replay.md) — apply-at-append, durable trace, replay
- [Gateway Reactor](concepts/gateway-reactor.md) — reactor modules, jobrunner, scheduler
- [Floops](concepts/floops.md) — how loop profiles ship as directories
- [the floofclaw floop](concepts/floofclaw_floop.md) — the affair concept (durable concerns) and its watcher

## Contracts (stable envelopes)

- [Floof Struct Definitions](concepts/floof-struct-definitions.md) — runtime/job/task/artifact/event envelopes
- [Task Feature](concepts/task-feature.md) — durable task and artifact contract
- [Runstate v1.3.8](concepts/runstate-v1.3.8.md) — durable run scheduling envelope

## Reference

- [CLI Reference](reference/cli.md)
- [Local client API v1](reference/local-client-api-v1.md)

## Performance

- [Footprint](performance/footprint.md) — LOC, binary size, runtime memory
- [Heap budget](performance/heap-budget.md) — per-event allocation discipline
- [RSS baseline](performance/rss_baseline.md) — ad-hoc RSS sampling reference,
  not a release gate
- [Durability](performance/durability.md) — what survives a crash, and what is
  deliberately refused instead of guessed
- [TLS verification](performance/tls-verification.md) — how certificates are
  checked and what that costs
- [Testing investment](performance/testing_investment.md) — what the suite
  cost to build and what each campaign actually caught

## Cross-cutting

- [Runtime orientation](comparison.md) — one-page tour of the shape
- [Discord and IRC](discord_and_irc.md) — hard convention: Discord is the human channel; IRC is where automated probes run
- [Testing](concepts/testing.md) — the hermetic routine gate, robustness gate,
  and opt-in live-provider check
- [Changelog](../CHANGELOG.md) — release changes, upgrade notes, and known
  public limitations
- [IRC probing](testing/irc_probing.md) — the probe tool for end-to-end agent tests
