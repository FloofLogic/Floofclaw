#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
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

int net_connect_addr_nb(const struct sockaddr *addr, socklen_t addrlen,
                        int *out_fd) {
  int fd, rc;
  if (out_fd) *out_fd = -1;
  if (!addr || addrlen == 0 || !out_fd) return -1;
  fd = socket(addr->sa_family, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (net_set_nonblocking(fd, 1) != 0) { close(fd); return -1; }
  rc = connect(fd, addr, addrlen);
  if (rc == 0 || errno == EINPROGRESS || errno == EALREADY) {
    *out_fd = fd;
    return 0;
  }
  close(fd);
  return -1;
}

/* ---- Asynchronous resolver -------------------------------------------
 *
 * getaddrinfo() has no portable timeout and blocks the whole reactor
 * thread. The reactor's own rule is that blocking work runs in a child
 * whose fd the loop polls (see runtime/gateway/reactor.h); this applies
 * that rule to name resolution. A child runs getaddrinfo and streams each
 * resolved sockaddr — one length byte then that many bytes — down a pipe,
 * then closes it (EOF = done, zero records = lookup failed). The parent
 * only ever does nonblocking reads on the pipe. */

#define NET_RESOLVE_MAX_ADDRS 8

struct NetResolve {
  pid_t pid;   /* resolver child, or -1 when none was forked */
  int rfd;     /* pollable read end of the pipe, or -1 */
  int reaped;  /* child already waited on */
  int eof;     /* child closed the pipe */
  unsigned char buf[NET_RESOLVE_MAX_ADDRS * (1 + sizeof(struct sockaddr_storage))];
  size_t buf_len;
};

static int net_write_all(int fd, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n > 0) { off += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

/* In the resolver child, drop every inherited descriptor except the pipe
 * write end, so a child stuck in a slow lookup cannot pin a socket the
 * parent has closed. */
static void net_resolve_child_close_fds(int keep) {
  long max = sysconf(_SC_OPEN_MAX);
  int fd;
  if (max < 0 || max > 4096) max = 4096;
  for (fd = 3; fd < (int)max; ++fd)
    if (fd != keep) (void)close(fd);
}

static void net_resolve_child(int wfd, const char *host, int port) {
  struct addrinfo hints, *res = NULL, *cur;
  char port_str[16];
  int count = 0;
  snprintf(port_str, sizeof(port_str), "%d", port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port_str, &hints, &res) == 0 && res) {
    for (cur = res; cur && count < NET_RESOLVE_MAX_ADDRS; cur = cur->ai_next) {
      unsigned char len;
      if (cur->ai_addrlen == 0 || cur->ai_addrlen > 255) continue;
      len = (unsigned char)cur->ai_addrlen;
      if (net_write_all(wfd, &len, 1) != 0) break;
      if (net_write_all(wfd, cur->ai_addr, len) != 0) break;
      ++count;
    }
  }
  if (res) freeaddrinfo(res);
}

static void net_resolve_reap(NetResolve *r) {
  if (r && r->pid > 0 && !r->reaped) {
    int st;
    while (waitpid(r->pid, &st, 0) < 0 && errno == EINTR) {}
    r->reaped = 1;
  }
}

NetResolve *net_resolve_begin(const char *host, int port) {
  NetResolve *r;
  int fds[2];
  pid_t pid;
  if (!host || !*host || port <= 0 || port > 65535) return NULL;
  r = (NetResolve *)calloc(1, sizeof(*r));
  if (!r) return NULL;
  r->pid = -1;
  r->rfd = -1;
  if (pipe(fds) != 0) { free(r); return NULL; }
  /* Test hook: report a DNS failure through the same pollable-fd contract
   * without forking. Closing the write end now leaves the read end at EOF
   * with zero records, which net_resolve_connect reads as a failed lookup. */
  if (g_test_fail_next_dns) {
    g_test_fail_next_dns = 0;
    (void)close(fds[1]);
    (void)net_set_nonblocking(fds[0], 1);
    r->rfd = fds[0];
    return r;
  }
  pid = fork();
  if (pid < 0) { (void)close(fds[0]); (void)close(fds[1]); free(r); return NULL; }
  if (pid == 0) {
    (void)close(fds[0]);
    net_resolve_child_close_fds(fds[1]);
    net_resolve_child(fds[1], host, port);
    _exit(0);
  }
  (void)close(fds[1]);
  (void)net_set_nonblocking(fds[0], 1);
  r->pid = pid;
  r->rfd = fds[0];
  return r;
}

int net_resolve_fd(const NetResolve *r) {
  return r ? r->rfd : -1;
}

int net_resolve_connect(NetResolve *r, int *out_fd) {
  size_t off = 0;
  if (out_fd) *out_fd = -1;
  if (!r || !out_fd) return -1;
  while (!r->eof) {
    ssize_t n;
    if (r->buf_len >= sizeof(r->buf)) { r->eof = 1; break; }
    n = read(r->rfd, r->buf + r->buf_len, sizeof(r->buf) - r->buf_len);
    if (n > 0) { r->buf_len += (size_t)n; continue; }
    if (n == 0) { r->eof = 1; break; }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 1; /* still resolving */
    r->eof = 1;                                            /* read error */
    net_resolve_reap(r);
    return NET_CONNECT_DNS_FAILED;
  }
  net_resolve_reap(r);
  while (off < r->buf_len) {
    unsigned char len = r->buf[off];
    struct sockaddr_storage ss;
    if (len == 0 || off + 1U + len > r->buf_len) break;
    memcpy(&ss, r->buf + off + 1, len);
    if (net_connect_addr_nb((struct sockaddr *)&ss, (socklen_t)len, out_fd) == 0)
      return 0;
    off += 1U + len;
  }
  return r->buf_len == 0 ? NET_CONNECT_DNS_FAILED : -1;
}

void net_resolve_free(NetResolve *r) {
  if (!r) return;
  if (r->pid > 0 && !r->reaped) {
    (void)kill(r->pid, SIGKILL);
    net_resolve_reap(r);
  }
  if (r->rfd >= 0) (void)close(r->rfd);
  free(r);
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
