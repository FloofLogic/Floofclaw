#ifndef FCLAW_HEAP_GUARD_H
#define FCLAW_HEAP_GUARD_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Explicit allocation accounting.
 *
 * macOS dyld interposition doesn't engage from a main executable, so
 * "count every malloc in the process" isn't reliably available without
 * a separate dylib. Instead we use *deliberate* instrumentation: every
 * allocation our code makes goes through fc_xmalloc / fc_xstrdup /
 * etc., which atomically increments per-class counters before
 * dispatching to libc.
 *
 * Goal: after init, the per-event hot path performs zero
 * fc_xmalloc/calloc/realloc/strdup calls. If a counter increments
 * during steady-state, the call site is in our code — grep it. */

typedef struct {
  uint64_t mallocs;
  uint64_t callocs;
  uint64_t reallocs;
  uint64_t strdups;
  uint64_t frees;
} FcHeapStats;

void  fc_heap_snapshot(FcHeapStats *out);

/* Wrappers. Call these instead of malloc/calloc/realloc/strdup/free
 * in code that should be heap-budgeted. They forward to libc. */
void *fc_xmalloc (size_t size);
void *fc_xcalloc (size_t n, size_t size);
void *fc_xrealloc(void *p, size_t size);
char *fc_xstrdup (const char *s);
void  fc_xfree   (void *p);

#endif
