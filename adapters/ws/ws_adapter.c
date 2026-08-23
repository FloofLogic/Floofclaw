/* Bounded loopback local-client listener.
 *
 * One nonblocking reactor module owns the local-client routes:
 *   GET /v1/health
 *   GET /v1/pulse
 *   GET /v1/usage
 *   GET /v1/usage/records
 *   GET /v1/cacheview/records
 *   WS  /v1/fchat
 *
 * HTTP requests authenticate with a bearer token. Browser WebSocket clients
 * authenticate in their first FloofClawWS hello frame. Chat input remains an
 * ordinary bus envelope; progress is ephemeral jobrunner presentation; final
 * success remains the committed delivery ledger. */

#include "ws_adapter.h"

#include "../../runtime/bus/bus.h"
#include "../../runtime/gateway/status_module.h"
#include "../../runtime/inspection_stream.h"
#include "../../runtime/runtime.h"
#include "../../runtime/secure/secret_store.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/json.h"
#include "../../runtime/support/net.h"
#include "../../runtime/support/reconnect_backoff.h"
#include "../../runtime/support/timing.h"

#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef FCLAW_HAVE_OPENSSL
#include <openssl/sha.h>
#endif

#define WS_MAGIC          "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_CLIENTS    16
#define WS_RX_BUF         16384
#define WS_TX_INITIAL     16384
#define WS_TX_CAP         (512U * 1024U + 2048U)
#define WS_PATH_DEFAULT   "/v1/fchat"
#define WS_DELIVERIES_LOG "workspace/logs/deliveries.jsonl"
#define WS_PARSE_PROGRESS_TIMEOUT_MS 30000ULL
#define WS_STREAM_FLUSH_MS 30ULL
#define WS_MAX_INFLIGHT   64
#define WS_TOKEN_CAP      8192
#define WS_PULSE_CAP      (512U * 1024U)

typedef enum {
  WSC_FREE,
  WSC_HANDSHAKING,
  WSC_OPEN,
  WSC_CLOSING
} WsConnState;

typedef struct {
  WsConnState state;
  int fd;
  char rx[WS_RX_BUF];
  size_t rx_len;
  uint64_t parse_progress_ms;
  char *tx;
  size_t tx_len;
  size_t tx_cap;
  int close_after_write;
  RtInspectionStream *http_stream;
  int authenticated;
  char client_id[RT_SMALL];
  char session_id[RT_SMALL];
} WsConn;

typedef struct {
  int used;
  int terminal;
  int canceled;
  int cancel_requested;
  int accepted_sent;
  int stream_dropped;
  char session_id[RT_SMALL];
  char corr_id[RT_SMALL];
  char origin_event_id[RT_SMALL];
  char run_id[RT_SMALL];
  char delivery_id[RT_SMALL];
  char final_body[RT_LARGE];
  char stream_body[RT_LARGE];
  size_t stream_len;
  char pending[1024];
  size_t pending_len;
  uint64_t flush_at_ms;
  unsigned seq;
  unsigned chunks_sent;
} WsInflight;

typedef struct {
  int enabled;
  char host[64];
  int port;
  char path[128];
  char module_id_buf[64];
  char token[WS_TOKEN_CAP];
  size_t token_len;

  int listen_fd;
  uint64_t next_listen_ms;
  FcReconnectBackoff reconnect_backoff;
  WsConn clients[WS_MAX_CLIENTS];
  WsInflight inflight[WS_MAX_INFLIGHT];
  RtScheduler *scheduler;

  long long delivery_cursor;
  int delivery_cursor_init;
} WsAdapter;

/* ----- base64 + SHA-1 handshake ----------------------------------- */

#ifdef FCLAW_HAVE_OPENSSL

/* Only the handshake accept-key uses this, and that needs SHA1 from
 * OpenSSL — so it compiles only where it is reachable. */
static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const unsigned char *src, size_t src_len,
                         char *dst, size_t dst_len) {
  size_t i, o = 0;
  if (dst_len < ((src_len + 2) / 3) * 4 + 1) return -1;
  for (i = 0; i + 2 < src_len; i += 3) {
    uint32_t v = ((uint32_t)src[i] << 16) |
                 ((uint32_t)src[i + 1] << 8) | src[i + 2];
    dst[o++] = base64_alphabet[(v >> 18) & 0x3f];
    dst[o++] = base64_alphabet[(v >> 12) & 0x3f];
    dst[o++] = base64_alphabet[(v >> 6) & 0x3f];
    dst[o++] = base64_alphabet[v & 0x3f];
  }
  if (i < src_len) {
    uint32_t v = (uint32_t)src[i] << 16;
    if (i + 1 < src_len) v |= (uint32_t)src[i + 1] << 8;
    dst[o++] = base64_alphabet[(v >> 18) & 0x3f];
    dst[o++] = base64_alphabet[(v >> 12) & 0x3f];
    dst[o++] = i + 1 < src_len
                   ? base64_alphabet[(v >> 6) & 0x3f] : '=';
    dst[o++] = '=';
  }
  dst[o] = '\0';
  return 0;
}

#endif /* FCLAW_HAVE_OPENSSL */

static int compute_accept(const char *client_key, char *out,
                          size_t out_len) {
#ifdef FCLAW_HAVE_OPENSSL
  unsigned char digest[20];
  char concat[512];
  if (snprintf(concat, sizeof(concat), "%s%s", client_key, WS_MAGIC) >=
      (int)sizeof(concat))
    return -1;
  SHA1((const unsigned char *)concat, strlen(concat), digest);
  return base64_encode(digest, sizeof(digest), out, out_len);
#else
  (void)client_key; (void)out; (void)out_len;
  return -1;
#endif
}

/* ----- socket and connection helpers ------------------------------ */

static int listen_nonblocking(const char *host, int port, int *out_fd) {
  int fd, on = 1;
  struct sockaddr_in addr;
  if (out_fd) *out_fd = -1;
  if (host && *host && strcmp(host, "127.0.0.1") != 0 &&
      strcmp(host, "localhost") != 0)
    return -1;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(fd, 8) != 0 ||
      net_set_nonblocking(fd, 1) != 0) {
    close(fd);
    return -1;
  }
  *out_fd = fd;
  return 0;
}

static WsConn *find_free_slot(WsAdapter *a) {
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i)
    if (a->clients[i].state == WSC_FREE) return &a->clients[i];
  return NULL;
}

static void close_conn(WsConn *c) {
  if (!c) return;
  if (c->fd >= 0) close(c->fd);
  rt_inspection_stream_close(c->http_stream);
  free(c->tx);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  c->state = WSC_FREE;
}

static int conn_reserve_tx(WsConn *c, size_t need) {
  size_t cap;
  char *next;
  if (!c || need > WS_TX_CAP || c->tx_len + need > WS_TX_CAP) return -1;
  if (c->tx_cap >= c->tx_len + need) return 0;
  cap = c->tx_cap ? c->tx_cap : WS_TX_INITIAL;
  while (cap < c->tx_len + need && cap < WS_TX_CAP) cap *= 2;
  if (cap > WS_TX_CAP) cap = WS_TX_CAP;
  if (cap < c->tx_len + need) return -1;
  next = (char *)realloc(c->tx, cap);
  if (!next) return -1;
  c->tx = next;
  c->tx_cap = cap;
  return 0;
}

static int conn_append_tx(WsConn *c, const void *bytes, size_t n) {
  if (conn_reserve_tx(c, n) != 0) return -1;
  memcpy(c->tx + c->tx_len, bytes, n);
  c->tx_len += n;
  return 0;
}

static int conn_drain_tx(WsConn *c) {
  while (c && c->tx_len > 0) {
    ssize_t n = net_write_nb(c->fd, c->tx, c->tx_len);
    if (n > 0) {
      memmove(c->tx, c->tx + n, c->tx_len - (size_t)n);
      c->tx_len -= (size_t)n;
    } else if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      if (errno == EINTR) continue;
      return -1;
    } else {
      break;
    }
  }
  return 0;
}

static int send_text_frame(WsConn *c, const char *text, size_t len) {
  unsigned char hdr[10];
  size_t hl;
  if (!c || len > 65535U) return -1;
  if (len < 126) {
    hdr[0] = 0x81; hdr[1] = (unsigned char)len; hl = 2;
  } else {
    hdr[0] = 0x81; hdr[1] = 126;
    hdr[2] = (unsigned char)(len >> 8);
    hdr[3] = (unsigned char)len;
    hl = 4;
  }
  return conn_append_tx(c, hdr, hl) == 0 &&
         (!len || conn_append_tx(c, text, len) == 0) ? 0 : -1;
}

