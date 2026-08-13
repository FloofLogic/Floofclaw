# Contributing

Short version: bug reports are gold, ideas are welcome, patches are
welcome, and contributions are submitted under Apache License 2.0 terms.

## Reports and ideas

Email support@flooflogic.com (security matters go to
security@flooflogic.com — see SECURITY.md). A great bug report names
the run: FloofClaw writes every event, provider call, and artifact to
`workspace/runs/<run_id>/`, so "attach the run directory" replaces
twenty questions. Use the issue tracker in the repository hosting this file for
public reports and feature requests.

## Patches

Welcome. FloofClaw is licensed under the
[Apache License 2.0](LICENSE). Unless you explicitly state otherwise, a
contribution intentionally submitted for inclusion in FloofClaw is provided
under that same license, without additional terms or conditions, as described
by Section 5 of Apache 2.0. Only submit work you have the right to license.

## House rules (the non-negotiables)

- **Keep the core stack narrow.** New runtime code is C11; new tests and
  developer tooling are bash. Existing Python/JavaScript apps and the IRC
  probe are grandfathered. Actions are a separate process boundary and may
  use the runtime their contract genuinely requires. Run `make metrics` for
  the current measured size.
- **`make test` green** (~30 s, hermetic, offline) with every patch.
  A behavior change without a regression test is a rumor, not a fix.
- **Loud beats clever.** Every cap rejects loudly, every failure names
  its fix, nothing truncates silently. Read
  `docs/concepts/constitution.md` before proposing anything ambitious
  — it is short and it is law.
- Runs on **macOS and Linux** with a C11 compiler, `make`, and a POSIX
  userland. The offline mock path needs no optional native library; libcurl
  enables real HTTP providers and OpenSSL enables TLS channels. Platform bugs
  are real bugs—please report them.

## Contribution license

Pull requests include an explicit contribution-license checkbox. By checking
it and submitting the change, you confirm that you have the right to submit
the contribution under the Apache License 2.0. There is no separate CLA.
