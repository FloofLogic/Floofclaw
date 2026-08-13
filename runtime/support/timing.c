#include "timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

typedef struct {
  uint64_t monotonic_ms;
  uint64_t wall_ms;
  int initialized;
} TimingAnchor;

static TimingAnchor g_anchor;

static int fake_ms(const char *specific_name, uint64_t *out) {
  const char *value = specific_name ? getenv(specific_name) : NULL;
  char *end = NULL;
  unsigned long long parsed;
  if (!value || !*value) value = getenv("FCLAW_FAKE_NOW_MS");
  if (!value || !*value) return 0;
  parsed = strtoull(value, &end, 10);
  if (!end || end == value || *end != '\0') return 0;
  if (out) *out = (uint64_t)parsed;
  return 1;
}

uint64_t timing_monotonic_ms(void) {
  struct timespec ts;
  uint64_t fake;
  if (fake_ms("FCLAW_FAKE_MONOTONIC_MS", &fake)) return fake;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#ifdef __APPLE__
  {
    mach_timebase_info_data_t info;
    if (mach_timebase_info(&info) == KERN_SUCCESS && info.denom != 0) {
      long double ns = (long double)mach_continuous_time() *
                       (long double)info.numer / (long double)info.denom;
      return (uint64_t)(ns / 1000000.0L);
    }
  }
#endif
  return 0;
}

long long timing_system_wall_ms(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0)
    return (long long)time(NULL) * 1000LL;
  return (long long)tv.tv_sec * 1000LL +
         (long long)tv.tv_usec / 1000LL;
}

uint64_t timing_wall_ms(void) {
  uint64_t fake;
  if (fake_ms("FCLAW_FAKE_WALL_MS", &fake)) return fake;
  return (uint64_t)timing_system_wall_ms();
}

static void ensure_anchor(void) {
  if (g_anchor.initialized) return;
  g_anchor.monotonic_ms = timing_monotonic_ms();
  g_anchor.wall_ms = timing_wall_ms();
  g_anchor.initialized = 1;
}

uint64_t timing_stable_wall_from_monotonic(uint64_t monotonic_ms) {
  uint64_t fake;
  if (fake_ms("FCLAW_FAKE_STABLE_WALL_MS", &fake)) return fake;
  ensure_anchor();
  if (monotonic_ms <= g_anchor.monotonic_ms) return g_anchor.wall_ms;
  if (UINT64_MAX - g_anchor.wall_ms < monotonic_ms - g_anchor.monotonic_ms)
    return UINT64_MAX;
  return g_anchor.wall_ms + (monotonic_ms - g_anchor.monotonic_ms);
}

uint64_t timing_stable_wall_ms(void) {
  uint64_t fake;
  if (fake_ms("FCLAW_FAKE_STABLE_WALL_MS", &fake)) return fake;
  return timing_stable_wall_from_monotonic(timing_monotonic_ms());
}

void timing_format_duration_ms(long long duration_ms, char *out, size_t out_len) {
  if (!out || out_len == 0) return;
  if (duration_ms < 0) duration_ms = 0;
  if (duration_ms < 1000) {
    snprintf(out, out_len, "%lldms", duration_ms);
    return;
  }
  snprintf(out, out_len, "%.1fs", (double)duration_ms / 1000.0);
}
