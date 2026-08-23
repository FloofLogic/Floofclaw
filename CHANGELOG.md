# Changelog

FloofClaw follows semantic versioning for public source releases. The public
release tag and `runtime/version.h` carry the same plain `X.Y.Z` version.

## 0.28.0 — 2026-08-23

A minor bump, not a patch: giving each conversation its own context changes
where Discord and IRC memory is filed. Existing records keep the old shared
tag and nothing reads them afterward, so **every conversation on an upgraded
deployment starts with empty memory** unless the operator retags them first.
There is deliberately no automatic migration — only the operator knows which
of those mixed records belonged to which conversation. Everything else in
this release is a fix.

### Fixed

- **A work-ledger rebuild could stop the gateway from starting.**
  `rt_work_state_rebuild_from_logs()` runs at startup and replays a window of
  history — run directories older than the retention keep count are pruned,
  and the scan is capped at `WORK_REBUILD_RUN_CAP`. An event whose ancestor
  revision or selected step fell outside that window aborted the entire
  rebuild, leaving the ledger unwritten and the gateway unable to start; the
  observed incident (`run_043`, task `task_run_038_000008`) was recoverable
  only by moving a run to `workspace/runs_quarantine/` by hand, which frees a
  run number for reuse and risks `workspace_task_id_collision`. The rebuild
  now skips such an event, names it exactly in
  `workspace/logs/narration.jsonl` (run, event id, type), reports the total
  skipped, and finishes. Skipping is not ignoring: the operator gets the run
  and event to inspect, and the run's event log is still the evidence. The
  live apply path is unchanged and still rejects the same event, because
  there the store is complete and a rejection is real. This became more
  reachable once terminal-run pruning started working in 0.27.2 — before
  that, run directories never disappeared and the replay window was
  effectively all of history.

- **Discord and IRC gave every conversation the same context.** Neither
  adapter published a `context_id`, so intake fell back to
  `chat:<channel>:<adapter_id>` and every guild channel, thread, DM, and user
  on a deployment shared one context — one conversational memory and one
  compacted summary across everybody, one `serialize_contexts` queue for the
  whole guild, and DM content recallable from a public channel. Each adapter
  now names the conversation it received: Discord keys on the channel or
  thread id, or `dm:<user_id>` for a direct message; IRC keys on the channel
  or `dm:<nick>`, lowercased because IRC names are case-insensitive and
  `#Ops` must not become a second `#ops`. Reply routing is unchanged — it has
  always used `ref`, not the context.

  **Upgrade note.** Records already in `workspace/memory/memory.jsonl` carry
  the old shared tag (`chat:discord:discord-main`, `chat:irc:irc-main`) and
  nothing reads them after this change, so every conversation starts with
  empty memory. Either accept the reset — the old lines stay on disk, inert —
  or retag the ones worth keeping to the conversation they belong to before
  restarting. There is no automatic migration: only the operator knows which
  of those mixed records belonged to which conversation.

## 0.27.2 — 2026-08-23

The one-day hygiene sprint: six isolated defects, each with its own
regression. `make asan` is green over the set.

### Fixed

- **Idle gateway RSS drops from ~11.6 MiB to ~7.6 MiB.** `rt_scheduler_init`
  memset the whole ~4.8 MB `RtScheduler` — nearly all of it the fixed run
  pool — which faulted every page at startup before a single event arrived.
  The struct is allocated with `fc_xcalloc` now and init leaves it alone;
  each pool slot is already cleared as it is taken, so only slots in use
  become resident. `rt_scheduler_init` therefore requires zeroed storage,
  which every caller already provided.
- **Poll-set overflow is counted and reported instead of silent.** The
  reactor watched at most 64 descriptors and four call sites discarded
  `fc_pollset_add`'s result, so a descriptor that did not fit was simply not
  watched — a child pipe stalling until the next poll deadline (5 s in
  daemon mode) or until its 4 KB buffer filled, with nothing in the logs or
  in `gateway status` to say so. The cap is 128 now, which covers the worst
  realistic pass (16 jobs × 4 descriptors, the wake and status sockets, the
  WebSocket listener and its 16 clients, Discord's two sockets, IRC's one),
  and the poll set counts every refusal itself so a caller that ignores the
  return value cannot lose it. `fclaw gateway status -a` reports
  `reactor.poll_overflows`, and the reactor logs the overflow on the same
  rate-limited ladder as callback errors.
