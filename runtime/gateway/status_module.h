#ifndef FCLAW_STATUS_MODULE_H
#define FCLAW_STATUS_MODULE_H

#include "../runtime_kernel.h"
#include "reactor.h"

#include <stddef.h>

#define FC_STATUS_JSON_CAP 8192

FcReactorModule *fc_status_module_create(RtScheduler *scheduler,
                                         const FcReactor *reactor);
void fc_status_module_destroy(FcReactorModule *m);
int fc_status_query_socket(char *out, size_t out_len);
int fc_status_build_json(const RtScheduler *scheduler, char *out, size_t out_len);
int fc_local_health_build_json(const RtScheduler *scheduler,
                               char *out, size_t out_len);
int fc_status_build_json_with_reactor(const RtScheduler *scheduler,
                                      const FcReactor *reactor,
                                      char *out, size_t out_len);

#endif
