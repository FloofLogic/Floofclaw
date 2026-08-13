# Extensions

**Preconditions:** a built `bin/fclaw` (`make -j4`). Paths are relative to the
repository root.

FloofClaw has three extension surfaces, and you add to each one by copying a
directory into it.

```text
floofclaw/
├── actions/      # copy action here; restart
├── adapters/     # copy native adapter here; make; restart
├── floops/       # copy floop here
└── runtime/      # does not know their names
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
