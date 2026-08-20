# CLI Reference (current runtime)

Top-level usage:

```text
fclaw run [-a|-h] [--floop <name>] [--text] TEXT
fclaw action list [-a|-h]
fclaw action exec [-a|-h] --name <action> --args '<json-object>'
fclaw action auth [-a|-h] --name <action> -- <action-owned arguments>
fclaw operation complete [-a|-h] [--operation-id <id>] [--token <token>] (--result-file <json> | --status <s> [--text <t>])
fclaw replay [-a|-h] <event_log.jsonl>
fclaw view [-a|-h] [-f] [-d] <run_id|run_dir>
fclaw usage [-a|-h] [--agent <id>] [--profile <id>]
fclaw cacheview [-a|-h] --agent <id> [-n <count>] [--run <run_id> [--call <seq>] | --list]
fclaw auth set|set-stdin|get|delete|header [-a|-h] <endpoint> [...]
fclaw bus publish|log [-a|-h] [...]
fclaw affair create|list|pause|resume|close|review|schedule [-a|-h] [...]
fclaw gateway start|run|status|stop|reload [-a|-h] [...]
fclaw -i [-a|-h] [--floop <name>]
fclaw channel cli|ws|irc [-a|-h] [--floop <name>]
fclaw clear [-a|-h] [--yes]
fclaw actions|adapters|floops|setup [-a|-h]
fclaw --version|-v [-a|-h]
```

Top-level dispatch and the summary live in `runtime/main.c`; each command's
parser and behavior live in the matching `runtime/*_cli.c` or channel/runtime
module.

## Output contract

Every public command has the same two output modes:

| flag | output |
|---|---|
| `-a` | compact agent output: JSON for one result, JSONL for a stream |
| `-h` | formatted, colored human output |

Omitting both flags is exactly equivalent to `-a`. Supplying both is an
error. This makes the shortest spelling the token-efficient, composable one:

```bash
./bin/fclaw actions                    # compact JSON (default / -a)
./bin/fclaw actions -a                 # the same compact JSON
./bin/fclaw actions -h                 # colored operator table
```

Agent mode keeps machine data on stdout and may also put a diagnostic on
stderr when a command fails. Human mode is for terminal use. The private
`fclaw internal …` child-process interface is not a public CLI and is outside
this presentation contract.

The old `-c`/`--color` and `gateway status --json` spellings are retired; use
`-h` and `-a` respectively. Unknown legacy flags fail instead of creating
command-specific exceptions to the shared rule.

---

## `fclaw run [-a|-h] [--floop <name>] [--text] TEXT`

Submit one user message and print the matching delivery. This is a
wrapper around the same runtime-owner path as channels:

- if the gateway is running, `run` follows its floop, checks any explicit
  floop assertion, then publishes to the bus and waits for the gateway-owned
  delivery
- if no gateway is running, `run` hosts an embedded single-process
  runtime owner, publishes the same bus envelope, drives the scheduler,
  and waits for the same delivery shape

```bash
./bin/fclaw run --text "hello"
./bin/fclaw run -h "hello"
```

The first form emits one compact object with `run_id`, terminal `status`, and
`result`. The second renders those fields with colored labels.

With no gateway active, `--floop` selects the embedded owner. With a live
gateway, omitting `--floop` follows the gateway's active floop and an explicit
matching value asserts that the expected owner is running. A conflicting
value exits before publishing the message and names both floops plus the fix:
`./bin/fclaw gateway reload -h --floop <name>`.

When no gateway is active and `--floop` is omitted, the embedded owner reads
`default_floop` from `config/floofclaw_config.json`; the committed fresh-clone
value is the mock-backed `hello`. `floofclaw`, `openclaw`, and `companion` are
provider-backed product/example bundles. `fast` and `thrash_cap` are
test/performance fixtures, not newcomer loops. If the config file is missing,
malformed, or has no nonempty `default_floop`, the compiled fallback is
`floofclaw`.

No command path allocates run IDs directly; the runtime owner creates
and advances runs after consuming events.

### Exit status

`fclaw run` reports either the run's terminal state or a pre-run refusal:

| code | meaning |
|---:|---|
| `0` | `run_done` — the printed `result` is the reply |
| `2` | `run_failed`, or a pre-run `workspace_task_id_collision` refusal; the printed `result` explains which |
| `1` | no terminal delivery, a setup/I-O error, or a live-gateway `--floop` conflict before the run started |

Scripts can rely on `set -e` catching either hard refusal. A script that needs
to distinguish a failed run from a pre-run collision must inspect the printed
result and whether a run ID was assigned; exit status `2` alone does not make
that distinction.

## `fclaw action list | exec | auth`

Discover and invoke the runtime's installed actions from a managed worker or
another command-line client:

