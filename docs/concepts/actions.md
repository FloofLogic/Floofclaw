# Actions

Actions are FloofClaw's tool boundary. Agents do not write files, fetch URLs,
run shell commands, or deliver user-visible messages directly. Agents emit
tool intent; most agents use the `{"calls":[]}` envelope, while selected-task
compatibility processors use the smaller `{"tool":...}` envelope. No shipped
modern floop depends on that compatibility path. The runtime maps accepted
calls into durable `action_request` events with runtime-owned `request_id`
values, validates them
against loaded `action.json` contracts, then executes the action and emits
terminal lifecycle events.

## Runtime Shape

```text
actions/
└── <area>/
    └── <action_dir>/
        ├── action.json
        └── run.sh        # subprocess actions only
```

Discovery is the directory tree. There is no registration list: `actions/` is
scanned recursively at startup, and every directory holding a valid
`action.json` is loaded. Copying a directory in is the whole install.

`action.json`'s `id` is the action's identity and must be unique across
everything under `actions/`. A collision fails startup naming both manifests,
so an action is never silently shadowed by installation order. The first path
segment (`<area>` above) is a display grouping only — it appears in
`fclaw actions -h` and names nothing.

Directories whose names start with `_` or `.` are skipped. That is the disable
mechanism: `mv actions/github actions/_github` unloads an area while leaving it
visible in the tree, and it is what keeps build tooling such as
`actions/common/web_read/_pluck/` out of the scan.

### The `common` and `core` areas

`actions/common/` holds capabilities shared by the shipped general-purpose
floops:

- `gcal` — Google Calendar read/create/update
- `git_github` — literal-argv Git and GitHub CLI operations with setup checks
- `http_call` — bounded HTTP request/response access
- `manage_codex` — managed Codex worker lifecycle
- `manage_claude` — managed Claude Code worker lifecycle
- `manage_hermes` — managed Hermes worker lifecycle
- `web_read` — Reader-style URL extraction via `pluck`

