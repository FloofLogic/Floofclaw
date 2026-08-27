#include "test_support.h"

#include "../../runtime/support/reconnect_backoff.h"

#include <stdint.h>
#include <stdlib.h>

static uint64_t nominal_for_attempt(unsigned attempt) {
  uint64_t nominal = FC_RECONNECT_BASE_MS;
  unsigned i;
  for (i = 1; i < attempt && nominal < FC_RECONNECT_CAP_MS; ++i) {
    if (nominal > FC_RECONNECT_CAP_MS / 2U) return FC_RECONNECT_CAP_MS;
    nominal *= 2U;
  }
  return nominal > FC_RECONNECT_CAP_MS ? FC_RECONNECT_CAP_MS : nominal;
}

int reconnect_backoff_is_bounded_jittered_and_stability_reset(void) {
  FcReconnectBackoff backoff;
  FcReconnectDelay delay;
  int rc = 0;

  fc_reconnect_backoff_init(&backoff, "unit-adapter");
  for (unsigned attempt = 1; attempt <= 16; ++attempt) {
    uint64_t nominal = nominal_for_attempt(attempt);
    uint64_t lower = nominal - nominal / 5U;
    uint64_t upper = nominal + nominal / 5U;
    if (upper > FC_RECONNECT_CAP_MS) upper = FC_RECONNECT_CAP_MS;
    delay = fc_reconnect_backoff_next(&backoff, (uint64_t)attempt * 10U);
    rc |= expect(delay.attempt == attempt,
                 "backoff attempt increments monotonically");
    rc |= expect(delay.delay_ms >= lower && delay.delay_ms <= upper,
                 "backoff jitter stays within twenty percent and hard cap");
  }

  fc_reconnect_backoff_mark_connected(&backoff, 100000U);
  rc |= expect(fc_reconnect_backoff_maybe_reset(&backoff, 159999U) == 0,
               "connection younger than sixty seconds keeps attempts");
  rc |= expect(fc_reconnect_backoff_maybe_reset(&backoff, 160000U) == 1,
               "connection surviving sixty seconds resets attempts");
  delay = fc_reconnect_backoff_next(&backoff, 160001U);
  rc |= expect(delay.attempt == 1,
               "first failure after stable connection restarts at attempt one");

  fc_reconnect_backoff_mark_connected(&backoff, 200000U);
  delay = fc_reconnect_backoff_next(&backoff, 259999U);
  rc |= expect(delay.attempt == 2,
               "short-lived reconnect preserves exponential attempt history");
  return rc;
}

int every_channel_reconnect_uses_shared_backoff_and_narration(void) {
  static const char *paths[] = {
    "adapters/irc/irc_adapter.c",
    "adapters/discord/discord_gateway.c",
    "adapters/ws/ws_adapter.c"
  };
  int rc = 0;
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    char *source = NULL;
    rc |= test_read_file(paths[i], &source);
    rc |= expect_substr(source, "fc_reconnect_backoff_next",
                        "adapter schedules with shared reconnect helper");
    rc |= expect_substr(source, "attempt %u",
                        "adapter narration carries reconnect attempt");
    free(source);
  }
  return rc;
}