```bash
fclaw action list -a
fclaw action exec -a --name read_file \
  --args '{"op":"start","path":"notes/today.txt"}'
fclaw action auth -h --name gcal -- status personal
```

`action list` loads the normal action registry and prints the same
manifest-backed catalog rendered into agent `{{tools}}` prompts. The output
contains each available action's name, description, and JSON argument schema.
It does not maintain a separate CLI catalog.

`action exec` accepts one action name and a JSON object (default `{}`), then
publishes an `action_exec` envelope to the active runtime owner. Intake turns
that envelope into an ordinary `action_request`; the normal action runner owns
lookup, argument coercion and validation, config and secret injection,
dispatch, result extraction, and terminal event recording. The command prints
the action result JSON and exits `0` for `action_succeeded`; rejection,
failure, timeout, or malformed input exits nonzero.

A gateway or embedded runtime owner must be active to consume the request.
Managed Codex processes already run beneath an active owner and receive both
commands in their injected worker instructions. No authentication token is
required by this local bridge.

`action auth` does not publish model work. It resolves the named action through
the same installed registry, then runs the manifest's optional
`operator_exec`. The executable receives the operator arguments after `--`
and a private action-scoped credential channel. OAuth consent, redirect URLs,
and destructive account maintenance therefore remain trusted terminal work;
they are never routed through chat or model text. Each action documents its
own stable operator syntax. In default/`-a` mode the command captures the
operator process and emits one compact object containing `ok`, `exit_code`,
`stdout`, `stderr`, and a truncation marker. Use `-h` for an interactive OAuth
or other operator session.

## `fclaw operation complete`

The action-facing managed-operation completion helper. A detached worker (or
its supervising runner) submits its terminal result through the ordinary bus:

```bash
fclaw operation complete -a --result-file result.json
fclaw operation complete -a --status succeeded --text "done"
```

`--operation-id` and `--token` default to `FCLAW_OPERATION_ID` and
`FCLAW_COMPLETION_TOKEN`, which the runtime places in the trusted child
environment when it launches a managed action. A `--result-file` must be a
JSON object with `status` (`succeeded` | `failed`) and an optional bounded
`text`; result text over the bounded cap fails loudly rather than
truncating. The helper accepts no routing, correlation, event ids, run ids,
request ids, task revisions, or timestamps — the runtime stamps all of those
from its durable operation record at intake.

Publication is a durable inbox envelope plus a best-effort wake knock, so it
works while the gateway is stopped; the claim is admitted after restart.
Duplicate invocations are safe: idempotent admission and the operation
store's terminal gate absorb them. A wrong token, unknown operation id, or
already-terminal operation is rejected into
`workspace/logs/rejected_events.jsonl` without creating any run.

Subprocess actions that need a long-lived local waiter should detach it with
`fclaw internal detach -- <command...>` (double fork + `setsid`, stdio to
`/dev/null`), so the waiter survives the action's own timeout, gateway
shutdown, and Ctrl-C.

## `fclaw replay <event_log.jsonl>`

Re-run the reducer over an event log and print the final state.

```bash
./bin/fclaw replay -h workspace/runs/<run_id>/event_log.jsonl
```

Agent mode emits the compact reconstructed state object; human mode labels the
formatted state. Output should match the run's `state.json`. This is the
canonical correctness check that behavior is event-driven.

## `fclaw view <run>`

Inspect a run.

| flag | meaning |
|---|---|
| `-f` | add trace, provider-call bodies, artifact-directory listings, and `state.json` |
| `-d` | add full failure/rejection payloads and request-correlated action subprocess stderr when present |

```bash
./bin/fclaw view -h -f -d <run_id>
```

Default/`-a` output is event and trace JSONL. Add `-h` when the timeline is
being read in a terminal.

For an `action_failed` terminal, `-d` follows `payload.request_id` to the
newest matching numbered `action_calls/<seq>_<request_id>.stderr` artifact.
Agent failure `error` events carry a `payload.artifact` naming their exact
numbered `agent_outputs/<seq>_<step>.stderr` file, and `-d` renders that file
directly — never guessing among attempts. Action rejections, runtime
intrinsics, and pre-exec failures may have no subprocess stderr; their
complete payload still appears in the debug section.

CLI argument parsing is strict everywhere: unknown arguments, missing
values, and flag-shaped values fail with a one-line fix-naming error
instead of silently running under a default.

## `fclaw usage`

Show LLM usage (tokens, latency, profile) from the current
`workspace/logs/llm_usage.jsonl` segment.

```bash
./bin/fclaw usage -a --agent main_claw
./bin/fclaw usage -h --profile anthropic_chat
```

The janitor rotates that ledger and `fclaw usage` does not aggregate its
rotated `.1` / `.gz` predecessors, so it is not necessarily lifetime usage for
the workspace. Human output states this boundary explicitly as `scope:
current log segment only; rotated history not included`. Exact per-run
provider evidence remains under
`workspace/runs/<run_id>/provider_calls/`.

