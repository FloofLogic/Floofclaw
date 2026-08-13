# Adding an action

**Preconditions:** a built `bin/fclaw` (`make -j4`).

```text
copy directory → restart → fclaw actions -h
```

No rebuild: actions are data and scripts, discovered by a directory scan at
startup. Nothing else is edited — there is no registration list.

## Copy it in

```console
$ cp -R some-action actions/mine/some-action
$ fclaw actions -a | jq -e '.[] | select(.id == "some_action")'
```

Any depth works. `actions/mine/some-action/` and `actions/some-action/` are
both found; the first path segment is only a display grouping. The repository
tracks only `actions/common/` and `actions/core/`, so a local area such as
`actions/mine/` remains installation-specific unless you deliberately change
the ignore policy.

## Or write one

A directory holding `action.json`, plus `run.sh` for subprocess actions:

```text
actions/mine/greet/
├── action.json
└── run.sh
```

```json
{
  "id": "greet",
  "description": "Say hello to a name. Returns the greeting text.",
  "outside_world": false,
  "args_schema": {
    "type": "object",
    "required": ["name"],
    "additionalProperties": false,
    "properties": {
      "name": { "type": "string", "description": "Who to greet." }
    }
  },
  "exec": ["bash", "run.sh"],
  "timeout_ms": 5000
}
```

`id` is the action's identity and what the model calls. It must be unique
across everything under `actions/`. The directory name is where it lives, not
what it is called — they can differ.

Per-action calling guidance belongs in `description` and the `args_schema`
field descriptions, not in an agent prompt. See
[Actions](../concepts/actions.md) for the full contract: `outside_world`,
managed operations, `config`, credentials, and runtime intrinsics.

## Let an agent call it

An action is discovered but not offered until an agent lists it. In
`floops/<floop>/agents/<agent>/agent.json`:

```json
{ "actions": ["greet", "message"] }
```

Order there is product policy — it is the order the model sees. Use
`all_actions` to expand the whole catalog in place.

## Turn one off without deleting it

```console
$ mv actions/mine/greet actions/mine/_greet
```

A `_` or `.` prefix keeps a directory in the tree and out of the scan. That is
the entire disable mechanism; there is no config flag.

## When it does not show up

- **`fclaw actions -h` prints an error instead of a list.** The scan failed. The
  most common cause is two actions sharing an `id`; the message names both
  manifests.
- **The agent fails to load** with `agent X allows unknown action Y`. The id
  in `agent.json` does not match any discovered `id`. Check `fclaw actions -h`.
- **Nothing at all.** The directory name starts with `_` or `.`, or it holds
  no `action.json`.
