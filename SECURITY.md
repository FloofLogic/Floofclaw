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

Please don't test against someone else's running bot. You get a whole
engine; kill -9 your own copy as hard as you like — it's rehearsed.

Use the private email above rather than a public issue for vulnerability
details. The issue tracker in the repository hosting this file is appropriate
for non-sensitive hardening questions.
