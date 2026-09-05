#ifndef FCLAW_SUPPORT_NET_H
#define FCLAW_SUPPORT_NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/* Nonblocking socket helpers used by channel adapters. Keep them tiny —
 * the reactor already drives the poll loop; these wrappers just translate
 * Unix calls into the "would block: try again later" idiom adapters need.
 *
 * getaddrinfo blocks, and the reactor is a single thread, so DNS runs off
 * the reactor: net_resolve_* resolves the host in a child and reports
 * through a pollable fd. Every channel adapter connects through it —
 * Discord gateway and REST, IRC, Telegram — then hands the address to
 * net_connect_addr_nb for the nonblocking connect. */

#define NET_CONNECT_DNS_FAILED (-2)

/* Connect to an already-resolved address, nonblocking. Same success
 * contract as net_connect_nb: on 0, *out_fd is a connect-in-progress
 * nonblocking fd (poll POLLOUT, then net_check_connected). Returns -1 and
 * leaves *out_fd = -1 when no socket could be opened to the address. */
int net_connect_addr_nb(const struct sockaddr *addr, socklen_t addrlen,
                        int *out_fd);

/* Asynchronous resolve-then-connect. getaddrinfo blocks for the platform
 * resolver timeout on a dead network, which would freeze the single
 * reactor thread (the accepted-tradeoff note above). This runs the lookup
 * in a short-lived child and hands back a pollable fd, so the reactor
 * keeps ticking every other module while a name resolves. Usage:
 *
 *   NetResolve *r = net_resolve_begin(host, port);   // returns at once
 *   // register net_resolve_fd(r) for POLLIN in the pollset each pass
 *   int fd; int rc = net_resolve_connect(r, &fd);    // when readable
 *     rc == 1                     still resolving; keep polling
 *     rc == 0                     *fd is a connect-in-progress fd; r is
 *                                 spent — free it and drive the connect
 *     rc == NET_CONNECT_DNS_FAILED  lookup failed
 *     rc == -1                    resolved, but no address was connectable
 *   net_resolve_free(r);          // always, exactly once
 *
 * A resolve that outlives the caller's own deadline is abandoned by
 * net_resolve_free, which kills the child. net_resolve_begin returns NULL
 * only on an immediate local failure (bad args, pipe/fork). */
typedef struct NetResolve NetResolve;
NetResolve *net_resolve_begin(const char *host, int port);
int net_resolve_fd(const NetResolve *r);
int net_resolve_connect(NetResolve *r, int *out_fd);
void net_resolve_free(NetResolve *r);

/* Test-only deterministic replacement for the next getaddrinfo call.
 * net_resolve_begin honors it, reporting the failure through the pollable
 * fd without forking. */
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
