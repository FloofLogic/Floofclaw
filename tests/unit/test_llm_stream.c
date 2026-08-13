#include "../../runtime/llm/stream.h"
#include "../test_support.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  char text[4096];
  size_t used;
  int calls;
} StreamCapture;

static int capture_delta(const char *text, size_t text_len, void *user) {
  StreamCapture *capture = (StreamCapture *)user;
  if (!capture || capture->used + text_len + 1 > sizeof(capture->text))
    return -1;
  memcpy(capture->text + capture->used, text, text_len);
  capture->used += text_len;
  capture->text[capture->used] = '\0';
  capture->calls++;
  return 0;
}

static int feed_one_byte_at_a_time(const char *json, StreamCapture *capture) {
  LlmMessageStream *stream =
      llm_message_stream_create(capture_delta, capture);
  int rc = 0;
  if (!stream) return -1;
  for (size_t i = 0; json[i]; ++i) {
    if (llm_message_stream_feed(stream, json + i, 1) != 0) {
      rc = -1;
      break;
    }
  }
  llm_message_stream_destroy(stream);
  return rc;
}

int streamed_message_extractor_handles_arbitrary_utf8_and_escape_splits(void) {
  StreamCapture capture = {{0}, 0, 0};
  const char *json =
      "{\"calls\":[{\"name\":\"message\",\"args\":{\"message\":"
      "\"Hi \\u263a \\ud83d\\udc3e caf\xc3\xa9\\nnext\"}}]}";
  int rc = 0;
  rc |= expect(feed_one_byte_at_a_time(json, &capture) == 0,
               "one-byte model frame splits decode safely");
  if (strcmp(capture.text,
             "Hi \xe2\x98\xba \xf0\x9f\x90\xbe caf\xc3\xa9\nnext") != 0)
    fprintf(stderr, "captured streamed text: [%s]\n", capture.text);
  rc |= expect(strcmp(capture.text,
                      "Hi \xe2\x98\xba \xf0\x9f\x90\xbe caf\xc3\xa9\nnext") == 0,
               "decoded deltas exactly reconstruct the message argument");
  rc |= expect(capture.calls > 1,
               "incremental extractor emits before the final envelope closes");
  return rc;
}

int streamed_message_extractor_never_emits_tool_or_envelope_fields(void) {
  StreamCapture capture = {{0}, 0, 0};
  const char *json =
      "{\"reasoning\":\"private\",\"calls\":["
      "{\"name\":\"write_file\",\"args\":{\"message\":\"secret\","
      "\"path\":\"leak.txt\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"public\"}}]}";
  int rc = 0;
  rc |= expect(feed_one_byte_at_a_time(json, &capture) == 0,
               "mixed tool/message envelope parses");
  rc |= expect(strcmp(capture.text, "public") == 0,
               "only a direct message action argument is emitted");
  rc |= expect_no_substr(capture.text, "secret",
                         "tool argument message fields never leak");
  rc |= expect_no_substr(capture.text, "private",
                         "reasoning and envelope fields never leak");
  return rc;
}
