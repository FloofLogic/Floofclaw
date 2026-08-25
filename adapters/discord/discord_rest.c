/* Discord REST POST.
 *
 * Sends outbound chunked messages to /api/v10/channels/{id}/messages.
 * Pops one DcOutbound at a time from the adapter's outq, runs the
 * TCP connect -> TLS handshake -> HTTP POST -> close state machine,
 * then becomes idle until the next tick. Failure re-queues the
 * payload via dc_queue_raw (declared as a shared helper). */

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

static int dc_rest_pop(DcAdapter *a, DcOutbound *out) {
  if (!a || !out || a->out_count <= 0) return -1;
  *out = a->outq[a->out_head];
  a->out_head = (a->out_head + 1) % DC_OUT_QUEUE_MAX;
  a->out_count--;
  return 0;
}

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
  if (dc_rest_pop(a, &a->rest_cur) != 0) return 0;
  a->rest_has_cur = 1;
  if (net_connect_nb(DC_REST_HOST, DC_REST_PORT, &fd) != 0 || fd < 0) {
    (void)dc_queue_raw(a, a->rest_cur.channel_id, a->rest_cur.payload);
    a->rest_retry_after_ms = now_ms + DC_REST_RETRY_MS;
    a->rest_has_cur = 0;
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

void dc_drive_rest(DcAdapter *a, int revents) {
  uint64_t now = fc_now_ms();
  if (!a || !a->enabled) return;
  if (a->rest_state == DC_REST_IDLE) { (void)dc_rest_start(a, now); return; }
  if (a->rest_state == DC_REST_TCP_CONNECTING && (revents & (POLLOUT | POLLERR | POLLHUP))) {
    if (net_check_connected(a->rest_fd) != 1) { dc_close_rest(a); a->rest_retry_after_ms = now + DC_REST_RETRY_MS; return; }
    a->rest_tls = dc_tls_client_new(a, a->rest_fd, DC_REST_HOST);
    if (!a->rest_tls) {
      (void)rt_narrate("discord: TLS client setup failed for %s",
                       DC_REST_HOST);
      dc_close_rest(a);
      return;
    }
    a->rest_state = DC_REST_TLS_HANDSHAKE;
    a->rest_want = POLLOUT;
  }
  if (a->rest_state == DC_REST_TLS_HANDSHAKE && (revents & (POLLIN | POLLOUT))) {
    FcTlsResult tr = fc_tls_handshake(a->rest_tls);
    if (tr == FC_TLS_OK) {
      if (dc_rest_build_request(a) != 0) { dc_close_rest(a); return; }
      a->rest_state = DC_REST_WRITING;
      a->rest_want = POLLOUT;
    } else if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      a->rest_want = dc_tls_want_events(tr);
    } else {
      dc_narrate_tls_failure(a, a->rest_tls, DC_REST_HOST);
      dc_close_rest(a);
      return;
    }
  }
  if (a->rest_state == DC_REST_WRITING && (revents & POLLOUT)) {
    if (dc_rest_flush(a) != 0) { dc_close_rest(a); return; }
    if (a->rest_tx_len == 0) {
      a->rest_state = DC_REST_READING;
      a->rest_read_started_ms = now;
    }
  }
  if (a->rest_state == DC_REST_READING && (revents & POLLIN)) {
    FcHttp1Response resp;
    int rc = dc_rest_read(a);
    int parsed;
    if (rc < 0) { dc_close_rest(a); return; }
    parsed = dc_rest_response(a, rc > 0, &resp);
    if (parsed < 0) {
      dc_diag(a, "REST response was not readable HTTP: %s",
              resp.error[0] ? resp.error : "malformed");
      dc_close_rest(a);
      return;
    }
    if (parsed > 0) {
      if (resp.status < 200 || resp.status >= 300)
        dc_diag(a, "REST message post failed status=%d", resp.status);
      dc_close_rest(a);
    }
  }
}

int dc_rest_progress_expired(DcAdapter *a, uint64_t now_ms) {
  if (!a || a->rest_state != DC_REST_READING ||
      !net_progress_timed_out(a->rest_read_started_ms, now_ms,
                              DC_PARSE_PROGRESS_TIMEOUT_MS))
    return 0;
  (void)rt_narrate(
      "discord: REST response made no parse progress for %llus; retrying",
      (unsigned long long)(DC_PARSE_PROGRESS_TIMEOUT_MS / 1000U));
  if (a->rest_has_cur)
    (void)dc_queue_raw(a, a->rest_cur.channel_id, a->rest_cur.payload);
  dc_close_rest(a);
  a->rest_retry_after_ms = now_ms + DC_REST_RETRY_MS;
  return 1;
}
