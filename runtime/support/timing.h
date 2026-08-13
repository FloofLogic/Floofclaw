#ifndef FCLAW_SUPPORT_TIMING_H
#define FCLAW_SUPPORT_TIMING_H

#include <stddef.h>
#include <stdint.h>

void timing_format_duration_ms(long long duration_ms, char *out, size_t out_len);

/* Process-local elapsed-time clock. Never use for persisted timestamps. */
uint64_t timing_monotonic_ms(void);

/* Actual Unix epoch time for human timestamps and age-based retention. */
uint64_t timing_wall_ms(void);

/* Actual Unix epoch time without the test-clock override. Reducer-owned
 * created/updated timestamps use this so a fake scheduling clock cannot leak
 * into persisted civil-time fields. */
long long timing_system_wall_ms(void);

/* Persisted deadlines are epoch values so they survive restart. During one
 * process lifetime, compare them to this wall-shaped value derived only from
 * monotonic elapsed time; NTP steps therefore cannot move active timers. */
uint64_t timing_stable_wall_from_monotonic(uint64_t monotonic_ms);
uint64_t timing_stable_wall_ms(void);

#endif
