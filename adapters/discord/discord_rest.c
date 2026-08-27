/* Discord REST POST.
 *
 * Sends outbound chunked messages to /api/v10/channels/{id}/messages.
 * Holds the head DcOutbound in place while it runs the
 * TCP connect -> TLS handshake -> HTTP POST -> close state machine,
 * then pops it only after success or a permanent rejection. Transient
 * failures therefore cannot reorder or silently lose a delivery. */

#include "discord_internal.h"

#include "../../runtime/support/http1.h"
#include "../../runtime/support/net.h"
#include "../../runtime/version.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dc_close_rest(DcAdapter *a) {
  if (!a) return;
  if (a->rest_tls) { fc_tls_free(a->rest_tls); a->rest_tls = NULL; }
  if (a->rest_fd >= 0) { net_close(a->rest_fd); a->rest_fd = -1; }
  a->rest_state = DC_REST_IDLE;
  a->rest_want = 0;
  a->rest_tx_len = 0;
  a->rest_rx_len = 0;
  a->rest_read_started_ms = 0;
  a->rest_has_cur = 0;
}

static int dc_rest_commit_head(DcAdapter *a) {
  if (!a || a->out_count <= 0) return -1;
  a->out_head = (a->out_head + 1) % DC_OUT_QUEUE_MAX;
  a->out_count--;
  return 0;
}

static void dc_rest_drop_or_commit(DcAdapter *a);

static int dc_rest_build_request(DcAdapter *a) {
  char auth[9000];
  char path[256];
  int n;
  if (dc_build_auth_header(a, auth, sizeof(auth)) != 0) return -1;
  snprintf(path, sizeof(path), DC_API_PREFIX "/channels/%s/messages", a->rest_cur.channel_id);
  n = snprintf(a->rest_tx, sizeof(a->rest_tx),
               "POST %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "User-Agent: floofclaw/%s\r\n"
               "%s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: %zu\r\n"
               "Connection: close\r\n\r\n"
               "%s",
               path, DC_REST_HOST, FCLAW_VERSION, auth, strlen(a->rest_cur.payload), a->rest_cur.payload);
  dc_secure_zero(auth, sizeof(auth));
  if (n < 0 || (size_t)n >= sizeof(a->rest_tx)) return -1;
  a->rest_tx_len = (size_t)n;
  return 0;
}

int dc_rest_start(DcAdapter *a, uint64_t now_ms) {
  int fd = -1;
  if (!a || a->rest_state != DC_REST_IDLE || a->out_count <= 0 || now_ms < a->rest_retry_after_ms) return 0;
  a->rest_cur = a->outq[a->out_head];
  a->rest_has_cur = 1;
  if (dc_rest_build_request(a) != 0) {
    (void)rt_narrate(
        "discord: REST request for channel %s could not be built; reply "
        "dropped",
        a->rest_cur.channel_id);
    dc_rest_drop_or_commit(a);
    return -1;
  }
  if (net_connect_nb(DC_REST_HOST, DC_REST_PORT, &fd) != 0 || fd < 0) {
    a->rest_retry_after_ms = now_ms + DC_REST_RETRY_MS;
    dc_close_rest(a);
    return -1;
  }
  a->rest_fd = fd;
  a->rest_state = DC_REST_TCP_CONNECTING;
  return 0;
}

static int dc_rest_flush(DcAdapter *a) {
  while (a->rest_tx_len > 0) {
    size_t wrote = 0;
    FcTlsResult tr = fc_tls_write(a->rest_tls, a->rest_tx, a->rest_tx_len, &wrote);
    if (wrote > 0) {
      memmove(a->rest_tx, a->rest_tx + wrote, a->rest_tx_len - wrote);
      a->rest_tx_len -= wrote;
    }
    if (tr == FC_TLS_OK) continue;
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) { a->rest_want = dc_tls_want_events(tr); return 0; }
    return -1;
  }
  a->rest_want = 0;
  return 0;
}

static int dc_rest_read(DcAdapter *a) {
  for (;;) {
    size_t n = 0;
    FcTlsResult tr;
    if (a->rest_rx_len >= sizeof(a->rest_rx) - 1U) return -1;
    tr = fc_tls_read(a->rest_tls, a->rest_rx + a->rest_rx_len, sizeof(a->rest_rx) - 1U - a->rest_rx_len, &n);
    if (n > 0) a->rest_rx_len += n;
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) { a->rest_want = dc_tls_want_events(tr); return 0; }
    if (tr == FC_TLS_CLOSED) return 1;
    if (tr == FC_TLS_ERROR || tr == FC_TLS_NOT_BUILT) return -1;
    if (n == 0) return 0;
  }
}

/* Decoded response body. The reactor is single-threaded and this buffer is
 * consumed entirely inside one dc_drive_rest call, so one module-static
 * buffer beats a 64 KB stack frame in a poll callback. Sized to the receive
 * buffer so a large-but-successful reply is never mistaken for a failure. */
