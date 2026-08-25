# Extensions

**Preconditions:** a built `bin/fclaw` (`make -j4`). Paths are relative to the
repository root.

FloofClaw has three extension surfaces you add to by copying a directory into
them, plus one you use without installing anything at all.

```text
floofclaw/
├── actions/      # copy action here; restart
├── adapters/     # copy native adapter here; make; restart
├── floops/       # copy floop here
└── runtime/      # does not know their names

fclaw bus publish --type   # typed ingress: nothing to install
scripts/mcp_sync.sh        # generation: a foreign catalog becomes actions
```

The invariant: **installing an extension never requires editing a registration
list, a core source file, or a build manifest.** There is no `registry.json`,
no factory function to append to, no `SOURCES` line to extend.

Extension-specific *configuration* may still be required to use one — a copied
Discord adapter cannot know your token, and `channels.discord` is where you
tell it. That is configuring a capability, not registering one.

## Three questions people conflate

| | question | answer |
| --- | --- | --- |
| **Discovery** | How does FloofClaw know an extension exists? | The directory tree. |
| **Linkage** | Compiled in, dynamically linked, or a separate process? | Adapters are statically linked; actions and floops remain files, and subprocess actions run out of process. Nothing uses `dlopen`. |
| **Lifecycle** | Can it be added while running? | No. Restart. |

Nothing is hot-loaded, and nothing needs to be. FloofClaw is not a dynamic
plugin host and will not become one: `dlopen` would cost the single static
binary, add a versioned C ABI and a platform-divergent loader across
macOS/Linux/Android, and put arbitrary code in-process past the
`outside_world` and per-agent allowlist gating that make actions safe.

## What is discovered when

Actions are data and scripts, so they are discovered at **runtime**: no
rebuild, just restart. Adapters are C, so they are discovered at **build
time** and statically linked into the same binary.

```text
action:          copy directory → restart          → fclaw actions -h
native adapter:  copy directory → make → restart   → fclaw adapters -h
floop:           copy directory → select and run   → fclaw floops -h
typed ingress:   gate a kind in a floop            → fclaw bus log -h --type <kind>
```

Only the adapter rebuilds. Then configure it if it needs configuring.

## Actions

`actions/` is scanned recursively at startup. Every directory holding a valid
`action.json` is loaded, at any depth.

- **`action.json`'s `id` is the action's identity.** The filesystem is
  discovery and location only — there is no second addressing system, and no
  qualified name. An `id` must be unique across everything under `actions/`;
  a collision fails startup naming both manifests, so an action is never
  silently shadowed by discovery order.
- **A `_` or `.` prefix unloads a directory.** `mv actions/github
  actions/_github` keeps it in the tree and out of the scan. This is also what
  keeps build tooling such as `actions/common/web_read/_pluck/` from being scanned.
- **Discovery is sorted**, so a collision reports the same two manifests on
  every machine.

Discovery happens once per process, at a defined lifecycle point, and produces
the validated catalog every other subsystem reads:

```text
filesystem → scan_actions_tree → RtActionRegistry → everything else
```

Nothing rescans per run, and nothing added later should: the filesystem is the
install surface, not a queryable database. Code that wants the action list
reads `RtScheduler.actions`.

See [Actions](actions.md) for the `action.json` contract and
[adding an action](../getting-started/adding-an-action.md) for the walkthrough.

### Generation: foreign tool catalogs

An action directory is plain data, so a directory of them can be *written* as
easily as installed by hand. That is the whole of FloofClaw's MCP support:
`scripts/mcp_sync.sh` asks each configured server for its `tools/list` and
writes one action per tool. See [MCP servers](../getting-started/mcp.md).

The point is what it avoids. The alternative — a real MCP client in the
binary — means a new executor, a protocol implementation, a connection
lifecycle, and a second gating surface parallel to the one actions already
have. Generation costs a script, and every guarantee is inherited rather than
rebuilt: allowlists, `force_disable`, `outside_world`, timeouts, artifacts,
and the managed-operation result path all apply because the output *is* an
action. The binary gains one wrapper verb, `fclaw mcp sync`, and no protocol:
a regression test fails if a JSON-RPC method name, transport, or tool handling
appears anywhere in `runtime/`, or if MCP leaks past that one wrapper file.