Default/`-a` output is the same versioned JSON aggregate served by `/v1/usage`,
with no table text or terminal color. Rows are grouped by agent, profile,
provider, and model and include provider-reported cached-input tokens when
available.

## `fclaw cacheview`

Compare the latest raw provider request bodies for one LLM-backed agent.
The command is intentionally agent-scoped because provider prompt caching
is agent-prefix behavior.

```bash
./bin/fclaw cacheview -a --agent main_claw
./bin/fclaw cacheview -a --agent main_claw -n 5
./bin/fclaw cacheview -h --agent main_claw
./bin/fclaw cacheview -h --agent main_claw --run run_031 --call 1
./bin/fclaw cacheview -h --agent main_claw --list
```

It reads only:

```text
workspace/runs/*/provider_calls/*_meta.json
workspace/runs/*/provider_calls/*_raw_request.json
```

Human output starts with the shared raw prefix, follows it with one
whole-request aggregate heatmap, and puts the summary at the bottom. It does
not dump every divergent tail. `--run` selects one run's divergence;
`--call` disambiguates multiple calls from the same agent in that run.
`--list` explicitly opts into the per-call divergent-tail list. Default/`-a`
emits one versioned JSON object containing the selected call records, raw
request data, provider cache evidence, shared-prefix boundary, heatmap buckets,
and summary, without ANSI text.

## `fclaw auth …`

Manage credentials in FloofClaw's local secret store. Files live in a
gitignored `./secrets/` in the working directory — part of the
self-contained deployment directory alongside `workspace/` and `.fclaw/`.
The directory is mode `0700` and each secret is mode `0600`. Set
`FCLAW_HOME` to relocate the store. A provider-specific environment
variable, when configured in `config/llm_registry.json`, takes precedence at
request time.

```bash
./bin/fclaw auth set -h <endpoint> <value>  # value is visible in argv/history
./bin/fclaw auth set-stdin -h <endpoint>    # preferred: reads value from stdin
./bin/fclaw auth get -h <endpoint>
./bin/fclaw auth delete -h <endpoint>
./bin/fclaw auth header -h <endpoint>       # prints the provider-specific header
```

Known provider endpoints include `anthropic_key`, `openai_key`,
`openrouter_key`, and `gemini_key`. Avoid `auth get` and `auth header` in logs:
both intentionally print secret material.

Default/`-a` mode returns compact objects. For example, `auth get` returns
`{"endpoint":"…","value":"…"}`; the warning about logs applies equally to
that machine-readable form.

## `fclaw bus …`

Inspect or publish bus envelopes. Publishing does not create runs; the
runtime owner consumes inbox envelopes and creates/wakes runs.

```bash
./bin/fclaw bus publish -h [--channel <id>] --text <text>
./bin/fclaw bus log -h [-f|--follow] [--channel <id>] [--type <name>] [-n|--last <count>]
```

`publish` writes a `user_message` envelope to `workspace/bus/inbox/`.
The gateway or embedded runtime owner picks it up. `log` reads
`workspace/logs/bus.jsonl` with optional filters.

## `fclaw affair …`

Manage durable concerns. An affair is a "thing to keep an eye on" —
distinct from a task. The watcher reactor module fires `affair_review`
bus envelopes for active affairs whose `next_review_at_ms` has passed.

```bash
./bin/fclaw affair create -h --context <id> --manage <text> [--in <duration>]
./bin/fclaw affair list -h [--context <id>]
./bin/fclaw affair pause -h <affair_id>
./bin/fclaw affair resume -h <affair_id>
./bin/fclaw affair close -h <affair_id>
./bin/fclaw affair review -h <affair_id>
./bin/fclaw affair schedule -h <affair_id> --in <duration>
```

`<duration>` accepts `ms | s | m | h | d` suffixes
(`60s`, `5m`, `1h`, `1d`). On `create`, `--in <duration>` sets
`next_review_at_ms = now + duration`. Omitting `--in` makes the
affair manual-only — it never fires from the watcher; only
`fclaw affair review` (or a later `schedule`) will trigger it.
`schedule` sets `next_review_at_ms` on an existing affair. After
the watcher fires once, the field is cleared to 0; the affair
goes dormant until the review manager calls `defer` with a duration in
`args.value`, or
the CLI runs `schedule` again.

`review` publishes an `affair_review` envelope synchronously (same
shape the watcher emits) so you can exercise the review path without
waiting for an interval.

Affair state lives at `workspace/memory/state/affairs.json`. See
[Architecture: Affair Watcher](../architecture.md) and
[the floofclaw floop](../concepts/floofclaw_floop.md) for the lifecycle and current
event-gated call contract.

## `fclaw gateway …`

Long-running coordinator that owns the reactor.

