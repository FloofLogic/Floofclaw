#include "net_tls.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fc_tls_hostname_is_local(const char *hostname) {
  struct in_addr ipv4;
  struct in6_addr ipv6;
  size_t len;
  if (!hostname || !*hostname) return 0;
  if (strcmp(hostname, "localhost") == 0) return 1;
  len = strlen(hostname);
  if (len > 10U && strcmp(hostname + len - 10U, ".localhost") == 0) return 1;
  if (inet_pton(AF_INET, hostname, &ipv4) == 1)
    return (ntohl(ipv4.s_addr) >> 24) == 127U;
  if (inet_pton(AF_INET6, hostname, &ipv6) == 1)
    return IN6_IS_ADDR_LOOPBACK(&ipv6);
  return 0;
}

#ifdef FCLAW_HAVE_OPENSSL

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

struct FcTls {
  SSL_CTX *ctx;
  SSL *ssl;
  int fd;
  int verify_peer;
};

static int g_tls_initialized = 0;

static void tls_init_once(void) {
  if (g_tls_initialized) return;
  SSL_load_error_strings();
  SSL_library_init();
  OpenSSL_add_all_algorithms();
  g_tls_initialized = 1;
}

int fc_tls_available(void) { return 1; }

static int hostname_is_ip(const char *hostname) {
  unsigned char addr[sizeof(struct in6_addr)];
  if (!hostname || !*hostname) return 0;
  return inet_pton(AF_INET, hostname, addr) == 1 ||
         inet_pton(AF_INET6, hostname, addr) == 1;
}

static FcTls *tls_client_new(int fd, const char *hostname, int verify_peer) {
  FcTls *t;
  X509_VERIFY_PARAM *param;
  if (fd < 0 || !hostname || !*hostname) return NULL;
  if (!verify_peer && !fc_tls_hostname_is_local(hostname)) return NULL;
  tls_init_once();
  t = (FcTls *)calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->fd = fd;
  t->verify_peer = verify_peer;
  t->ctx = SSL_CTX_new(TLS_client_method());
  if (!t->ctx) { free(t); return NULL; }
  if (SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION) != 1) {
    SSL_CTX_free(t->ctx); free(t); return NULL;
  }
  if (verify_peer) {
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(t->ctx) != 1) {
      SSL_CTX_free(t->ctx); free(t); return NULL;
    }
  } else {
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_NONE, NULL);
  }
  t->ssl = SSL_new(t->ctx);
  if (!t->ssl) { SSL_CTX_free(t->ctx); free(t); return NULL; }
  if (SSL_set_fd(t->ssl, fd) != 1) {
    SSL_free(t->ssl); SSL_CTX_free(t->ctx); free(t); return NULL;
  }
  if (!hostname_is_ip(hostname) &&
      SSL_set_tlsext_host_name(t->ssl, hostname) != 1) {
    SSL_free(t->ssl); SSL_CTX_free(t->ctx); free(t); return NULL;
  }
  if (verify_peer) {
    param = SSL_get0_param(t->ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (hostname_is_ip(hostname)) {
      if (X509_VERIFY_PARAM_set1_ip_asc(param, hostname) != 1) {
        SSL_free(t->ssl); SSL_CTX_free(t->ctx); free(t); return NULL;
      }
    } else if (SSL_set1_host(t->ssl, hostname) != 1) {
      SSL_free(t->ssl); SSL_CTX_free(t->ctx); free(t); return NULL;
    }
  }
  return t;
}

FcTls *fc_tls_client_new(int fd, const char *hostname) {
  return tls_client_new(fd, hostname, 1);
}

FcTls *fc_tls_client_new_local_insecure(int fd, const char *hostname) {
  return tls_client_new(fd, hostname, 0);
}

static FcTlsResult translate(SSL *ssl, int rc) {
  int err = SSL_get_error(ssl, rc);
  switch (err) {
  case SSL_ERROR_NONE:        return FC_TLS_OK;
  case SSL_ERROR_WANT_READ:   return FC_TLS_WANT_READ;
  case SSL_ERROR_WANT_WRITE:  return FC_TLS_WANT_WRITE;
  case SSL_ERROR_ZERO_RETURN: return FC_TLS_CLOSED;
  default:                    return FC_TLS_ERROR;
  }
}