static int token_equal(const char expected[WS_TOKEN_CAP],
                       const char *candidate) {
  unsigned diff = 0;
  size_t candidate_len = candidate ? strlen(candidate) : 0;
  unsigned char padded[WS_TOKEN_CAP];
  memset(padded, 0, sizeof(padded));
  if (candidate && candidate_len < sizeof(padded))
    memcpy(padded, candidate, candidate_len);
  diff |= (unsigned)(candidate_len ^ strlen(expected));
  for (size_t i = 0; i < WS_TOKEN_CAP; ++i)
    diff |= (unsigned)((unsigned char)expected[i] ^ padded[i]);
  secret_store_zero(padded, sizeof(padded));
  return diff == 0;
}

/* ----- fixed HTTP route handling ---------------------------------- */

static int header_get(const char *headers, const char *name,
                      char *out, size_t out_len) {
  const char *p = headers;
  size_t name_len = strlen(name);
  while (p && *p) {
    const char *eol = strstr(p, "\r\n");
    size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
    if (line_len > name_len + 1 &&
        strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
      const char *v = p + name_len + 1;
      size_t vl;
      while (*v == ' ' || *v == '\t') v++;
      vl = (size_t)(p + line_len - v);
      if (vl >= out_len) return -1;
      memcpy(out, v, vl);
      out[vl] = '\0';
      return 0;
    }
    if (!eol) break;
    p = eol + 2;
  }
  return -1;
}

static const char *http_reason(int status) {
  switch (status) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 413: return "Payload Too Large";
  case 426: return "Upgrade Required";
  case 429: return "Too Many Requests";
  case 500: return "Internal Server Error";
  default: return "Error";
  }
}

static int queue_http_response(WsConn *c, int status, const char *body) {
  char head[512];
  size_t body_len = body ? strlen(body) : 0;
  int n = snprintf(
      head, sizeof(head),
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: application/json\r\n"
      "Cache-Control: no-store\r\n"
      "Content-Length: %zu\r\n"
      "Connection: close\r\n"
      "%s"
      "\r\n",
      status, http_reason(status), body_len,
      status == 405 ? "Allow: GET\r\n" :
      status == 426 ? "Upgrade: websocket\r\n" : "");
  if (n < 0 || (size_t)n >= sizeof(head) ||
      conn_append_tx(c, head, (size_t)n) != 0 ||
      (body_len && conn_append_tx(c, body, body_len) != 0))
    return -1;
  c->state = WSC_CLOSING;
  c->close_after_write = 1;
  return 0;
}

