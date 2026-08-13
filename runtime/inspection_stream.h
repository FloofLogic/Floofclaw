#ifndef FCLAW_INSPECTION_STREAM_H
#define FCLAW_INSPECTION_STREAM_H

#include "runtime.h"

#include <stddef.h>

#define RT_INSPECTION_RECORD_MAX (144U * 1024U)

typedef struct RtInspectionStream RtInspectionStream;

typedef struct {
  const char *floop;
  const char *agent;
  const char *run_id;
  int call_seq;
  int call_seq_set;
  int recent;
  const RtAgentMeta *agents;
  size_t agent_count;
} RtInspectionOptions;

RtInspectionStream *rt_usage_record_stream_open(
    const RtInspectionOptions *options);
RtInspectionStream *rt_cacheview_record_stream_open(
    const RtInspectionOptions *options);

/* Returns 1 with one complete NDJSON record, 0 at end, and -1 on error. */
int rt_inspection_stream_next(RtInspectionStream *stream,
                              char *out, size_t out_len);
void rt_inspection_stream_close(RtInspectionStream *stream);

#endif
