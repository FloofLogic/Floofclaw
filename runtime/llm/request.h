#ifndef FCLAW_LLM_REQUEST_H
#define FCLAW_LLM_REQUEST_H

#include "llm.h"

typedef struct {
  char *data;
  size_t len;
  int owned;
} LlmRequestBody;

/* Build the Gemini generateContent request body. escaped_prompt must already
 * be JSON-escaped by the transport. Kept as a pure provider-boundary helper
 * so request-shape behavior can be unit tested without libcurl.
 *
 * Gemini 2.5 maps low/medium/high effort to a bounded thinkingBudget;
 * newer Gemini models receive thinkingLevel. An empty effort omits
 * thinkingConfig so the selected model keeps its provider default. */
int llm_build_gemini_body(const LlmProfile *profile,
                          const char *escaped_prompt,
                          char *body, size_t body_len);

/* Validate the transport-neutral media list against the selected wire
 * protocol. This is deliberately model-agnostic: the provider remains the
 * authority on model capabilities, while MIME types that have no valid wire
 * representation fail before HTTP instead of being silently dropped. */
int llm_request_validate(const char *provider_kind, const LlmRequest *request,
                         char *err, size_t err_len);

/* Serialize one provider request with an exact byte length. Text-only calls
 * reuse caller scratch and allocate nothing; media calls own one exact-sized
 * dynamic JSON body, released by llm_request_body_free. The explicit byte
 * length lets HTTP post the exact body today and permits a later streaming
 * response transport to evolve independently. */
int llm_request_body_build(const LlmProfile *profile,
                           const char *provider_kind,
                           const LlmRequest *request,
                           char *text_scratch, size_t text_scratch_len,
                           LlmRequestBody *out,
                           char *err, size_t err_len);
void llm_request_body_free(LlmRequestBody *body);

#endif
