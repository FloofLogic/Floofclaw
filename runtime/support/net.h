#ifndef FCLAW_SUPPORT_NET_H
#define FCLAW_SUPPORT_NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Nonblocking socket helpers used by channel adapters. Keep them tiny —
 * the reactor already drives the poll loop; these wrappers just translate
 * Unix calls into the "would block: try again later" idiom adapters need.
 *
 * DNS is intentionally synchronous. A failed lookup is an ordinary
 * connection failure handled by adapter backoff; a hung resolver can block
 * the reactor for the platform resolver timeout (accepted v1 tradeoff). */

#define NET_CONNECT_DNS_FAILED (-2)

/* Resolve+connect to host:port nonblocking. On success, returns 0 and
 * writes the new fd to *out_fd. The fd is already nonblocking. The
 * connect itself is in progress — caller must add the fd to the
 * pollset for POLLOUT and call net_check_connected once writable.
 *
 * DNS failure returns NET_CONNECT_DNS_FAILED. Other hard failures return -1.
 * Every failure leaves *out_fd = -1. */
int net_connect_nb(const char *host, int port, int *out_fd);

/* Test-only deterministic replacement for the next getaddrinfo call. */
void net_test_fail_next_dns(void);
void net_test_reset_dns(void);

/* Monotonic progress deadline. A zero start means no partial record is held. */
int net_progress_timed_out(uint64_t started_ms, uint64_t now_ms,
                           uint64_t timeout_ms);

/* Test whether a connect-in-progress fd has finished. Returns:
 *   1  connected and ready to read/write
 *   0  still in progress (poll for POLLOUT and try again)
 *  -1  connect failed; caller should close the fd */
int net_check_connected(int fd);

/* Set or clear O_NONBLOCK on fd. Returns 0 on success. */
int net_set_nonblocking(int fd, int yes);

/* Nonblocking read. Returns:
 *   >0  bytes read
 *    0  EOF (peer closed)
 *   -1  error; errno may be EAGAIN/EWOULDBLOCK (no data right now)
 *       or EINTR (interrupted; caller should retry). Other errno
 *       values are real I/O errors. */
ssize_t net_read_nb(int fd, void *buf, size_t buf_len);

/* Nonblocking write. Returns:
 *   >=0 bytes written (may be partial — caller must remember tail)
 *    -1 error; errno may be EAGAIN/EWOULDBLOCK (buffer full, retry on
 *       POLLOUT) or EINTR. */
ssize_t net_write_nb(int fd, const void *buf, size_t buf_len);

/* Best-effort close. Safe to call with fd < 0. */
void net_close(int fd);

#endif