The pattern generalises to any foreign catalog you can enumerate — an OpenAPI
document, a plugin manifest, an internal tool registry. What makes it work is
the rule the whole surface is built on:

- **Generation is explicit.** Sync runs when an operator runs it, never at
  startup. Between syncs the on-disk catalog is the truth, so a deployment
  cannot change shape because a remote server did.
- **Generated output is not source.** It is git-ignored and rewritten from
  scratch; the shared call-time bridge is the only committed piece.
- **What comes back is untrusted text.** A foreign description or schema ends
  up in a prompt catalog. Names are validated against a grammar before they
  reach the filesystem, and the ordinary gates still stand between a
  generated action and anything it can do.

## Adapters

An adapter is a **self-contained source directory** under `adapters/`,
statically linked into the same binary. A *channel* is the user-facing
concept — the conversation surface, and the config key. An *adapter* is the
implementation concept: what connects one.

The whole contract is in `runtime/gateway/adapter.h`:

```c
typedef struct {
  const char       *name;    /* also the channels.<name> config key */
  FcReactorModule *(*create)(struct RtScheduler *sched, int *startup_error);
  void             (*destroy)(FcReactorModule *module);
} FcAdapter;
```

`FcAdapter` describes how an adapter is *integrated*. `FcReactorModule`
([gateway reactor](gateway-reactor.md)) describes how the resulting thing
*participates* in the reactor loop, and holds the real callbacks. The two stay
separate deliberately; `FcAdapter` does not wrap the reactor vtable.

`create` returns `NULL` when the adapter is disabled or unconfigured — that is
the normal "nothing to register" path, not an error — and sets
`*startup_error` when it is enabled but cannot start. An adapter needing a
build dependency the binary lacks (OpenSSL, libcurl) reports that itself
rather than disappearing silently.

The build discovers the directories under `adapters/` and generates
`build/adapter_registry.gen.c`: the extern declarations and the
`fc_adapters[]` array, sorted by name. Nobody edits it. The gateway loops over
that array, so no adapter is named anywhere in the engine or the Makefile.

**The contract enforces itself through the linker.** The generated registry
references `fc_adapter_<directory>`, so a directory that fails to export its
descriptor fails the build:

```text
Undefined symbols for architecture arm64:
  "_fc_adapter_probe", referenced from:
      _fc_adapters in adapter_registry.o
```

That is adequate. Nothing translates it into a friendlier message.

See [adding an adapter](../getting-started/adding-an-adapter.md).

## Floops

Floops need nothing architectural: `floops/<name>/` holds `loop.json` plus the
agents that loop uses. Copy the directory, select it with `--floop <name>` or
`default_floop` in config. See [floops](floops.md).

## Typed ingress

The fourth surface installs nothing. A local producer — a cron job, a git
hook, a build wrapper, an MQTT bridge, an app on the same machine — publishes
a product-defined fact straight onto the bus:

```bash
printf '%s' '{"scope":"installed_applications"}' |
  ./bin/fclaw bus publish -a --type device_scan_requested \
    --channel local_product --context-id device --payload-stdin
```

A floop opts in by gating a step on that exact kind
(`"gate": "event_kind:device_scan_requested"`); the handler receives the kind
and the payload unchanged. Nothing is registered, nothing is rebuilt, and the
kernel keeps no list of product event names — it only enforces a token
grammar, the runtime-owned namespaces, and the payload bound.

Use it when the thing you observed is a **fact**, not a conversation. Write a
channel adapter instead when you are connecting a human conversation surface,
and write an action when you want FloofClaw to *cause* an effect rather than
be told about one.

See [`fclaw bus publish --type`](../reference/cli.md#--type-typed-event-ingress)
for the full flag, bound, and exit-code contract, and
[gating a custom event kind](floops.md#gating-a-custom-event-kind) for the
floop side.

## Seeing what a deployment has

The gateway prints its adapters at startup, including the ones that are off:

```text
gateway: adapters discord=off irc=on ws=off
```

And each surface lists the same way:

```console
$ fclaw actions -h     # every discovered action, its area, and its manifest
$ fclaw adapters -h    # every built adapter, whether enabled, and its config key
$ fclaw floops -h      # every floop directory holding a loop.json
```
