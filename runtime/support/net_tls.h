#ifndef FCLAW_SUPPORT_NET_TLS_H
#define FCLAW_SUPPORT_NET_TLS_H

#include <stddef.h>
#include <sys/types.h>

/* Nonblocking TLS wrapper around an already-connected, already-nonblocking
 * socket fd. The reactor owns the fd; this layer just translates OpenSSL's
 * WANT_READ / WANT_WRITE into pollset interest hints the adapter can act on.
 *
 * Reactor boundary rule: no internal retry loops. Every call returns
 * promptly with FC_TLS_WANT_READ or FC_TLS_WANT_WRITE if it would block.
 *
 * Built only when -DFCLAW_HAVE_OPENSSL is set; otherwise every entry point
 * returns FC_TLS_NOT_BUILT so adapters can fail with a clear message. */

typedef struct FcTls FcTls;

typedef enum {
  FC_TLS_OK = 0,         /* completed successfully */
  FC_TLS_WANT_READ = 1,  /* poll for POLLIN, then call again */
  FC_TLS_WANT_WRITE = 2, /* poll for POLLOUT, then call again */
  FC_TLS_CLOSED = 3,     /* peer closed cleanly */
  FC_TLS_ERROR = -1,     /* unrecoverable */
  FC_TLS_NOT_BUILT = -2  /* OpenSSL not compiled in */
} FcTlsResult;

/* Create a verified TLS client side bound to fd. The hostname is mandatory:
 * it is sent via SNI and checked against the peer certificate. System trust
 * roots are loaded by default. */
FcTls *fc_tls_client_new(int fd, const char *hostname);

/* Explicit development-only escape hatch. It returns NULL unless hostname is
 * localhost, a .localhost name, or a loopback IP address. */
FcTls *fc_tls_client_new_local_insecure(int fd, const char *hostname);
int fc_tls_hostname_is_local(const char *hostname);

/* Drive the TLS handshake. Call repeatedly when the fd is readable or
 * writable per the previous return value, until FC_TLS_OK or FC_TLS_ERROR. */
FcTlsResult fc_tls_handshake(FcTls *t);

/* After FC_TLS_ERROR, returns 1 and copies the certificate verification
 * reason when peer/hostname verification caused the failure. */
int fc_tls_verify_error(FcTls *t, char *out, size_t out_len);

/* After handshake completes, encrypt/decrypt application bytes. Same
 * WANT_READ / WANT_WRITE protocol — re-arm the pollset and call again. */
FcTlsResult fc_tls_read(FcTls *t, void *buf, size_t buf_len, size_t *bytes_out);
FcTlsResult fc_tls_write(FcTls *t, const void *buf, size_t buf_len, size_t *bytes_out);

/* Initiate a graceful shutdown. Returns FC_TLS_OK once both sides are done. */
FcTlsResult fc_tls_shutdown(FcTls *t);

/* Free the TLS context (does not close the fd). */
void fc_tls_free(FcTls *t);

/* Returns 1 if TLS is compiled in, 0 otherwise. Adapters check this in
 * init() when they're configured for TLS. */
int fc_tls_available(void);

#endif