static int queue_http_record_stream(WsConn *c, RtInspectionStream *stream) {
  static const char head[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/x-ndjson\r\n"
      "Cache-Control: no-store\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n";
  if (!c || !stream || conn_append_tx(c, head, sizeof(head) - 1) != 0)
    return -1;
  c->http_stream = stream;
  c->state = WSC_CLOSING;
  c->close_after_write = 0;
  return 0;
}

static int pump_http_record_stream(WsConn *c) {
  char chunk_head[32];
  char *record;
  int next;
  int head_len;
  size_t record_len;
  if (!c || !c->http_stream || c->tx_len > WS_TX_INITIAL) return 0;
  record = (char *)malloc(RT_INSPECTION_RECORD_MAX);
  if (!record) return -1;
  next = rt_inspection_stream_next(c->http_stream, record,
                                   RT_INSPECTION_RECORD_MAX);
  if (next < 0) {
    free(record);
    return -1;
  }
  if (next == 0) {
    free(record);
    rt_inspection_stream_close(c->http_stream);
    c->http_stream = NULL;
    c->close_after_write = 1;
    return conn_append_tx(c, "0\r\n\r\n", 5);
  }
  record_len = strlen(record);
  head_len = snprintf(chunk_head, sizeof(chunk_head), "%zx\r\n", record_len);
  if (head_len < 0 || (size_t)head_len >= sizeof(chunk_head) ||
      conn_append_tx(c, chunk_head, (size_t)head_len) != 0 ||
      conn_append_tx(c, record, record_len) != 0 ||
      conn_append_tx(c, "\r\n", 2) != 0) {
    free(record);
    return -1;
  }
  free(record);
  return 0;
}

static int http_error(WsConn *c, int status, const char *code,
                      const char *message) {
  char ec[128], em[512], body[768];
  if (json_escape(code ? code : "ERROR", ec, sizeof(ec)) != 0 ||
      json_escape(message ? message : "Request failed.", em, sizeof(em)) != 0)
    return -1;
  snprintf(body, sizeof(body),
           "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}\n", ec, em);
  return queue_http_response(c, status, body);
}

static int http_authorized(const WsAdapter *a, const char *request) {
  char auth[WS_TOKEN_CAP + 16];
  const char *candidate;
  if (header_get(request, "Authorization", auth, sizeof(auth)) != 0)
    return 0;
  if (strncmp(auth, "Bearer ", 7) != 0) return 0;
  candidate = auth + 7;
  return token_equal(a->token, candidate);
}

static int route_matches(const char *path, const char *route) {
  size_t len = strlen(route);
  return strncmp(path, route, len) == 0 &&
         (path[len] == '\0' || path[len] == '?');
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

/* 0 found, 1 absent, -1 malformed or too large. */
static int query_value(const char *path, const char *name,
                       char *out, size_t out_len) {
  const char *query = strchr(path, '?');
  size_t name_len = strlen(name);
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (!query) return 1;
  query++;
  while (*query) {
    const char *end = strchr(query, '&');
    const char *equals;
    size_t pair_len = end ? (size_t)(end - query) : strlen(query);
    equals = memchr(query, '=', pair_len);
    if (equals && (size_t)(equals - query) == name_len &&
        strncmp(query, name, name_len) == 0) {
      const char *src = equals + 1;
      const char *src_end = query + pair_len;
      size_t pos = 0;
      while (src < src_end) {
        int value;
        if (pos + 1 >= out_len) return -1;
        if (*src == '%' && src + 2 < src_end &&
            (value = hex_value(src[1])) >= 0 && hex_value(src[2]) >= 0) {
          out[pos++] = (char)(value * 16 + hex_value(src[2]));
          src += 3;
        } else if (*src == '+') {
          out[pos++] = ' ';
          src++;
        } else {
          out[pos++] = *src++;
        }
      }
      out[pos] = '\0';
      return 0;
    }
    if (!end) break;
    query = end + 1;
  }
  return 1;
}

static int start_record_route(WsAdapter *a, WsConn *c, const char *path,
                              int cacheview) {
  RtInspectionOptions options;
  RtInspectionStream *stream;
  char agent[RT_SMALL] = "";
  char run_id[RT_SMALL] = "";
  char recent_text[32] = "";
  char call_text[32] = "";
  char *end = NULL;
  long parsed;
  memset(&options, 0, sizeof(options));
  options.floop = a->scheduler->loop_name;
  options.agents = a->scheduler->agent_meta;
  options.agent_count = a->scheduler->agent_meta_count;
  options.recent = 10;
  if (query_value(path, "agent", agent, sizeof(agent)) < 0 ||
      query_value(path, "run", run_id, sizeof(run_id)) < 0 ||
      query_value(path, "n", recent_text, sizeof(recent_text)) < 0 ||
      query_value(path, "call", call_text, sizeof(call_text)) < 0)
    return http_error(c, 400, "BAD_SELECTOR", "Malformed record selector.");
  if (agent[0]) options.agent = agent;
  if (run_id[0]) options.run_id = run_id;
  if (recent_text[0]) {
    parsed = strtol(recent_text, &end, 10);
    if (!end || *end || parsed < 1 || parsed > 10)
      return http_error(c, 400, "BAD_SELECTOR", "n must be from 1 through 10.");
    options.recent = (int)parsed;
  }
  if (call_text[0]) {
    parsed = strtol(call_text, &end, 10);
    if (!end || *end || parsed < 1 || parsed > 998 || !options.run_id)
      return http_error(c, 400, "BAD_SELECTOR",
                        "call requires run and must be from 1 through 998.");
    options.call_seq = (int)parsed;
    options.call_seq_set = 1;
  }
  if (cacheview && !options.agent)
    return http_error(c, 400, "AGENT_REQUIRED",
                      "cacheview records require agent.");
  stream = cacheview ? rt_cacheview_record_stream_open(&options)
                     : rt_usage_record_stream_open(&options);
  if (!stream)
    return http_error(c, 500, "STREAM_FAILED",
                      "Inspection record stream failed.");
  if (queue_http_record_stream(c, stream) != 0) {
    rt_inspection_stream_close(stream);
    return -1;
  }
  return pump_http_record_stream(c);
}

static int handle_http_get(WsAdapter *a, WsConn *c, const char *path) {
  char small[RT_XL];
  if (strcmp(path, "/v1/health") == 0) {
    if (fc_local_health_build_json(a->scheduler, small, sizeof(small)) != 0)
      return http_error(c, 500, "PROJECTION_FAILED",
                        "Health projection failed.");
    return queue_http_response(c, 200, small);
  }
  if (strcmp(path, "/v1/usage") == 0) {
    if (rt_usage_client_json(small, sizeof(small)) != 0)
      return http_error(c, 500, "PROJECTION_FAILED",
                        "Usage projection failed.");
    return queue_http_response(c, 200, small);
  }
  if (route_matches(path, "/v1/usage/records"))
    return start_record_route(a, c, path, 0);
  if (route_matches(path, "/v1/cacheview/records"))
    return start_record_route(a, c, path, 1);
  if (strcmp(path, "/v1/pulse") == 0) {
    char *body = (char *)malloc(WS_PULSE_CAP);
    char projection_error[RT_MED] = "";
    int rc;
    if (!body)
      return http_error(c, 500, "ALLOCATION_FAILED",
                        "Pulse projection allocation failed.");
    rc = rt_pulse_projection_json(
        ".", (long long)timing_wall_ms(), 40, 25,
        body, WS_PULSE_CAP, projection_error, sizeof(projection_error));
    if (rc != 0) {
      (void)rt_narrate("local client pulse projection failed: %s",
                       projection_error[0] ? projection_error : "unknown");
      free(body);
      return http_error(c, 500, "PROJECTION_FAILED",
                        "Pulse projection failed.");
    }
    rc = queue_http_response(c, 200, body);
    free(body);
    return rc;
  }
  if (strcmp(path, a->path) == 0)
    return http_error(c, 426, "UPGRADE_REQUIRED",
                      "Use a WebSocket upgrade for this route.");
  return http_error(c, 404, "NOT_FOUND", "Unknown local client route.");
}

static int handle_request_head(WsAdapter *a, WsConn *c) {
  char *req_end = strstr(c->rx, "\r\n\r\n");
  char method[16], path[256], proto[16], upgrade[64] = "";
  char content_length[64] = "";
  int is_upgrade;
  size_t consumed;
  if (!req_end) return 0;
  if (sscanf(c->rx, "%15s %255s %15s", method, path, proto) != 3)
    return http_error(c, 400, "BAD_REQUEST", "Malformed request line.");
  consumed = (size_t)(req_end - c->rx) + 4;
  if (header_get(c->rx, "Content-Length", content_length,
                 sizeof(content_length)) == 0 &&
      strtoll(content_length, NULL, 10) > 0)
    return http_error(c, 400, "REQUEST_BODY_NOT_ALLOWED",
                      "GET request bodies are not supported.");
  (void)header_get(c->rx, "Upgrade", upgrade, sizeof(upgrade));
  is_upgrade = strcasecmp(upgrade, "websocket") == 0;
  if (strcasecmp(method, "GET") != 0)
    return http_error(c, 405, "METHOD_NOT_ALLOWED",
                      "Only GET is supported.");
  if (!is_upgrade) {
    if (!http_authorized(a, c->rx))
      return http_error(c, 401, "UNAUTHORIZED",
                        "A valid bearer token is required.");
    return handle_http_get(a, c, path);
  }
  if (strcmp(path, a->path) != 0)
    return http_error(c, 404, "NOT_FOUND",
                      "Unknown WebSocket route.");
  {
    char key[256], accept_b64[64], response[512];
    if (header_get(c->rx, "Sec-WebSocket-Key", key, sizeof(key)) != 0 ||
        compute_accept(key, accept_b64, sizeof(accept_b64)) != 0)
      return http_error(c, 400, "BAD_UPGRADE",
                        "Invalid WebSocket upgrade.");
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_b64);
    if (conn_append_tx(c, response, strlen(response)) != 0) return -1;
  }
  memmove(c->rx, c->rx + consumed, c->rx_len - consumed);
  c->rx_len -= consumed;
  c->state = WSC_OPEN;
  c->parse_progress_ms = c->rx_len ? fc_now_ms() : fc_now_ms();
  return 1;
}

/* ----- WebSocket frame parsing ------------------------------------ */

static int extract_frame(WsConn *c, char *text_out, size_t text_out_max,
                         size_t *text_out_len, int *opcode_out) {
  unsigned char *p;
  unsigned char b0, b1, mask[4];
  size_t header = 2;
  uint64_t payload_len;
  if (c->rx_len < 2) return 0;
  p = (unsigned char *)c->rx;
  b0 = p[0]; b1 = p[1];
  if ((b0 & 0x70U) != 0 || !(b0 & 0x80U)) return -1;
  *opcode_out = b0 & 0x0f;
  payload_len = b1 & 0x7f;
  if (payload_len == 126) {
    if (c->rx_len < 4) return 0;
    payload_len = ((uint64_t)p[2] << 8) | p[3];
    header = 4;
  } else if (payload_len == 127) {
    if (c->rx_len < 10) return 0;
    payload_len = 0;
    for (int i = 0; i < 8; ++i)
      payload_len = (payload_len << 8) | p[2 + i];
    header = 10;
  }
  if (!(b1 & 0x80U)) return -1;
  if (c->rx_len < header + 4) return 0;
  memcpy(mask, p + header, 4);
  header += 4;
  if (payload_len + 1 > text_out_max) return -1;
  if (c->rx_len < header + payload_len) return 0;
  for (uint64_t i = 0; i < payload_len; ++i)
    text_out[i] = (char)(p[header + i] ^ mask[i & 3]);
  text_out[payload_len] = '\0';
  *text_out_len = (size_t)payload_len;
  {
    size_t used = header + (size_t)payload_len;
    memmove(c->rx, c->rx + used, c->rx_len - used);
    c->rx_len -= used;
  }
  return 1;
}

/* ----- FloofClawWS v1 --------------------------------------------- */

static int send_json(WsConn *c, const char *json) {
  if (!c || !json || send_text_frame(c, json, strlen(json)) != 0) {
    if (c) close_conn(c);
    return -1;
  }
  return 0;
}

static void send_to_session(WsAdapter *a, const char *session_id,
                            const char *json) {
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i) {
    WsConn *c = &a->clients[i];
    if (c->state == WSC_OPEN && c->authenticated &&
        strcmp(c->session_id, session_id) == 0)
      (void)send_json(c, json);
  }
}

static int ws_error(WsConn *c, const char *corr_id,
                    const char *code, const char *message) {
  char ecorr[RT_MED], ecode[RT_MED], emsg[RT_MED * 2], json[RT_LARGE];
  if (json_escape(corr_id ? corr_id : "", ecorr, sizeof(ecorr)) != 0 ||
      json_escape(code ? code : "INTERNAL", ecode, sizeof(ecode)) != 0 ||
      json_escape(message ? message : "Request failed.", emsg,
                  sizeof(emsg)) != 0)
    return -1;
  snprintf(json, sizeof(json),
           "{\"type\":\"error\"%s%s%s,\"code\":\"%s\","
           "\"message\":\"%s\"}",
           corr_id && *corr_id ? ",\"corr_id\":\"" : "",
           corr_id && *corr_id ? ecorr : "",
           corr_id && *corr_id ? "\"" : "",
           ecode, emsg);
  return send_json(c, json);
}

static void error_to_session(WsAdapter *a, const char *session_id,
                             const char *corr_id, const char *code,
                             const char *message) {
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i) {
    WsConn *c = &a->clients[i];
    if (c->state == WSC_OPEN && c->authenticated &&
        strcmp(c->session_id, session_id) == 0)
      (void)ws_error(c, corr_id, code, message);
  }
}

