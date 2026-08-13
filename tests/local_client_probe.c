#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PROBE_CAP 131072

static int write_all(int fd, const void *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, (const char *)buf + sent, len - sent);
    if (n > 0) { sent += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static int read_exact(int fd, void *buf, size_t len) {
  size_t used = 0;
  while (used < len) {
    ssize_t n = read(fd, (char *)buf + used, len - used);
    if (n > 0) { used += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static int connect_local(int port) {
  struct sockaddr_in addr;
  struct timeval timeout = {5, 0};
  for (int attempt = 0; attempt < 100; ++attempt) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
      (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout));
      (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                       &timeout, sizeof(timeout));
      return fd;
    }
    close(fd);
    usleep(20000);
  }
  return -1;
}

static int http_raw(int port, const char *request,
                    int expected_status, const char *needle) {
  char status[64], response[PROBE_CAP];
  size_t used = 0;
  int fd = connect_local(port);
  if (fd < 0) return -1;
  if (write_all(fd, request, strlen(request)) != 0) {
    close(fd);
    return -1;
  }
  while (used + 1 < sizeof(response)) {
    ssize_t n = read(fd, response + used, sizeof(response) - used - 1);
    if (n > 0) { used += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  close(fd);
  response[used] = '\0';
  snprintf(status, sizeof(status), "HTTP/1.1 %d", expected_status);
  return strstr(response, status) && strstr(response, needle) ? 0 : -1;
}

static int http_get(int port, const char *token, const char *path,
                    int expected_status, const char *needle) {
  char request[1024];
  if (token) {
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
             path, token);
  } else {
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Connection: close\r\n\r\n", path);
  }
  return http_raw(port, request, expected_status, needle);
}

static int http_oversized_head(int port, const char *token) {
  char *request = (char *)malloc(20000);
  int n, rc;
  if (!request) return -1;
  n = snprintf(request, 20000,
               "GET /v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n"
               "Authorization: Bearer %s\r\nX-Fill: ", token);
  if (n < 0 || n >= 19000) { free(request); return -1; }
  memset(request + n, 'x', 19000U - (size_t)n);
  request[19000] = '\0';
  rc = http_raw(port, request, 413, "REQUEST_TOO_LARGE");
  free(request);
  return rc;
}

static int ws_send_text(int fd, const char *text) {
  unsigned char header[14];
  const unsigned char mask[4] = {0x19, 0x73, 0xa4, 0x5c};
  size_t len = strlen(text), hlen;
  char *frame;
  size_t total;
  if (len > 65535U) return -1;
  header[0] = 0x81;
  if (len < 126U) {
    header[1] = (unsigned char)(0x80U | len);
    memcpy(header + 2, mask, 4);
    hlen = 6;
  } else {
    header[1] = 0xfe;
    header[2] = (unsigned char)(len >> 8);
    header[3] = (unsigned char)len;
    memcpy(header + 4, mask, 4);
    hlen = 8;
  }
  total = hlen + len;
  frame = (char *)malloc(total);
  if (!frame) return -1;
  memcpy(frame, header, hlen);
  for (size_t i = 0; i < len; ++i)
    frame[hlen + i] = (char)((unsigned char)text[i] ^ mask[i % 4]);
  if (write_all(fd, frame, total) != 0) {
    free(frame);
    return -1;
  }
  free(frame);
  return 0;
}

static int ws_read_text(int fd, char *out, size_t out_len) {
  unsigned char head[2], ext[8], mask[4];
  unsigned long long len;
  int masked;
  if (read_exact(fd, head, sizeof(head)) != 0) return -1;
  if ((head[0] & 0x0fU) == 0x8U) return -1;
  if ((head[0] & 0x0fU) != 0x1U) return -1;
  masked = (head[1] & 0x80U) != 0;
  len = head[1] & 0x7fU;
  if (len == 126U) {
    if (read_exact(fd, ext, 2) != 0) return -1;
    len = ((unsigned long long)ext[0] << 8) | ext[1];
  } else if (len == 127U) {
    if (read_exact(fd, ext, 8) != 0) return -1;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }
  if (masked && read_exact(fd, mask, sizeof(mask)) != 0) return -1;
  if (len + 1 > out_len) return -1;
  if (read_exact(fd, out, (size_t)len) != 0) return -1;
  if (masked)
    for (size_t i = 0; i < (size_t)len; ++i)
      out[i] = (char)((unsigned char)out[i] ^ mask[i % 4]);
  out[len] = '\0';
  return 0;
}

static int json_string_field(const char *json, const char *field,
                             char *out, size_t out_len) {
  char mark[128];
  const char *p, *e;
  snprintf(mark, sizeof(mark), "\"%s\":\"", field);
  p = strstr(json, mark);
  if (!p) return -1;
  p += strlen(mark);
  e = strchr(p, '"');
  if (!e || (size_t)(e - p) >= out_len) return -1;
  memcpy(out, p, (size_t)(e - p));
  out[e - p] = '\0';
  return 0;
}

static int ws_open(int port) {
  char request[2048], response[8192];
  size_t used = 0;
  int fd = connect_local(port);
  if (fd < 0) return -1;
  snprintf(request, sizeof(request),
           "GET /v1/fchat HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
  if (write_all(fd, request, strlen(request)) != 0) {
    close(fd);
    return -1;
  }
  while (used + 1 < sizeof(response)) {
    ssize_t n = read(fd, response + used, 1);
    if (n != 1) { close(fd); return -1; }
    used++;
    response[used] = '\0';
    if (strstr(response, "\r\n\r\n")) break;
  }
  if (!strstr(response, "HTTP/1.1 101")) { close(fd); return -1; }
  return fd;
}

static int ws_hello(int fd, const char *token, const char *session,
                    const char *after_delivery_id) {
  char frame[8192];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"hello\",\"protocol\":\"FloofClawWS\","
           "\"version\":\"1.0\",\"token\":\"%s\","
           "\"client_id\":\"probe\",\"session_id\":\"%s\"%s%s%s}",
           token, session,
           after_delivery_id ? ",\"after_delivery_id\":\"" : "",
           after_delivery_id ? after_delivery_id : "",
           after_delivery_id ? "\"" : "");
  return ws_send_text(fd, frame) == 0 &&
         ws_read_text(fd, frame, sizeof(frame)) == 0 &&
         strstr(frame, "\"type\":\"hello_ack\"") ? 0 : -1;
}

static int ws_expect_code(int fd, const char *code) {
  char frame[8192], needle[256];
  snprintf(needle, sizeof(needle), "\"code\":\"%s\"", code);
  for (int i = 0; i < 64; ++i)
    if (ws_read_text(fd, frame, sizeof(frame)) == 0 &&
        strstr(frame, "\"type\":\"error\"") && strstr(frame, needle))
      return 0;
  return -1;
}

static int websocket_security_probe(int port, const char *token) {
  char frame[8192];
  int fd = ws_open(port);
  if (fd < 0) return -1;
  if (ws_send_text(fd,
          "{\"type\":\"message\",\"id\":\"unauthorized\","
          "\"body\":\"must not publish\"}") != 0 ||
      ws_expect_code(fd, "UNAUTHORIZED") != 0) {
    close(fd);
    return -1;
  }
  close(fd);

  fd = ws_open(port);
  if (fd < 0) return -1;
  snprintf(frame, sizeof(frame),
           "{\"type\":\"hello\",\"protocol\":\"FloofClawWS\","
           "\"version\":\"1.0\",\"token\":\"wrong-token\","
           "\"client_id\":\"probe\",\"session_id\":\"sess_wrong\"}");
  if (ws_send_text(fd, frame) != 0 ||
      ws_expect_code(fd, "UNAUTHORIZED") != 0) {
    close(fd);
    return -1;
  }
  close(fd);

  fd = ws_open(port);
  if (fd < 0 || ws_hello(fd, token, "sess_cursor", "missing_delivery") != 0 ||
      ws_expect_code(fd, "CURSOR_EXPIRED") != 0) {
    if (fd >= 0) close(fd);
    return -1;
  }
  if (ws_send_text(fd, "{") != 0 ||
      ws_expect_code(fd, "BAD_REQUEST") != 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int websocket_probe(int port, const char *token) {
  char frame[8192];
  char chunks[8192] = "", delta[4096], body[4096];
  char first_delivery[128] = "";
  int saw_chunk = 0, saw_done = 0;
  int fd = ws_open(port);
  if (fd < 0 || ws_hello(fd, token, "sess_probe", NULL) != 0) {
    if (fd >= 0) close(fd);
    return -1;
  }
  if (ws_send_text(
          fd, "{\"type\":\"ping\",\"id\":\"ping_probe\"}") != 0 ||
      ws_read_text(fd, frame, sizeof(frame)) != 0 ||
      !strstr(frame, "\"type\":\"pong\"")) {
    close(fd);
    return -1;
  }
  if (ws_send_text(
          fd, "{\"type\":\"message\",\"id\":\"msg_probe\","
              "\"ctx_id\":\"main\",\"body\":\"hello\"}") != 0) {
    close(fd);
    return -1;
  }
  for (int i = 0; i < 128 && !saw_done; ++i) {
    if (ws_read_text(fd, frame, sizeof(frame)) != 0) {
      close(fd);
      return -1;
    }
    if (strstr(frame, "\"type\":\"reply_chunk\"")) {
      size_t have = strlen(chunks);
      if (json_string_field(frame, "delta", delta, sizeof(delta)) != 0 ||
          have + strlen(delta) + 1 > sizeof(chunks)) {
        close(fd);
        return -1;
      }
      strcat(chunks, delta);
      saw_chunk = 1;
    } else if (strstr(frame, "\"type\":\"reply_done\"")) {
      if (!saw_chunk ||
          json_string_field(frame, "body", body, sizeof(body)) != 0 ||
          strcmp(chunks, body) != 0 ||
          json_string_field(frame, "delivery_id", first_delivery,
                            sizeof(first_delivery)) != 0) {
        close(fd);
        return -1;
      }
      saw_done = 1;
    } else if (strstr(frame, "\"type\":\"error\"")) {
      fprintf(stderr, "probe websocket error: %s\n", frame);
      close(fd);
      return -1;
    }
  }
  if (!saw_done) { close(fd); return -1; }
  if (ws_send_text(
          fd, "{\"type\":\"message\",\"id\":\"msg_probe\","
              "\"ctx_id\":\"main\",\"body\":\"hello again\"}") != 0 ||
      ws_read_text(fd, frame, sizeof(frame)) != 0 ||
      !strstr(frame, "\"type\":\"reply_done\"")) {
    close(fd);
    return -1;
  }

  if (ws_send_text(
          fd, "{\"type\":\"message\",\"id\":\"msg_resume\","
              "\"ctx_id\":\"main\",\"body\":\"disconnect me\"}") != 0) {
    close(fd);
    return -1;
  }
  for (int i = 0; i < 64; ++i) {
    if (ws_read_text(fd, frame, sizeof(frame)) != 0) {
      close(fd);
      return -1;
    }
    if (strstr(frame, "\"type\":\"reply_chunk\"") &&
        strstr(frame, "\"corr_id\":\"msg_resume\""))
      break;
    if (i == 63) { close(fd); return -1; }
  }
  close(fd);

  usleep(500000);
  fd = ws_open(port);
  if (fd < 0 ||
      ws_hello(fd, token, "sess_probe", first_delivery) != 0) {
    if (fd >= 0) close(fd);
    return -1;
  }
  saw_done = 0;
  for (int i = 0; i < 64 && !saw_done; ++i) {
    if (ws_read_text(fd, frame, sizeof(frame)) != 0) {
      close(fd);
      return -1;
    }
    if (strstr(frame, "\"type\":\"reply_done\"") &&
        strstr(frame, "\"corr_id\":\"msg_resume\""))
      saw_done = 1;
  }
  if (!saw_done ||
      ws_send_text(
          fd, "{\"type\":\"message\",\"id\":\"msg_cancel\","
              "\"ctx_id\":\"main\",\"body\":\"cancel me\"}") != 0 ||
      ws_send_text(
          fd, "{\"type\":\"cancel\",\"corr_id\":\"msg_cancel\"}") != 0 ||
      ws_expect_code(fd, "CANCELED") != 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int websocket_terminal_probe(int port, const char *token,
                                    const char *session,
                                    const char *expected_error,
                                    int expected_chunks) {
  char frame[8192];
  int chunks = 0, done = 0;
  int fd = ws_open(port);
  if (fd < 0 || ws_hello(fd, token, session, NULL) != 0) {
    if (fd >= 0) close(fd);
    return -1;
  }
  if (ws_send_text(
          fd, "{\"type\":\"message\",\"id\":\"msg_terminal\","
              "\"ctx_id\":\"main\",\"body\":\"hello\"}") != 0) {
    close(fd);
    return -1;
  }
  for (int i = 0; i < 128 && !done; ++i) {
    if (ws_read_text(fd, frame, sizeof(frame)) != 0) {
      close(fd);
      return -1;
    }
    if (strstr(frame, "\"type\":\"reply_chunk\"")) chunks++;
    if (strstr(frame, "\"type\":\"reply_done\"")) {
      if (expected_error) { close(fd); return -1; }
      done = 1;
    }
    if (strstr(frame, "\"type\":\"error\"")) {
      char needle[256];
      if (!expected_error) { close(fd); return -1; }
      snprintf(needle, sizeof(needle), "\"code\":\"%s\"", expected_error);
      if (!strstr(frame, needle)) { close(fd); return -1; }
      done = 1;
    }
  }
  close(fd);
  return done && (expected_error || chunks == expected_chunks) ? 0 : -1;
}

int main(int argc, char **argv) {
  const char *mode;
  char request[2048];
  int port;
  if (argc < 3 || argc > 4) {
    fprintf(stderr, "usage: local_client_probe <port> <token> "
                    "[stream|fallback|error|overflow]\n");
    return 2;
  }
  port = atoi(argv[1]);
  mode = argc == 4 ? argv[3] : "stream";
  if (strcmp(mode, "fallback") == 0)
    return websocket_terminal_probe(port, argv[2], "sess_fallback",
                                    NULL, 1);
  if (strcmp(mode, "error") == 0)
    return websocket_terminal_probe(port, argv[2], "sess_error",
                                    "PROVIDER_ERROR", 0);
  if (strcmp(mode, "overflow") == 0)
    return websocket_terminal_probe(port, argv[2], "sess_overflow",
                                    "STREAM_MISMATCH", 0);

  snprintf(request, sizeof(request),
           "POST /v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
           argv[2]);
  if (http_get(port, NULL, "/v1/health", 401, "UNAUTHORIZED") != 0)
    { fprintf(stderr, "probe failed: missing HTTP auth\n"); return 1; }
  if (http_get(port, "wrong-token", "/v1/health", 401,
               "UNAUTHORIZED") != 0)
    { fprintf(stderr, "probe failed: wrong HTTP auth\n"); return 1; }
  if (http_get(port, argv[2], "/v1/health", 200,
               "\"schema_version\":1") != 0)
    { fprintf(stderr, "probe failed: health\n"); return 1; }
  if (http_get(port, argv[2], "/v1/pulse", 200,
               "\"schema_version\":1") != 0)
    { fprintf(stderr, "probe failed: pulse\n"); return 1; }
  if (http_get(port, argv[2], "/v1/usage", 200,
               "\"schema_version\":1") != 0)
    { fprintf(stderr, "probe failed: usage\n"); return 1; }
  if (http_get(port, argv[2], "/v1/usage/records", 200,
               "\"type\":\"scope\"") != 0)
    { fprintf(stderr, "probe failed: usage records\n"); return 1; }
  if (http_get(port, argv[2],
               "/v1/cacheview/records?agent=chat_manager&n=10", 200,
               "\"type\":\"scope\"") != 0)
    { fprintf(stderr, "probe failed: cacheview records\n"); return 1; }
  if (http_get(port, argv[2], "/v1/missing", 404,
               "NOT_FOUND") != 0)
    { fprintf(stderr, "probe failed: missing route\n"); return 1; }
  if (http_raw(port, request, 405, "METHOD_NOT_ALLOWED") != 0)
    { fprintf(stderr, "probe failed: method\n"); return 1; }
  snprintf(request, sizeof(request),
           "GET /v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Authorization: Bearer %s\r\nContent-Length: 1\r\n"
           "Connection: close\r\n\r\nx", argv[2]);
  if (http_raw(port, request, 400, "REQUEST_BODY_NOT_ALLOWED") != 0)
    { fprintf(stderr, "probe failed: request body\n"); return 1; }
  if (http_oversized_head(port, argv[2]) != 0)
    { fprintf(stderr, "probe failed: oversized request\n"); return 1; }
  if (websocket_security_probe(port, argv[2]) != 0)
    { fprintf(stderr, "probe failed: websocket security\n"); return 1; }
  if (websocket_probe(port, argv[2]) != 0)
    { fprintf(stderr, "probe failed: websocket lifecycle\n"); return 1; }
  puts("local client API v1 probe passed");
  return 0;
}
