/* Heap-sample budget tests — drive a real gateway with N events and
 * read the gateway.log heap counters. Live here because they need a
 * real runtime cycle. Pure source-scan budget tests are in
 * tests/unit/test_budget.c. */

#include "test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int heap_value(const char *log, const char *key) {
  const char *p = log ? strstr(log, key) : NULL;
  if (!p) return -1;
  p += strlen(key);
  if (*p == '=') p++;
  return atoi(p);
}

static int run_heap_sample(int events, char **log_out) {
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= expect(test_gateway_fast_loop(events, log_out) == 0,
               "gateway heap sample completes");
  rc |= expect_substr(*log_out, "gateway: heap", "gateway emitted heap counters");
  return rc;
}

int native_noop_heap_budget_stays_under_limit(void) {
  char *log = NULL;
  int events = 3;
  int mallocs;
  int rc = run_heap_sample(events, &log);
  mallocs = heap_value(log, "mallocs");
  rc |= expect(mallocs >= 0, "malloc counter parsed");
  rc |= expect(mallocs <= events * 40, "mallocs/event stays within budget <= 40");
  free(log);
  return rc;
}

int native_noop_has_zero_reallocs(void) {
  char *log = NULL;
  int rc = run_heap_sample(3, &log);
  rc |= expect(heap_value(log, "reallocs") == 0, "native noop has zero reallocs");
  free(log);
  return rc;
}

int native_noop_has_zero_strdups(void) {
  char *log = NULL;
  int rc = run_heap_sample(3, &log);
  rc |= expect(heap_value(log, "strdups") == 0, "native noop has zero strdups");
  free(log);
  return rc;
}