- **The advertised mock-only build compiles again, and is now a gate.**
  `clear_active_part()` had drifted inside `runtime/llm/media.c`'s
  `FCLAW_HAVE_LIBCURL` region while the load path calls it unconditionally,
  so a build without libcurl failed on an undeclared function. Nothing
  noticed, because every gate builds with both libraries present.
  `make MOCK_ONLY=1` is the supported way to build without libcurl and
  OpenSSL, and `make test` now compiles that configuration warning-clean,
  proves the binary links neither library, and runs the mock `hello` floop
  through it.
- **A build with no libcurl is refused instead of silently shipped.** It
  could not reach any provider, including a loopback OpenAI-compatible
  endpoint, and only failed at the agent layer at runtime. The error names
  how to install the library and how to ask for the library-free build on
  purpose.
- **A command-line `CFLAGS=` no longer disarms the build.** The feature
  defines and include paths are appended with `override`, so
  `make CFLAGS=-O0` refines the base flags instead of dropping
  `-DFCLAW_HAVE_LIBCURL`, `-DFCLAW_HAVE_OPENSSL`, and the build revision.
  `MOCK_ONLY=1` strips those defines and libraries from an inherited
  `CFLAGS`/`LDFLAGS` too, so it cannot end up declaring a transport it links
  nothing for. `asan`/`ubsan` hand their sub-make the *base* flags and let it
  run its own detection, instead of re-passing an augmented set for it to
  append to a second time.
- **Channel adapters no longer read the whole delivery log every tick.** All
  three tailed `deliveries.jsonl` (and IRC also `narration.jsonl`) by reading
  the entire file just to learn its length, then reopening it from the
  cursor. Rotation is at 64 MiB and the foreground reactor ticks at ~16.7 Hz,
  so the worst case was three 64 MB read+malloc pairs per tick to answer a
  question `stat()` answers for free. An idle tick now allocates nothing at
  any log size. The WebSocket adapter also gained the rotation check the
  other two already had; without it a rotation stranded its cursor past every
  future delivery.
- **Terminal-run pruning works.** The janitor read the lifecycle field from
  `runstate.json` as `state` while the serializer wrote `status`, so
  `run_is_terminal` was always false and nothing was ever pruned — silently,
  because "can't read → keep" is also the fail-safe. Every deployment
  retained 100% of its run directories with the janitor reporting zero
  errors. The key now has one owner (`RT_RUNSTATE_STATUS_KEY`) that the
  serializer and every reader spell through, so it cannot drift again.
  `appliance.runs.keep` is a floor, not a ceiling: only terminal, unpinned
  runs are eligible, so a crashed, hung, or publication-pinned run is still
  kept deliberately.
- The janitor's bounded directory scan selected which entries to consider by
  readdir order and only then sorted, so past the scan cap (2048 runs, 12288
  processed bus records) it could carry the newest entries and prune the
  wrong end. It now selects the oldest entries during the scan, which is the
  end every one of its tasks deletes from.

## 0.27.1 — 2026-08-23

### Added

- **Generic typed-event ingress.** `fclaw bus publish --type <kind>` lets a
  local producer — a cron job, a git hook, a build wrapper, a device bridge —
  publish a product-defined fact onto the bus without disguising it as chat,
  putting product behavior in a channel layer, calling an action outside a
  floop decision, or hand-writing an inbox file. The payload is one JSON
  object supplied by `--payload` or, preferably, `--payload-stdin`; it reaches
  `event.payload` byte-for-byte, with no required `text` key. A floop opts in
  by gating a step on the exact kind (`"gate": "event_kind:device_scan_requested"`),
  and a deterministic `script` agent can answer with its allowlisted actions
  and a reply with no provider call at all. A valid kind no step gates
  completes as an ordinary inspectable no-op run rather than falling through
  to chat handling. The kernel stays product-ignorant: it keeps no list of
  event names and enforces only a token grammar (1–63 bytes,
  `[A-Za-z][A-Za-z0-9_.:-]*`), the runtime-owned namespaces (`user_`,
  `action_`, `affair_`, `operation_`, `work_`, `task_`, `run_`, `memory_`),
  and the ordinary bus payload bound.
- `fclaw bus reserve` issues one bus identity without publishing it, so a
  producer that may crash between commit and acknowledgement can record the
  id beside its own source row and republish the identical envelope under
  `--event-id`. An identical republish is absorbed and creates no second
  envelope or run; reusing the id with a different type, payload, or routing
  fails as a conflict. Publication idempotency only — it never retries a
  model call, an action, or a product effect.
- Bus envelopes may carry an optional top-level `routing` block
  (`adapter_id`, `context_id`, `ref`) beside the payload. Typed events use it
  so a `context_id` or `ref` key *inside* a product payload stays ordinary
  data and steers nothing. Built-in kinds carry no routing block and read
  those fields from their payloads exactly as before.
