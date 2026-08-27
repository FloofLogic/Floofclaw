#include "../../runtime/llm/request.h"
#include "../test_support.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Gemini 2.5 uses numeric thinking budgets while Gemini 3 uses levels. */
int gemini_body_maps_effort_to_thinking_level(void) {
  LlmProfile profile;
  char body[2048];
  int rc = 0;

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.model, sizeof(profile.model), "gemini-3-flash-preview");

  snprintf(profile.effort, sizeof(profile.effort), "medium");
  rc |= expect(llm_build_gemini_body(&profile, "hi", body, sizeof(body)) == 0,
               "builder succeeds with effort set");
  rc |= expect_substr(body, "\"thinkingLevel\":\"medium\"",
                      "effort renders as thinkingLevel");
  rc |= expect_substr(body, "\"maxOutputTokens\":4096",
                      "Gemini receives the default output-token cap");
  rc |= expect_no_substr(body, "thinkingBudget",
                         "no legacy budget when effort is set");

  snprintf(profile.effort, sizeof(profile.effort), "thinking");
  rc |= expect(llm_build_gemini_body(&profile, "hi", body, sizeof(body)) == 0,
               "builder passes unrecognized effort values through");
  rc |= expect_substr(body, "\"thinkingLevel\":\"thinking\"",
                      "effort value is not whitelisted by the engine");

  snprintf(profile.model, sizeof(profile.model), "gemini-2.5-flash");
  snprintf(profile.effort, sizeof(profile.effort), "medium");
  rc |= expect(llm_build_gemini_body(&profile, "hi", body, sizeof(body)) == 0,
               "Gemini 2.5 builder accepts generic medium effort");
  rc |= expect_substr(body, "\"thinkingBudget\":1024",
                      "Gemini 2.5 medium effort maps to a valid budget");
  rc |= expect_no_substr(body, "thinkingLevel",
                         "Gemini 2.5 does not receive Gemini 3 levels");

  profile.effort[0] = '\0';
  rc |= expect(llm_build_gemini_body(&profile, "hi", body, sizeof(body)) == 0,
               "builder succeeds with effort absent");
  rc |= expect_substr(body, "\"thinkingBudget\":0",
                      "Gemini 2.5 Flash without effort keeps thinking off, as before 0.30.0");
  rc |= expect_no_substr(body, "thinkingLevel",
                         "no thinkingLevel without effort");

  snprintf(profile.model, sizeof(profile.model), "gemini-2.5-flash-lite");
  rc |= expect(llm_build_gemini_body(&profile, "hi", body, sizeof(body)) == 0 &&
                   strstr(body, "\"thinkingBudget\":0") != NULL,
               "Gemini 2.5 Flash-Lite without effort keeps thinking off");

  snprintf(profile.model, sizeof(profile.model), "gemini-2.5-pro");
  rc |= expect(llm_build_gemini_body(&profile, "hi", body, sizeof(body)) == 0 &&
                   strstr(body, "thinkingConfig") == NULL,
               "Gemini 2.5 Pro cannot disable thinking, so no budget is sent");

  snprintf(profile.model, sizeof(profile.model), "gemini-3-flash-preview");
  rc |= expect(llm_build_gemini_body(&profile, "hi", body, sizeof(body)) == 0 &&
                   strstr(body, "thinkingConfig") == NULL,
               "newer Gemini models without effort defer to the model default");

  {
    char tiny[32];
    rc |= expect(llm_build_gemini_body(&profile, "hi", tiny, sizeof(tiny)) != 0,
                 "undersized body buffer fails loudly");
  }
  return rc;
}

static int write_temp_bytes(const unsigned char *bytes, size_t len,
                            char *path, size_t path_len) {
  char pattern[] = "/tmp/fclaw-llm-media-XXXXXX";
  size_t off = 0U;
  int fd = mkstemp(pattern);
  if (fd < 0) return -1;
  while (off < len) {
    ssize_t n = write(fd, bytes + off, len - off);
    if (n <= 0) {
      close(fd);
      unlink(pattern);
      return -1;
    }
    off += (size_t)n;
  }
  if (close(fd) != 0 ||
      snprintf(path, path_len, "%s", pattern) >= (int)path_len) {
    unlink(pattern);
    return -1;
  }
  return 0;
}

