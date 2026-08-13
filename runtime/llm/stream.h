#ifndef FCLAW_LLM_STREAM_H
#define FCLAW_LLM_STREAM_H

#include <stddef.h>

typedef int (*LlmTextProgressSink)(const char *text, size_t text_len,
                                   void *user);

typedef struct LlmMessageStream LlmMessageStream;

/* Incrementally accepts the model's structured JSON text and emits only
 * decoded bytes belonging to a direct message result:
 *
 *   {"calls":[{"name":"message","args":{"message":"..."}}]}
 *
 * The legacy top-level {"message":"..."} shape is also safe. Incomplete
 * JSON escapes and UTF-8 sequences are retained until complete. The
 * decoder is bounded by LLM_RESP_MAX and never repairs or validates the
 * final model response; the ordinary normalization path still owns that. */
LlmMessageStream *llm_message_stream_create(LlmTextProgressSink sink,
                                            void *user);
void llm_message_stream_destroy(LlmMessageStream *stream);
int llm_message_stream_feed(LlmMessageStream *stream,
                            const char *bytes, size_t len);
size_t llm_message_stream_emitted(const LlmMessageStream *stream);

/* Sink used by an LLM child. Writes one bounded JSONL progress record to
 * the parent-owned FCLAW_PROGRESS_FD. Returns 1 when no progress pipe is
 * present, 0 on success, and -1 on a write/encoding error. */
int llm_progress_fd_sink(const char *text, size_t text_len, void *user);
int llm_progress_fd_available(void);

#endif
