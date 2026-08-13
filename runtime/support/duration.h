#ifndef FCLAW_SUPPORT_DURATION_H
#define FCLAW_SUPPORT_DURATION_H

/* Parse a human duration string like "60s", "5m", "1h", "1d", "100ms"
 * into milliseconds. Grammar: <positive-integer><unit> where unit is
 * one of ms, s, m (or min), h, d. Bare digits with no unit are treated
 * as milliseconds.
 *
 * Returns 0 on success and writes the millisecond value to *out.
 * Returns -1 on malformed input, non-positive number, or unknown unit. */
int fc_parse_duration_ms(const char *s, long long *out);

#endif
