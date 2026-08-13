#ifndef FCLAW_AGENT_ERRORS_H
#define FCLAW_AGENT_ERRORS_H

#include "runtime_kernel.h"

FcErrorCode rt_classify_agent_failure(const RtJob *job, const char *stderr_text,
                                      char *domain, size_t domain_len,
                                      char *message, size_t message_len);

#endif
