#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int g_test_fail_next_dns;

void net_test_fail_next_dns(void) {
  g_test_fail_next_dns = 1;
}

void net_test_reset_dns(void) {
  g_test_fail_next_dns = 0;
}

int net_progress_timed_out(uint64_t started_ms, uint64_t now_ms,
                           uint64_t timeout_ms) {
  if (started_ms == 0 || timeout_ms == 0 || now_ms < started_ms) return 0;
  return now_ms - started_ms >= timeout_ms;
}

int net_set_nonblocking(int fd, int yes) {
  int flags;
  if (fd < 0) return -1;
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (yes) flags |= O_NONBLOCK;
  else     flags &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}

int net_connect_nb(const char *host, int port, int *out_fd) {
  struct addrinfo hints, *res = NULL, *cur;
  char port_str[16];
  int fd = -1;
  int rc;
  if (out_fd) *out_fd = -1;
  if (!host || !*host || port <= 0 || port > 65535 || !out_fd) return -1;
  snprintf(port_str, sizeof(port_str), "%d", port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  /* Intentionally blocking: see the accepted tradeoff in net.h. */
  if (g_test_fail_next_dns) {
    g_test_fail_next_dns = 0;
    return NET_CONNECT_DNS_FAILED;
  }
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
    return NET_CONNECT_DNS_FAILED;
  for (cur = res; cur; cur = cur->ai_next) {
    fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
    if (fd < 0) continue;
    if (net_set_nonblocking(fd, 1) != 0) { close(fd); fd = -1; continue; }
    rc = connect(fd, cur->ai_addr, cur->ai_addrlen);
    if (rc == 0 || errno == EINPROGRESS || errno == EALREADY) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return -1;
  *out_fd = fd;
  return 0;
}

int net_check_connected(int fd) {
  int err = 0;
  socklen_t len = sizeof(err);
  if (fd < 0) return -1;
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
  if (err == 0) return 1;
  if (err == EINPROGRESS || err == EALREADY) return 0;
  errno = err;
  return -1;
}

ssize_t net_read_nb(int fd, void *buf, size_t buf_len) {
  if (fd < 0 || !buf || buf_len == 0) { errno = EINVAL; return -1; }
  return read(fd, buf, buf_len);
}

ssize_t net_write_nb(int fd, const void *buf, size_t buf_len) {
  if (fd < 0 || !buf) { errno = EINVAL; return -1; }
  if (buf_len == 0) return 0;
  return write(fd, buf, buf_len);
}

void net_close(int fd) {
  if (fd >= 0) (void)close(fd);
}