```bash
./bin/fclaw gateway start -h [--floop <name>]     # daemonize
./bin/fclaw gateway run -h [--floop <name>]       # foreground, ctrl-C to stop
./bin/fclaw gateway status -h
./bin/fclaw gateway stop -h
./bin/fclaw gateway reload -h [--floop <name>]    # stop, then start
```

`start` and `run` share the same runtime owner path. `start` daemonizes
around it; `run` keeps it in the foreground. Both register
`.fclaw/run/gateway.pid` and `.fclaw/run/gateway.loop`, so `status` and
`stop` work for either form.

For process-listing clarity, `start` and `run` best-effort label the gateway
process as `fclaw_<bot_name>`, for example `fclaw_FloofClaw` from the committed
`config/floofclaw_config.json`. This is only a process label; it does not
namespace gateway state, ports, pid files, sockets, or logs.

Default/`-a` `status` queries the live status socket and reports gateway, run,
job, bus, and reactor health. If the gateway is down, it emits a small
down-state document and exits nonzero. `reload` is a full orderly stop followed
by a new daemon start; it is not an in-process configuration reload.

Configure with:

- `FCLAW_GATEWAY_POLL_MS` — poll(2) timeout ceiling (default 5000 ms;
  the reactor clamps below this when modules report immediate work).
- `FCLAW_LLM_ATTEMPT_TIMEOUT_S` — per-attempt LLM transport timeout
  (default 25 s, sized for hosted providers). Raise it for slow local
  models, typically with `FCLAW_LLM_MAX_RETRIES=0`; both overrides are
  clamped so the retry ladder always fits the agent job budget.

## `fclaw -i`

Interactive REPL on the `cli` channel. Each line is submitted through
the `cli` channel and waits for gateway deliveries. `fclaw -i` does not host
an embedded runtime owner; a gateway must already be running.

```bash
./bin/fclaw -i -h
./bin/fclaw -i -h --floop openclaw  # assert the live gateway owns openclaw
```

Type a line, see the message reply. Ctrl-C to exit. The running gateway owns
the floop. Omitting `--floop` follows that owner; a matching explicit value is
an assertion, while a conflict fails before any message is published and
tells you to run `./bin/fclaw gateway reload -h --floop <name>`. The client never
switches the live owner itself.

## `fclaw channel <cli|ws|irc>`

Run a simple stdin/stdout channel shim. These are local pipe interfaces, not
the gateway's real WebSocket or IRC network adapters.

```bash
./bin/fclaw channel cli -h                   # terminal UI; gateway required
./bin/fclaw channel cli -h --floop openclaw  # assert the live gateway floop
./bin/fclaw channel ws -a --floop openclaw   # newline JSON: {"text":"..."}
./bin/fclaw channel irc -a --floop openclaw  # each raw line is chat text
```

`channel ws` and `channel irc` can host an embedded owner when no gateway is
running; in that case `--floop` selects it. With a live gateway, omission
follows the active floop, a matching explicit value asserts it, and a
conflicting value fails before publication with the
`gateway reload -h --floop <name>` fix. `channel cli`, like `-i`, always requires
the gateway.

Note: when the **gateway** is running, channel modules
(`adapters/{irc,ws,discord}/`) handle real network
I/O directly inside the reactor. The `fclaw channel` CLI publishes
events and waits for deliveries; it does not allocate runs itself.

## `fclaw clear | actions | adapters | floops | setup | --version`

These commands follow the same default-agent/`-h` presentation rule. For
example, `fclaw actions` returns a compact array while `fclaw actions -h`
prints its colored table.

- `clear -h --yes` — reset workspace artifacts (runs, memory state, inbound
  media, bus, logs, `.fclaw/`). The documented "start fresh" path. Required
  whenever you've partially wiped the workspace — see
  [Project Layout: Starting fresh](../getting-started/project-layout.md#starting-fresh)
  for the collision contract and the
  `workspace_task_id_collision` refusal it prevents.
- `actions` — list every discovered action with its area, `outside_world`
  flag, and manifest path. Fails loudly, naming both manifests, if two
  actions share an `id`. `tools` is the former name and still works.
- `adapters` — list every adapter built into this binary, whether its
  channel is enabled, and the config key that enables it.
- `floops` — list every floop directory holding a `loop.json`.
- `setup` — emit structured first-run steps; `-h` prints the checklist. It
  does not write configuration.
- `--version` / `-v` — emit the version object; `-h` prints a labeled version.

## `fclaw internal …` (private)

Implementation detail: `internal agent-llm` is the child entrypoint the
gateway spawns for LLM-backed agents so blocking `llm_call` never runs
inside the reactor. Not for direct human use, but it's how
`executor: "llm"` agents work end-to-end. A selected media-bearing invocation
adds an internal `--media <manifest>` argument; callers must not construct or
scan that path themselves.
