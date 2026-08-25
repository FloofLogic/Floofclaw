# Changelog

FloofClaw follows semantic versioning for public source releases. The public
release tag and `runtime/version.h` carry the same plain `X.Y.Z` version.

## 0.29.0 — 2026-08-24

### Added

- **MCP servers, as generated actions.** `scripts/mcp_sync.sh` asks each
  server in `config/mcp.json` for its `tools/list` and writes one ordinary
  action per tool to `actions/mcp/<server>__<tool>/`, carrying the tool's own
  `inputSchema` through as `args_schema`. The kernel never learns the
  protocol: there is no MCP executor, no `dlopen`, and no JSON-RPC method,
  transport, or tool handling in the binary — a regression test fails if any
  appears in `runtime/`, or if MCP leaks past the one CLI wrapper file.
  Everything downstream applies because the output *is* an action:
  agent allowlists, deployment `force_disable`, `outside_world`, the action
  deadline, per-call artifacts, and the `managed_operation` result path.
  Config uses the same `mcpServers` shape as Claude Desktop and Claude Code,
  so an existing one can be pasted in. Both transports are supported: stdio
  (spawned per call, cold start accepted for v1) and HTTP via `curl`.
  See `docs/getting-started/mcp.md`.
- **`actions/mcp/_bridge.sh`** — the only piece that speaks MCP, and only at
  call time. Sends `initialize`, `notifications/initialized`, and
  `tools/call`, matches the reply by JSON-RPC id, strips FloofClaw's own `op`
  field so a tool never sees an argument it did not declare, writes the
  complete reply to the call's artifact, and returns a bounded 8 KiB text
  excerpt as the operation result. `isError`, a JSON-RPC error, a dead
  command, or a server that answers nonsense fails that one call with a
  coded error object; a hung server is killed with its whole process group
  after `FCLAW_MCP_CALL_TIMEOUT_S` (default 55s, inside the action's 60s
  deadline). No failure mode reaches the gateway.
- **`fclaw mcp sync|secret`** — the operator entry point, so MCP is
  discoverable from `fclaw` with no arguments and obeys the deployment-wide
  output contract like every other command: `-h` is the scripts' operator
  text, and agent mode (the default) returns one JSON object carrying
  `added`/`changed`/`removed`/`unreachable` and the applied/dry-run state.
  `runtime/mcp_cli.c` parses the output mode and `exec`s the script; it holds
  no protocol. `--dry-run` passes through, an unreachable server is a nonzero
  exit naming it in `unreachable`, and `fclaw mcp secret` reads the value on
  stdin so it never enters argv.
- **`scripts/mcp_secret.sh`** — stores one server credential across every
  action generated from that server, reading the value on stdin so it never
  enters argv. Action secrets are scoped to a single action id by design, so
  a server with eight tools needs eight copies; this is the one command that
  does it. `config/mcp.json` references them as `${NAME}` and never holds a
  value.
- **Sync is explicit and idempotent.** It rewrites `actions/mcp/` from the
  servers' current catalogs, prints added/changed/removed ids, supports
  `--dry-run`, and **never runs at gateway startup** — between syncs the
  on-disk catalog is the truth, exactly like every other action. A server
  that fails to answer keeps the actions it already has and the run exits
  nonzero naming it, so a transient outage cannot delete a deployment's tool
  catalog. Server and tool names are validated against
  `[A-Za-z0-9_.-]{1,48}` before they can reach the filesystem.
- **`tests/test_mcp_generation.sh`** — hermetic fixture servers (stdio and
  loopback HTTP) proving manifest generation, re-sync idempotence and stale
  removal, the bridge round-trip, every server failure mode failing loudly
  inside its bound, an end-to-end run through `openclaw` returning an
  `operation_result`, `force_disable` masking a generated action, the tool's
  own required fields enforced from the copied schema, credential injection
  reaching the server but not the artifacts, and the runtime staying free of
  the protocol.

- **Telegram channel adapter.** `adapters/telegram/` connects a BotFather
  bot over the plain-HTTPS Bot API: long-poll `getUpdates` inbound,
  `sendMessage` outbound. No websocket, no heartbeat, no reconnect state
  machine, and self-echo runaways are structurally absent — `getUpdates`
  never returns the bot's own messages. Each conversation gets its own
  context (`chat:telegram:<chat_id>`, `chat:telegram:dm:<user_id>`), group
  traffic requires an explicit `allowed_chat_ids` entry, and the poll offset
  is persisted only after a publish so a crash replays an update rather than
  losing it. v1 is text only: no media, editing, inline keyboards, or
  webhooks. See `docs/getting-started/telegram.md`.
