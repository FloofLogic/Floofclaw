/* Shared HTTP/1.1 response reader.
 *
 * Discord's REST path used to take the status with `atoi(rx + 9)`, parse no
 * headers, and treat "the peer closed" as "the response ended". That is
 * wrong for any chunked reply and forecloses keep-alive entirely, which a
 * long-poll API needs. These pin the framing rules the adapters depend on. */

#include "../test_support.h"

#include "../../runtime/support/http1.h"

#include <stdio.h>
#include <string.h>

#define BODY_CAP 4096

static FcHttp1Status parse(const char *raw, char *body, FcHttp1Response *r) {
  return fc_http1_parse_response(raw, strlen(raw), body, BODY_CAP, r);
}

int http1_reads_status_headers_and_both_body_framings(void) {
  char body[BODY_CAP];
  FcHttp1Response r;
  int rc = 0;

  /* Content-Length framing. */
  rc |= expect(parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
                     body, &r) == FC_HTTP1_DONE,
               "a Content-Length response completes");
  rc |= expect(r.status == 200, "status line is parsed, not guessed");
  rc |= expect(r.body_len == 5 && strcmp(body, "hello") == 0,
               "the body is exactly Content-Length bytes");
  rc |= expect(r.keep_alive == 1, "HTTP/1.1 defaults to keep-alive");

  /* Chunked framing, several chunks plus the terminating zero chunk. */
  rc |= expect(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n1\r\n \r\n5\r\nworld\r\n0\r\n\r\n",
                     body, &r) == FC_HTTP1_DONE,
               "a chunked response completes");
  rc |= expect(r.body_len == 11 && strcmp(body, "hello world") == 0,
               body[0] ? body : "chunks are joined in order");

  /* A chunk-size extension is legal and ignored. */
  rc |= expect(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5;name=value\r\nhello\r\n0\r\n\r\n",
                     body, &r) == FC_HTTP1_DONE &&
                   strcmp(body, "hello") == 0,
               "a chunk extension does not break decoding");

  /* Trailers after the zero chunk. */
  rc |= expect(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "2\r\nhi\r\n0\r\nX-Trailer: v\r\n\r\n",
                     body, &r) == FC_HTTP1_DONE &&
                   strcmp(body, "hi") == 0,
               "trailers are consumed, not treated as body");

  /* Header lookup is case-insensitive and trims the value. */
  {
    static const char raw[] =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Retry-After:   7  \r\nContent-Length: 0\r\n\r\n";
    char value[32];
    rc |= expect(parse(raw, body, &r) == FC_HTTP1_DONE && r.status == 429,
                 "a 429 is an ordinary parsed response");
    rc |= expect(fc_http1_header(raw, strlen(raw), "retry-after",
                                 value, sizeof(value)) == 0 &&
                     strcmp(value, "7") == 0,
                 "header lookup is case-insensitive and trimmed");
    rc |= expect(fc_http1_header(raw, strlen(raw), "x-absent",
                                 value, sizeof(value)) != 0,
                 "an absent header is reported, not invented");
  }

  /* Connection: close and HTTP/1.0 both end the connection. */
  rc |= expect(parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n", body, &r) == FC_HTTP1_DONE &&
                   r.keep_alive == 0,
               "Connection: close clears keep-alive");
  rc |= expect(parse("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
                     body, &r) == FC_HTTP1_DONE && r.keep_alive == 0,
               "HTTP/1.0 does not keep the connection");

  /* Bodyless statuses carry no body even without Content-Length. */
  rc |= expect(parse("HTTP/1.1 204 No Content\r\n\r\n", body, &r) ==
                       FC_HTTP1_DONE && r.body_len == 0,
               "204 completes with no body");

  /* consumed lets a keep-alive caller keep the pipelined tail. */
  {
    static const char two[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 500 X\r\n"
        "Content-Length: 0\r\n\r\n";
    rc |= expect(fc_http1_parse_response(two, strlen(two), body, BODY_CAP,
                                         &r) == FC_HTTP1_DONE,
                 "the first of two pipelined responses parses");
    rc |= expect(r.status == 200 && r.body_len == 2, "and is the right one");
    rc |= expect(fc_http1_parse_response(two + r.consumed,
                                         strlen(two) - r.consumed, body,
                                         BODY_CAP, &r) == FC_HTTP1_DONE &&
                     r.status == 500,
                 "consumed points exactly at the next response");
  }
  return rc;
}

