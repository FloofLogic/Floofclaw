#ifndef FCLAW_LLM_EXTRACT_H
#define FCLAW_LLM_EXTRACT_H

#include <stddef.h>

/* Pull the model's text out of a provider response body. Each extractor
 * returns 0 and writes the text on success, -1 when the body carries no
 * text block. They are provider-shape functions with no transport or
 * runtime dependency so the unit gate can pin them against canned bodies.
 *
 * Anthropic: content[] is walked for the first {"type":"text"} block. On
 * Claude Opus 5 and Sonnet 5 a response that thinks LEADS with a
 * {"type":"thinking"} block (empty text under the default display), so
 * content[0] is not where the answer lives. Gemini: candidates[0]'s parts
 * are walked for the first part that is not a thought. */
int llm_extract_anthropic_text(const char *response_json, char *out, size_t out_len);
int llm_extract_openai_compat_text(const char *response_json, char *out, size_t out_len);
int llm_extract_openai_responses_text(const char *response_json, char *out, size_t out_len);
int llm_extract_gemini_text(const char *response_json, char *out, size_t out_len);

/* The provider's own stop reason, for the failure line when no text was
 * extracted: Anthropic stop_reason, OpenAI choices[0].finish_reason,
 * Responses status, Gemini candidates[0].finishReason. Empty when absent. */
void llm_extract_stop_reason(const char *provider_kind, const char *response_json,
                             char *out, size_t out_len);

#endif