static WsInflight *find_inflight(WsAdapter *a, const char *session,
                                 const char *corr) {
  for (size_t i = 0; i < WS_MAX_INFLIGHT; ++i)
    if (a->inflight[i].used &&
        strcmp(a->inflight[i].session_id, session) == 0 &&
        strcmp(a->inflight[i].corr_id, corr) == 0)
      return &a->inflight[i];
  return NULL;
}

static WsInflight *find_inflight_origin(WsAdapter *a, const char *origin) {
  for (size_t i = 0; i < WS_MAX_INFLIGHT; ++i)
    if (a->inflight[i].used &&
        strcmp(a->inflight[i].origin_event_id, origin) == 0)
      return &a->inflight[i];
  return NULL;
}

static WsInflight *alloc_inflight(WsAdapter *a) {
  for (size_t i = 0; i < WS_MAX_INFLIGHT; ++i)
    if (!a->inflight[i].used) {
      memset(&a->inflight[i], 0, sizeof(a->inflight[i]));
      a->inflight[i].used = 1;
      return &a->inflight[i];
    }
  for (size_t i = 0; i < WS_MAX_INFLIGHT; ++i)
    if (a->inflight[i].terminal) {
      memset(&a->inflight[i], 0, sizeof(a->inflight[i]));
      a->inflight[i].used = 1;
      return &a->inflight[i];
    }
  return NULL;
}

static void send_status(WsAdapter *a, WsInflight *f,
                        const char *state) {
  char ecorr[RT_MED], erun[RT_MED], json[1024];
  if (!f || json_escape(f->corr_id, ecorr, sizeof(ecorr)) != 0 ||
      json_escape(f->run_id, erun, sizeof(erun)) != 0)
    return;
  snprintf(json, sizeof(json),
           "{\"type\":\"status\",\"corr_id\":\"%s\",\"state\":\"%s\"%s%s%s}",
           ecorr, state,
           f->run_id[0] ? ",\"run_id\":\"" : "",
           f->run_id[0] ? erun : "",
           f->run_id[0] ? "\"" : "");
  send_to_session(a, f->session_id, json);
}

static int send_chunk(WsAdapter *a, WsInflight *f,
                      const char *delta, size_t len) {
  char plain[RT_LARGE], escaped[RT_LARGE * 2], ecorr[RT_MED];
  char json[RT_LARGE * 2 + 512];
  if (!f || !delta || len == 0 || len >= sizeof(plain)) return -1;
  memcpy(plain, delta, len); plain[len] = '\0';
  if (json_escape(plain, escaped, sizeof(escaped)) != 0 ||
      json_escape(f->corr_id, ecorr, sizeof(ecorr)) != 0)
    return -1;
  snprintf(json, sizeof(json),
           "{\"type\":\"reply_chunk\",\"corr_id\":\"%s\","
           "\"seq\":%u,\"delta\":\"%s\"}",
           ecorr, f->seq++, escaped);
  send_to_session(a, f->session_id, json);
  f->chunks_sent++;
  return 0;
}

static void flush_pending(WsAdapter *a, WsInflight *f) {
  if (!f || !f->pending_len) return;
  (void)send_chunk(a, f, f->pending, f->pending_len);
  f->pending_len = 0;
  f->flush_at_ms = 0;
}

static void send_done(WsAdapter *a, WsInflight *f) {
  char ecorr[RT_MED], erun[RT_MED], edelivery[RT_MED];
  char ebody[RT_LARGE * 2], json[RT_LARGE * 2 + 768];
  if (!f ||
      json_escape(f->corr_id, ecorr, sizeof(ecorr)) != 0 ||
      json_escape(f->run_id, erun, sizeof(erun)) != 0 ||
      json_escape(f->delivery_id, edelivery, sizeof(edelivery)) != 0 ||
      json_escape(f->final_body, ebody, sizeof(ebody)) != 0)
    return;
  snprintf(json, sizeof(json),
           "{\"type\":\"reply_done\",\"corr_id\":\"%s\","
           "\"run_id\":\"%s\",\"delivery_id\":\"%s\",\"body\":\"%s\"}",
           ecorr, erun, edelivery, ebody);
  send_to_session(a, f->session_id, json);
}

