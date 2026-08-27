# Provider setup

**Preconditions:** build with libcurl support (`make -j4`) and choose the floop
you want to run. Keep keys in the auth store with `auth set-stdin`, or in the
documented environment variable; never put a key in JSON or a command argument.

## How resolution works

Every LLM call follows one visible chain:

```text
floops/<name>/agents/<agent>/agent.json  model.ref
    -> config/model_profiles.json        profile id, provider id, model id
    -> config/llm_registry.json          transport kind, URL, auth route
    -> auth store or named environment variable
```

Startup resolves that chain before accepting work. A missing profile,
provider, or key stops the command and prints its fix. The provider `kind`, not
the documentary `protocol` field, selects the wire format.

To try any recipe with the one-agent starter, change the `hello` profile in
`config/model_profiles.json`, then run:

```bash
./bin/fclaw run -h --text "Say hello in one sentence"
```

For a new profile instead, add it to `config/model_profiles.json` and change
`model.ref` in `floops/hello/agents/hello/agent.json` to its id. That is the
two-file switch for `hello`; for a multi-agent floop, update each LLM agent
that should share the new profile.

## Gemini

To use Gemini, store the key without putting it in shell history:

```bash
./bin/fclaw auth set-stdin -h gemini_key
```

Alternatively set `GEMINI_API_KEY` in the environment that starts FloofClaw.
The committed `chat`, `responses`, and `floofclaw_manager` profiles already
use the `gemini_key` provider. A minimal profile is:

```json
{
  "id": "my_gemini",
  "provider": "gemini_key",
  "model": "gemini-2.5-flash-lite",
  "max_output_tokens": 4096
}
```

The provider's optional local limits live in `config/llm_registry.json`:

```json
"limits": { "rpm": 15, "daily": 500, "daily_usd": 5.00 }
```

Omit a limit for no local cap; `0` deliberately blocks calls. These are local
guardrails, not a claim about the account's server-side quota. `daily_usd`
uses the dated USD-per-million-token assumptions in `config/pricing.json`.
Before dispatch, the runtime reserves the request's configured maximum output
plus a deliberately high text-input estimate; the reservation survives a
crash and resets on the next local day. An unlisted provider/model—or media,
whose provider-specific billing is not token-complete—fails closed while a
dollar cap is active. The raw provider artifact names that rejection as
`provider_budget_exceeded`; no network request is sent.

