# TLS verification

FloofClaw verifies TLS peers by default. TLS clients load the operating
system/OpenSSL default trust roots, require a valid certificate chain, and
match the configured DNS hostname or IP address against the certificate.
Certificate failures are narrated with the channel, endpoint, and OpenSSL
verification reason before the adapter applies its reconnect policy.

`channels.irc.tls_verify` and `channels.discord.tls_verify` are optional and
default to `true`. Omitting the field is therefore the secure production
configuration.

The only supported opt-out is a development endpoint on `localhost`, a
`.localhost` name, `127.0.0.0/8`, or `::1`:

```json
{
  "channels": {
    "irc": {
      "enabled": true,
      "server": "localhost",
      "port": 6697,
      "tls": true,
      "tls_verify": false
    }
  }
}
```

An adapter refuses to start if `tls_verify` is false for a private-network or
public hostname. Discord uses fixed public endpoints, so its verification
cannot be disabled. Use a trusted certificate or a localhost development
proxy; do not use the opt-out to normalize an invalid production certificate.
