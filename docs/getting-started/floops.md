# Choosing a floop

**Preconditions:** none to read this page. A new runtime owner selects its
floop from `default_floop` in `config/floofclaw_config.json` or an explicit
`--floop <name>`. Once a gateway is running, it owns that choice: client
commands cannot switch its floop per message. Omit `--floop` to follow the
live owner. A matching explicit value asserts the expected floop; a conflict
fails before publication and tells you to run
`./bin/fclaw gateway reload -h --floop <name>`.

A floop is the on-disk definition of an agent loop: its ordered steps, agent
metadata, prompts, and allowed actions. Channels deliver events to a floop but
are not embedded in one, so every product floop can run from the terminal or a
configured channel.

The committed configuration explicitly selects `hello`. If
`config/floofclaw_config.json` is absent, the runtime's built-in fallback is
`floofclaw`; that fallback is not the normal fresh-clone default.

| Name | Purpose | Model needs | Channel needs | Good first floop? |
|---|---|---|---|---|
| `hello` | Minimal conversational first run with a deterministic friendly reply. | The `hello` profile, using the built-in mock by default. | None; terminal, gateway, or any configured channel. | **Yes.** It is the fresh-clone default and needs no account. |
| `companion` | Caring conversational companion with memory and durable-concern extraction; no work delegation. | The `responses` profile for three LLM agents. | None; terminal, gateway, or any configured channel. | No; choose it after configuring a real provider. |
| `floofclaw` | Durable background-work manager for chat, scheduled affair reviews, and managed-operation results. | `floofclaw_manager` for the chat/review/result managers, `floofclaw_work_manager` for the bound work controller, and `responses` for memory compaction. | None intrinsically; configure a channel for an always-on bot. | No; it is the full advanced workflow. |
| `openclaw` | One-main-agent coding/tool loop that can read, write, patch, run shell commands, and keep selecting actions across durable managed-operation result turns. | `default_gemini_flash` for `main_claw`, `responses` for memory compaction. | None intrinsically. | No; start here only when you want the coding-action surface. |
| `fast` | Native no-op fixture used by integration, performance, chaos, and thrash harnesses. | None. | None. | No; test harness only. |
| `thrash_cap` | Holds serialized work while the thrash harness proves the active-run cap. | None; one test script agent. | None. | No; test harness only. |

Only directories containing a `loop.json` are runnable floops. Test fixture
fallback directories under `loops/` and `agents/` are not additional shipped
products.

For a one-shot embedded run, `fclaw run -h --floop <name> --text ...` selects
the requested bundle only when no gateway is active. For an always-on
instance, select it on `fclaw gateway start -h`, `fclaw gateway run -h`, or
`fclaw gateway reload -h`; `fclaw -i -h` and other clients then follow the
gateway's active floop when the option is omitted. Passing that same floop
explicitly is an assertion, not a per-message switch; a different value is
rejected before the message reaches the bus.

To put `hello` on a real model, edit its single `hello` entry in
`config/model_profiles.json`: set `provider` to a configured provider such as
`gemini_key` and `model` to that provider's model ID. `companion` runs entirely
on `responses`; `floofclaw` and `openclaw` each name their own profiles per
agent, listed in the table above. Every floop's memory compactor uses
`responses`. The [provider guide](providers.md) gives complete recipes.