- **Telegram live-transport gate.** `channels.telegram.api_base` is a
  hermetic test seam: refused entirely unless
  `FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK=1`, and then it accepts exactly
  `http://127.0.0.1:<port>` — the same posture as the media loopback
  switch. `tests/test_telegram_transport.sh` drives the production conn
  state machine, http1 parser, bus publish, hello reply, sendMessage, and
  the offset-acks-after-publish rule against a local fixture playing the
  Bot API; the routine gate runs it via `bin/fclaw_test_all`, a gateway
  binary that always contains every adapter, so coverage never depends on
  `FCLAW_ADAPTERS`.
- **`runtime/support/http1.{c,h}`** — a shared HTTP/1.1 request builder and
  response reader with real status-line and header parsing, `Content-Length`
  and chunked framing, keep-alive detection, and bounds that refuse rather
  than truncate. Discord's REST path is ported onto it, replacing
  `atoi(rx + 9)` for the status, no header parsing, and no chunked support.
- **`FCLAW_ADAPTERS` picks the channels compiled into `bin/fclaw`.** The
  WebSocket adapter is always linked because `fclaw -i` and the local client
  API ride on it; the outward channels are chosen —
  `make FCLAW_ADAPTERS="ws telegram"`, or `all` for everything. Test
  binaries always link every adapter, so `make test` proves the same thing
  on every machine. `fclaw adapters -a` is the authority for what a binary
  contains.

### Changed

- **The default build now contains only the WebSocket adapter.** A
  deployment that wants Discord, IRC, or Telegram rebuilds with
  `make FCLAW_ADAPTERS="ws discord"` (or `all`). A config enabling a channel
  the binary does not contain refuses startup and names the rebuild, rather
  than starting, looking healthy, and answering nothing on that channel.
  **Upgrade note:** existing deployments keep working by adding their
  channels to the build command — `make FCLAW_ADAPTERS="ws discord"` for a
  Discord bot — and changing nothing else. The refusal message contains the
  exact command.
- **`make FCLAW_ADAPTERS="ws discord" test` works as one command, and the
  Makefile says when it relinks.** The selection reached the
  adapter-selection test's deliberate default build through `MAKEFLAGS`;
  that test and the mock-only build contract now sever themselves from the
  parent make. The android build contract runs in an isolated copy so its
  own `make` cannot flip the working tree's selection mid-suite. When the
  selection changes the Makefile prints one line on stderr before it
  removes and relinks `bin/fclaw`. The shipped tests read `bot_name`,
  `default_floop`, and the channel set from the config under test instead
  of assuming the shipped one, so `make test` passes on a deployment branch.

### Added

- **Every managed-worker store is under retention.** `codex_ops` had a
  janitor task; Claude Code directories under
  `workspace/logs/claude_ops/<op_id>` and Hermes handles under
  `workspace/logs/hermes_ops/` had none and grew unbounded (jet's `codex_ops`
  is 7.2 MB across 22 directories today; the other two only grow). All three
  now share one task, one count-only rule, and one config shape —
  `appliance.<store>.keep` and `.check_interval_ms`, so `appliance.codex_ops`
  keeps meaning exactly what it meant. A worker added later that writes one
  entry per operation joins that table rather than getting a task of its own.
  New in the rule for all three, `codex_ops` included: an entry whose
  operation is still `running` in `workspace/memory/state/operations.json` is
  never removed, however old its number makes it look — its worker is still
  writing into it. An operation the bounded store has since forgotten is
  ordinary debris and stays prunable.

- **Bounded decision repair without an output contract.** A structurally
  malformed decision — a stray key beside `args`, a missing `name` — is now
  repaired for any LLM agent, not only one that declares
  `output_contract`. The budget is a top-level `repair_attempts` in
  `agent.json`, default `2`, `0` through `4` accepted; `0` restores the old
  behavior. Before this, a contract-less agent failed terminally as
  `agent_output_invalid` on the first rejection, and floop `retry_attempts`
  then re-ran the identical turn at full model price with nothing said about
  what was wrong (Scraps run_781: three clean envelopes, three identical
  rejections, zero repair prompts). The shipped `floofclaw` `chat_manager`
  declares no contract either, so the default floop could not repair a single
  slip. Contract agents are untouched: same budget key, same correction text,
  same `agent_decision_contract_exhausted` on exhaustion. A contract-less
  agent is pointed back at its own prompt with the rejection named rather
  than shown two forms it was never asked for, and exhausts as
  `agent_output_invalid`. Script and native agents are never re-run — their
  output is deterministic, so a second identical turn buys nothing.