FcTlsResult fc_tls_handshake(FcTls *t) {
  int rc;
  if (!t || !t->ssl) return FC_TLS_ERROR;
  rc = SSL_connect(t->ssl);
  if (rc == 1) return FC_TLS_OK;
  return translate(t->ssl, rc);
}

int fc_tls_verify_error(FcTls *t, char *out, size_t out_len) {
  long result;
  const char *reason;
  if (out && out_len > 0) out[0] = '\0';
  if (!t || !t->ssl || !t->verify_peer) return 0;
  result = SSL_get_verify_result(t->ssl);
  if (result == X509_V_OK) return 0;
  reason = X509_verify_cert_error_string(result);
  if (out && out_len > 0)
    snprintf(out, out_len, "%s", reason ? reason : "certificate verify failed");
  return 1;
}

FcTlsResult fc_tls_read(FcTls *t, void *buf, size_t buf_len, size_t *bytes_out) {
  int rc;
  if (bytes_out) *bytes_out = 0;
  if (!t || !t->ssl || !buf) return FC_TLS_ERROR;
  if (buf_len == 0) return FC_TLS_OK;
  rc = SSL_read(t->ssl, buf, (int)buf_len);
  if (rc > 0) {
    if (bytes_out) *bytes_out = (size_t)rc;
    return FC_TLS_OK;
  }
  return translate(t->ssl, rc);
}

FcTlsResult fc_tls_write(FcTls *t, const void *buf, size_t buf_len, size_t *bytes_out) {
  int rc;
  if (bytes_out) *bytes_out = 0;
  if (!t || !t->ssl || !buf) return FC_TLS_ERROR;
  if (buf_len == 0) return FC_TLS_OK;
  rc = SSL_write(t->ssl, buf, (int)buf_len);
  if (rc > 0) {
    if (bytes_out) *bytes_out = (size_t)rc;
    return FC_TLS_OK;
  }
  return translate(t->ssl, rc);
}

FcTlsResult fc_tls_shutdown(FcTls *t) {
  int rc;
  if (!t || !t->ssl) return FC_TLS_ERROR;
  rc = SSL_shutdown(t->ssl);
  if (rc == 1) return FC_TLS_OK;
  if (rc == 0) return FC_TLS_WANT_READ; /* peer hasn't sent close_notify yet */
  return translate(t->ssl, rc);
}

void fc_tls_free(FcTls *t) {
  if (!t) return;
  if (t->ssl) SSL_free(t->ssl);
  if (t->ctx) SSL_CTX_free(t->ctx);
  free(t);
}

#else /* !FCLAW_HAVE_OPENSSL */

int fc_tls_available(void) { return 0; }
FcTls *fc_tls_client_new(int fd, const char *hostname) { (void)fd; (void)hostname; return NULL; }
FcTls *fc_tls_client_new_local_insecure(int fd, const char *hostname) { (void)fd; (void)hostname; return NULL; }
FcTlsResult fc_tls_handshake(FcTls *t)                              { (void)t; return FC_TLS_NOT_BUILT; }
int fc_tls_verify_error(FcTls *t, char *out, size_t out_len) { (void)t; if (out && out_len) out[0] = '\0'; return 0; }
FcTlsResult fc_tls_read(FcTls *t, void *b, size_t l, size_t *o)     { (void)t; (void)b; (void)l; (void)o; return FC_TLS_NOT_BUILT; }
FcTlsResult fc_tls_write(FcTls *t, const void *b, size_t l, size_t *o) { (void)t; (void)b; (void)l; (void)o; return FC_TLS_NOT_BUILT; }
FcTlsResult fc_tls_shutdown(FcTls *t)                               { (void)t; return FC_TLS_NOT_BUILT; }
void fc_tls_free(FcTls *t)                                           { (void)t; }

#endif
