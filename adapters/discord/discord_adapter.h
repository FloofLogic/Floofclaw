#ifndef FCLAW_DISCORD_ADAPTER_H
#define FCLAW_DISCORD_ADAPTER_H

#include "../../runtime/gateway/adapter.h"

/* Discord adapter. Loads config/floofclaw_config.json channels.discord.
 * If enabled, it connects to Discord Gateway over WSS, publishes attended
 * MESSAGE_CREATE payloads as ordinary bus user_message envelopes, and tails
 * workspace/logs/deliveries.jsonl to POST message action results back to
 * Discord through REST.
 *
 * Boundary: Discord protocol decisions stay here. The runtime sees the same
 * bus/delivery shape it sees from CLI, IRC, and WebSocket; it does not know
 * about Discord gateway opcodes, channel IDs, mention policy, REST payloads,
 * retry details, or message chunking. Token is resolved from secure storage
 * through channels.discord.token_key.
 *
 * v1 intentionally supports normal message-channel traffic first. Slash
 * command callbacks and typing indicators remain adapter-local follow-ups.
 *
 * The descriptor is the only symbol the gateway needs; create and destroy are
 * adapter-local. Disabled configuration yields NULL with startup_error 0;
 * enabled-but-broken sets startup_error. Nothing includes this header — the
 * generated registry declares the descriptor itself — so it exists for
 * readers. */
extern const FcAdapter fc_adapter_discord;

#endif