### Added

- **Write confinement for every executing action.** `bash`, `apply_patch`,
  `web_fetch`, and `http_call` ran unconfined as the gateway user —
  `actions/core/bash/run.sh` was `subprocess.run(cmd, shell=True)` with the
  whole filesystem writable. They now run under `sandbox-exec` on macOS or
  `bwrap` on Linux, with writes permitted to the workspace root and nowhere
  else; reads, execution, and the network are untouched, and scratch space is
  a per-invocation directory inside the workspace that `TMPDIR` points at. It
  fails closed: on a host with neither mechanism the action refuses rather
  than running unconfined, and names the way past it. The opt-out is declared
  action config (`actions.<id>.sandbox: "off"`), so it comes from the
  deployment's own file and a model cannot reach it through arguments. The
  whole mechanism is in `actions/common/_lib/sandbox.sh`; the kernel learns
  nothing. One documented cost: macOS `mktemp(1)` with no template ignores
  `TMPDIR` and targets the per-user Darwin temp directory, which is denied —
  permitting that directory would void the confinement for any deployment
  installed under it. `SECURITY.md` states what this does and does not
  protect against.

### Changed

- **Shipped docs re-measured and de-contradicted.** Every published number was
  stale, including the ones a previous sweep had "corrected": the test counts
  said 32/139 or 33/156 against an actual 37/177, the binary size said 684,424
  or 650,664 bytes against 667,304, `sizeof(RtRun)` said 210,688 against
  226,048, and `sizeof(RtScheduler)` appeared as two different wrong numbers.
  All re-measured with `make metrics` and struct probes rather than copied
  forward, and the current-value sites now name the revision they were
  measured at so drift is visible without rebuilding. Contradictions fixed:
  the install guide expected `fclaw 0.26.2`; the floop table gave `openclaw`
  and `floofclaw` the wrong model profiles; the provider guide said
  `thinkingLevel` is sent only for Gemini 3 when the transport sends it for
  any Gemini profile with `effort` (the shipped `floofclaw_manager` is
  gemini-2.5-flash); `DEVELOPMENT_PROCESS.md` described a squash-based
  `release` branch that `RELEASING.md` replaced with an assembled public
  history, and used the retired `gateway status --json` spelling; the IRC
  channel comment called the real transport "intentionally deferred" when it
  ships in `adapters/irc/`; `project-layout.md` and `gateway-reactor.md` had
  never listed the Telegram adapter. `fclaw config` had no CLI reference
  section and was missing, with `-i`, from the `-a` usage `commands` array —
  both fixed. `docs/index.md` now links every doc that ships; six were
  unreachable, including the MCP guide. The one remaining omission is
  deliberate and now recorded in `release_manifest.txt`:
  `docs/testing/baseline_v0.18.0.md` is an internal baseline that does not
  ship, so linking it dangles in the assembled public tree. Dead files removed:
  `runtime/support/stringhelp.h` (no includers), the two `run.sh` scripts for
  actions that are runtime intrinsics, and two `.gitignore` entries
  un-ignoring directories that do not exist.

- **One deadline per action instead of three.** `actions/core/bash` declared
  `timeout_ms: 30000` while its script enforced its own `timeout=30` and its
  manifest also claimed a 300s/600s managed-operation window — ten times the
  deadline the kernel actually kills the job at. Same shape in `apply_patch`,
  `web_fetch`, and `http_call`. The kernel now exports the manifest's
  `timeout_ms` to every subprocess action as `FCLAW_ACTION_TIMEOUT_MS`, and
  each script derives its command budget from it with a small reserve, so a
  command that runs long reports a clean timeout instead of being SIGKILLed
  mid-write. The managed-operation window on those four is now a backstop
  just above the job deadline (60s/120s and 90s/180s) rather than an
  unrelated order of magnitude.

### Fixed

- **Linux sandbox: no tmpfs over `/tmp`.** The `bwrap` argv mounted a
  private tmpfs over `/tmp` after binding the workspace, so anything the
  host keeps there vanished inside the sandbox — including a deployment or
  test copy that lives under `/tmp`, whose own workspace bind the tmpfs then
  shadowed (on jet, `web_fetch` could not see the test's fake `curl` and ran
  the real one). Scratch already lives inside the workspace via `TMPDIR`;
  `/tmp` is now the same read-only view of the host it is on macOS.
