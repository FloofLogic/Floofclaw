# Changelog

FloofClaw follows semantic versioning for public source releases. The public
release tag and `runtime/version.h` carry the same plain `X.Y.Z` version.

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
