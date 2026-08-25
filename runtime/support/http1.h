#ifndef FCLAW_SUPPORT_HTTP1_H
#define FCLAW_SUPPORT_HTTP1_H

#include <stddef.h>

/* Minimal HTTP/1.1 request builder and response reader, shared by every
 * adapter that talks to an HTTPS API.
 *
 * Transport-free by design: the caller owns the socket, the TLS session,
 * and the receive buffer, because each adapter already drives those through
 * the reactor's collect_fds/on_fd loop. This module only turns bytes into a
 * status, headers, and a decoded body.
 *
 * Discord's REST path used to do this by hand — `atoi(rx + 9)` for the
 * status, no header parsing, no chunked decoding, and "the response is over
 * when the peer closes". That works only for Connection: close, which
 * forecloses long-poll APIs and quietly mis-reads any chunked reply. */

#define FC_HTTP1_ERROR_MAX 128

typedef enum {
  FC_HTTP1_NEED_MORE = 0, /* incomplete: read more bytes and call again */
  FC_HTTP1_DONE = 1,      /* one complete response is available */
  FC_HTTP1_BAD = -1       /* malformed, unsupported, or over a bound */
} FcHttp1Status;

typedef struct {
  int status;              /* status code from the status line */
  int keep_alive;          /* 1 when the connection may be reused */
  size_t body_len;         /* decoded body length in body_out */
  size_t consumed;         /* bytes of the input this response occupied */
  char error[FC_HTTP1_ERROR_MAX]; /* set when the result is FC_HTTP1_BAD */
} FcHttp1Response;

/* Build a request into `out`. `body` may be NULL for a bodyless method;
 * Content-Length is emitted whenever body is non-NULL, including for an
 * empty body. `extra_headers`, when non-NULL, must already be CRLF
 * terminated per header. Returns 0, or -1 when the request does not fit. */
int fc_http1_build_request(char *out, size_t out_cap,
                           const char *method, const char *path,
                           const char *host, const char *user_agent,
                           const char *extra_headers,
                           const char *content_type, const char *body);

/* Parse one response from the front of `buf`. Handles both Content-Length
 * and chunked framing, and decodes the body into `body_out`.
 *
 * Returns FC_HTTP1_NEED_MORE while the response is incomplete, so the
 * caller reads more and calls again with the longer buffer. A response
 * whose body exceeds `body_cap`, or whose framing is malformed, is
 * FC_HTTP1_BAD with a reason in out->error — never a silent truncation.
 *
 * On FC_HTTP1_DONE, out->consumed says how many bytes of `buf` this
 * response used, so a caller reusing the connection can keep the tail. */
FcHttp1Status fc_http1_parse_response(const char *buf, size_t len,
                                      char *body_out, size_t body_cap,
                                      FcHttp1Response *out);

/* Case-insensitive header lookup over a complete response head. Returns 0
 * and copies the trimmed value on success, -1 when the header is absent or
 * does not fit. Exposed for callers that need a header the parser does not
 * interpret (Telegram's Retry-After, for example). */
int fc_http1_header(const char *buf, size_t len, const char *name,
                    char *out, size_t out_cap);

#endif