- **A crash could duplicate any local mutation.** `outside_world: false`
  makes a started request without a terminal eligible for replay, and
  recovery dispatches it again under the same request id — but the mutating
  intrinsics ignored that id entirely (`(void)rid;`), so a crash in the
  window between `action_started` and the terminal repeated whatever the
  first dispatch had already committed. The audit found every one of them
  affected: `note_add` added the note twice, `counter_bump` applied its delta
  twice (a number quietly wrong rather than visibly duplicated),
  `affair_close` closed twice, `defer` recomputed its deadline from the
  restart clock and silently pushed the review out by however long the
  gateway was down, `affair_open` minted a second concern under a different
  id because the id comes from the run's event counter, and `work` created a
  second work task — after which `rt_task_select_unique_revisable_work` saw
  two revisable tasks in the context and refused every later `work` call as
  ambiguous. `work_complete` and `work_blocked` did not duplicate, but failed
  the replay as a stale binding, stamping a failed terminal on work that had
  completed.
  Each now stamps its request id into the event it appends and consults the
  shared `rt_action_committed_effect` first, returning the committed values
  with `"recovered": true` instead of appending a second time — generalizing
  the guard `working_memory_append` already had, which now uses the same
  helper. Both halves of the crash window are covered per mutation class in
  `tests/integration/test_local_action_replay.c`.

- **Memory compaction never once succeeded in deployment.** Both live bots
  held `conversation_summary: null` with 172 and 218 uncompacted messages,
  and 489 of one bot's 492 `rejected_events.jsonl` lines since 07-24 were the
  same compaction-cutoff rejection — burying every real rejection in the file
  the debug flow names as its first stop. The rejection was a symptom. The
  compactor was called with *no input*: the agent-input builder's
  `conversational_payload_only` / `affair_extraction_context_only` /
  `memory_compaction_context_only` branch emitted the rendered prompt alone,
  so a template that does not name `{{json}}` — which all three shipped
  `memory_compactor` prompts do not — silently dropped the projected request.
  The provider request was the 421-byte template and nothing else; given no
  messages, the model invented a cutoff and summarized its own instructions,
  and the runtime rejected the invention once per turn, forever. Those three
  shapes now get the same appended `=== model input ===` section every other
  templateless agent gets, which repairs the shipped floops and every
  `fclaw_*` fork of them without touching a prompt.
- **A deployment-sized message store no longer wedges compaction.** The
  compaction request is built into a fixed buffer, travels as an event
  payload, replays into the run context, and is rendered into the compactor's
  input — and 147 messages already build ~50 KB. A window that overflowed
  failed the run and left the store to grow, so every later turn overflowed
  harder and nothing ever compacted. The window is now narrowed to fit the
  payload budget instead: compaction is incremental, so a narrowed round
  still advances the summary cursor and the next round takes the rest. Only
  a window that cannot fit even its configured minimum fails, and it names
  `memory_min_compact_messages` as the fix.

- **A call-level `task_id` binds instead of failing the turn.** gemini-2.5-flash
  intermittently writes `task_id` beside `args` rather than inside it — a
  perfect call otherwise. The normalizer rejected that shape as a stray key,
  and because the repair prompt said only "not the correct output format"
  beside the rejected response, the model repeated the identical shape
  through every repair attempt and the turn died as
  `agent_decision_contract_exhausted` (Truly run_130, three in a row; the
  same rejection had hit three earlier turns). The binding is the same fact
  either way, so the placement is now accepted (two disagreeing values are
  still refused). Genuinely stray keys are still rejected, and the repair
  reason now carries the normalizer's own detail, naming the key.
  Regressions: `openclaw_call_level_task_id_binds_without_repair`,
  `openclaw_stray_call_key_repair_names_the_key`.
- **A failed continuation run now closes the ask it was handling.** A run
  that fails or is canceled has always stamped `task_failed` /
  `task_canceled` on the input task born in it; a run started by a
  runtime-owned event such as `operation_result` — which carries the
  `task_id` of an older ask — left that ask `open` forever when it died,
  since nothing else was ever scheduled for it. Truly leaked three open
  input tasks this way in two weeks. The engine now also terminalizes the
  task the dying run was bound to, trusting the binding only from a
  reserved event kind and only within the run's own context. Regression:
  `failed_continuation_run_fails_the_ask_it_was_handling`.
- **Docs: who closes an input task.** `docs/concepts/task-feature.md` names
  the three ways an input task closes and the failure mode when an agent
  uses none of them: the 32-slot store fills and the bot degrades to
  chat-only with `invalid_agent_call: action call requires existing
  task_id` in `rejected_events.jsonl`. Shipped agents carry a `_note` next
  to `autocomplete_message_task_on_send` pointing there, and
  `DEVELOPMENT_PROCESS.md` records that engine merges do not migrate bot
  floops.

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