static int replay_after_delivery(WsAdapter *a, WsConn *c,
                                 const char *after_delivery_id) {
  char *text = NULL;
  const char *p;
  int cursor_seen = 0;
  if (!after_delivery_id || !*after_delivery_id) return 0;
  if (fs_read_text(WS_DELIVERIES_LOG, &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  p = text;
  while (*p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char line[RT_LARGE * 3];
    JsonRef root, delivery, ref;
    char did[RT_SMALL] = "", ch[RT_SMALL] = "", aid[RT_SMALL] = "";
    char session[RT_SMALL] = "", corr[RT_SMALL] = "", run_id[RT_SMALL] = "";
    char body[RT_LARGE] = "";
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len); line[len] = '\0';
    if (json_ref_first_object(line, &root) == 0 &&
        json_ref_object_get_object(&root, "delivery", &delivery) == 0 &&
        json_ref_object_get_string(&delivery, "delivery_id",
                                   did, sizeof(did)) == 0) {
      if (strcmp(did, after_delivery_id) == 0) {
        cursor_seen = 1;
      } else if (cursor_seen &&
                 json_ref_object_get_string(&delivery, "channel",
                                            ch, sizeof(ch)) == 0 &&
                 strcmp(ch, "ws") == 0 &&
                 json_ref_object_get_string(&delivery, "adapter_id",
                                            aid, sizeof(aid)) == 0 &&
                 strcmp(aid, a->module_id_buf) == 0 &&
                 json_ref_object_get_object(&delivery, "ref", &ref) == 0 &&
                 json_ref_object_get_string(&ref, "session_id",
                                            session, sizeof(session)) == 0 &&
                 strcmp(session, c->session_id) == 0 &&
                 json_ref_object_get_string(&ref, "corr_id",
                                            corr, sizeof(corr)) == 0 &&
                 json_ref_object_get_string(&delivery, "text",
                                            body, sizeof(body)) == 0) {
        WsInflight replay;
        memset(&replay, 0, sizeof(replay));
        snprintf(replay.session_id, sizeof(replay.session_id), "%s", session);
        snprintf(replay.corr_id, sizeof(replay.corr_id), "%s", corr);
        snprintf(replay.delivery_id, sizeof(replay.delivery_id), "%s", did);
        snprintf(replay.final_body, sizeof(replay.final_body), "%s", body);
        (void)json_ref_object_get_string(&root, "run_id",
                                         run_id, sizeof(run_id));
        snprintf(replay.run_id, sizeof(replay.run_id), "%s", run_id);
        send_done(a, &replay);
      }
    }
    if (!e) break;
    p = e + 1;
  }
  free(text);
  return cursor_seen ? 0 : -1;
}

static int replay_terminal_for_corr(WsAdapter *a, const char *session_id,
                                    const char *corr_id) {
  char *text = NULL;
  const char *p;
  int found = 0;
  if (fs_read_text(WS_DELIVERIES_LOG, &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return 0;
  p = text;
  while (*p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char line[RT_LARGE * 3];
    JsonRef root, delivery, ref;
    char session[RT_SMALL] = "", corr[RT_SMALL] = "";
    char channel[RT_SMALL] = "", adapter[RT_SMALL] = "";
    char did[RT_SMALL] = "", run_id[RT_SMALL] = "", body[RT_LARGE] = "";
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len); line[len] = '\0';
    if (json_ref_first_object(line, &root) == 0 &&
        json_ref_object_get_object(&root, "delivery", &delivery) == 0 &&
        json_ref_object_get_string(&delivery, "channel",
                                   channel, sizeof(channel)) == 0 &&
        strcmp(channel, "ws") == 0 &&
        json_ref_object_get_string(&delivery, "adapter_id",
                                   adapter, sizeof(adapter)) == 0 &&
        strcmp(adapter, a->module_id_buf) == 0 &&
        json_ref_object_get_object(&delivery, "ref", &ref) == 0 &&
        json_ref_object_get_string(&ref, "session_id",
                                   session, sizeof(session)) == 0 &&
        strcmp(session, session_id) == 0 &&
        json_ref_object_get_string(&ref, "corr_id",
                                   corr, sizeof(corr)) == 0 &&
        strcmp(corr, corr_id) == 0 &&
        json_ref_object_get_string(&delivery, "delivery_id",
                                   did, sizeof(did)) == 0 &&
        json_ref_object_get_string(&delivery, "text",
                                   body, sizeof(body)) == 0) {
      WsInflight replay;
      memset(&replay, 0, sizeof(replay));
      snprintf(replay.session_id, sizeof(replay.session_id), "%s", session);
      snprintf(replay.corr_id, sizeof(replay.corr_id), "%s", corr);
      snprintf(replay.delivery_id, sizeof(replay.delivery_id), "%s", did);
      snprintf(replay.final_body, sizeof(replay.final_body), "%s", body);
      (void)json_ref_object_get_string(&root, "run_id",
                                       run_id, sizeof(run_id));
      snprintf(replay.run_id, sizeof(replay.run_id), "%s", run_id);
      send_done(a, &replay);
      found = 1;
      break;
    }
    if (!e) break;
    p = e + 1;
  }
  free(text);
  return found;
}

static int prior_message_seen(const char *session_id, const char *corr_id) {
  char *text = NULL;
  const char *p;
  int found = 0;
  if (fs_read_text("workspace/logs/bus.jsonl", &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return 0;
  p = text;
  while (*p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char line[RT_LARGE * 3];
    JsonRef root, payload, ref;
    char channel[RT_SMALL] = "", type[RT_SMALL] = "";
    char session[RT_SMALL] = "", corr[RT_SMALL] = "";
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len); line[len] = '\0';
    if (json_ref_first_object(line, &root) == 0 &&
        json_ref_object_get_string(&root, "channel",
                                   channel, sizeof(channel)) == 0 &&
        strcmp(channel, "ws") == 0 &&
        json_ref_object_get_string(&root, "type", type, sizeof(type)) == 0 &&
        strcmp(type, "user_message") == 0 &&
        json_ref_object_get_object(&root, "payload", &payload) == 0 &&
        json_ref_object_get_object(&payload, "ref", &ref) == 0 &&
        json_ref_object_get_string(&ref, "session_id",
                                   session, sizeof(session)) == 0 &&
        strcmp(session, session_id) == 0 &&
        json_ref_object_get_string(&ref, "corr_id",
                                   corr, sizeof(corr)) == 0 &&
        strcmp(corr, corr_id) == 0) {
      found = 1;
      break;
    }
    if (!e) break;
    p = e + 1;
  }
  free(text);
  return found;
}

static void handle_hello(WsAdapter *a, WsConn *c, const JsonRef *root) {
  char protocol[64] = "", version[32] = "", token[WS_TOKEN_CAP] = "";
  char client_id[RT_SMALL] = "", session_id[RT_SMALL] = "";
  char after[RT_SMALL] = "", esession[RT_MED], json[1024];
  if (json_ref_object_get_string(root, "protocol", protocol,
                                 sizeof(protocol)) != 0 ||
      json_ref_object_get_string(root, "version", version,
                                 sizeof(version)) != 0 ||
      strcmp(protocol, "FloofClawWS") != 0 ||
      strcmp(version, "1.0") != 0 ||
      json_ref_object_get_string(root, "token", token,
                                 sizeof(token)) != 0 ||
      !token_equal(a->token, token)) {
    secret_store_zero(token, sizeof(token));
    (void)ws_error(c, NULL, "UNAUTHORIZED",
                   "A valid FloofClawWS hello is required.");
    c->state = WSC_CLOSING;
    c->close_after_write = 1;
    return;
  }
  secret_store_zero(token, sizeof(token));
  (void)json_ref_object_get_string(root, "client_id", client_id,
                                   sizeof(client_id));
  (void)json_ref_object_get_string(root, "session_id", session_id,
                                   sizeof(session_id));
  (void)json_ref_object_get_string(root, "after_delivery_id", after,
                                   sizeof(after));
  if (!session_id[0])
    snprintf(session_id, sizeof(session_id), "sess_%08llx",
             (unsigned long long)fc_now_ms());
  snprintf(c->client_id, sizeof(c->client_id), "%s",
           client_id[0] ? client_id : "local-client");
  snprintf(c->session_id, sizeof(c->session_id), "%s", session_id);
  c->authenticated = 1;
  c->parse_progress_ms = 0;
  if (json_escape(c->session_id, esession, sizeof(esession)) != 0)
    esession[0] = '\0';
  snprintf(json, sizeof(json),
           "{\"type\":\"hello_ack\",\"protocol\":\"FloofClawWS\","
           "\"version\":\"1.0\",\"session_id\":\"%s\","
           "\"capabilities\":[\"chat_stream\",\"resume\",\"cancel\"]}",
           esession);
  (void)send_json(c, json);
  if (after[0] && replay_after_delivery(a, c, after) != 0)
    (void)ws_error(c, NULL, "CURSOR_EXPIRED",
                   "The delivery cursor is unknown or no longer retained.");
}

static void handle_message(WsAdapter *a, WsConn *c, const JsonRef *root) {
  char corr[RT_SMALL] = "", ctx[RT_SMALL] = "chat_main";
  char body[RT_LARGE] = "", ebody[RT_LARGE * 2], eaid[RT_MED];
  char esession[RT_MED], ecorr[RT_MED], ectx[RT_MED];
  char payload[RT_LARGE * 2 + 1024], origin[BUS_ID_MAX];
  WsInflight *f;
  if (!c->authenticated) {
    (void)ws_error(c, NULL, "UNAUTHORIZED",
                   "Authenticate with hello before sending messages.");
    return;
  }
  if (json_ref_object_get_string(root, "id", corr, sizeof(corr)) != 0 ||
      !corr[0] ||
      json_ref_object_get_string(root, "body", body, sizeof(body)) != 0 ||
      !body[0]) {
    (void)ws_error(c, corr, "BAD_REQUEST",
                   "message.id and non-empty message.body are required.");
    return;
  }
  (void)json_ref_object_get_string(root, "ctx_id", ctx, sizeof(ctx));
  if (!ctx[0]) snprintf(ctx, sizeof(ctx), "chat_main");
  f = find_inflight(a, c->session_id, corr);
  if (f) {
    if (f->terminal && !f->canceled && f->delivery_id[0])
      send_done(a, f);
    else if (f->canceled)
      (void)ws_error(c, corr, "CANCELED", "Reply canceled.");
    else
      send_status(a, f, f->run_id[0] ? "accepted" : "queued");
    return;
  }
  if (replay_terminal_for_corr(a, c->session_id, corr)) return;
  if (prior_message_seen(c->session_id, corr)) {
    (void)ws_error(c, corr, "DUPLICATE_ID",
                   "The message id was already used in this session.");
    return;
  }
  f = alloc_inflight(a);
  if (!f) {
    (void)ws_error(c, corr, "BUSY",
                   "The local client request table is full.");
    return;
  }
  if (json_escape(body, ebody, sizeof(ebody)) != 0 ||
      json_escape(a->module_id_buf, eaid, sizeof(eaid)) != 0 ||
      json_escape(c->session_id, esession, sizeof(esession)) != 0 ||
      json_escape(corr, ecorr, sizeof(ecorr)) != 0 ||
      json_escape(ctx, ectx, sizeof(ectx)) != 0) {
    memset(f, 0, sizeof(*f));
    (void)ws_error(c, corr, "BAD_REQUEST", "Message fields are too large.");
    return;
  }
  snprintf(payload, sizeof(payload),
           "{\"text\":\"%s\",\"adapter_id\":\"%s\","
           "\"context_id\":\"%s:%s\","
           "\"ref\":{\"client_id\":\"%s\",\"session_id\":\"%s\","
           "\"corr_id\":\"%s\",\"ctx_id\":\"%s\"}}",
           ebody, eaid, esession, ectx, eaid, esession, ecorr, ectx);
  if (bus_publish("ws", "user_message", payload, origin,
                  sizeof(origin)) != 0) {
    memset(f, 0, sizeof(*f));
    (void)ws_error(c, corr, "INTERNAL",
                   "The message could not be published.");
    return;
  }
  snprintf(f->session_id, sizeof(f->session_id), "%s", c->session_id);
  snprintf(f->corr_id, sizeof(f->corr_id), "%s", corr);
  snprintf(f->origin_event_id, sizeof(f->origin_event_id), "%s", origin);
  send_status(a, f, "queued");
}

static void handle_cancel(WsAdapter *a, WsConn *c, const JsonRef *root) {
  char corr[RT_SMALL] = "";
  WsInflight *f;
  if (!c->authenticated) {
    (void)ws_error(c, NULL, "UNAUTHORIZED",
                   "Authenticate with hello before cancellation.");
    return;
  }
  if (json_ref_object_get_string(root, "corr_id", corr,
                                 sizeof(corr)) != 0 || !corr[0]) {
    (void)ws_error(c, NULL, "BAD_REQUEST", "cancel.corr_id is required.");
    return;
  }
  f = find_inflight(a, c->session_id, corr);
  if (!f || f->terminal) {
    (void)ws_error(c, corr, "NOT_FOUND", "No active reply has that id.");
    return;
  }
  f->cancel_requested = 1;
  if (f->run_id[0])
    (void)rt_scheduler_cancel_origin(a->scheduler, f->origin_event_id,
                                     "client_canceled");
  f->canceled = 1;
  f->terminal = 1;
  error_to_session(a, f->session_id, f->corr_id,
                   "CANCELED", "Reply canceled.");
}

static void handle_protocol_text(WsAdapter *a, WsConn *c,
                                 const char *text) {
  JsonRef root;
  char type[64] = "";
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_string(&root, "type", type, sizeof(type)) != 0) {
    (void)ws_error(c, NULL, "BAD_REQUEST",
                   "Each frame must be one JSON object with type.");
    return;
  }
  if (!c->authenticated && strcmp(type, "hello") != 0) {
    (void)ws_error(c, NULL, "UNAUTHORIZED",
                   "The first frame must be an authenticated hello.");
    return;
  }
  if (strcmp(type, "hello") == 0) {
    if (c->authenticated)
      (void)ws_error(c, NULL, "BAD_REQUEST",
                     "hello was already accepted.");
    else
      handle_hello(a, c, &root);
  } else if (strcmp(type, "message") == 0) {
    handle_message(a, c, &root);
  } else if (strcmp(type, "cancel") == 0) {
    handle_cancel(a, c, &root);
  } else if (strcmp(type, "ping") == 0) {
    long long ts = 0;
    char pong[256];
    (void)json_ref_object_get_long(&root, "ts", &ts);
    if (ts)
      snprintf(pong, sizeof(pong),
               "{\"type\":\"pong\",\"ts\":%lld}", ts);
    else
      snprintf(pong, sizeof(pong), "{\"type\":\"pong\"}");
    (void)send_json(c, pong);
  } else {
    (void)ws_error(c, NULL, "UNSUPPORTED_TYPE",
                   "Unsupported FloofClawWS message type.");
  }
}

/* ----- ephemeral progress + durable final reconciliation ----------- */

static void drain_progress(WsAdapter *a, uint64_t now_ms) {
  RtJobProgress progress;
  while (rt_jobrunner_progress_pop(&a->scheduler->runner, &progress) == 0) {
    JsonRef root;
    char kind[64] = "", delta[RT_LARGE] = "";
    WsInflight *f = find_inflight_origin(a, progress.origin_event_id);
    size_t len;
    if (!f || f->terminal ||
        json_ref_top_object(progress.record, &root) != 0 ||
        json_ref_object_get_string(&root, "kind", kind, sizeof(kind)) != 0 ||
        strcmp(kind, "assistant_text_delta") != 0 ||
        json_ref_object_get_string(&root, "delta", delta,
                                   sizeof(delta)) != 0 ||
        !delta[0])
      continue;
    if (!f->run_id[0])
      snprintf(f->run_id, sizeof(f->run_id), "%s", progress.run_id);
    if (!f->accepted_sent) {
      send_status(a, f, "accepted");
      f->accepted_sent = 1;
    }
    len = strlen(delta);
    if (f->stream_len + len + 1 > sizeof(f->stream_body)) {
      f->stream_dropped = 1;
      continue;
    }
    memcpy(f->stream_body + f->stream_len, delta, len);
    f->stream_len += len;
    f->stream_body[f->stream_len] = '\0';
    if (f->pending_len + len > sizeof(f->pending)) flush_pending(a, f);
    if (len > sizeof(f->pending)) {
      f->stream_dropped = 1;
      continue;
    }
    memcpy(f->pending + f->pending_len, delta, len);
    f->pending_len += len;
    if (!f->flush_at_ms) f->flush_at_ms = now_ms + WS_STREAM_FLUSH_MS;
  }
}

static void reconcile_delivery(WsAdapter *a, const char *line) {
  JsonRef root, delivery, ref;
  char ch[32] = "", aid[64] = "", origin[RT_SMALL] = "";
  char request_id[RT_SMALL] = "";
  char session[RT_SMALL] = "", corr[RT_SMALL] = "", body[RT_LARGE] = "";
  char run_id[RT_SMALL] = "", delivery_id[RT_SMALL] = "";
  WsInflight *f;
  if (json_ref_first_object(line, &root) != 0 ||
      json_ref_object_get_string(&root, "request_id",
                                 request_id, sizeof(request_id)) != 0 ||
      json_ref_object_get_object(&root, "delivery", &delivery) != 0 ||
      json_ref_object_get_string(&delivery, "channel", ch, sizeof(ch)) != 0 ||
      strcmp(ch, "ws") != 0 ||
      json_ref_object_get_string(&delivery, "adapter_id",
                                 aid, sizeof(aid)) != 0 ||
      strcmp(aid, a->module_id_buf) != 0 ||
      json_ref_object_get_string(&delivery, "origin_event_id",
                                 origin, sizeof(origin)) != 0 ||
      json_ref_object_get_string(&delivery, "delivery_id",
                                 delivery_id, sizeof(delivery_id)) != 0 ||
      json_ref_object_get_string(&delivery, "text", body, sizeof(body)) != 0 ||
      json_ref_object_get_object(&delivery, "ref", &ref) != 0 ||
      json_ref_object_get_string(&ref, "session_id",
                                 session, sizeof(session)) != 0 ||
      json_ref_object_get_string(&ref, "corr_id", corr, sizeof(corr)) != 0)
    return;
  /* The ordinary ports layer durably records failed runs as deliveries so
   * CLI/channel clients receive a terminal failure. That sentinel is not an
   * assistant message and must never be reconciled as reply_done. */
  if (strcmp(request_id, "run_failed") == 0) return;
  f = find_inflight_origin(a, origin);
  if (!f) {
    f = alloc_inflight(a);
    if (!f) return;
    snprintf(f->session_id, sizeof(f->session_id), "%s", session);
    snprintf(f->corr_id, sizeof(f->corr_id), "%s", corr);
    snprintf(f->origin_event_id, sizeof(f->origin_event_id), "%s", origin);
  }
  if (f->terminal && f->canceled) return;
  (void)json_ref_object_get_string(&root, "run_id",
                                   run_id, sizeof(run_id));
  snprintf(f->run_id, sizeof(f->run_id), "%s", run_id);
  snprintf(f->delivery_id, sizeof(f->delivery_id), "%s", delivery_id);
  snprintf(f->final_body, sizeof(f->final_body), "%s", body);
  flush_pending(a, f);
  if (f->stream_len > 0 && strcmp(f->stream_body, body) != 0) {
    error_to_session(a, f->session_id, f->corr_id,
                     "STREAM_MISMATCH",
                     "Streamed text did not match the committed reply.");
    f->terminal = 1;
    return;
  }
  if (f->stream_dropped && f->chunks_sent > 0) {
    error_to_session(a, f->session_id, f->corr_id,
                     "STREAM_DROPPED",
                     "Ephemeral stream backpressure prevented reconciliation.");
    f->terminal = 1;
    return;
  }
  if (f->stream_len == 0)
    (void)send_chunk(a, f, body, strlen(body));
  send_done(a, f);
  f->terminal = 1;
}

static void drain_deliveries(WsAdapter *a) {
  long long total = fs_file_size(WS_DELIVERIES_LOG);
  FILE *fp;
  /* stat, not a full read: the tick only needs the end offset, and the
   * live log runs to 64 MiB before the janitor rotates it. */
  if (!a->delivery_cursor_init) {
    a->delivery_cursor = total > 0 ? total : 0;
    a->delivery_cursor_init = 1;
    return;
  }
  if (total < 0) return;
  /* Rotation detected: the janitor renamed the live file and the append
   * site created a fresh one. Start from the top of the new file — the
   * IRC and Discord tailers have always done this; this one did not, so
   * a rotation stranded its cursor past every future delivery. */
  if (total < a->delivery_cursor) a->delivery_cursor = 0;
  if (total <= a->delivery_cursor) return;
  fp = fopen(WS_DELIVERIES_LOG, "rb");
  if (!fp) return;
  if (fseek(fp, a->delivery_cursor, SEEK_SET) == 0) {
    char line[RT_LARGE * 3];
    while (fgets(line, sizeof(line), fp)) {
      size_t len = strlen(line);
      if (len && line[len - 1] == '\n') line[len - 1] = '\0';
      reconcile_delivery(a, line);
    }
  }
  fclose(fp);
  a->delivery_cursor = total;
}

static int run_terminal_kind(const char *run_id, char *kind,
                             size_t kind_len) {
  char path[PATH_MAX], *text = NULL;
  int found = 0;
  if (!run_id || !*run_id ||
      snprintf(path, sizeof(path),
               "workspace/runs/%s/event_log.jsonl", run_id) >=
          (int)sizeof(path) ||
      fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return 0;
  if (strstr(text, "\"type\":\"run_canceled\"")) {
    snprintf(kind, kind_len, "run_canceled"); found = 1;
  } else if (strstr(text, "\"type\":\"run_failed\"")) {
    snprintf(kind, kind_len, "run_failed"); found = 1;
  } else if (strstr(text, "\"type\":\"run_done\"")) {
    snprintf(kind, kind_len, "run_done"); found = 1;
  }
  free(text);
  return found;
}

static void sync_inflight_runs(WsAdapter *a) {
  for (size_t i = 0; i < WS_MAX_INFLIGHT; ++i) {
    WsInflight *f = &a->inflight[i];
    char run_id[RT_SMALL] = "", terminal[RT_SMALL] = "";
    int active;
    if (!f->used) continue;
    active = rt_scheduler_lookup_origin(a->scheduler, f->origin_event_id,
                                        run_id, sizeof(run_id)) == 0;
    if (active && !f->run_id[0]) {
      snprintf(f->run_id, sizeof(f->run_id), "%s", run_id);
      if (!f->accepted_sent) {
        send_status(a, f, "accepted");
        f->accepted_sent = 1;
      }
    }
    if (f->cancel_requested && active)
      (void)rt_scheduler_cancel_origin(a->scheduler, f->origin_event_id,
                                       "client_canceled");
    if (f->terminal || !f->run_id[0] || active) continue;
    if (!run_terminal_kind(f->run_id, terminal, sizeof(terminal))) continue;
    if (strcmp(terminal, "run_canceled") == 0)
      error_to_session(a, f->session_id, f->corr_id,
                       "CANCELED", "Reply canceled.");
    else if (strcmp(terminal, "run_failed") == 0)
      error_to_session(a, f->session_id, f->corr_id,
                       "PROVIDER_ERROR", "The reply run failed.");
    else
      error_to_session(a, f->session_id, f->corr_id,
                       "NO_REPLY", "The run completed without a reply.");
    f->terminal = 1;
  }
}

/* ----- reactor callbacks ------------------------------------------ */

static void ws_schedule_reconnect(WsAdapter *a, uint64_t now_ms) {
  FcReconnectDelay delay;
  if (!a || !a->enabled) return;
  delay = fc_reconnect_backoff_next(&a->reconnect_backoff, now_ms);
  a->next_listen_ms = now_ms + delay.delay_ms;
  (void)rt_narrate("ws: reconnect in %llus (attempt %u)",
                   (unsigned long long)((delay.delay_ms + 999U) / 1000U),
                   delay.attempt);
}

static int ws_start_listener(WsAdapter *a, uint64_t now_ms) {
  if (!a || !a->enabled) return -1;
  if (listen_nonblocking(a->host, a->port, &a->listen_fd) != 0) {
    fprintf(stderr, "ws: failed to listen on loopback %s:%d\n",
            a->host, a->port);
    a->listen_fd = -1;
    ws_schedule_reconnect(a, now_ms);
    return -1;
  }
  a->next_listen_ms = 0;
  fc_reconnect_backoff_mark_connected(&a->reconnect_backoff, now_ms);
  return 0;
}

static int ws_init(FcReactorModule *module) {
  WsAdapter *a = (WsAdapter *)module->state;
  if (!a || !a->enabled) return 0;
  fc_reconnect_backoff_init(&a->reconnect_backoff, a->module_id_buf);
  return ws_start_listener(a, fc_now_ms());
}

static int ws_collect_fds(FcReactorModule *module, FcPollSet *ps) {
  WsAdapter *a = (WsAdapter *)module->state;
  if (!a || !a->enabled) return 0;
  if (a->listen_fd >= 0)
    (void)fc_pollset_add(ps, a->listen_fd, POLLIN, module, NULL);
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i) {
    WsConn *c = &a->clients[i];
    short events;
    if (c->state == WSC_FREE) continue;
    events = POLLIN;
    if (c->tx_len) events |= POLLOUT;
    (void)fc_pollset_add(ps, c->fd, events, module, c);
  }
  return 0;
}

static int ws_on_fd(FcReactorModule *module, int fd, int revents,
                    void *tag) {
  WsAdapter *a = (WsAdapter *)module->state;
  WsConn *c = (WsConn *)tag;
  uint64_t now_ms = fc_now_ms();
  if (!a || !a->enabled) return 0;
  if (fd == a->listen_fd && (revents & (POLLERR | POLLHUP | POLLNVAL))) {
    close(a->listen_fd); a->listen_fd = -1;
    ws_schedule_reconnect(a, now_ms);
    return 0;
  }
  if (fd == a->listen_fd && (revents & POLLIN)) {
    int client = accept(fd, NULL, NULL);
    while (client >= 0) {
      WsConn *slot = find_free_slot(a);
      if (!slot) { close(client); break; }
      (void)net_set_nonblocking(client, 1);
      memset(slot, 0, sizeof(*slot));
      slot->fd = client;
      slot->state = WSC_HANDSHAKING;
      slot->parse_progress_ms = now_ms;
      client = accept(fd, NULL, NULL);
    }
    return 0;
  }
  if (!c) return 0;
  if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
    close_conn(c);
    return 0;
  }
  if (revents & POLLIN) {
    char buf[4096];
    ssize_t n;
    while ((n = net_read_nb(c->fd, buf, sizeof(buf))) > 0) {
      if ((size_t)n >= sizeof(c->rx) - c->rx_len) {
        if (c->state == WSC_HANDSHAKING)
          (void)http_error(c, 413, "REQUEST_TOO_LARGE",
                           "Request head exceeds 16 KiB.");
        else
          close_conn(c);
        return 0;
      }
      memcpy(c->rx + c->rx_len, buf, (size_t)n);
      c->rx_len += (size_t)n;
      c->parse_progress_ms = now_ms;
    }
    if (n == 0 ||
        (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
         errno != EINTR)) {
      close_conn(c);
      return 0;
    }
    if (c->state == WSC_HANDSHAKING) {
      c->rx[c->rx_len] = '\0';
      if (handle_request_head(a, c) < 0) {
        close_conn(c);
        return 0;
      }
    }
    while (c->state == WSC_OPEN) {
      char text[WS_RX_BUF];
      size_t text_len = 0;
      int opcode = 0;
      int frame = extract_frame(c, text, sizeof(text),
                                &text_len, &opcode);
      if (frame < 0) { close_conn(c); return 0; }
      if (frame == 0) break;
      c->parse_progress_ms =
          c->rx_len || !c->authenticated ? now_ms : 0;
      if (opcode == 0x8) { close_conn(c); break; }
      if (opcode == 0x9) {
        unsigned char pong[2] = {0x8a, 0x00};
        if (conn_append_tx(c, pong, sizeof(pong)) != 0) close_conn(c);
        continue;
      }
      if (opcode != 0x1) {
        close_conn(c);
        break;
      }
      handle_protocol_text(a, c, text);
    }
  }
  if (c->state != WSC_FREE && (revents & POLLOUT)) {
    if (conn_drain_tx(c) != 0) close_conn(c);
    else if (c->http_stream && pump_http_record_stream(c) != 0) close_conn(c);
    else if (c->close_after_write && c->tx_len == 0) close_conn(c);
  }
  return 0;
}

static int ws_tick(FcReactorModule *module, uint64_t now_ms) {
  WsAdapter *a = (WsAdapter *)module->state;
  if (!a || !a->enabled) return 0;
  if (a->listen_fd < 0 && now_ms >= a->next_listen_ms)
    (void)ws_start_listener(a, now_ms);
  if (a->listen_fd >= 0)
    (void)fc_reconnect_backoff_maybe_reset(&a->reconnect_backoff, now_ms);
  drain_progress(a, now_ms);
  /* A successful run appends its delivery before it becomes terminal.
   * Reconcile that durable truth before terminal fallback classification,
   * otherwise one reactor pass can report NO_REPLY immediately ahead of
   * the delivery cursor. */
  drain_deliveries(a);
  sync_inflight_runs(a);
  for (size_t i = 0; i < WS_MAX_INFLIGHT; ++i) {
    WsInflight *f = &a->inflight[i];
    if (f->used && !f->terminal && f->pending_len &&
        f->flush_at_ms && now_ms >= f->flush_at_ms)
      flush_pending(a, f);
  }
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i) {
    WsConn *c = &a->clients[i];
    int waiting_for_parse =
        c->state == WSC_HANDSHAKING ||
        (c->state == WSC_OPEN && (!c->authenticated || c->rx_len > 0));
    if (waiting_for_parse &&
        net_progress_timed_out(c->parse_progress_ms, now_ms,
                               WS_PARSE_PROGRESS_TIMEOUT_MS)) {
      (void)rt_narrate(
          "ws: client input made no parse progress for %llus; closing client",
          (unsigned long long)(WS_PARSE_PROGRESS_TIMEOUT_MS / 1000U));
      if (c->state == WSC_HANDSHAKING) {
        (void)http_error(c, 408, "REQUEST_TIMEOUT",
                         "The request made no parse progress.");
        c->parse_progress_ms = 0;
      } else {
        close_conn(c);
      }
      continue;
    }
    if (c->state != WSC_FREE && c->tx_len > 0) {
      if (conn_drain_tx(c) != 0) close_conn(c);
      else if (c->http_stream && pump_http_record_stream(c) != 0) close_conn(c);
      else if (c->close_after_write && c->tx_len == 0) close_conn(c);
    } else if (c->state != WSC_FREE && c->http_stream) {
      if (pump_http_record_stream(c) != 0) close_conn(c);
    }
  }
  return 0;
}

