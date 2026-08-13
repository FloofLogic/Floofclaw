#include "duration.h"

#include <stdlib.h>
#include <string.h>

int fc_parse_duration_ms(const char *s, long long *out) {
  char *end = NULL;
  long long n;
  if (!s || !*s || !out) return -1;
  n = strtoll(s, &end, 10);
  if (!end || end == s || n <= 0) return -1;
  if (!*end || strcmp(end, "ms") == 0)                   { *out = n;              return 0; }
  if (strcmp(end, "s") == 0)                             { *out = n * 1000LL;     return 0; }
  if (strcmp(end, "m") == 0 || strcmp(end, "min") == 0)  { *out = n * 60000LL;    return 0; }
  if (strcmp(end, "h") == 0)                             { *out = n * 3600000LL;  return 0; }
  if (strcmp(end, "d") == 0)                             { *out = n * 86400000LL; return 0; }
  return -1;
}