`actions/core/` holds runtime primitives. These two areas are the only action
directories tracked by the repository; other top-level areas are local
installation surface and ignored by Git. Common `web_read` owns a public
`pluck` submodule from [FloofLogic/pluck](https://github.com/FloofLogic/pluck),
and public releases flatten its pinned contents into ordinary files.

Discovery makes an area loadable, not universally visible. An agent must name
an action explicitly or receive it through the deliberate `all_actions`
grant. The same `action.json` contract applies — runtime intrinsic vs
subprocess, allowlist enforcement, terminal-event shape — so an agent cannot
tell which display area supplied an action. See `actions/common/README.md` for
current dependencies and contracts.

Core `web_fetch` is the bounded raw-fetch path used by OpenClaw. It writes the
full body to the runtime-provided action-call artifact, records the HTTP code
as `http_status`, and returns a small `text` summary containing that code and a
1,200-character body excerpt. The managed-operation wrapper owns the separate
`status:"finished"` lifecycle field and publishes the summary exactly once on
a later `operation_result` turn.

### Delegate actions

`actions/common/manage_codex/` and `actions/common/manage_claude/` are
runtime-intrinsic actions (`exec: ["runtime", "fc_manage_codex()"]` /
`fc_manage_claude()`) that wrap an external agent CLI as a managed
lifecycle. Op kinds: `start | poll | reply | abort | status`. Op state
persists under `workspace/logs/codex_ops/<op_id>/` and
`workspace/logs/claude_ops/<op_id>/`. A floop's native workers agent
invokes the action named by its configured `handler`; the runtime dispatches
the declared managed-operation contract without matching action names. See
[Architecture](../architecture.md) and [the floofclaw floop](floofclaw_floop.md).

`manage_codex` also prepends its action-owned `AGENTS.md` to each delegated
prompt and places the running `fclaw` binary on the child process's `PATH`.
That worker can use `fclaw action list -a` to inspect the exact catalog rendered
for native agents and `fclaw action exec -a --name <action> --args
'<json-object>'` to invoke one through the active runtime owner. The exec
bridge becomes a normal runtime-authored `action_request`; it does not
implement a second action dispatcher.

## `action.json`

`action.json` is the single source of truth for the action contract:

```json
{
  "id": "write_file",
  "description": "Write UTF-8 text content to a workspace-relative file path. Managed operation: call with op:start.",
  "outside_world": true,
  "args_schema": {
    "type": "object",
    "required": ["op"],
    "additionalProperties": false,
    "properties": {
      "op": {
        "type": "string",
        "enum": ["start", "poll"]
      },
      "path": {
        "type": "string",
        "description": "Workspace-relative path to write. Required with op:start."
      },
      "content": {
        "type": "string",
        "description": "File content to write. Required with op:start."
      }
    }
  },
  "exec": ["runtime", "fc_write_file()"],
  "timeout_ms": 0,
  "contract": "managed_operation"
}
```

Runtime-facing fields:

- `id` is matched against model tool intent before the runtime writes
  `action_request.payload.action`.
- `outside_world` marks actions that perform outside-world effects.
- `args_schema` is used for pre-execution argument validation.
- `exec` selects either a subprocess command or a compiled runtime function.
- `timeout_ms` is the action-specific execution timeout.

Model-facing fields:

- `id`, `description`, and the model-facing parts of `args_schema` are rendered
  as a JSON array in the generated available-tools section in LLM prompts.
  Each item has `name`, `description`, and a compact model-facing `args` object.
- `exec`, `timeout_ms`, and `outside_world` are not shown to the model.

Action authors own the complete model-facing description. The engine does not
truncate it or derive a second routing summary. If one agent's complete
eligible catalog cannot fit the prompt boundary, startup fails and names the
source manifest descriptions/schemas or agent catalog that must be smaller.

## Where calling guidance lives

**All per-action calling guidance goes in `action.json` — never in an agent
prompt.** The top-level `description` says when to reach for the action and
what to provide; each `args_schema` property `description` carries the format
rules for that field.

If a model calls an action with bad or missing arguments, **that is the
action's bug.** Fix the manifest, not the caller. The registry renders
`description` and `args_schema` to every agent allowed to call it, so one edit
reaches every caller. A prompt patch fixes one agent in one floop and starts a
second source of truth that will drift.

Write field descriptions with an example, not just a format name:

```json
"time_min": {
  "type": "string",
  "description": "list only: earliest event start, as an absolute RFC3339 dateTime like 2026-08-01T18:00:00-05:00. Never a relative word such as \"today\"."
}
```

A real failure this prevents: a work manager called `gcal` with
`time_min: "todayT18:00:00-00:00"` and Google returned `400`. The field's
description said only `RFC3339` — correct, and useless. The neighbouring
`start` field gave a concrete example and was called correctly every time.

See [Principles §19](principles.md#19-a-bad-tool-call-is-the-actions-bug-not-the-prompts).

## Args schema

FloofClaw's runtime validator enforces a deliberately small JSON Schema
subset:

- `type`: `object`, `string`, `number`, `integer`, `boolean`, or `array`.
- `required`: array of required object property names.
- `properties`: object property schemas.
- `additionalProperties`: `false` rejects unknown object properties.
- `enum`: string enum values.
- `description`: model-facing field or property description.
- `items`: item schema for arrays.

An action manifest may carry additional JSON Schema keywords as model-facing
guidance or validate them inside the action itself. The runtime does not treat
keywords outside the list above as an authorization or validation boundary.

If a model omits `args`, the runtime normalizes it as `{}` for schema
validation. If `args` is present but is not an object, the model output
is malformed and no `action_request` is appended.

## Exec forms

Subprocess actions use:

```json
"exec": ["bash", "run.sh"]
```

The runtime writes a request JSON object to stdin, runs the command, captures
stdout/stderr under `workspace/runs/<run_id>/action_calls/`, and maps process
exit status plus stdout `{ "result": ..., "error": ... }` to `action_succeeded`
or `action_failed`.

Runtime-function actions use:

```json
"exec": ["runtime", "fc_list_directory()"]
```

The second array item is a stable contract-facing function name. It is resolved
through a compiled runtime action function table, not dynamic symbol lookup.
Runtime functions do not create subprocess artifacts.

## Agent allowlists

LLM-visible and runtime-permitted actions are selected in `floops/<floop>/agents/<id>/agent.json`:

```json
{
  "id": "main_claw",
  "executor": "llm",
  "model": { "ref": "responses" },
  "actions": ["write_file", "read_file", "bash", "web_fetch"]
}
```

The scheduler loads these arrays once at startup and resolves registry actions
to numeric slots. Explicit ids are both hard authorization and bootstrap
presentation order. Authorization thereafter is a bit-mask test, while
durable events retain stable string ids. The runtime rejects any request
outside the mask with `action_rejected.reason = "not_allowed"`.

`all_actions` is a special grant-and-presentation sentinel, not a registered
action. It fills the agent's allow mask and expands in place to every loaded
action's complete authoritative description and schema. An agent that should
receive the complete registry therefore needs only:

```json
"actions": ["all_actions"]
```

Expansion preserves exact configuration order and deliberately does not
deduplicate. For example, `["message", "all_actions"]` presents `message`
once explicitly and once again inside the full expansion; that overlap is a
configuration flaw and must be removed from `agent.json`, not hidden by the
renderer.

`action_list` remains an ordinary managed action. Without `name` it returns
the ids authorized by the calling agent's mask. With one returned `name` it
returns that action's complete authoritative `description` and `args_schema`
in one result. Restricted agents receive only their restricted ids. The engine
does not rank or automatically select actions.

The catalog is deterministic text. Put `{{tools}}` once in the stable part of
`prompt.md`, before the changing `{{json}}` input, when provider prefix caching
matters. FloofClaw preserves the bytes and order; whether a provider serves a
cache hit is provider behavior, not an engine guarantee.

## Request lifecycle

Agents emit model-facing tool intent as a call:

```json
{
  "calls": [
    {
      "name": "write_file",
      "args": {
        "task_id": "task_run_001_000002",
        "op": "start",
        "path": "note.txt",
        "content": "hello"
      }
    }
  ]
}
```

Ordinary task-bound calls include an existing `task_id` from trusted task
input. A controller configured with `"bind_task":"open_work"` is narrower:
the runtime selects the exact same-context open task and `work_rev`, injects
that trusted binding, and rejects model attempts to replace it. A
`work_consequence` gate invokes that controller only for a correlated
selection result. The retained selected-task compatibility processor may
instead return simple `{"tool":...}` intent; no shipped modern floop depends
on it.

The runtime validates readiness, strips task linkage before action-schema
validation, stamps linkage onto the durable `action_request`, then emits one
of:

- `action_rejected` for pre-execution refusal.
- `action_started` followed by `action_succeeded` for success.
- `action_failed` before `action_started` when subprocess artifact preparation
  or executable validation fails. Process launch and execution failures occur
  after the checked, synchronous `action_started` claim.

V1 rejection reasons are:

- `unknown_action`: not loaded from an enabled area.
- `not_allowed`: action exists, but the source agent allowlist does not permit it.
- `invalid_args`: normalized arguments failed the action's `args_schema`.
- `outside_world_denied`: reserved for outside-world policy denial.
- `duplicate_terminal_request_id`: an already-terminal request id was seen
  again; the runtime records this instead of silently ignoring it.
- `duplicate_equivalent_action_call`: the same source, action, args, and work
  binding are already in flight. A completed equivalent call is memoized from
  its recorded terminal outcome under the new request id rather than executed
  again.

### Task working-memory sidecar

`working_memory_append` is the one task-working-memory mutation. Its content
argument is only `working_memory`; task identity remains routing metadata.
The task reducer appends the opaque text in event order and the runtime never
interprets, replaces, deletes, or compacts it.

A controller bound with `bind_task: "open_work"` may return this sidecar plus
one ordinary action. Normalization validates both calls, orders the sidecar
first even when the model returned it second, and omits `work_rev` and
`trigger_event_id` from the sidecar request. It therefore neither competes
with nor increments the controller's semantic selection and repair budgets.

`action_result` is no longer emitted. All normal agent executors use the same
authority boundary: they emit call intent only, and the runtime stamps durable
IDs, trusted `source`, and lifecycle events.

At the untrusted model boundary, a call object may use `action` as a safe alias
for the canonical `name` field. The normalizer accepts exactly one of those
keys, never both, and still rejects every other non-intent key. Raw provider
bytes remain unchanged in artifacts; durable events always use the canonical
runtime-owned action field.

Successful data is not projected into task artifacts. A data-returning
managed action publishes a later `operation_result`, and the floop's configured
agent turn decides what to message, note, or do next. OpenClaw routes that
later turn to the same `main_claw`; FloofClaw routes it to `result_manager`.
Ordinary managed-action failure or rejection also returns through the
correlated `operation_result` continuation. Failed task-bound actions retain
one narrow diagnostic path: the runtime may append tool, reason, and error
context to the task through a runtime-owned `task_updated` event. That failure
record is for debugging, not a second result channel.
The runtime stamps the diagnostic artifact with the exact bound `work_rev`;
agent-facing task projections expose retained artifacts under separate
`current` and `historical` arrays by comparing that stamp with the task's
current revision.
For the shipped FloofClaw manager, a result turn may record affair evidence
but cannot close the affair; closure belongs to a later `affair_review` turn
with active review context.

### Bound work consequences and recovery

A trigger-bearing controller selection is synchronously committed before it
can enter the dispatch queue. Its terminal action result, failure, or
rejection becomes a correlated `work_step_result` or `operation_result`
consequence with the originating task, `work_rev`, `request_id`, action,
trigger, and source event. The reducer-owned work-step ledger allows at most
eight semantic selections in one external work lineage. The bound controller's
agent packet owns `work_policy.max_repair_attempts` (default `1`, accepted
range `0` through the engine safety ceiling of `8`). A rejected or failed
selection may receive another repair while that configured budget remains.
Exceeding either the semantic bound or the configured repair budget appends
`work_blocked` mechanically before another model invocation.

A controller-selected `work` revision inherits both counters. Only a new
external revision resets them. On cold recovery, an outside-world request
without `action_started` remains launchable under its stable request id. Once
`action_started` is durable and no terminal can be proven, the runtime never
replays or infers success: it records `ambiguous_completion` and blocks the
current bound revision. This conservative recovery behavior is why
`outside_world` must describe the manifest honestly.

## `run.sh` Contract

Subprocess actions read one JSON object from stdin:

```json
{
  "request_id": "actionreq_run_042_000005",
  "action": "bash",
  "args": {
    "op": "start",
    "cmd": "printf hello"
  },
  "run_id": "run_042",
  "task_id": "task_run_042_000002",
  "call_id": "001_actionreq_run_042_000005",
  "artifacts": {
    "body": "runs/run_042/action_calls/001_actionreq_run_042_000005.body.html"
  }
}
```

The runtime also sets `FCLAW_WORKSPACE_ROOT` for subprocess actions. Paths in
the JSON envelope are workspace-relative; actions must not hardcode the current
workspace directory name into inputs or outputs.

### Secrets

Legacy read-only action credentials are injected as plain environment
variables the action names *without any prefix*:

```sh
: "${CLIENT_ID:?}" "${CLIENT_SECRET:?}" "${REFRESH_TOKEN:?}"
```

The operator stores each one under the action's own namespace:

```sh
printf '%s' "$CLIENT_ID" \
  | ./bin/fclaw auth set-stdin -h action:<action_id>:client_id
printf '%s' "$CLIENT_SECRET" \
  | ./bin/fclaw auth set-stdin -h action:<action_id>:client_secret
printf '%s' "$REFRESH_TOKEN" \
  | ./bin/fclaw auth set-stdin -h action:<action_id>:refresh_token
```

Before `exec`, the runtime enumerates `action:<this action's id>:*`, strips the
prefix, uppercases what remains, and sets it in the child's environment.
For a legacy environment-backed action, `action:example:client_id` becomes
`$CLIENT_ID` inside that action's subprocess.

**The action never names its own namespace, so it cannot reach another's.**
The prefix is built by the runtime from the id it dispatched, not from
anything the action supplies. `gcal` cannot read `action:another_action:*`, cannot
read provider keys like `gemini_key`, and cannot sweep the store: an empty or
path-bearing prefix is rejected by the same charset check that guards whole
keys.

Secrets live at `secrets/<key>`, mode 0600, gitignored. They are never written
to the run dir, the event log, or trace artifacts.

Actions that need credential rotation declare both an action-owned operator
entry point and the private broker:

```json
{
  "private_credentials": true,
  "operator_exec": ["bash", "auth.sh"]
}
```

The runtime then suppresses environment injection and passes a private
full-duplex `FCLAW_SECRET_FD`. The generic broker supports validated suffix
read, list, atomic set, and delete operations only below the resolved action's
`action:<id>:` namespace. The parent stamps the registry identity; the child
cannot choose a sibling namespace. Values have fixed size limits and never
travel through argv, stdout, stderr, events, traces, or workspace files.

The trusted operator surface uses the same registry definition and broker:

```sh
fclaw action auth -h --name <action> -- <action-owned arguments>
```

The runtime knows only how to resolve and execute that entry point. Command
names, OAuth flows, account policy, and sanitized diagnostics remain owned by
the action.

Do **not** source `.env` files into the gateway to supply action credentials.
That puts every action's secrets in every action's environment, and it fails
silently and totally the first time someone starts the gateway without the
extra step. New mutable credential integrations should use the private broker;
existing read-only integrations may retain scoped environment injection.

### Config

Non-secret deployment settings — an ssh host, a mail profile, a base URL —
are **declared** in `action.json` and **supplied** by the deployment.

Declare the names in the manifest. Values never go here:

```json
"config": [
  {"name": "SERVICE_BASE_URL", "description": "service endpoint", "required": true},
  {"name": "SERVICE_PROFILE", "description": "local profile",     "required": false}
]
```

Supply the values in `config/floofclaw_config.json`, keyed by action id:

```json
"actions": {
  "my_action": {
    "SERVICE_BASE_URL": "https://service.example.test",
    "SERVICE_PROFILE": "example"
  }
}
```

The runtime sets each resolved name in the action's environment, so `run.sh`
reads `$SERVICE_BASE_URL` and never learns where the value came from.

**Why the split:** an action can be shared across deployments. A concrete
endpoint is a fact about *one install*, not about the action. The manifest says
what the action *needs*; the deployment says what it *points at*.
The test is whether the value would be identical for someone else who
installed the action: `timeout_ms` yes, your ssh host no.

A **required** name with no supplied value fails the call loudly with
`config_missing`, naming the config path to fix. It does not start the action
and let it misbehave.

Config names must be `[A-Z0-9_]`. A name that looks like a credential
(`*SECRET*`, `*TOKEN*`, `*PASSWORD*`, `*API_KEY*`, …) is **rejected at load**:
manifests are shipped, shared, world-readable code, so secrets belong in the
action's secret namespace above, not here.

Per-call values the model chooses — a recipient, a message body — are neither
config nor secrets. Those are `args_schema` args.

Subprocess actions write one JSON object to stdout:

```json
{
  "events": [],
  "result": {
    "stdout": "hello",
    "stderr": "",
    "exit_code": 0
  },
  "error": null
}
```

`events` is reserved for future expansion. Today, the runtime preserves raw
stdout as an artifact and trusts only `result` and `error` body content when
emitting durable terminal events. For subprocess actions, the process exit status
decides success or failure.

### Result size limits

The serialized result of a subprocess action must fit in `RT_LARGE` (a
4096-byte C buffer, including its terminating NUL). Larger ordinary payloads
are refused: the runtime emits `action_failed` with error
`result_too_large: N bytes exceeds max 4096`, calls `rt_narrate` so the ops
channel logs the refusal, and preserves raw stdout on disk in
`workspace/runs/<run_id>/action_calls/*.output.json` for forensics. For a
managed-operation result, the runtime first attempts to salvage the small
contract fields `status` and `handle`, plus bounded `text` when present. A
successful salvage emits an `action_succeeded` result containing those fields
plus `_salvaged:true`; the full output remains in the artifact. Missing or
oversized required contract fields use the same loud failure path as an
ordinary result.

The operation driver may forward at most 8192 decoded bytes from a terminal
managed operation's `text` or `final` field. Its correlated event and bus
payloads each have a fixed 16384-byte cap. `action_list` checks the first cap
before returning a definition, so a named action contract arrives complete in
one result or the discovery call fails loudly; it is never silently truncated
or paginated.

This is a design cap, not a memory constraint. Data returned through a managed
operation enters an LLM prompt on a later turn, so small results are the
discipline that keeps prompt cost and context usage predictable. Managed-op
contract fields (`status`, `handle`, `text`) all fit easily; the cap only
bites when an action author stuffs redundant structured data into the
result alongside the human-facing text.

For big data — a fetched page, a query result, a report — write it to a
workspace file and return a pointer + short excerpt.
`actions/common/http_call/run.sh` is the current canonical example: it
writes the body to `artifacts.body` and returns

```json
{
  "url": "...",
  "status": "finished",
  "handle": "http_call_example",
  "http_status": "200",
  "body_artifact": "body.txt",
  "body_excerpt": "first 1200 chars",
  "body_bytes": 4531234
}
```

The LLM sees the excerpt, decides whether it needs more, and calls
`read_file` with the pointer (with `offset` / `length` for pagination).
For multi-step research, use a managed-op action shaped like `manage_codex`
that summarizes internally to a small `text` field — its working notes live
under `workspace/logs/codex_ops/<op_id>/` (or the corresponding adapter-owned
operation directory). The terminal result carries the operation's `log_dir`
when follow-up inspection is needed.

## Add an Action

### 1. Pick the action ID and location

Use a stable, lowercase action id such as `write_file`, `read_dir`, or
`web_fetch`. Put shipped runtime actions under `actions/core/<id>/`. Any other
directory under `actions/` works exactly the same way — there is nothing to
register — as long as the id does not collide with an existing action.

The directory must contain `action.json`. Subprocess actions also contain
`run.sh`. Runtime-function actions do not need `run.sh`.

### 2. Write `action.json`

Every enabled action contract needs these fields:

```json
{
  "id": "my_action",
  "description": "Do one clear thing.",
  "outside_world": true,
  "args_schema": {
    "type": "object",
    "required": ["name"],
    "additionalProperties": false,
    "properties": {
      "name": {
        "type": "string",
        "description": "Name to process."
      }
    }
  },
  "exec": ["bash", "run.sh"],
  "timeout_ms": 30000
}
```

Keep the schema small and explicit. Use `additionalProperties: false` unless
the action intentionally accepts open-ended JSON. Descriptions are model-facing,
so they should explain what each argument means without mentioning runtime
implementation details.

### 3. Choose an execution form

For a subprocess action:

- Set `exec` to `["bash", "run.sh"]`.
- Make `run.sh` executable.
- Read one request JSON object from stdin.
- Return one JSON object on stdout containing `result` and `error`.
- Exit `0` for success and nonzero for failure.
- Do not emit durable runtime events from the script. `events` is reserved and
  ignored for truth today.

For a runtime-function action:

- Set `exec` to `["runtime", "fc_name()"]`.
- Register the stable manifest-facing function name in the compiled runtime
  action function table in `runtime/action_runtime.c`.
- Keep the function name stable even if the internal C function changes later.
- Do not add subprocess-only files or artifact assumptions.

### 4. Add the action to agent allowlists

Add the action id to each `floops/<floop>/agents/<id>/agent.json` `actions`
array that should be allowed to see and call it. The registry only says the
action exists; explicit entries control bootstrap visibility and permission.
Agents deliberately configured with `all_actions` receive it in the expanded
catalog. A loaded action outside the resolved mask is rejected
as `not_allowed`.

Do not hand-document the new tool in `prompt.md`. LLM tool docs are generated
from `action.json` and the agent allowlist.

### 5. Store any secrets it needs

If the action reads credentials, store them under its namespace and put the
exact commands in the action's own `SETUP_<NAME>.md` so an operator can
copy-paste them:

```bash
printf '%s' '<value>' | ./bin/fclaw auth set-stdin -h action:my_action:api_key
```

The action reads `$API_KEY`. See [Secrets](#secrets) for the scoping rules.

### 6. Validate it

Run the direct subprocess smoke for `run.sh` actions:

```bash
printf '%s\n' \
  '{"request_id":"x","action":"my_action","args":{"name":"Ada"},"run_id":"manual"}' \
  | ./actions/mine/my_action/run.sh
```

Then run registry and end-to-end checks:

```bash
./bin/fclaw actions -h
make test
```

## Managed-operation contract

An action that runs detached external work declares it in
`action.json`:

```json
{ "contract": "managed_operation" }
```

Every managed-operation action implements the two common bounded ops through
its normal args:
`start` returns `{"status":"running","handle":"..."}` (or
`finished` immediately), and `poll` with the handle returns
`running` or `{"status":"finished","handle":"...","text":"..."}`.
Delegates may expose additional schema ops such as `reply`, `abort`, or
`status`; those are adapter features layered on the same start/poll contract.
`handle` is adapter-owned and opaque; `text` is opaque result prose.
The kernel's operation driver (enabled per floop by
`poll_interval_ms`) records, polls, and publishes results for any
contract-declaring action without knowing its id — see
[the floofclaw floop](floofclaw_floop.md). `manage_codex` and `manage_claude`
implement it; the fake adapters in the acceptance tests prove the
handler is swappable by config alone.

### When to use it: the "LLM needs a result to answer" pattern

Plain actions run after the LLM's answer turn; their return payload
never becomes part of a subsequent LLM prompt. So any time a user's
question requires data the LLM does not have in-context — current
prices, a URL's live contents, a database lookup, a computed value
the runtime can't precompute cheaply — the answer is a
managed-operation action, not an engine change to prefetch that
data into the input.

The two-turn flow:

1. LLM sees the question and calls the action with `op:"start"`. A floop may
   separately acknowledge long background work; OpenClaw instead reserves its
   final `message` for the completed answer or a real block.
2. The action fetches / computes / whatever, returns
   `{"status":"finished","handle":"<id>","text":"<summary>"}`.
   (If the work is fast — sub-second — this happens on the start
   call itself; if it's slow, start returns `running` and the
   kernel polls until finished.)
3. On the LLM's next turn, an `operation_result` envelope carries
   the `text` field. The LLM composes the real answer using it.

`web_read` and the bounded file/HTTP actions are synchronous examples that
can return `finished` from `start`. `manage_codex` is the canonical async
example: real running
processes with poll cycles.

`manage_codex` starts in FloofClaw's runtime workspace unless the action call
explicitly sets `"root":"project"`. Deployments may opt into that second
root with:

```json
{
  "actions": {
    "manage_codex": {
      "allowed_project_root": "/absolute/app-private/projects"
    }
  }
}
```

The configured directory is canonicalized once when the action registry
loads. For `root:"project"`, `cwd` remains relative and must resolve to an
existing directory at or beneath that root after symlink resolution. Missing
configuration, absolute paths, empty/dot-dot components, sibling-prefix
tricks, and symlink escapes fail before Codex launches. Operation prompts,
stdout, stderr, exit status, final text, and state remain under
`workspace/logs/codex_ops/`; project files do not move into the clearable
runtime workspace. Existing calls that omit `root` retain workspace-only
behavior.

**Prefer this over engine surgery.** If a feature is tempting you
to add a periodic tick that stashes data into the LLM's input
projection, or to hard-code a lookup table into a C reducer — stop
and write a managed-operation action first. The two-turn UX cost
is real but small, and the surface stays entirely in config +
scripts. The engine-vs-config principle bites hardest exactly
here, because prefetch/reducer patches feel small until three of
them stack.
