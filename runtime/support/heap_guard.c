/* Explicit allocation accounting — see heap_guard.h. */

#include "heap_guard.h"

static FcHeapStats g_stats;

void fc_heap_snapshot(FcHeapStats *out) {
  if (out) *out = g_stats;
}

void *fc_xmalloc(size_t size) {
  __sync_fetch_and_add(&g_stats.mallocs, 1);
  return malloc(size);
}

void *fc_xcalloc(size_t n, size_t size) {
  __sync_fetch_and_add(&g_stats.callocs, 1);
  return calloc(n, size);
}

void *fc_xrealloc(void *p, size_t size) {
  __sync_fetch_and_add(&g_stats.reallocs, 1);
  return realloc(p, size);
}

char *fc_xstrdup(const char *s) {
  __sync_fetch_and_add(&g_stats.strdups, 1);
  return strdup(s);
}

void fc_xfree(void *p) {
  if (p) __sync_fetch_and_add(&g_stats.frees, 1);
  free(p);
}