static char g_rest_body[DC_RX_MAX];

/* Returns 1 when a complete response has been parsed, 0 when more bytes are
 * needed, -1 when the response is malformed. */
static int dc_rest_response(DcAdapter *a, int peer_closed,
                            FcHttp1Response *resp) {
  FcHttp1Status s = fc_http1_parse_response(a->rest_rx, a->rest_rx_len,
                                            g_rest_body, sizeof(g_rest_body),
                                            resp);
  if (s == FC_HTTP1_BAD) return -1;
  if (s == FC_HTTP1_DONE) return 1;
  /* A reply with no framing header ends at connection close; the parser
   * fills the body and reports NEED_MORE until the transport says so. */
  return peer_closed ? (resp->status > 0 ? 1 : -1) : 0;
}

static uint64_t dc_rest_retry_delay_ms(const char *raw, size_t raw_len,
                                       const char *body) {
  char value[64];
  char *end = NULL;
  double seconds = 0.0;
  JsonRef root;
  if (raw && fc_http1_header(raw, raw_len, "Retry-After", value,
                             sizeof(value)) == 0) {
    seconds = strtod(value, &end);
    if (!end || *end != '\0') seconds = 0.0;
  }
  if (seconds <= 0.0 && body && json_ref_from_text(body, &root) == 0 &&
      root.type == JSON_REF_OBJECT)
    (void)json_ref_object_get_double(&root, "retry_after", &seconds);
  if (!(seconds > 0.0)) return DC_REST_RETRY_MS;
  if (seconds > (double)(DC_REST_RETRY_AFTER_MAX_MS / 1000ULL))
    return DC_REST_RETRY_AFTER_MAX_MS;
  return (uint64_t)(seconds * 1000.0 + 0.999);
}

static void dc_rest_retry(DcAdapter *a, uint64_t now_ms,
                          uint64_t delay_ms) {
  dc_close_rest(a);
  a->rest_retry_after_ms = now_ms + delay_ms;
}

static void dc_rest_drop_or_commit(DcAdapter *a) {
  (void)dc_rest_commit_head(a);
  a->rest_server_retries = 0;
  a->rest_ambiguous_retries = 0;
  a->rest_retry_after_ms = 0;
  dc_close_rest(a);
}

static void dc_rest_ambiguous_failure(DcAdapter *a, uint64_t now_ms,
                                      const char *cause) {
  if (!a || a->out_count <= 0) {
    dc_close_rest(a);
    return;
  }
  if (a->rest_ambiguous_retries < DC_REST_AMBIGUOUS_RETRY_MAX) {
    a->rest_ambiguous_retries++;
    (void)rt_narrate(
        "discord: REST post to channel %s has ambiguous completion (%s); "
        "retrying once in %dms",
        a->rest_cur.channel_id, cause, DC_REST_RETRY_MS);
    dc_rest_retry(a, now_ms, DC_REST_RETRY_MS);
    return;
  }
  (void)rt_narrate(
      "discord: REST post to channel %s has ambiguous completion (%s); "
      "reply dropped after one retry to avoid duplicate delivery",
      a->rest_cur.channel_id, cause);
  dc_rest_drop_or_commit(a);
}

/* A reply that Discord will never accept (a non-retryable 4xx, or a 5xx
 * that failed its one retry) is committed off the queue so the head cannot
 * wedge. That is a delivery the operator did not get, so it goes to
 * narration -- the production channel -- not the debug-only diagnostic.
 * Discord's error body carries the reason ({"message":..,"code":..}). */
static void dc_rest_narrate_drop(DcAdapter *a, const FcHttp1Response *resp) {
  char message[96] = "";
  char detail[160] = "";
  size_t off = 0;
  long long code = 0;
  JsonRef root;
  if (g_rest_body[0] && json_ref_from_text(g_rest_body, &root) == 0 &&
      root.type == JSON_REF_OBJECT) {
    (void)json_ref_object_get_string(&root, "message", message, sizeof(message));
    (void)json_ref_object_get_long(&root, "code", &code);
  }
  if (code != 0)
    off += (size_t)snprintf(detail + off, sizeof(detail) - off, " code=%lld", code);
  if (message[0] && off < sizeof(detail))
    (void)snprintf(detail + off, sizeof(detail) - off, " message=%s", message);
  (void)rt_narrate("discord: REST post to channel %s failed status=%d%s; reply dropped%s",
                   a->rest_cur.channel_id, resp->status, detail,
                   a->rest_server_retries ? " after one retry" : "");
}

