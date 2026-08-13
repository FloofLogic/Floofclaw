# Runtime Flow

Runtime Flow is a static loop-configuration explorer for the current FloofClaw
runtime.

It reads repo truth from:

- `config/floofclaw_config.json`
- `floops/*/loop.json`
- `floops/*/agents/*/agent.json`
- `floops/*/agents/*/prompt.md`
- `floops/*/agents/*/run.sh`
- every action directory discovered under `actions/`, such as `actions/common/*`
- `config/model_profiles.json`
- the tracked `config/llm_registry.json`
- `workspace/runs/run_*/` for optional run overlays

It does not inspect a live process, call the gateway, or mutate runtime state.
The highlighted default comes from `default_floop` in
`config/floofclaw_config.json`; the shipped neutral value is `hello`.

## Generate

From the repo root:

```bash
python3 app/runtime_flow/generate.py --repo-root . --out app/runtime_flow/runtime_flow.json --no-runs
```

Public source releases omit generated `runtime_flow.json` snapshots. Generate
one before serving the static page; `scripts/up.sh` does this automatically.
Use the command without `--no-runs` only when you specifically want local
`workspace/runs/run_*` overlays in the generated file.

## Serve

```bash
cd app/runtime_flow
python3 -m http.server 8002
```

Open:

```text
http://127.0.0.1:8002/
```

## What it shows

- all floop profiles under `floops/`
- the configured default floop, highlighted in the loop list
- each loop phase in top-to-bottom execution order
- phase executor strategy: `llm`, `script`, `builtin`, or `action`
- agent prompts and `agent.json` executor/action metadata
- model/provider resolution for LLM-backed phases
- action-runner behavior and emitted event types
- source refs for runtime rules that shape loop execution
- selectable run overlays showing actual processor inputs, agent call outputs, raw LLM requests/responses, and action call artifacts

## Run overlays

The run selector overlays recent `workspace/runs/run_*` artifacts on top of the configured loop.

For a selected phase, the detail panel shows observed runtime boundaries first:

- external user payload and resulting `user_message` event
- processor call input from `agent_outputs/<seq>_<phase>.input.json`
- raw provider request/response from `provider_calls/`
- agent call output from `agent_outputs/<seq>_<phase>.json`
- pending `action_request` input to `action_runner`
- action input/output/stderr from `action_calls/`
- final delivered message from the `message` `action_succeeded` event
- run-level analyzer findings for repeated equivalent action calls, duplicate
  rejections, whether processor task input was visible to the model without old
  global roots, and whether a later agent recovery pass happened after rejection

The configured phase shape remains below the observed run data.
