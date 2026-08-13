#ifndef FCLAW_WAKE_MODULE_H
#define FCLAW_WAKE_MODULE_H

#include "reactor.h"

FcReactorModule *fc_bus_wake_module_create(void);
void fc_bus_wake_module_destroy(FcReactorModule *m);

#endif
