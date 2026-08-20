# Changelog

FloofClaw follows semantic versioning for public source releases. The public
release tag and `runtime/version.h` carry the same plain `X.Y.Z` version.

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
- Configured terminal-run pruning is currently ineffective because the
  janitor reads the wrong lifecycle field from `runstate.json`. JSONL rotation
  and other configured cleanup tasks still run, but run directories
  accumulate.
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
