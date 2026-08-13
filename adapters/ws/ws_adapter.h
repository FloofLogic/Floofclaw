#ifndef FCLAW_WS_ADAPTER_H
#define FCLAW_WS_ADAPTER_H

#include "../../runtime/gateway/adapter.h"
#include "../../runtime/runtime_kernel.h"

/* Authenticated loopback local-client listener. Owns fixed read-only
 * /v1/health, /v1/pulse, and /v1/usage routes plus the FloofClawWS 1.0
 * upgrade at channels.ws.path (default /v1/fchat). Inbound chat remains an
 * ordinary bus envelope; final replies remain committed deliveries.
 *
 * This is the one adapter that uses the scheduler the descriptor hands it;
 * the others ignore that argument. Disabled in config, or a build without
 * libssl (for SHA1), yields NULL. */
extern const FcAdapter fc_adapter_ws;

#endif
