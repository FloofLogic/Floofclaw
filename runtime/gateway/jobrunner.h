#ifndef FCLAW_JOBRUNNER_H
#define FCLAW_JOBRUNNER_H

#include "../runtime.h"
#include "../secure/action_secret_broker.h"
#include "reactor.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define JOB_OUTPUT_CAP_STDOUT_DEFAULT (8U * 1024U * 1024U)
#define JOB_OUTPUT_CAP_STDERR_DEFAULT (1U * 1024U * 1024U)
#define JOB_KILL_GRACE_MS_DEFAULT     1000
#define JOB_PROGRESS_RECORD_CAP       4096
#define JOB_PROGRESS_QUEUE_CAP        64
#define JOB_PROGRESS_PARTIAL_CAP      8192

typedef enum { JOB_AGENT, JOB_ACTION } JobKind;
typedef enum { JOB_RUNNING, JOB_FINISHED, JOB_FAILED } JobState;

typedef struct RtJob {
  JobKind kind;
  JobState state;
  char job_id[RT_SMALL];
  char run_id[RT_SMALL];
  char origin_event_id[RT_SMALL];
  char step_id[RT_SMALL];
  char request_id[RT_SMALL];
  char bound_task_id[RT_SMALL];
  long long bound_work_rev;
  pid_t pid;
  uint64_t launched_ms;
  uint64_t deadline_ms;            /* absolute */
  char in_path[PATH_MAX];
  char out_path[PATH_MAX];
  char err_path[PATH_MAX];
  size_t stdout_cap;
  size_t stderr_cap;

  /* Pipe read ends (parent-owned). -1 once closed. */
  int stdout_fd;
  int stderr_fd;
  int progress_fd;
  ActionSecretBroker *secret_broker;
  int stdout_eof;
  int stderr_eof;
  int progress_eof;
  size_t stdout_drained;
  size_t stderr_drained;
  char progress_partial[JOB_PROGRESS_PARTIAL_CAP];
  size_t progress_partial_len;
  int progress_dropped;
  /* Tee files: parent writes pipe bytes here so the rest of the system
   * (event reducer, action terminal-event builder) keeps reading the same paths. */
  FILE *out_file;
  FILE *err_file;

  int exited;                      /* waitpid returned for this pid */
  int exit_status;                 /* WEXITSTATUS once exited */
  int signaled;                    /* 1 if killed by signal */
  int term_signal;                 /* signal number if signaled */
  int killed_for_timeout;
  int killed_for_output_limit;
  uint64_t sigterm_at_ms;          /* 0 = haven't sigterm'd */
  struct RtJob *next;
} RtJob;

typedef struct {
  char job_id[RT_SMALL];
  char run_id[RT_SMALL];
  char origin_event_id[RT_SMALL];
  char record[JOB_PROGRESS_RECORD_CAP];
} RtJobProgress;

typedef struct {
  RtJob *jobs;
  uint64_t kill_grace_ms;
  unsigned long long next_job_id;
  RtJobProgress progress_queue[JOB_PROGRESS_QUEUE_CAP];
  size_t progress_head;
  size_t progress_count;
  unsigned long long progress_dropped;
} RtJobRunner;

void rt_jobrunner_init(RtJobRunner *r);
void rt_jobrunner_destroy(RtJobRunner *r);

/* Launch a child running argv[]. Children's fd 0 reads in_path (or /dev/null
 * if NULL/empty); fd 1 / fd 2 are pipes whose read ends the parent owns. The
 * parent tees pipe bytes into out_path / err_path so callers can read the
 * artifacts the same way as before. deadline_ms is absolute (fc_now_ms() +
 * budget). Returns 0 on success, -1 on error. */
int rt_jobrunner_launch(RtJobRunner *r,
                        char *const argv[],
                        const char *in_path,
                        const char *out_path,
                        const char *err_path,
                        JobKind kind,
                        const char *run_id,
                        const char *origin_event_id,
                        const char *step_id,
                        const char *request_id,
                        const char *bound_task_id,
                        long long bound_work_rev,
                        const char *workspace_root,
                        const char *action_id,
                        int private_credentials,
                        char *const env_extra[],
                        uint64_t deadline_ms,
                        size_t stdout_cap,
                        size_t stderr_cap,
                        RtJob **out_job);

/* Drain bytes from any readable pipes (nonblocking), reap exited children
 * with waitpid(WNOHANG), enforce deadlines and output caps, escalate SIGTERM
 * to SIGKILL once kill_grace_ms has elapsed. A job's state becomes
 * JOB_FINISHED / JOB_FAILED only when (exited && stdout_eof && stderr_eof). */
int rt_jobrunner_tick(RtJobRunner *r, uint64_t now_ms);

/* Reactor wiring. Add each running job's stdout/stderr pipes to the pollset
 * with the supplied owner (the jobrunner module). The job pointer is used
 * as the per-fd tag. */
int rt_jobrunner_collect_fds(RtJobRunner *r, FcPollSet *ps, FcReactorModule *owner);

/* When the reactor reports a fd ready, drain it nonblocking. tag is the
 * RtJob* registered at collect_fds time. */
int rt_jobrunner_on_fd(RtJobRunner *r, int fd, int events, void *tag);

/* Count active (state == JOB_RUNNING) jobs of a kind. Used for action
 * serialization (cap = 1). */
int rt_jobrunner_count_active(const RtJobRunner *r, JobKind kind);
int rt_jobrunner_count_active_all(const RtJobRunner *r);

/* Pop one child-emitted progress JSON record. Progress is bounded,
 * best-effort presentation only; queue overflow increments the dropped
 * counter and never blocks or fails the owned job. */
int rt_jobrunner_progress_pop(RtJobRunner *r, RtJobProgress *out);
unsigned long long rt_jobrunner_progress_dropped(const RtJobRunner *r);

/* Best-effort cancellation of jobs owned by one run. Returns the number
 * signaled. The scheduler owns the durable run_canceled transition. */
int rt_jobrunner_cancel_run(RtJobRunner *r, const char *run_id,
                            uint64_t now_ms);

/* Pop the first non-running job (FINISHED or FAILED). Caller takes ownership
 * and must rt_jobrunner_release it. */
RtJob *rt_jobrunner_pop_finished(RtJobRunner *r);

/* Free a job and remove from runner. */
void rt_jobrunner_release(RtJobRunner *r, RtJob *job);

/* Shutdown: SIGTERM all running, wait up to grace_ms ticking, SIGKILL leftovers,
 * reap everything. After this call, no jobs are tracked. */
int rt_jobrunner_shutdown_all(RtJobRunner *r, uint64_t grace_ms);

#endif