static int write_sparse_temp(uint64_t len, char *path, size_t path_len) {
  char pattern[] = "/tmp/fclaw-llm-media-large-XXXXXX";
  int fd = mkstemp(pattern);
  if (fd < 0) return -1;
  if (len > (uint64_t)INT64_MAX ||
      ftruncate(fd, (off_t)len) != 0) {
    (void)close(fd);
    unlink(pattern);
    return -1;
  }
  if (close(fd) != 0 ||
      snprintf(path, path_len, "%s", pattern) >= (int)path_len) {
    unlink(pattern);
    return -1;
  }
  return 0;
}

static void init_media(LlmMedia *media, const char *id, const char *filename,
                       const char *media_type, const char *path,
                       uint64_t size_bytes) {
  memset(media, 0, sizeof(*media));
  snprintf(media->id, sizeof(media->id), "%s", id);
  snprintf(media->filename, sizeof(media->filename), "%s", filename);
  snprintf(media->media_type, sizeof(media->media_type), "%s", media_type);
  snprintf(media->path, sizeof(media->path), "%s", path);
  media->size_bytes = size_bytes;
}

int llm_text_request_shapes_are_exact_and_unallocated(void) {
  static const struct {
    const char *kind;
    const char *expected;
  } cases[] = {
    {
      "http",
      "{\"model\":\"model-x\",\"max_tokens\":4096,"
      "\"output_config\":{\"effort\":\"medium\"},"
      "\"messages\":[{\"role\":"
      "\"user\",\"content\":\"hi\\n\\\"ride\\\"\"}]}"
    },
    {
      "openai_chat",
      "{\"model\":\"model-x\",\"max_completion_tokens\":4096,"
      "\"reasoning_effort\":\"medium\",\"stream\":false,\"messages\":"
      "[{\"role\":\"user\",\"content\":\"hi\\n\\\"ride\\\"\"}]}"
    },
    {
      "openai_compat",
      "{\"model\":\"model-x\",\"max_tokens\":4096,\"reasoning_effort\":"
      "\"medium\",\"temperature\":0,\"stream\":false,\"messages\":"
      "[{\"role\":\"user\",\"content\":\"hi\\n\\\"ride\\\"\"}]}"
    },
    {
      "openai_responses",
      "{\"model\":\"model-x\",\"input\":\"hi\\n\\\"ride\\\"\",\"reasoning\":"
      "{\"effort\":\"medium\"},\"max_output_tokens\":4096,"
      "\"temperature\":0,\"store\":false,"
      "\"stream\":false}"
    }
  };
  LlmProfile profile;
  LlmRequest request = {"hi\n\"ride\"", NULL, 0U};
  char scratch[LLM_PROMPT_MAX + 1024];
  char legacy[2048];
  LlmRequestBody body;
  int rc = 0;

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.model, sizeof(profile.model), "model-x");
  snprintf(profile.effort, sizeof(profile.effort), "medium");
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    rc |= expect(llm_request_body_build(&profile, cases[i].kind, &request,
                                        scratch, sizeof(scratch), &body,
                                        NULL, 0U) == 0,
                 "text request body builds");
    rc |= expect(body.data == scratch && !body.owned,
                 "text request reuses caller scratch without allocation");
    rc |= expect(strcmp(body.data, cases[i].expected) == 0,
                 "text request bytes match the provider contract");
    rc |= expect(body.len == strlen(cases[i].expected),
                 "text request exposes exact byte length");
    llm_request_body_free(&body);
  }
  rc |= expect(llm_build_gemini_body(&profile, "hi\\n\\\"ride\\\"",
                                      legacy, sizeof(legacy)) == 0,
               "legacy Gemini builder produces comparison body");
  rc |= expect(llm_request_body_build(&profile, "gemini_key", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "new Gemini text request builds");
  rc |= expect(body.data == scratch && !body.owned,
               "Gemini text request does not allocate");
  rc |= expect(strcmp(body.data, legacy) == 0,
               "Gemini text request is byte-identical to legacy builder");
  llm_request_body_free(&body);
  return rc;
}