static uint64_t ws_next_deadline(FcReactorModule *module,
                                 uint64_t now_ms) {
  WsAdapter *a = (WsAdapter *)module->state;
  uint64_t deadline = FC_NO_DEADLINE;
  if (!a || !a->enabled) return FC_NO_DEADLINE;
  if (a->listen_fd < 0) deadline = a->next_listen_ms;
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i) {
    WsConn *c = &a->clients[i];
    int waiting_for_parse =
        c->state == WSC_HANDSHAKING ||
        (c->state == WSC_OPEN && (!c->authenticated || c->rx_len > 0));
    uint64_t d = c->parse_progress_ms + WS_PARSE_PROGRESS_TIMEOUT_MS;
    if (waiting_for_parse && c->parse_progress_ms && d < deadline)
      deadline = d;
    if (c->http_stream && now_ms < deadline) deadline = now_ms;
  }
  for (size_t i = 0; i < WS_MAX_INFLIGHT; ++i) {
    WsInflight *f = &a->inflight[i];
    if (f->used && !f->terminal && f->pending_len &&
        f->flush_at_ms && f->flush_at_ms < deadline)
      deadline = f->flush_at_ms;
  }
  (void)now_ms;
  return deadline;
}

static void ws_shutdown_cb(FcReactorModule *module) {
  WsAdapter *a = (WsAdapter *)module->state;
  if (!a) return;
  a->enabled = 0;
  if (a->listen_fd >= 0) close(a->listen_fd);
  a->listen_fd = -1;
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i)
    if (a->clients[i].state != WSC_FREE) close_conn(&a->clients[i]);
  secret_store_zero(a->token, sizeof(a->token));
}

