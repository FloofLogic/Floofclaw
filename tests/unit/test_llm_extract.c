#include "test_support.h"

#include "../../runtime/llm/extract.h"

#include <string.h>

/* Claude Opus 5 / Sonnet 5 think by default and lead with a thinking
 * block whose text is empty under the default display; the answer is the
 * first text block, wherever it sits. */
int anthropic_text_follows_a_thinking_block(void) {
  char out[256] = "";
  int rc = 0;
  const char *body =
      "{\"id\":\"msg_01\",\"type\":\"message\",\"role\":\"assistant\","
      "\"model\":\"claude-opus-5\",\"content\":["
      "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"sig\"},"
      "{\"type\":\"text\",\"text\":\"{\\\"calls\\\":[]}\"}],"
      "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":10,\"output_tokens\":4}}";
  rc |= expect(llm_extract_anthropic_text(body, out, sizeof(out)) == 0,
               "a text block after a thinking block is extracted");
  rc |= expect(strcmp(out, "{\"calls\":[]}") == 0,
               "the extracted text is the text block, not the thinking block");
  return rc;
}

/* The pre-5 shape (Sonnet 4.6 without thinking) still extracts. */
int anthropic_first_block_text_still_extracts(void) {
  char out[64] = "";
  int rc = 0;
  const char *body = "{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],"
                     "\"stop_reason\":\"end_turn\"}";
  rc |= expect(llm_extract_anthropic_text(body, out, sizeof(out)) == 0 &&
                   strcmp(out, "hi") == 0,
               "a leading text block extracts as before");
  return rc;
}

/* A body with no text block is a refusal or a tool turn: fail loudly and
 * name the provider's stop reason rather than hand back an empty string. */
int anthropic_without_a_text_block_fails_with_its_stop_reason(void) {
  char out[64] = "untouched";
  char reason[32] = "";
  int rc = 0;
  const char *body = "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"}],"
                     "\"stop_reason\":\"refusal\","
                     "\"stop_details\":{\"type\":\"refusal\",\"category\":\"cyber\"}}";
  rc |= expect(llm_extract_anthropic_text(body, out, sizeof(out)) != 0,
               "a thinking-only body does not extract");
  llm_extract_stop_reason("http", body, reason, sizeof(reason));
  rc |= expect(strcmp(reason, "refusal") == 0,
               "the stop reason names the refusal");
  return rc;
}

/* Gemini thinking models return thought parts ahead of the answer when
 * thoughts are included; the answer is the first non-thought part. */
int gemini_text_skips_thought_parts(void) {
  char out[64] = "";
  char reason[32] = "";
  int rc = 0;
  const char *body =
      "{\"candidates\":[{\"content\":{\"parts\":["
      "{\"thought\":true,\"text\":\"let me think\"},"
      "{\"text\":\"{\\\"calls\\\":[]}\"}],\"role\":\"model\"},"
      "\"finishReason\":\"STOP\"}]}";
  rc |= expect(llm_extract_gemini_text(body, out, sizeof(out)) == 0 &&
                   strcmp(out, "{\"calls\":[]}") == 0,
               "the first non-thought part is the model text");
  llm_extract_stop_reason("gemini_key", body, reason, sizeof(reason));
  rc |= expect(strcmp(reason, "STOP") == 0, "the Gemini finish reason is read");
  return rc;
}

/* The OpenAI shapes are unchanged; pin them so a refactor cannot drift. */
int openai_chat_and_responses_text_extract_as_before(void) {
  char out[64] = "";
  char reason[32] = "";
  int rc = 0;
  const char *chat = "{\"choices\":[{\"message\":{\"role\":\"assistant\","
                     "\"content\":\"chat-ok\"},\"finish_reason\":\"stop\"}]}";
  const char *responses =
      "{\"status\":\"completed\",\"output\":["
      "{\"type\":\"reasoning\",\"summary\":[]},"
      "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"responses-ok\"}]}]}";
  rc |= expect(llm_extract_openai_compat_text(chat, out, sizeof(out)) == 0 &&
                   strcmp(out, "chat-ok") == 0,
               "chat completions content extracts");
  llm_extract_stop_reason("openai_chat", chat, reason, sizeof(reason));
  rc |= expect(strcmp(reason, "stop") == 0, "the chat finish reason is read");
  rc |= expect(llm_extract_openai_responses_text(responses, out, sizeof(out)) == 0 &&
                   strcmp(out, "responses-ok") == 0,
               "the Responses message item after a reasoning item extracts");
  llm_extract_stop_reason("openai_responses", responses, reason, sizeof(reason));
  rc |= expect(strcmp(reason, "completed") == 0, "the Responses status is read");
  return rc;
}
