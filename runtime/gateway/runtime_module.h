#ifndef FCLAW_RUNTIME_MODULE_H
#define FCLAW_RUNTIME_MODULE_H

#include "reactor.h"
#include "../runtime_kernel.h"

/* Reactor module that owns run lifecycle: pumps finished jobs into runs,
 * advances ready runs (bounded), retires done/failed runs. Never blocks.
 *
 * If a step requires agent/action/LLM work, runtime asks the jobrunner to
 * launch a child, marks the run waiting, and returns. The scheduler is
 * borrowed; the gateway owns its storage. */
FcReactorModule *fc_runtime_module_create(RtScheduler *scheduler);
void             fc_runtime_module_destroy(FcReactorModule *m);

#endif
