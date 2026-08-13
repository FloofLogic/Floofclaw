#ifndef FCLAW_IRC_ADAPTER_H
#define FCLAW_IRC_ADAPTER_H

#include "../../runtime/gateway/adapter.h"

/* IRC adapter that connects to a real IRC server, publishes inbound
 * PRIVMSGs as user_message bus envelopes, and tails
 * workspace/logs/deliveries.jsonl for outbound message deliveries
 * addressed to it.
 *
 * Configuration is read from config/floofclaw_config.json under
 * channels.irc.{enabled,server,port,tls,nick,username,realname,join,
 * require_mention_in_channel}.
 *
 * `tls`: when true, the adapter wraps the socket with OpenSSL via
 * runtime/support/net_tls.c. The build must include OpenSSL
 * (FCLAW_HAVE_OPENSSL); without it the adapter refuses to start
 * with a clear stderr message. Default port is 6697 when tls is
 * true and 6667 when it's false; either is overridden by an
 * explicit `port` in config.
 *
 * SASL is intentionally not implemented. The adapter targets
 * local / tailnet IRC servers where Tailscale (WireGuard) is the
 * security boundary and there's no services layer to authenticate
 * against. If you ever point it at Libera/OFTC, add SASL PLAIN
 * before doing so.
 *
 * The descriptor is the only symbol the gateway needs; create and destroy are
 * adapter-local. Not enabled or not configured yields NULL with
 * startup_error 0 — "no IRC adapter to register", not an error. */
extern const FcAdapter fc_adapter_irc;

#endif
