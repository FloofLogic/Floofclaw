# MCP servers

FloofClaw uses your MCP servers by **generating actions from them**. A
`tools/list` catalog maps onto `action.json` nearly field for field, so
`scripts/mcp_sync.sh` writes one ordinary action directory per tool and
everything downstream already applies: agent allowlists, deployment
`force_disable`, the managed-operation contract, per-call artifacts.

The binary contains no MCP *protocol* code. `fclaw mcp sync` is a wrapper that
execs the script; no JSON-RPC method, transport, or tool handling exists in
`runtime/`, and a regression test keeps it that way.

**Preconditions:** a built `bin/fclaw` (`make -j4`), `jq`, and `curl` if any
of your servers use the HTTP transport.

## 1. Describe your servers

```bash
cp config/mcp.example.json config/mcp.json
```

The shape is the `mcpServers` map Claude Desktop and Claude Code use, so an
existing config can be pasted straight in:

```json
{
  "mcpServers": {
    "files": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
    },
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": { "GITHUB_PERSONAL_ACCESS_TOKEN": "${GITHUB_TOKEN}" }
    },
    "remote": { "url": "https://mcp.example.test/rpc" }
  }
}
```

`command` selects the stdio transport, `url` selects HTTP. Server names must
match `[A-Za-z0-9_.-]{1,48}`, because the name becomes part of an action id
and a directory name.

**stdio is the supported transport.** It is how nearly every MCP server in a
Claude Desktop or Claude Code config runs, and it is the path the bridge
fully implements and the suite proves end to end.

**`url` targets plain stateless JSON-RPC POST endpoints only.** The bridge
sends `initialize` and `tools/call` as two independent POSTs and expects a
bare JSON body back. A spec-compliant remote MCP server (Streamable HTTP)
issues an `Mcp-Session-Id` on initialize, requires it on every later
request, and may answer as SSE — the bridge speaks none of that, so such a
server will reject the call. Reach real remote servers through the stdio
transport with a proxy instead:

```json
"remote": {
  "command": "npx",
  "args": ["-y", "mcp-remote", "https://server.example.com/mcp"]
}
```

`mcp-remote` handles the session, SSE, and OAuth on its side of the pipe;
the bridge spawns it per call like any other stdio server. Native
Streamable-HTTP support is deliberately deferred until someone actually
needs an endpoint the proxy cannot serve.

`config/mcp.json` is git-ignored — it names commands and paths specific to
one machine. `config/mcp.example.json` is the template that ships.

## 2. Store credentials

An `env` value written as `${NAME}` means "take `NAME` from this action's
environment at call time". Never put the secret itself in `config/mcp.json`.

FloofClaw scopes action secrets to a single action id: the gateway injects
only `endpoint:action:<id>:*` into that action's environment, so one tool can
never read another's credential. A server with eight tools therefore needs
the value stored eight times, which is right and tedious. Do it in one
command **after** the first sync:

```bash
printf %s "$GITHUB_TOKEN" | ./bin/fclaw mcp secret github GITHUB_TOKEN
```

The value is read from stdin and handed to `fclaw auth set-stdin` on a pipe.
It never appears in argv or in the process list. Re-run it after a sync that
adds tools to that server.

## 3. Sync

```bash
./bin/fclaw mcp sync -h
```

`fclaw mcp sync` wraps `scripts/mcp_sync.sh` so the entry point is
discoverable and follows the CLI output contract; run either. Without `-h` it
returns one JSON object instead of operator text.

This asks each server for its catalog and rewrites `actions/mcp/`:

```text
  server github: 26 tool(s)
mcp-sync: 28 tool(s) from config/mcp.json
  + github__create_issue
  ~ github__search_code
  - github__legacy_thing
mcp-sync: restart the gateway to pick up the catalog
```

`+` added, `~` changed, `-` removed. `--dry-run` reports the same difference
without touching anything.

Sync is explicit and idempotent, and **never runs at gateway startup**.
Between syncs the on-disk catalog is the truth, exactly like every other
action — your deployment does not silently change shape because a remote
server did. A server that fails to answer keeps the actions it already has:
a transient outage must not delete your tool catalog. That case exits
nonzero and names the server.

