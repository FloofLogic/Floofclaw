#include "scratch_guard.h"

#include <stdio.h>

int fc_scratch_guard_acquire(FcScratchGuard *guard) {
  if (!guard) return -1;
  if (atomic_exchange_explicit(&guard->busy, 1, memory_order_acquire) != 0) {
    fprintf(stderr,
            "%s: scratch re-entry violates single-threaded runtime invariant\n",
            guard->owner ? guard->owner : "runtime");
    return -1;
  }
  return 0;
}

void fc_scratch_guard_release(FcScratchGuard *guard) {
  if (!guard) return;
  atomic_store_explicit(&guard->busy, 0, memory_order_release);
}
