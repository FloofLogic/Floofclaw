# Floops

A floop is a self-contained FloofClaw loop bundle. It is the unit you copy,
share, and select at runtime.

```text
floops/<name>/
├── loop.json
└── agents/
    └── <agent_id>/
        ├── agent.json
        ├── prompt.md
        └── run.sh
```

`loop.json` defines the ordered phases. The `agents/` directory contains the
agent packets those phases reference. Actions stay runtime-wide under
`actions/`; a floop chooses visible actions through its agent `actions` allowlists.

## Running a floop

The floop name is the directory name:

```bash
./bin/fclaw run -h --floop floofclaw --text "hello"
./bin/fclaw gateway start -h --floop openclaw
./bin/fclaw -i -h  # client of that gateway-owned openclaw runtime
```

When a command becomes the runtime owner and `--floop` is omitted, it reads
`default_floop` from `config/floofclaw_config.json`. If that file is missing,
malformed, or does not contain a nonempty value, the compiled fallback is
`floofclaw`. A fresh clone's committed config selects `hello`, the no-account
mock loop.

A live gateway already owns one floop: clients with no `--floop` follow it,
and a matching explicit value is only an assertion. A conflicting value fails
before bus publication and names the active and requested floops plus the fix,
`./bin/fclaw gateway reload -h --floop <name>`.

## Sharing a floop

To share a configured loop, copy the whole directory:

```bash
cp -R floops/floofclaw floops/my_assistant
```

Then set `name` to `my_assistant` in the copied `loop.json`; the directory and
declared profile name should agree.

The receiving checkout can run it with:

```bash
./bin/fclaw run -h --floop my_assistant --text "hello"
```

This keeps prompts, executor config, model references, listen blocks, and loop
ordering together instead of splitting them across unrelated top-level paths.

## Resolution

Production resolution prefers:

```text
floops/<name>/loop.json
floops/<name>/agents/<agent_id>/
```

The runtime also supports fallback paths for tests:

```text
loops/<name>.json
agents/<agent_id>/
```

Those fallback paths are for fixture setup. Production floops should live under
`floops/`.

## Profile Options

Beyond `steps`, a `loop.json` may declare generic execution options:

- `one_pass` — one event → one run → one complete profile pass; the
  end-of-profile check never rewinds to phase zero. It does not change task
  state.
- `serialize_contexts` — same-context runs advance in intake order;
  other contexts advance concurrently.
- `retry_attempts` — bounded per-event retry after a failed phase job;
  committed calls are memoized by effect signature.
- `operation_timeout_ms` — optional floop-level shortening of
  managed-operation completion deadlines; the action's declared
  `default_timeout_ms`/`max_timeout_ms` policy still bounds it.

Per step: `gate` (including the declarative
`"event_kind:kind1,kind2"` form) and `non_critical` (step failure never
fails the processed event).

### Gating a custom event kind

`event_kind:` tokens are opaque to the kernel — it compares bytes. That means
a step can gate a **product-defined** kind published by a local producer
through [`fclaw bus publish --type`](../reference/cli.md#--type-typed-event-ingress),
with no engine change and no registration:

```json
{
  "steps": [
    {"id": "scan", "type": "agent", "agent": "scan_handler",
     "gate": "event_kind:device_scan_requested"},
    {"id": "dispatch", "type": "builtin", "builtin": "action_runner"}
  ]
}
```

The handler listens to `event` and sees the kind plus the producer's payload
unchanged:

```json
{"event": {"kind": "device_scan_requested",
           "text": "",
           "payload": {"scope": "installed_applications"}}}
```

`text` is empty for a custom kind — the projection keeps the field for shape
stability, and the payload is where the facts are. A deterministic `script`
agent can map those facts to its allowlisted actions and a reply with no
provider call at all; an LLM agent is for when interpretation is actually
needed.

Opting in is the gate. A floop that does not name a kind never sees it: the
event still becomes an inspectable run, every step skips, and the run
completes. It does not fall through to the chat agent.

## LLM Output Contract

An ordinary LLM agent may opt into a bounded task-decision contract in
`agent.json`:

```json
{
  "output_contract": {
    "kind": "task_action_or_finalize",
    "repair_attempts": 2
  }
}
```

For task-bearing `user_message` and `operation_result` turns, the accepted
forms are one substantive action (optionally one progress `message` and one
`working_memory_append`) without `task.update`, or one final `message` plus the
exact current `task.update(state:"completed")` without a substantive action.
Validation happens before any event or delivery is committed. An invalid
response is preserved, and the same agent phase receives it with a specific
correction through another ordinary provider call. `repair_attempts` accepts
`0` through `4`; exhaustion fails the run. Events without a task binding, such
as an affair review, retain the ordinary calls-array contract.

## Bound Work Controller Policy

An agent with `"bind_task": "open_work"` may configure its semantic repair
persistence in `agent.json`:

```json
{
  "bind_task": "open_work",
  "work_policy": {
    "max_repair_attempts": 3
  }
}
```

`max_repair_attempts` is the number of additional controller selections
allowed after rejected or failed actions. It accepts `0` through `8` and
defaults to `1` when omitted for compatibility. The floop owns the behavioral
choice; the engine only enforces the selected value and the absolute safety
ceiling. The separate eight-selection semantic-lineage ceiling can still stop
a lineage earlier.

## Shipped Floops

- `hello` — the fresh-clone default: a no-account mock conversation that proves
  setup and local execution without a provider key.
- `floofclaw` — the full durable-concern manager loop: standing affairs with
  scheduled reviews, detached background
  work through the managed-operation contract, proactive messages. See
  [the floofclaw floop](floofclaw_floop.md).
- `openclaw` — one-main-agent coding/tool loop; managed action results return
  to the same `main_claw` on later `operation_result` runs until it replies or
  is blocked.
- `fast` — native `noop_agent` perf/test floop.
- `companion` — conversational companion with concern extraction.
- `thrash_cap` — cap-saturation harness profile, shipped for robustness tests
  rather than normal conversation.

Future routing can classify input and choose a floop before submitting work,
but the floop bundle itself stays the same: one directory with a loop profile
and its agent packets.