Each tool becomes `actions/mcp/<server>__<tool>/`, holding an `action.json`
and a two-line `run.sh` that execs the shared bridge. Generated directories
are git-ignored; regenerate them rather than committing them.

## 4. Allowlist and restart

Generated actions obey the same gates as everything else. An agent whose
allowlist is `["all_actions"]` picks them up automatically; a narrower one
names them:

```json
"actions": ["message", "github__create_issue"]
```

To keep a generated tool installed but unusable, mask it in
`config/floofclaw_config.json`:

```json
"force_disable": ["github__delete_repository"]
```

Then restart:

```bash
./bin/fclaw gateway stop -a && ./bin/fclaw gateway start -a --floop openclaw
./bin/fclaw actions -h | grep github__
```

## How a call runs

Every generated tool is a managed operation. The agent calls it with
`op: "start"`, the run continues without blocking, and the tool's result
arrives later as an `operation_result` event that wakes the same agent.

`actions/mcp/_bridge.sh` is the only piece that speaks the protocol, and it
speaks it at call time only. Per call it sends `initialize`,
`notifications/initialized`, then `tools/call`, and matches the response by
JSON-RPC id.

- **Cold start per call.** The stdio server is spawned for the call and exits
  when its stdin closes. Long-lived supervised servers are a separate
  deferred project, not this feature.
- **Bounded.** A server that never answers fails that one call after
  `FCLAW_MCP_CALL_TIMEOUT_S` (default 55s, under the action's own 60s
  deadline) and is killed along with anything it spawned.
- **Failures are local.** `isError: true`, a JSON-RPC error, a dead command,
  or a server that answers nonsense fails that call with the message on
  stderr and an error object in the result. Nothing else in the gateway
  notices.
- **The full reply is kept.** The complete JSON-RPC response is written to the
  call's artifact; only a bounded 8 KiB text excerpt travels as the operation
  result.
- **`op` is FloofClaw's, not the tool's.** It is stripped before
  `tools/call`, so a tool never receives an argument it did not declare.

## What the generated manifest looks like

```json
{
  "id": "fix__echo",
  "description": "MCP tool echo from server fix. Echo the text back.",
  "outside_world": true,
  "args_schema": {
    "type": "object",
    "required": ["op", "text"],
    "additionalProperties": false,
    "properties": {
      "text": { "type": "string", "description": "Text to echo." },
      "op": { "type": "string", "enum": ["start"] }
    }
  },
  "exec": ["bash", "run.sh"],
  "timeout_ms": 60000,
  "contract": "managed_operation",
  "default_timeout_ms": 60000,
  "max_timeout_ms": 300000
}
```

The tool's own `inputSchema` is carried through verbatim — including keywords
FloofClaw does not itself interpret — with `op` added. Required fields are
enforced from that copied schema before the bridge ever runs.

The description is the server's own text, prefixed with the tool and server
name and truncated to the 512-byte manifest convention.

## Trust

A generated action is as trustworthy as the server it came from. The
descriptions and schemas are that server's text, and they end up in the
prompt catalog an agent reads.

Sync refuses server and tool names outside `[A-Za-z0-9_.-]{1,48}` rather than
letting them reach the filesystem, and every call is still gated by the
agent's allowlist, `outside_world`, `force_disable`, and the action deadline.
Read what a sync added before allowlisting it — `--dry-run` and the `+`/`~`
lines exist for that.

## Troubleshooting

| symptom | cause |
| --- | --- |
| `no tool catalog (skipped)` | the server never answered `tools/list`; run its command by hand |
| `did not answer within Ns` | the server hung; raise `FCLAW_MCP_CALL_TIMEOUT_S` or fix the server |
| `needs env ${X} but it is not set` | run `scripts/mcp_secret.sh <server> X`, then restart |
| a new tool is missing from `fclaw actions` | sync writes files; the gateway reads them at startup — restart |
| `mcp sync requires jq` | install `jq` |
| `the mcp scripts were not found` | run `fclaw` from the deployment root |

## See also

- [`fclaw mcp`](../reference/cli.md#fclaw-mcp-syncsecret) — the command reference
- [Actions](../concepts/actions.md) — the contract every generated action obeys
- [Extensions](../concepts/extensions.md) — generation as the pattern for foreign tool catalogs