- The `event:` context namespace. A typed event routes to
  `event:<channel>:<context-id>`, falling back to the adapter id and then the
  channel alone, so publishing a device or build fact never inherits a
  conversation's memory scope or `serialize_contexts` lane. A fully qualified
  `chat:` id is preserved verbatim for a product that deliberately wants that
  join; the runtime-owned `action:` namespace is refused from caller input.
- `fclaw bus publish` now reports validation (`1`), conflict (`3`), and
  publication failure (`4`) with distinct exit codes, a JSON error object on
  stdout in agent mode, and the exact offending field on stderr. A refused
  command never leaves a partial inbox envelope.

### Changed

- An ordinary action call from a run with no task-bearing context is now
  emitted unbound instead of rejected. This only affects typed-ingress runs,
  which have no input task by design; every runtime-owned kind still requires
  its existing task binding, unchanged.

## 0.27.0 — 2026-08-20

### Added

- `fclaw config get|set|enable|disable` makes the binary the one owner of
  `config/floofclaw_config.json` edits. Every edit is a byte-span splice —
  key order, indentation, and `_comment` entries the edit does not target
  survive verbatim; nothing reserializes the file. `get`/`set` take dotted
  paths (`gateway.port`, `channels.discord.enabled`); a value that parses
  as a complete JSON value is spliced verbatim and anything else becomes a
  JSON string, so values that must stay strings are passed pre-quoted
  (`'"123"'`). `enable`/`disable` edit `force_disable` membership as
  idempotent facts — enabling an id that is not listed succeeds and says
  so.
- `scripts/onboard.sh` is the guided first-run wizard, built entirely on
  the existing CLI verbs (bash and `bin/fclaw` are its only dependencies).
  It walks identity, provider key, floop, and channels, then the
  enablement checklist: each masked bundled action is checked for its
  prerequisite, verified with a live call through `fclaw action exec`, and
  enabled only on proof — `web_read` verifies and enables without prompts
  when pluck is already built. A gateway health probe closes the run.
  Re-running keeps existing state and repairs; it never resets. The
  presentation adapts to the terminal: colors, glyphs, and a live
  output ticker on interactive UTF-8 terminals; plain deterministic text
  when piped; pure ASCII under C locales or the Linux virtual console
  (`TERM=linux`), where an end-to-end run emits zero non-ASCII bytes.
  The wizard is referenced from the README quickstart, the installation
  guide, and the `fclaw setup` reference card.

### Changed

- The shipped default `bot_name` is now `Mox`.
- Deployments merging this release into a customized branch config should
  keep their own `force_disable` list (typically `[]` once their actions
  are set up): the shipped mask is a fresh-clone default, and letting a
  merge carry it into a provisioned deployment silently hides working
  actions from the models.

## 0.26.2 — 2026-08-18

### Added

- The `@{user}` prompt variable renders operator-curated standing user facts
  from `config/user.md` into agent prompts. The file is deployment-owned like
  `floofclaw_config.json`, unreachable from the workspace-scoped file
  actions, and reread per run so edits land on the next run without a
  restart; an absent file renders empty, so shipped floops may reference the
  variable unconditionally, and a file the prompt boundary can never fit
  fails loudly. The openclaw and floofclaw conversation prompts gained a
  "Your user" section.
- `force_disable` in `config/floofclaw_config.json` is the operator's mask
  over registered actions: a masked action stays registered (floop
  allowlists keep declaring intent) but is omitted from every model-facing
  catalog, and a call that arrives anyway is rejected with
  `action_rejected.reason=force_disabled`. Operator surfaces are exempt —
  `fclaw action list` renders the whole registry with `force_disabled`
  stated per entry, and `fclaw action exec` runs masked actions directly.
  The shipped config masks every bundled action that cannot work before
  operator setup: `web_read`, `gcal`, `git_github`, `manage_codex`,
  `manage_claude`, `manage_hermes`.

### Fixed

- A structurally malformed agent decision (a stray key on a call object, a
  missing call name) now spends the agent's configured output-contract
  repair budget — the model reruns with a generic format reason beside its
  rejected response — instead of terminally failing the run as
  `agent_output_invalid` without ever consuming a repair attempt.
- The pluck submodule is repinned to the republished public v0.1.0; the old
  pin no longer exists on the remote, so submodule init failed in every
  fresh checkout.

## 0.26.1 — 2026-08-18

### Fixed

- The OpenAI Responses transport now sends a model profile's `effort` as
  `"reasoning": {"effort": ...}` (JSON-escaped) on text and media requests;
  previously the field was loaded but never put on the wire, so
  Responses-backed profiles silently ran at the model's default reasoning
  level. Profiles without `effort` omit `reasoning` and keep the previous
  request bytes. The engine still does not whitelist values — the provider
  remains the authority, so values such as `minimal` pass through.
