#ifndef FCLAW_FLOOP_H
#define FCLAW_FLOOP_H

#include <stddef.h>

int rt_floop_loop_path(const char *name, char *out, size_t out_len);
int rt_floop_list_available(char *out, size_t out_len);
int rt_floop_agent_dir(const char *floop_or_loop_name, const char *agent_id,
                       char *out, size_t out_len);

#endif
