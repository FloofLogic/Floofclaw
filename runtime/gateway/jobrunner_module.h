#ifndef FCLAW_JOBRUNNER_MODULE_H
#define FCLAW_JOBRUNNER_MODULE_H

#include "reactor.h"
#include "jobrunner.h"

/* Reactor module wrapping a long-lived RtJobRunner. The runner is borrowed,
 * not owned: the caller (gateway) keeps it alive across the module's
 * lifetime. The module:
 *   - publishes each running job's stdout/stderr pipes via collect_fds,
 *   - drains them in on_fd,
 *   - reaps with waitpid(WNOHANG) and finalizes job state in tick.
 *
 * The module never reads/writes the bus or runs themselves. Pumping
 * finished jobs back into runs is the runtime module's job. */
FcReactorModule *fc_jobrunner_module_create(RtJobRunner *runner);
void             fc_jobrunner_module_destroy(FcReactorModule *m);

#endif