/* ----- configuration + factory ------------------------------------ */

static int load_config(WsAdapter *a) {
  char *text = NULL;
  JsonRef root, channels, ws, gateway;
  int enabled = 0;
  long long port = 0;
  if (!a) return -1;
  memset(a, 0, sizeof(*a));
  a->listen_fd = -1;
  for (size_t i = 0; i < WS_MAX_CLIENTS; ++i) a->clients[i].fd = -1;
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "ws-main");
  snprintf(a->path, sizeof(a->path), "%s", WS_PATH_DEFAULT);
  snprintf(a->host, sizeof(a->host), "127.0.0.1");
  if (fs_read_text("config/floofclaw_config.json", &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  if (json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_object(&root, "channels", &channels) != 0 ||
      json_ref_object_get_object(&channels, "ws", &ws) != 0) {
    free(text);
    return -1;
  }
  (void)json_ref_object_get_bool(&ws, "enabled", &enabled);
  if (!enabled) { free(text); return 0; }
  (void)json_ref_object_get_string(&ws, "path", a->path, sizeof(a->path));
  if (json_ref_object_get_object(&root, "gateway", &gateway) == 0) {
    (void)json_ref_object_get_string(&gateway, "host",
                                     a->host, sizeof(a->host));
    (void)json_ref_object_get_long(&gateway, "port", &port);
  }
  free(text);
  if (strcmp(a->host, "127.0.0.1") != 0 &&
      strcmp(a->host, "localhost") != 0)
    return -1;
  if (port <= 0 || port > 65535 || a->path[0] != '/') return -1;
  if (secret_store_get("endpoint:local_client_api",
                       a->token, sizeof(a->token)) != 0)
    return -2;
  a->token_len = strlen(a->token);
  a->port = (int)port;
  a->enabled = 1;
  return 1;
}

static FcReactorModule *ws_create(struct RtScheduler *scheduler,
                                      int *startup_error) {
  FcReactorModule *module;
  WsAdapter *a = (WsAdapter *)calloc(1, sizeof(*a));
  int config_rc;
  if (startup_error) *startup_error = 0;
  if (!a) return NULL;
  config_rc = load_config(a);
  if (config_rc == -2) {
    fprintf(stderr,
            "ws: missing local client token; fix: printf 'TOKEN' | "
            "./bin/fclaw auth set-stdin local_client_api\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  if (config_rc < 0) {
    fprintf(stderr,
            "ws: invalid config; fix: use a loopback gateway host, valid "
            "port, and channels.ws path in config/floofclaw_config.json\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  if (config_rc == 0) { free(a); return NULL; }
#ifndef FCLAW_HAVE_OPENSSL
  fprintf(stderr,
          "ws: OpenSSL is required for the WebSocket handshake; "
          "fix: install OpenSSL 3 and run make clean && make\n");
  if (startup_error) *startup_error = 1;
  secret_store_zero(a->token, sizeof(a->token));
  free(a);
  return NULL;
#endif
  module = (FcReactorModule *)calloc(1, sizeof(*module));
  if (!module) {
    secret_store_zero(a->token, sizeof(a->token));
    free(a);
    return NULL;
  }
  a->scheduler = scheduler;
  module->name = "ws";
  module->module_id = a->module_id_buf;
  module->state = a;
  module->init = ws_init;
  module->collect_fds = ws_collect_fds;
  module->on_fd = ws_on_fd;
  module->tick = ws_tick;
  module->next_deadline_ms = ws_next_deadline;
  module->shutdown = ws_shutdown_cb;
  return module;
}

static void ws_destroy(FcReactorModule *adapter) {
  WsAdapter *a;
  if (!adapter) return;
  a = (WsAdapter *)adapter->state;
  if (a) {
    for (size_t i = 0; i < WS_MAX_CLIENTS; ++i)
      if (a->clients[i].state != WSC_FREE) close_conn(&a->clients[i]);
    secret_store_zero(a->token, sizeof(a->token));
    free(a);
  }
  free(adapter);
}

/* The contract this directory satisfies, stated where you can see it. */
const FcAdapter fc_adapter_ws = {
  .name    = "ws",
  .create  = ws_create,
  .destroy = ws_destroy,
};