int llm_media_request_shapes_map_without_omission(void) {
  const unsigned char bytes[] = {0x01U, 0x02U, 0x03U};
  LlmProfile profile;
  LlmMedia media;
  LlmRequest request;
  LlmRequestBody body;
  char scratch[LLM_PROMPT_MAX + 1024];
  char path[256];
  int rc = 0;

  if (write_temp_bytes(bytes, sizeof(bytes), path, sizeof(path)) != 0)
    return expect(0, "create media request fixture");
  memset(&profile, 0, sizeof(profile));
  snprintf(profile.model, sizeof(profile.model), "model-x");
  init_media(&media, "123", "salad.png", "image/png", path, sizeof(bytes));
  request.text = "count calories";
  request.media = &media;
  request.media_count = 1U;

  rc |= expect(llm_request_body_build(&profile, "gemini_key", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "Gemini image request builds");
  rc |= expect(body.owned && body.data != scratch,
               "media request owns an exact dynamic body");
  rc |= expect_substr(body.data,
                      "{\"inline_data\":{\"mime_type\":\"image/png\","
                      "\"data\":\"AQID\"}}",
                      "Gemini uses generic inline_data");
  rc |= expect_no_substr(body.data, path,
                         "provider body never exposes local media path");
  llm_request_body_free(&body);

  rc |= expect(llm_request_body_build(&profile, "http", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "Anthropic image request builds");
  rc |= expect_substr(body.data,
                      "{\"type\":\"image\",\"source\":{\"type\":\"base64\","
                      "\"media_type\":\"image/png\",\"data\":\"AQID\"}}",
                      "Anthropic uses an image source block");
  llm_request_body_free(&body);

  rc |= expect(llm_request_body_build(&profile, "openai_compat", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "OpenAI-compatible image request builds");
  rc |= expect_substr(body.data,
                      "{\"type\":\"image_url\",\"image_url\":{\"url\":"
                      "\"data:image/png;base64,AQID\"}}",
                      "OpenAI-compatible chat uses image_url");
  llm_request_body_free(&body);

  rc |= expect(llm_request_body_build(&profile, "openai_responses", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "OpenAI Responses image request builds");
  rc |= expect_substr(body.data,
                      "{\"type\":\"input_image\",\"image_url\":"
                      "\"data:image/png;base64,AQID\"}",
                      "OpenAI Responses uses input_image for images");
  llm_request_body_free(&body);

  snprintf(media.filename, sizeof(media.filename), "menu.pdf");
  snprintf(media.media_type, sizeof(media.media_type), "application/pdf");
  rc |= expect(llm_request_body_build(&profile, "openai_responses", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "OpenAI Responses PDF request builds");
  rc |= expect_substr(body.data,
                      "{\"type\":\"input_file\",\"filename\":\"menu.pdf\","
                      "\"file_data\":\"data:application/pdf;base64,AQID\"}",
                      "OpenAI Responses uses input_file for documents");
  llm_request_body_free(&body);

  rc |= expect(llm_request_body_build(&profile, "http", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "Anthropic PDF request builds");
  rc |= expect_substr(body.data,
                      "{\"type\":\"document\",\"source\":{\"type\":\"base64\","
                      "\"media_type\":\"application/pdf\",\"data\":\"AQID\"}}",
                      "Anthropic uses document source for PDF");
  llm_request_body_free(&body);

  rc |= expect(llm_request_body_build(&profile, "gemini_key", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "Gemini PDF request builds");
  rc |= expect_substr(body.data, "\"mime_type\":\"application/pdf\"",
                      "Gemini preserves PDF MIME in inline_data");
  llm_request_body_free(&body);
  unlink(path);
  return rc;
}

/* The profile "effort" value passes through to OpenAI Responses as
 * reasoning.effort; an absent effort omits the reasoning object and keeps
 * the legacy request shape. The provider is the authority on valid values —
 * the builder does not maintain a whitelist, so "minimal" passes through. */
int openai_responses_maps_effort_to_reasoning(void) {
  const unsigned char bytes[] = {0x01U, 0x02U, 0x03U};
  LlmProfile profile;
  LlmMedia media;
  LlmRequest request = {"hi", NULL, 0U};
  LlmRequestBody body;
  char scratch[LLM_PROMPT_MAX + 1024];
  char path[256];
  int rc = 0;

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.model, sizeof(profile.model), "model-x");

  snprintf(profile.effort, sizeof(profile.effort), "minimal");
  rc |= expect(llm_request_body_build(&profile, "openai_responses", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "text request builds with effort set");
  rc |= expect(strcmp(body.data,
                      "{\"model\":\"model-x\",\"input\":\"hi\","
                      "\"reasoning\":{\"effort\":\"minimal\"},"
                      "\"max_output_tokens\":4096,"
                      "\"temperature\":0,\"store\":false,"
                      "\"stream\":false}") == 0,
               "effort renders as reasoning.effort without a whitelist");
  rc |= expect(body.data == scratch && !body.owned,
               "text request with effort reuses caller scratch");
  rc |= expect(body.len == strlen(body.data),
               "text request with effort exposes exact byte length");
  llm_request_body_free(&body);

  snprintf(profile.effort, sizeof(profile.effort), "hi\"gh");
  rc |= expect(llm_request_body_build(&profile, "openai_responses", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "request builds with an effort needing escapes");
  rc |= expect_substr(body.data, "\"reasoning\":{\"effort\":\"hi\\\"gh\"}",
                      "effort value is JSON-escaped");
  llm_request_body_free(&body);

  profile.effort[0] = '\0';
  rc |= expect(llm_request_body_build(&profile, "openai_responses", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "text request builds with effort absent");
  rc |= expect_no_substr(body.data, "reasoning",
                         "absent effort omits the reasoning object");
  rc |= expect(strcmp(body.data,
                      "{\"model\":\"model-x\",\"input\":\"hi\","
                      "\"max_output_tokens\":4096,"
                      "\"temperature\":0,\"store\":false,"
                      "\"stream\":false}") == 0,
               "absent effort keeps the legacy request shape");
  llm_request_body_free(&body);

  if (write_temp_bytes(bytes, sizeof(bytes), path, sizeof(path)) != 0)
    return rc | expect(0, "create effort media fixture");
  init_media(&media, "123", "salad.png", "image/png", path, sizeof(bytes));
  request.media = &media;
  request.media_count = 1U;

  snprintf(profile.effort, sizeof(profile.effort), "minimal");
  rc |= expect(llm_request_body_build(&profile, "openai_responses", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "media request builds with effort set");
  rc |= expect_substr(body.data, "\"reasoning\":{\"effort\":\"minimal\"}",
                      "media request preserves reasoning.effort");
  rc |= expect(body.owned && body.data != scratch,
               "media request with effort owns an exact dynamic body");
  rc |= expect(body.len == strlen(body.data),
               "media request with effort exposes exact byte length");
  llm_request_body_free(&body);

  profile.effort[0] = '\0';
  rc |= expect(llm_request_body_build(&profile, "openai_responses", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "media request builds with effort absent");
  rc |= expect_no_substr(body.data, "reasoning",
                         "media request without effort omits reasoning");
  llm_request_body_free(&body);
  unlink(path);
  return rc;
}

int anthropic_request_maps_profile_controls(void) {
  LlmProfile profile;
  LlmRequest request = {"hi", NULL, 0U};
  LlmRequestBody body;
  char scratch[LLM_PROMPT_MAX + 1024];
  char err[256];
  int rc = 0;

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.model, sizeof(profile.model), "claude-sonnet-4-6");
  snprintf(profile.effort, sizeof(profile.effort), "high");
  profile.max_output_tokens = 8192;
  rc |= expect(llm_request_body_build(&profile, "http", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "Anthropic request builds with profile controls");
  rc |= expect_substr(body.data, "\"max_tokens\":8192",
                      "Anthropic maps the output-token cap");
  rc |= expect(strcmp(body.data,
                      "{\"model\":\"claude-sonnet-4-6\","
                      "\"max_tokens\":8192,\"output_config\":{"
                      "\"effort\":\"high\"},\"messages\":[{\"role\":"
                      "\"user\",\"content\":\"hi\"}]}" ) == 0,
               "Anthropic request bytes match the Messages contract");
  llm_request_body_free(&body);

  snprintf(profile.effort, sizeof(profile.effort), "minimal");
  rc |= expect(llm_request_body_build(&profile, "http", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) != 0,
               "Anthropic rejects an unsupported effort before dispatch");
  rc |= expect_substr(err, "Anthropic effort",
                      "Anthropic effort error names the provider control");
  return rc;
}

int openai_compat_request_maps_profile_controls(void) {
  LlmProfile profile;
  LlmRequest request = {"hi", NULL, 0U};
  LlmRequestBody body;
  char scratch[LLM_PROMPT_MAX + 1024];
  char err[256];
  int rc = 0;

  memset(&profile, 0, sizeof(profile));
  snprintf(profile.model, sizeof(profile.model), "model-x");
  snprintf(profile.effort, sizeof(profile.effort), "low");
  profile.max_output_tokens = 2048;
  rc |= expect(llm_request_body_build(&profile, "openai_compat", &request,
                                      scratch, sizeof(scratch), &body,
                                      NULL, 0U) == 0,
               "OpenAI-compatible request builds with profile controls");
  rc |= expect_substr(body.data, "\"max_tokens\":2048",
                      "OpenAI chat maps the output-token cap");
  rc |= expect_substr(body.data, "\"reasoning_effort\":\"low\"",
                      "OpenAI-compatible chat passes reasoning_effort");
  llm_request_body_free(&body);

  rc |= expect(llm_request_body_build(&profile, "openai_chat", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) == 0,
               "official OpenAI chat request builds");
  rc |= expect(strcmp(body.data,
                      "{\"model\":\"model-x\","
                      "\"max_completion_tokens\":2048,"
                      "\"reasoning_effort\":\"low\",\"stream\":false,"
                      "\"messages\":[{\"role\":\"user\","
                      "\"content\":\"hi\"}]}" ) == 0,
               "official OpenAI chat bytes use the current token field");
  rc |= expect_no_substr(body.data, "temperature",
                         "official OpenAI chat omits reasoning-incompatible temperature");
  llm_request_body_free(&body);

  profile.max_output_tokens = LLM_MAX_OUTPUT_TOKENS + 1;
  rc |= expect(llm_request_body_build(&profile, "openai_chat", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) != 0,
               "serializer rejects an output cap beyond its response bound");
  rc |= expect_substr(err, "max_output_tokens",
                      "output-cap failure names the profile key");
  return rc;
}

int llm_media_request_rejects_unsupported_or_unverified_inputs(void) {
  const unsigned char bytes[] = {0x7fU};
  LlmProfile profile;
  LlmMedia media;
  LlmRequest request;
  LlmRequestBody body;
  char scratch[LLM_PROMPT_MAX + 1024];
  char path[256];
  char large_path[256];
  char err[512];
  int rc = 0;

  if (write_temp_bytes(bytes, sizeof(bytes), path, sizeof(path)) != 0)
    return expect(0, "create unsupported media fixture");
  memset(&profile, 0, sizeof(profile));
  snprintf(profile.model, sizeof(profile.model), "model-x");
  init_media(&media, "456", "sound.mp3", "audio/mpeg", path, sizeof(bytes));
  request.text = "identify sound";
  request.media = &media;
  request.media_count = 1U;

  rc |= expect(llm_request_body_build(&profile, "openai_compat", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) != 0,
               "OpenAI-compatible audio fails before serialization");
  rc |= expect_substr(err, "does not support MIME type",
                      "unsupported MIME failure is explicit");
  rc |= expect(llm_request_body_build(&profile, "http", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) != 0,
               "Anthropic audio fails before serialization");
  rc |= expect(llm_request_body_build(&profile, "gemini_key", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) == 0,
               "Gemini generic audio inline_data is supported");
  rc |= expect_substr(body.data, "\"mime_type\":\"audio/mpeg\"",
                      "Gemini preserves audio MIME");
  llm_request_body_free(&body);

  snprintf(media.filename, sizeof(media.filename), "clip.mp4");
  snprintf(media.media_type, sizeof(media.media_type), "video/mp4");
  rc |= expect(llm_request_body_build(&profile, "gemini_key", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) == 0,
               "Gemini generic video inline_data is supported");
  rc |= expect_substr(body.data, "\"mime_type\":\"video/mp4\"",
                      "Gemini preserves video MIME");
  llm_request_body_free(&body);

  media.size_bytes++;
  rc |= expect(llm_request_body_build(&profile, "gemini_key", &request,
                                      scratch, sizeof(scratch), &body,
                                      err, sizeof(err)) != 0,
               "declared-size mismatch is rejected");
  rc |= expect_substr(err, "verified regular file",
                      "size mismatch names verification failure");

  if (write_sparse_temp(15000000U, large_path,
                        sizeof(large_path)) != 0) {
    rc |= expect(0, "create sparse Gemini inline-limit fixture");
  } else {
    init_media(&media, "789", "large.png", "image/png",
               large_path, 15000000U);
    rc |= expect(llm_request_body_build(&profile, "gemini_key", &request,
                                        scratch, sizeof(scratch), &body,
                                        err, sizeof(err)) != 0,
                 "Gemini rejects an inevitably oversized inline body");
    rc |= expect_substr(err, "20 MB transport limit",
                        "Gemini limit failure names the Files API boundary");
    rc |= expect(body.data == NULL && !body.owned,
                 "Gemini size preflight fails before body allocation");
    unlink(large_path);
  }
  unlink(path);
  return rc;
}