static void dc_rest_apply_response(DcAdapter *a, const char *raw,
                                   size_t raw_len,
                                   const FcHttp1Response *resp,
                                   uint64_t now_ms) {
  if (!a || !resp) return;
  if (resp->status >= 200 && resp->status < 300) {
    dc_rest_drop_or_commit(a);
    return;
  }
  if (resp->status == 429) {
    uint64_t delay = dc_rest_retry_delay_ms(raw, raw_len, g_rest_body);
    (void)rt_narrate("discord: REST rate limited; retrying in %llums",
                     (unsigned long long)delay);
    dc_rest_retry(a, now_ms, delay);
    return;
  }
  if (resp->status >= 500 && resp->status < 600 &&
      a->rest_server_retries == 0U) {
    a->rest_server_retries = 1U;
    (void)rt_narrate("discord: REST status=%d; retrying once in %dms",
                     resp->status, DC_REST_RETRY_MS);
    dc_rest_retry(a, now_ms, DC_REST_RETRY_MS);
    return;
  }
  dc_rest_narrate_drop(a, resp);
  dc_rest_drop_or_commit(a);
}

void dc_drive_rest(DcAdapter *a, int revents) {
  uint64_t now = fc_now_ms();
  if (!a || !a->enabled) return;
  if (a->rest_state == DC_REST_IDLE) { (void)dc_rest_start(a, now); return; }
  if (a->rest_state == DC_REST_TCP_CONNECTING && (revents & (POLLOUT | POLLERR | POLLHUP))) {
    if (net_check_connected(a->rest_fd) != 1) {
      dc_rest_retry(a, now, DC_REST_RETRY_MS);
      return;
    }
    a->rest_tls = dc_tls_client_new(a, a->rest_fd, DC_REST_HOST);
    if (!a->rest_tls) {
      (void)rt_narrate("discord: TLS client setup failed for %s",
                       DC_REST_HOST);
      dc_rest_retry(a, now, DC_REST_RETRY_MS);
      return;
    }
    a->rest_state = DC_REST_TLS_HANDSHAKE;
    a->rest_want = POLLOUT;
  }
  if (a->rest_state == DC_REST_TLS_HANDSHAKE && (revents & (POLLIN | POLLOUT))) {
    FcTlsResult tr = fc_tls_handshake(a->rest_tls);
    if (tr == FC_TLS_OK) {
      a->rest_state = DC_REST_WRITING;
      a->rest_want = POLLOUT;
    } else if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      a->rest_want = dc_tls_want_events(tr);
    } else {
      dc_narrate_tls_failure(a, a->rest_tls, DC_REST_HOST);
      dc_rest_retry(a, now, DC_REST_RETRY_MS);
      return;
    }
  }
  if (a->rest_state == DC_REST_WRITING && (revents & POLLOUT)) {
    if (dc_rest_flush(a) != 0) {
      dc_rest_retry(a, now, DC_REST_RETRY_MS);
      return;
    }
    if (a->rest_tx_len == 0) {
      a->rest_state = DC_REST_READING;
      a->rest_read_started_ms = now;
    }
  }
  if (a->rest_state == DC_REST_READING && (revents & POLLIN)) {
    FcHttp1Response resp;
    int rc = dc_rest_read(a);
    int parsed;
    if (rc < 0) {
      dc_rest_ambiguous_failure(a, now, "TLS read failed after request write");
      return;
    }
    parsed = dc_rest_response(a, rc > 0, &resp);
    if (parsed < 0) {
      dc_diag(a, "REST response was not readable HTTP: %s",
              resp.error[0] ? resp.error : "malformed");
      dc_rest_ambiguous_failure(a, now,
                                "unreadable HTTP response after request write");
      return;
    }
    if (parsed > 0) {
      dc_rest_apply_response(a, a->rest_rx, a->rest_rx_len, &resp, now);
    }
  }
}

int dc_rest_progress_expired(DcAdapter *a, uint64_t now_ms) {
  if (!a || a->rest_state != DC_REST_READING ||
      !net_progress_timed_out(a->rest_read_started_ms, now_ms,
                              DC_PARSE_PROGRESS_TIMEOUT_MS))
    return 0;
  dc_rest_ambiguous_failure(a, now_ms,
                            "response timeout after request write");
  return 1;
}

int dc_test_rest_response(DcAdapter *a, const char *response,
                          uint64_t now_ms) {
  FcHttp1Response resp;
  int parsed;
  if (!a || !response || a->out_count <= 0) return -1;
  if (!a->rest_has_cur) {
    a->rest_cur = a->outq[a->out_head];
    a->rest_has_cur = 1;
  }
  snprintf(a->rest_rx, sizeof(a->rest_rx), "%s", response);
  a->rest_rx_len = strlen(a->rest_rx);
  parsed = dc_rest_response(a, 1, &resp);
  if (parsed < 0) {
    dc_rest_ambiguous_failure(a, now_ms,
                              "unreadable HTTP response after request write");
    return 0;
  }
  if (parsed != 1) return -1;
  dc_rest_apply_response(a, a->rest_rx, a->rest_rx_len, &resp, now_ms);
  return 0;
}
