#ifndef FCLAW_AGENT_EXEC_H
#define FCLAW_AGENT_EXEC_H

#include "runtime.h"

typedef struct {
  char agent_id[RT_SMALL];
  char executor[RT_SMALL];
  char model_ref[RT_SMALL];
  char agent_dir[PATH_MAX];
  char script_path[PATH_MAX];
  char in_path[PATH_MAX];
  char out_path[PATH_MAX];
  char err_path[PATH_MAX];
  /* Explicit ingress manifest selected for this invocation. Empty for every
   * text-only or non-event-listening call; children must not scan run_dir. */
  char media_manifest_path[PATH_MAX];
  char bound_task_id[RT_SMALL];
  long long bound_work_rev;
  char input[RT_XL];
} RtAgentInvocation;

int rt_agent_read_meta(const char *floop_name, const char *agent_id,
                      char *executor, size_t executor_len,
                      char *model_ref, size_t model_ref_len,
                      char *err, size_t err_len);
int rt_agent_read_action_allowlist(const char *floop_name, const char *agent_id, const char *executor,
                                  const RtActionRegistry *actions, RtAgentMeta *meta,
                                  char *err, size_t err_len);
int rt_agent_read_listen_config(const char *floop_name, const char *agent_id, const char *executor,
                                RtAgentMeta *meta, char *err, size_t err_len);
int rt_agent_prepare_invocation(RtContext *ctx, const char *floop_name, const RtStep *step,
                               const char *executor,
                               const RtActionRegistry *actions,
                               const RtAgentMeta *agent_meta,
                               const char *bound_task_id,
                               long long bound_work_rev,
                               RtAgentInvocation *out);
int rt_build_processor_input(const RtContext *ctx, const RtAgentMeta *agent_meta,
                             const char *bound_task_id,
                             long long bound_work_rev,
                             char *out, size_t out_len);
int rt_agent_artifact_seq(const char *run_dir, const char *executor);
int rt_agent_write_llm_response_artifact(RtContext *ctx, const RtStep *step,
                                         const char *agent_output_path,
                                         const char *model_text,
                                         const char *normalized);
int rt_agent_normalize_output(RtContext *ctx, const RtStep *step,
                              const RtAgentMeta *agent_meta,
                              const char *bound_task_id,
                              long long bound_work_rev,
                              const char *output_json,
                              char *out, size_t out_len);
/* Returns 0 when the configured contract accepts this decision, 1 when the
 * contract does not apply to the current event, and -1 with a specific reason
 * when the entire response must be withheld and repaired. */
int rt_agent_validate_output_contract(const RtContext *ctx,
                                      const RtAgentMeta *agent_meta,
                                      const char *output_json,
                                      char *reason, size_t reason_len);
int rt_agent_normalize_affair_output(RtContext *ctx, const RtStep *step,
                                     const char *bound_task_id,
                                     const char *output_json,
                                     char *out, size_t out_len);
int rt_agent_normalize_conversational_output(RtContext *ctx, const RtStep *step,
                                             const char *output_json,
                                             char *out, size_t out_len);
int rt_agent_normalize_memory_compaction_output(RtContext *ctx, const RtStep *step,
                                                const char *output_json,
                                                char *out, size_t out_len);
int rt_agent_append_output(RtContext *ctx, const RtStep *step,
                           const RtAgentMeta *agent_meta,
                           const char *bound_task_id,
                           long long bound_work_rev,
                           const char *output_json,
                           char *normalized_out, size_t normalized_len);
int rt_agent_append_output_file(RtContext *ctx, const RtStep *step,
                                const char *executor, const RtAgentMeta *agent_meta,
                                const char *bound_task_id,
                                long long bound_work_rev,
                                const char *path,
                                char *normalized_out, size_t normalized_len);

#endif