- Gateway identity now requires the runtime-owner file lock, not just a live
  recorded PID. After an unclean exit the OS can reuse the recorded PID for
  an unrelated process; that stale control state no longer blocks a new
  runtime owner — the advisory PID and floop files under `.fclaw/run/` are
  removed and startup proceeds.

## 0.26.0 — 2026-08-16

### Changed

- Managed operations no longer poll. The runtime allocates each operation's
  identity durably before the action executes (a runtime-owned operation id,
  a secret completion token, and an action-declared completion deadline), and
  detached workers complete through the ordinary bus: one
  `operation_completed` claim — submitted with the new
  `fclaw operation complete` helper via the trusted child environment — is
  validated against the durable operation store before any run exists and
  becomes the one correlated `operation_result` run. Deadlines feed the
  reactor's poll timeout directly; a silent worker produces exactly one
  `operation_result` with `status:"timed_out"`, and completion, timeout, and
  immediate results all pass through one terminal gate.
- Managed-action manifests now declare `default_timeout_ms` (and optionally
  `max_timeout_ms`) instead of `poll_handle_arg`; floops may shorten the
  deadline with `operation_timeout_ms` but can never extend it. The
  `poll_interval_ms` floop option, the `operation_poll` envelope, and the
  native `operation_poller` agent are removed; declaring the contract itself
  enables the driver.
- `manage_codex`/`manage_claude` runners verify the honest terminal state
  (including the named-deliverable check) and submit completion themselves;
  `manage_hermes` detaches a local waiter (`fclaw internal detach`) that
  polls the Hermes API leaf-side and submits one claim.
- Completion claims that survive a crash between intake's destructive rename
  and the run claim are reconciled back into intake; the janitor pins pending
  claim envelopes; a completion queued behind an expired deadline
  deterministically wins the race.
- Upgrade note: operations left `running` by a pre-upgrade gateway carry no
  deadline and converge through one immediate timeout consequence at first
  startup.

## 0.25.0 — 2026-08-14

### Changed

- Rebuilt the project README around FloofClaw's defining advantages: its
  dependency-free C runtime, database-free durable filesystem, agent-first
  JSON/JSONL interface, and fully inspectable model-call artifacts.
- Added compact project badges, an architecture overview, faster onboarding,
  emoji-led navigation, and a prominent Floof Logic attribution and business
  contact path above the fold.

## 0.24.1 — 2026-08-13

### Changed

- FloofClaw is now licensed under the Apache License 2.0, replacing the
  PolyForm Noncommercial License 1.0.0. Commercial and noncommercial use,
  modification, and distribution are permitted under the Apache 2.0 terms.
- Every public command now uses one output contract: `-a` emits compact
  JSON/JSONL for agents, `-h` emits formatted terminal output for people, and
  omitting both is equivalent to `-a`. The retired `-c`, `--color`, and
  command-specific `--json` aliases are no longer accepted.
- Bundled actions now live only under `actions/common/` and `actions/core/`;
  other action areas are local installation surface and ignored by Git.
- `web_read` now owns a public `pluck` submodule from
  <https://github.com/FloofLogic/pluck>. Public source releases flatten its
  pinned source, so public clones do not need a submodule checkout.
- The public release gate now assembles from an allowlist, scans for private
  material and dangling links, and runs the routine suite from the assembled
  tree.

### Fixed

- Recovery documentation now matches the durable outside-world action
  contract: ambiguous completion blocks rather than replaying an effect.
- CLI examples and agent guidance consistently request structured output.

### Upgrade notes

- Scripts that previously requested color with `-c` must use `-h`.
- Scripts that previously used `-a` for a command-specific meaning must use
  that command's replacement option; `-a` is reserved for agent output.
- Automation should parse agent-mode output as JSON or JSONL and should not
  scrape human headings or ANSI formatting.

### Known limitations

- Real providers require libcurl; TLS channels require OpenSSL.
- `web_read` additionally requires Zig 0.15.2 and libxml2 to build `pluck`.
- Janitor worker-artifact retention currently covers `codex_ops` only;
  `claude_ops` and `hermes_ops` accumulate until cleared manually.
- The optional iMessage/email delivery bridge is best-effort: it advances its
  cursor after each batch, including failed sends, and a crash after external
  acceptance but before the cursor write can resend that batch.
- Source releases are supported; prebuilt binaries and package-manager
  publication are not yet provided.

## Earlier releases

The private development repository retains the detailed v0.23.0 and v0.23.1
release records. This public changelog begins with the independently assembled
0.24.1 source release.
