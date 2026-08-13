#ifndef FCLAW_SUPPORT_RECONNECT_BACKOFF_H
#define FCLAW_SUPPORT_RECONNECT_BACKOFF_H

#include <stdint.h>

#define FC_RECONNECT_BASE_MS       1000ULL
#define FC_RECONNECT_CAP_MS      300000ULL
#define FC_RECONNECT_STABLE_MS    60000ULL

typedef struct {
  unsigned attempt;
  uint32_t random_state;
  uint64_t stable_since_ms;
  int connected;
} FcReconnectBackoff;

typedef struct {
  unsigned attempt;
  uint64_t delay_ms;
} FcReconnectDelay;

/* Identity seeds deterministic per-adapter jitter. The helper owns no clock,
 * I/O, narration, or channel state; adapters supply monotonic timestamps. */
void fc_reconnect_backoff_init(FcReconnectBackoff *backoff,
                               const char *identity);
void fc_reconnect_backoff_mark_connected(FcReconnectBackoff *backoff,
                                         uint64_t now_ms);
int fc_reconnect_backoff_maybe_reset(FcReconnectBackoff *backoff,
                                     uint64_t now_ms);
FcReconnectDelay fc_reconnect_backoff_next(FcReconnectBackoff *backoff,
                                            uint64_t now_ms);

#endif
