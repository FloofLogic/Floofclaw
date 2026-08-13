#include "reconnect_backoff.h"

#include <limits.h>
#include <stddef.h>

static uint32_t identity_seed(const char *identity) {
  uint32_t hash = 2166136261U;
  const unsigned char *p = (const unsigned char *)(identity ? identity : "");
  while (*p) {
    hash ^= (uint32_t)*p++;
    hash *= 16777619U;
  }
  return hash ? hash : 0x9e3779b9U;
}

static uint32_t next_random(FcReconnectBackoff *backoff) {
  uint32_t x = backoff->random_state;
  if (x == 0) x = 0x9e3779b9U;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  backoff->random_state = x;
  return x;
}

static uint64_t nominal_delay(unsigned attempt) {
  uint64_t delay = FC_RECONNECT_BASE_MS;
  unsigned i;
  for (i = 1; i < attempt && delay < FC_RECONNECT_CAP_MS; ++i) {
    if (delay > FC_RECONNECT_CAP_MS / 2U) {
      delay = FC_RECONNECT_CAP_MS;
      break;
    }
    delay *= 2U;
  }
  return delay > FC_RECONNECT_CAP_MS ? FC_RECONNECT_CAP_MS : delay;
}

void fc_reconnect_backoff_init(FcReconnectBackoff *backoff,
                               const char *identity) {
  if (!backoff) return;
  backoff->attempt = 0;
  backoff->random_state = identity_seed(identity);
  backoff->stable_since_ms = 0;
  backoff->connected = 0;
}

void fc_reconnect_backoff_mark_connected(FcReconnectBackoff *backoff,
                                         uint64_t now_ms) {
  if (!backoff || backoff->connected) return;
  backoff->connected = 1;
  backoff->stable_since_ms = now_ms;
}

int fc_reconnect_backoff_maybe_reset(FcReconnectBackoff *backoff,
                                     uint64_t now_ms) {
  if (!backoff || !backoff->connected || now_ms < backoff->stable_since_ms ||
      now_ms - backoff->stable_since_ms < FC_RECONNECT_STABLE_MS)
    return 0;
  backoff->attempt = 0;
  /* Start a fresh stable interval so this remains an edge, not a level. */
  backoff->stable_since_ms = now_ms;
  return 1;
}

FcReconnectDelay fc_reconnect_backoff_next(FcReconnectBackoff *backoff,
                                            uint64_t now_ms) {
  FcReconnectDelay out = {0, FC_RECONNECT_BASE_MS};
  uint64_t nominal;
  uint64_t span;
  uint64_t width;
  uint64_t jittered;
  if (!backoff) return out;

  (void)fc_reconnect_backoff_maybe_reset(backoff, now_ms);
  backoff->connected = 0;
  backoff->stable_since_ms = 0;
  if (backoff->attempt < UINT_MAX) backoff->attempt++;
  out.attempt = backoff->attempt;

  nominal = nominal_delay(out.attempt);
  span = nominal / 5U; /* +/-20 percent. */
  width = span * 2U + 1U;
  jittered = nominal - span + ((uint64_t)next_random(backoff) % width);
  /* Five minutes is a hard operator-facing cap, including jitter. */
  out.delay_ms = jittered > FC_RECONNECT_CAP_MS
                     ? FC_RECONNECT_CAP_MS
                     : jittered;
  return out;
}