A model profile may also declare `"effort": "low"`, `"medium"`, or
`"high"`. Gemini 2.5 profiles map those values to tested numeric thinking
budgets (512, 1024, and 4096 tokens); newer Gemini models receive
`generationConfig.thinkingConfig.thinkingLevel`. A Gemini 2.5 Flash or
Flash-Lite profile without `effort` sends `thinkingBudget: 0` — thinking off,
the fast default every deployment had before 0.30.0; other Gemini models
without `effort` omit `thinkingConfig` and keep the provider default. See
[Executors and model profiles](../concepts/executors.md#model-effort).

## OpenAI

The official OpenAI chat-completions route uses the `openai_chat` transport
and a Bearer token:

```bash
./bin/fclaw auth set-stdin -h openai_key
```

Add this profile block to `config/model_profiles.json`, replacing the model
placeholder with an ID available to your account:

```json
{
  "id": "openai_chat",
  "provider": "openai_key",
  "model": "YOUR_OPENAI_MODEL_ID",
  "max_output_tokens": 4096
}
```

The chat transport sends `max_output_tokens` as OpenAI's
`max_completion_tokens`, sends profile `effort` as `reasoning_effort`, and
omits `temperature` so reasoning and non-reasoning chat models share one safe
shape. The registry also ships `openai_responses_key` for the OpenAI Responses
wire format; that transport sends `max_output_tokens` unchanged and sends
`effort` as `"reasoning": {"effort": ...}`. Profiles without `effort` omit
either effort field. Generic OpenAI-compatible servers remain on the separate
`openai_compat` kind, which keeps their widely supported `max_tokens` field
and `temperature: 0`; use that kind for vLLM, LM Studio, Ollama, LiteLLM, and
OpenRouter. Choose a model that accepts the selected effort control; the
provider remains the authority on supported values. See
[Executors and model profiles](../concepts/executors.md#model-effort).

## Anthropic

Anthropic uses the native Messages request shape, `x-api-key`, and the
required `anthropic-version` header:

```bash
./bin/fclaw auth set-stdin -h anthropic_key
```

The committed `anthropic_chat` profile is ready to select:

```json
{
  "id": "anthropic_chat",
  "provider": "anthropic",
  "model": "claude-sonnet-4-6",
  "max_output_tokens": 4096
}
```

Both `anthropic` and its compatibility alias `anthropic_key` use provider
kind `http`; in this runtime that kind specifically means the Anthropic
Messages shape.

Anthropic profiles send `effort` inside the native Messages
`output_config` object. Accepted values are `low`, `medium`, `high`, `xhigh`,
and `max`; unsupported values fail before dispatch. Omit it for the model
default. This control does not require an Anthropic beta header.

## OpenRouter

One OpenRouter key can access many models, including models OpenRouter marks
free. Store it or use the registry's `OPENROUTER_API_KEY` fallback:

```bash
./bin/fclaw auth set-stdin -h openrouter_key
```

The committed profiles include chat-completions and Responses examples. A
chat-completions profile has this shape:

```json
{
  "id": "openrouter_example",
  "provider": "openrouter_chat",
  "model": "deepseek/deepseek-v4-flash:free"
}
```

Model availability and free-tier limits belong to OpenRouter and can change;
replace the model id with one currently available to your account.

## Local OpenAI-compatible servers

Ollama, vLLM, LM Studio, and similar servers use the same profile shape. The
committed registry supplies these loopback examples:

| Server | Provider id | Default URL |
|---|---|---|
| Ollama | `local_ollama` | `http://127.0.0.1:11434/v1/chat/completions` |
| vLLM or generic server | `local_vllm` | `http://127.0.0.1:1234/v1/chat/completions` |
| LM Studio | `local_lmstudio` | `http://127.0.0.1:1234/v1/chat/completions` |

Start the server and load a model first, then add a profile using the exact
model id exposed by that server:

```json
{
  "id": "my_local_model",
  "provider": "local_ollama",
  "model": "qwen3:8b"
}
```

Loopback providers omit `auth_endpoint`, so no key is read or transmitted.
To use another port, duplicate a provider entry and change only its id and
loopback URL.

### LiteLLM on port 4000

The committed registry includes this provider for a LiteLLM proxy with no
virtual-key requirement:

```json
{
  "id": "local_litellm",
  "kind": "openai_compat",
  "url": "http://127.0.0.1:4000/v1/chat/completions"
}
```

Then use it directly from a profile:

```json
{
  "id": "litellm_chat",
  "provider": "local_litellm",
  "model": "YOUR_LITELLM_MODEL_ALIAS"
}
```

For a virtual key, select the committed `litellm_bearer` provider, which adds:

```json
"auth_endpoint": "openai_key",
"auth_env": "LITELLM_API_KEY"
```

The auth endpoint selects the existing Bearer-header route. The named
environment variable keeps the LiteLLM credential separate from an OpenAI
auth-store entry. Set only the environment variable in the gateway
environment; do not write its value into the registry.

## What is not implemented

The registry provider `openai_oauth` (kind `openai_codex_oauth`) and provider
`anthropic_oauth` (also kind `anthropic_oauth`) are marked `unsupported`
because those transport kinds are not implemented. They are not aliases for
API-key providers, and startup rejects a profile that resolves to either one.
Use `openai_key`, `anthropic`, or the managed Codex/Claude worker actions
instead.

## Verify and inspect

After a successful call, inspect the selected provider and wire artifacts:

```bash
RUN_ID=run_001  # replace with the generated run_id printed by fclaw
./bin/fclaw view -a "$RUN_ID"
ls "workspace/runs/$RUN_ID/provider_calls/"
```

Raw request and response bodies are retained there. Authentication headers
and key values are not written to those artifacts or to the event log.
