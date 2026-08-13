#ifndef FCLAW_BUS_INTAKE_MODULE_H
#define FCLAW_BUS_INTAKE_MODULE_H

#include "reactor.h"
#include "../runtime_kernel.h"

/* Reactor module that imports a bounded number of bus envelopes per tick
 * into the scheduler as new RtRuns. No fds, no bus events — just tick.
 * The scheduler is borrowed, owned by gateway. */
FcReactorModule *fc_bus_intake_module_create(RtScheduler *scheduler);
void             fc_bus_intake_module_destroy(FcReactorModule *m);

#endif
