#ifndef FCLAW_SUPPORT_SCRATCH_GUARD_H
#define FCLAW_SUPPORT_SCRATCH_GUARD_H

#include <stdatomic.h>

typedef struct {
  atomic_int busy;
  const char *owner;
} FcScratchGuard;

#define FC_SCRATCH_GUARD_INIT(owner_name) { ATOMIC_VAR_INIT(0), owner_name }

/* A scratch store is valid only under the runtime's single-threaded,
 * non-reentrant state-reducer invariant. Contention is an invariant failure,
 * not work to queue: diagnose directly to stderr and let the caller fail. */
int fc_scratch_guard_acquire(FcScratchGuard *guard);
void fc_scratch_guard_release(FcScratchGuard *guard);

#endif