int http1_asks_for_more_bytes_instead_of_guessing(void) {
  char body[BODY_CAP];
  FcHttp1Response r;
  int rc = 0;

  /* Every prefix of a complete response must be NEED_MORE — never a
   * truncated body reported as DONE. */
  {
    static const char raw[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
    size_t full = strlen(raw);
    for (size_t n = 1; n < full; ++n) {
      FcHttp1Status s = fc_http1_parse_response(raw, n, body, BODY_CAP, &r);
      if (s != FC_HTTP1_NEED_MORE) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "prefix of %zu bytes must need more, got %d", n, (int)s);
        rc |= expect(0, msg);
        break;
      }
    }
    rc |= expect(fc_http1_parse_response(raw, full, body, BODY_CAP, &r) ==
                     FC_HTTP1_DONE,
                 "the complete response then completes");
  }
  /* Same for chunked: a half-arrived chunk is not a short body. */
  {
    static const char raw[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "b\r\nhello world\r\n0\r\n\r\n";
    size_t full = strlen(raw);
    for (size_t n = 1; n < full; ++n) {
      if (fc_http1_parse_response(raw, n, body, BODY_CAP, &r) !=
          FC_HTTP1_NEED_MORE) {
        char msg[96];
        snprintf(msg, sizeof(msg), "chunked prefix of %zu bytes completed early", n);
        rc |= expect(0, msg);
        break;
      }
    }
    rc |= expect(fc_http1_parse_response(raw, full, body, BODY_CAP, &r) ==
                     FC_HTTP1_DONE && r.body_len == 11,
                 "the complete chunked response then completes");
  }
  return rc;
}

int http1_refuses_malformed_and_oversized_responses(void) {
  char body[64];
  FcHttp1Response r;
  int rc = 0;
  struct { const char *raw; const char *why; } cases[] = {
    { "OK 200\r\n\r\n", "not an HTTP status line" },
    { "HTTP/1.1 2XX OK\r\nContent-Length: 0\r\n\r\n", "non-numeric status" },
    { "HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n", "unsupported version" },
    { "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n", "non-numeric length" },
    { "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", "unsupported coding" },
    { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", "bad chunk size" },
    { NULL, NULL }
  };
  for (int i = 0; cases[i].raw; ++i) {
    FcHttp1Status s = fc_http1_parse_response(cases[i].raw,
                                              strlen(cases[i].raw),
                                              body, sizeof(body), &r);
    rc |= expect(s == FC_HTTP1_BAD, cases[i].why);
    rc |= expect(r.error[0] != '\0', "a refusal states its reason");
  }
  /* Over-cap bodies are refused, not truncated — both framings. */
  rc |= expect(fc_http1_parse_response(
                   "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n", 41,
                   body, sizeof(body), &r) == FC_HTTP1_BAD,
               "a Content-Length past the cap is refused up front");
  {
    char big[512];
    int n = snprintf(big, sizeof(big),
                     "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "100\r\n");
    rc |= expect(n > 0, "build the oversized chunk probe");
    rc |= expect(fc_http1_parse_response(big, (size_t)n, body, sizeof(body),
                                         &r) == FC_HTTP1_BAD,
                 "a chunk larger than the cap is refused");
  }
  return rc;
}

int http1_builds_requests_with_correct_framing(void) {
  char out[1024];
  int rc = 0;
  rc |= expect(fc_http1_build_request(out, sizeof(out), "GET",
                                      "/bot123/getUpdates?timeout=50",
                                      "api.telegram.org", "floofclaw/0.28.0",
                                      NULL, NULL, NULL) == 0,
               "build a bodyless GET");
  rc |= expect_substr(out, "GET /bot123/getUpdates?timeout=50 HTTP/1.1\r\n",
                      "request line is exact");
  rc |= expect_substr(out, "Host: api.telegram.org\r\n", "Host is set");
  rc |= expect_no_substr(out, "Content-Length",
                         "a bodyless request declares no length");
  rc |= expect(fc_http1_build_request(out, sizeof(out), "POST", "/x",
                                      "example.test", "ua", "X-A: b\r\n",
                                      "application/json", "{\"k\":1}") == 0,
               "build a POST with a body");
  rc |= expect_substr(out, "X-A: b\r\n", "extra headers are carried");
  rc |= expect_substr(out, "Content-Type: application/json\r\n",
                      "content type is set");
  rc |= expect_substr(out, "Content-Length: 7\r\n\r\n{\"k\":1}",
                      "length matches the body and the head ends once");
  rc |= expect(fc_http1_build_request(out, 32, "POST", "/x", "example.test",
                                      "ua", NULL, "application/json",
                                      "{\"k\":1}") != 0,
               "a request that does not fit is refused, not truncated");
  return rc;
}
