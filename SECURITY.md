# Security

Found a way to make FloofClaw lie, leak, or fall over? We genuinely
want to know — we spent hundreds of hours trying to do that ourselves
(see `docs/performance/testing_investment.md`) and we'd be a little
annoyed, but mostly impressed, if you found one we missed.

**Report to security@flooflogic.com.** Best-effort response — there
is no SLA, no bounty program, and no swag. Payment is gratitude and a
CHANGELOG credit, which is more than most of us get at work.

Especially interesting to us:

- Memory safety in the runtime (it survives ASan, UBSan, fuzzing, and
  500-SIGKILL robustness campaigns—surprise us).
- A secret value appearing anywhere it shouldn't: argv, event logs,
  provider artifacts, narration. The design says references only.
- An action escaping its declared boundary, or untrusted model output
  becoming behavioral truth without passing through the reducer.

## What the executing actions are and are not confined to

`bash`, `apply_patch`, `web_fetch`, and `http_call` run under a write
confinement built at the action level, not in the kernel: `sandbox-exec` on
macOS, `bwrap` on Linux. Writes are permitted to the workspace root and
nowhere else; reads and execution are unrestricted, because every action
needs its interpreter, its libraries, and the network it was asked to use.
Scratch space is a per-invocation directory inside the workspace that
`TMPDIR` points at for the life of the call.

It fails closed. On a host with neither mechanism the action refuses to run
rather than running unconfined, and names the way past it: an operator sets
`actions.<id>.sandbox` to `"off"` in `config/floofclaw_config.json`. That is
declared action config, so it comes from the deployment's own file — a model
cannot reach it through arguments.

What this is not: it is not a privilege boundary against a determined
attacker who already runs code as the gateway user. `bash` executes
arbitrary commands by design; confinement bounds the blast radius of a
model's mistake or a prompt injection to the workspace, and keeps it out of
the deployment tree, the operator's home, and shared temp. One documented
cost: macOS `mktemp(1)` with no template ignores `TMPDIR` and targets the
per-user Darwin temp directory, which is denied. Permitting that directory
would void the confinement for any deployment installed under it, so the
denial stands and commands use `"$TMPDIR"` instead.

Please don't test against someone else's running bot. You get a whole
engine; kill -9 your own copy as hard as you like — it's rehearsed.

Use the private email above rather than a public issue for vulnerability
details. The issue tracker in the repository hosting this file is appropriate
for non-sensitive hardening questions.
