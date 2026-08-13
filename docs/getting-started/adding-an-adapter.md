# Adding an adapter

**Preconditions:** a C11 compiler, `make`, and a POSIX userland.

```text
copy directory → make → restart → fclaw adapters -h
```

The rebuild is the difference from an action: an adapter is C, statically
linked into the same single binary. Nothing is dynamically loaded, and nothing
else is edited — no registration list, no Makefile line, no `#include` in the
gateway.

## The contract

Create `adapters/<name>/` and export a `const FcAdapter fc_adapter_<name>`
matching the directory name. That is all of it.

```c
#include "../../runtime/gateway/adapter.h"

static FcReactorModule *matrix_create(struct RtScheduler *sched,
                                      int *startup_error) {
  (void)sched;                 /* ignore it unless you need the scheduler */
  if (startup_error) *startup_error = 0;
  if (!enabled_in_config()) return NULL;   /* not an error */
  return build_reactor_module();
}

static void matrix_destroy(FcReactorModule *m) { /* free your state */ }

const FcAdapter fc_adapter_matrix = {
  .name    = "matrix",
  .create  = matrix_create,
  .destroy = matrix_destroy,
};
```

Then:

```console
$ make -j4
$ fclaw adapters -h
adapter          enabled?   config key
discord          no         channels.discord
matrix           no         channels.matrix
```

The build discovered the directory, generated the registry, and linked it in.

## What `create` must do

- Return **`NULL`** when the adapter is disabled or unconfigured. This is the
  normal path and is not an error — the gateway simply registers nothing.
- Set **`*startup_error`** when the adapter is enabled but cannot start. That
  aborts gateway startup rather than running a bot with a silently missing
  channel.
- **Report your own missing build dependency.** If you need OpenSSL or libcurl
  and the binary lacks it, say so on stderr and set `*startup_error`. Do not
  vanish. See `adapters/irc/irc_adapter.h` for the shipped wording.

Configuration is read from `channels.<name>` in
`config/floofclaw_config.json`, where `<name>` is your descriptor's `name`.

## What the module must do

`create` returns an `FcReactorModule` — the reactor contract in
`runtime/gateway/reactor.h`: `init`, `collect_fds`, `on_fd`, `tick`,
`shutdown`, `next_deadline_ms`.

**No callback may block.** The reactor is a single-threaded poll loop, and a
blocking adapter stalls the whole gateway. Long work goes into a child
observed through the jobrunner.

Inbound: publish an ordinary `user_message` bus envelope, tagging `ref` with
the reply address. Outbound: tail `workspace/logs/deliveries.jsonl` for
deliveries matching your `module_id` and do the last-mile send. The runtime
never hands you a message — that is why the contract has no `send`. An agent
emitting `message` does not choose a channel; the delivery goes back where the
conversation came from.

`adapters/discord/` is the reference implementation: WSS plus REST, an outbox,
and reconnect backoff.

## If the symbol name is wrong

The generated registry references `fc_adapter_<directory>`, so a mismatch
fails the link naming exactly what is missing:

```text
Undefined symbols for architecture arm64:
  "_fc_adapter_matrix", referenced from:
      _fc_adapters in adapter_registry.o
```

Rename the exported descriptor to match the directory, or rename the
directory.

## Removing one

Delete the directory and rebuild. The registry regenerates from what is
actually on disk.

## What is deliberately not supported

`dlopen`, runtime plugin loading, adding an adapter without a restart, and
adapters as separate executables. See [Extensions](../concepts/extensions.md)
for why. A supervised child process that bridges a channel is a reasonable
thing to want and is tracked separately; it is not this.
