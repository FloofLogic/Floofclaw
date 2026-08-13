#include "test_support.h"

#include "../../runtime/support/net_tls.h"

#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int tls_local_opt_out_is_loopback_only(void) {
  int sockets[2] = {-1, -1};
  FcTls *tls = NULL;
  int rc = 0;

  rc |= expect(fc_tls_hostname_is_local("localhost") == 1,
               "localhost is eligible for development opt-out");
  rc |= expect(fc_tls_hostname_is_local("dev.localhost") == 1,
               ".localhost name is eligible for development opt-out");
  rc |= expect(fc_tls_hostname_is_local("127.42.0.1") == 1,
               "IPv4 loopback is eligible for development opt-out");
  rc |= expect(fc_tls_hostname_is_local("::1") == 1,
               "IPv6 loopback is eligible for development opt-out");
  rc |= expect(fc_tls_hostname_is_local("192.168.1.10") == 0,
               "private network host cannot disable verification");
  rc |= expect(fc_tls_hostname_is_local("example.com") == 0,
               "public host cannot disable verification");

  rc |= expect(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
               "create local socket pair for TLS constructor policy");
  if (sockets[0] >= 0) {
    tls = fc_tls_client_new_local_insecure(sockets[0], "example.com");
    rc |= expect(tls == NULL,
                 "insecure TLS constructor rejects non-local hostname");
    tls = fc_tls_client_new_local_insecure(sockets[0], "localhost");
#ifdef FCLAW_HAVE_OPENSSL
    rc |= expect(tls != NULL,
                 "insecure TLS constructor accepts localhost explicitly");
#else
    rc |= expect(tls == NULL,
                 "TLS constructor remains unavailable without OpenSSL");
#endif
    fc_tls_free(tls);
  }
  if (sockets[0] >= 0) close(sockets[0]);
  if (sockets[1] >= 0) close(sockets[1]);
  return rc;
}

int tls_verify_defaults_and_channel_diagnostics_are_wired(void) {
  char *tls = NULL;
  char *irc = NULL;
  char *discord = NULL;
  char *discord_gateway = NULL;
  char *discord_rest = NULL;
  int rc = 0;

  rc |= test_read_file("runtime/support/net_tls.c", &tls);
  rc |= expect_substr(tls, "SSL_VERIFY_PEER",
                      "TLS default enables peer verification");
  rc |= expect_substr(tls, "SSL_CTX_set_default_verify_paths",
                      "TLS default loads system trust roots");
  rc |= expect_substr(tls, "SSL_set1_host",
                      "TLS default verifies DNS hostname");
  rc |= expect_substr(tls, "X509_VERIFY_PARAM_set1_ip_asc",
                      "TLS default verifies IP-address endpoints");
  rc |= expect_substr(tls, "return tls_client_new(fd, hostname, 1)",
                      "public TLS constructor is verify-on");

  rc |= test_read_file("adapters/irc/irc_adapter.c", &irc);
  rc |= expect_substr(irc, "a->tls_verify = 1",
                      "IRC verification defaults on");
  rc |= expect_substr(irc, "json_ref_object_get_bool(&irc, \"tls_verify\"",
                      "IRC owns a per-endpoint verification field");
  rc |= expect_substr(irc, "fc_tls_verify_error",
                      "IRC narrates certificate verification failure");

  rc |= test_read_file("adapters/discord/discord_adapter.c", &discord);
  rc |= test_read_file("adapters/discord/discord_gateway.c", &discord_gateway);
  rc |= test_read_file("adapters/discord/discord_rest.c", &discord_rest);
  rc |= expect_substr(discord, "a->tls_verify = 1",
                      "Discord verification defaults on");
  rc |= expect_substr(discord, "fc_tls_verify_error",
                      "Discord shared helper narrates verification failure");
  rc |= expect_substr(discord_gateway, "dc_narrate_tls_failure",
                      "Discord Gateway reports verification failure");
  rc |= expect_substr(discord_rest, "dc_narrate_tls_failure",
                      "Discord REST reports verification failure");

  free(discord_rest); free(discord_gateway); free(discord);
  free(irc); free(tls);
  return rc;
}
