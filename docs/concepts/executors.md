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

## Model profiles

LLM agents reference an entry in `config/model_profiles.json`. The profile
loader currently consumes `id`, `provider`, `model`, and optional `effort`.
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
`"effort"` (for example `"low"`, `"medium"`, `"high"`). The current Gemini
transport renders it as `generationConfig.thinkingConfig.thinkingLevel`;
the other current transports load the field but do not put it on the wire.
The engine does not whitelist Gemini values: the provider is the authority,
and an invalid value or unsupported model fails loudly at the API with
request artifacts on disk. A Gemini profile without `effort` uses
`thinkingBudget: 0`. Only set `effort` when the selected provider/model
transport actually supports it.
