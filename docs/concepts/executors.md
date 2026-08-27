# Executors and model profiles

An agent chooses one of three executors in `agent.json`:

- `native` runs a deterministic runtime-owned agent such as memory, workers,
  or the performance no-op.
- `llm` renders the agent prompt and its declared listen blocks, calls the
  referenced model profile, and normalizes the response to call intent.
- `script` runs the agent-local `run.sh` and captures its standard streams as
  run artifacts.

All three executors cross the same boundary: an agent proposes calls, while
the runtime owns event IDs, requests, jobs, lifecycle transitions, and event
append order. LLM and script agents must declare `listen`; native agents may
construct their input internally.

Only an `llm` agent gets a rejected decision back. When the normalizer refuses
its output, the phase re-runs as another provider call carrying the rejected
response and the reason, bounded by `repair_attempts` (default 2). A `script`
or `native` agent is deterministic, so a second identical turn buys nothing
and the rejection is terminal. See
[Floops — LLM output repair](floops.md#llm-output-repair).

## Processor input

Ordinary agents receive only their declared blocks:

- `event` — the envelope that triggered the run
- `memory` — bounded conversation summaries and recent messages
- `tasks` — active durable tasks
- `affairs` — durable concerns relevant to the context
- `usage` — measured agent/model and worker-operation usage

The normal output is `{"calls":[]}`. `conversational_payload_only`,
`affair_extraction_context_only`, and `memory_compaction_context_only` are
narrow input/output shapes retained for the companion and memory flows. They
do not grant authority over runtime-owned fields.

These three shapes get no action catalog, but they get the same projected
input every other agent gets: a prompt template that names `{{json}}` places
it, and one that does not gets an appended `=== model input ===` section.
Neither form can send the model an instruction with nothing to act on.

## Model profiles

LLM agents reference an entry in `config/model_profiles.json`. The profile
loader consumes `id`, `provider`, `model`, optional `effort`, and optional
`max_output_tokens`. The output limit defaults to 4096 and is bounded at
16384 so the requested token budget cannot outrun the fixed 64 KiB response
buffer (the transport separately enforces the actual byte cap).
The referenced provider-registry entry supplies transport kind, endpoint,
authentication, and request limits. Profile fields such as `protocol`,
`context_size`, and `prefer` are currently documentary and unread by the
runtime; do not infer dispatch or limits from them until a boundary contract
explicitly implements them.

## Actions

An agent's `actions` array resolves once through the runtime registry into a
numeric authorization mask and its exact bootstrap presentation order.
`all_actions` fills the mask and expands at its configured position to every
loaded action contract. The renderer does not deduplicate overlap with
explicit entries; configuration owns that mistake. `action_list` can still
list allowed ids and return one selected source manifest. The engine does not
rank or select actions, and the runner still rejects every request outside the
mask.

Put `{{tools}}` once in the stable prefix of an LLM prompt, before the changing
`{{json}}` input, to make provider prefix caching possible. The engine
preserves the catalog bytes and configured order; it does not promise that a
provider will return a cache hit.

Successful ordinary data-returning actions use the managed-operation
contract and re-enter a floop as an `operation_result`; fire-and-forget calls
finish without supplying same-run data to another agent. A controller with
`bind_task: "open_work"` is bound by the runtime to one exact same-context
task/revision. Its `work_consequence` gate receives the selected action's
correlated intermediate result and distinguishes it from terminal
`work_completed`/`work_blocked` outcomes. See
[Action result flow](action_result_flow.md).

## Model effort

Model profiles live in `config/model_profiles.json`. A profile may set
`"effort"` (for example `"low"`, `"medium"`, `"high"`). Anthropic Messages
sends it as `output_config.effort`; OpenAI chat as `reasoning_effort`; OpenAI
Responses as `reasoning.effort`. Anthropic accepts low/medium/high/xhigh/max
and rejects another value locally. Gemini 2.5 maps low/medium/high to numeric
thinking budgets of 512/1024/4096, while newer Gemini models receive
`thinkingLevel`. A Gemini 2.5 Flash/Flash-Lite profile without `effort`
sends `thinkingBudget: 0` (thinking off); other Gemini profiles without
`effort` omit `thinkingConfig` and use the model default. Other than the Gemini 2.5 mapping, the provider is the
authority on supported values and models; an invalid combination fails at the
API with request artifacts on disk. Official OpenAI chat maps the profile's
`max_output_tokens` to `max_completion_tokens` and omits `temperature`; the
generic `openai_compat` transport keeps `max_tokens` and `temperature: 0` for
local servers and OpenRouter. Only set `effort` when that selected
provider/model supports it.
