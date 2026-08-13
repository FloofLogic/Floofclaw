#include "jobrunner.h"
#include "reactor.h"

#include "../secure/secret_store.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void rt_jobrunner_init(RtJobRunner *r) {
  if (!r) return;
  r->jobs = NULL;
  r->kill_grace_ms = JOB_KILL_GRACE_MS_DEFAULT;
  r->next_job_id = 1;
  r->progress_head = 0;
  r->progress_count = 0;
  r->progress_dropped = 0;
}

/* Child-side secret injection. Runs between fork and exec in the
 * single-threaded reactor's child, alongside the existing setenv. */
typedef struct { const char *prefix; } SecretInject;

static int inject_one_secret(const char *name, void *user) {
  const SecretInject *inj = (const SecretInject *)user;
  char key[512], env_name[256], value[8192];
  size_t i;
  if (!name || !*name) return 0;
  if (snprintf(key, sizeof(key), "%s%s", inj->prefix, name) >= (int)sizeof(key))
    return 0;
  if (secret_store_get(key, value, sizeof(value)) != 0) return 0;
  for (i = 0; name[i] && i + 1 < sizeof(env_name); ++i)
    env_name[i] = (char)toupper((unsigned char)name[i]);
  env_name[i] = '\0';
  (void)setenv(env_name, value, 1);
  secret_store_zero(value, sizeof(value));
  return 0;
}

static void inject_action_secrets(const char *action_id) {
  SecretInject inj;
  char prefix[512];
  if (snprintf(prefix, sizeof(prefix), "endpoint:action:%s:", action_id) >=
      (int)sizeof(prefix))
    return;
  inj.prefix = prefix;
  (void)secret_store_list_prefix(prefix, inject_one_secret, &inj);
}

static void unlink_job(RtJobRunner *r, RtJob *job) {
  RtJob **cur;
  if (!r || !job) return;
  for (cur = &r->jobs; *cur; cur = &(*cur)->next) {
    if (*cur == job) { *cur = job->next; job->next = NULL; return; }
  }
}

static void close_pipes(RtJob *job) {
  if (!job) return;
  if (job->stdout_fd >= 0) { close(job->stdout_fd); job->stdout_fd = -1; }
  if (job->stderr_fd >= 0) { close(job->stderr_fd); job->stderr_fd = -1; }
  if (job->progress_fd >= 0) { close(job->progress_fd); job->progress_fd = -1; }
  if (job->secret_broker) {
    action_secret_broker_close(job->secret_broker);
    free(job->secret_broker);
    job->secret_broker = NULL;
  }
  if (job->out_file) { fflush(job->out_file); fclose(job->out_file); job->out_file = NULL; }
  if (job->err_file) { fflush(job->err_file); fclose(job->err_file); job->err_file = NULL; }
}

void rt_jobrunner_release(RtJobRunner *r, RtJob *job) {
  if (!job) return;
  close_pipes(job);
  unlink_job(r, job);
  free(job);
}

void rt_jobrunner_destroy(RtJobRunner *r) {
  if (!r) return;
  while (r->jobs) {
    RtJob *j = r->jobs;
    r->jobs = j->next;
    close_pipes(j);
    free(j);
  }
}

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_pipe_pair(int fds[2]) {
  if (pipe(fds) != 0) return -1;
  /* parent read end is nonblocking; child end stays blocking and is dup2'd
   * to fd 1/2 in the child. Both ends become CLOEXEC for the parent so a
   * later spawn doesn't inherit them. */
  if (set_nonblocking(fds[0]) != 0) { close(fds[0]); close(fds[1]); return -1; }
  (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  /* fds[1] keeps default flags — child needs blocking writes. */
  return 0;
}

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
                        RtJob **out_job) {
  RtJob *job;
  pid_t pid;
  int p_out[2] = {-1, -1};
  int p_err[2] = {-1, -1};
  int p_progress[2] = {-1, -1};
  int p_secret[2] = {-1, -1};
  if (!r || !argv || !argv[0] || !out_path || !err_path) return -1;
  job = (RtJob *)calloc(1, sizeof(*job));
  if (!job) return -1;
  job->kind = kind;
  job->state = JOB_RUNNING;
  snprintf(job->job_id, sizeof(job->job_id), "job_%03llu", r->next_job_id++);
  job->stdout_fd = -1;
  job->stderr_fd = -1;
  job->progress_fd = -1;
  job->progress_eof = kind == JOB_AGENT ? 0 : 1;
  if (run_id)     snprintf(job->run_id, sizeof(job->run_id), "%s", run_id);
  if (origin_event_id)
    snprintf(job->origin_event_id, sizeof(job->origin_event_id), "%s",
             origin_event_id);
  if (step_id)    snprintf(job->step_id, sizeof(job->step_id), "%s", step_id);
  if (request_id) snprintf(job->request_id, sizeof(job->request_id), "%s", request_id);
  if (bound_task_id) snprintf(job->bound_task_id, sizeof(job->bound_task_id), "%s", bound_task_id);
  job->bound_work_rev = bound_work_rev;
  if (in_path)    snprintf(job->in_path, sizeof(job->in_path), "%s", in_path);
  snprintf(job->out_path, sizeof(job->out_path), "%s", out_path);
  snprintf(job->err_path, sizeof(job->err_path), "%s", err_path);
  job->launched_ms = fc_now_ms();
  job->deadline_ms = deadline_ms;
  job->stdout_cap = stdout_cap > 0 ? stdout_cap : JOB_OUTPUT_CAP_STDOUT_DEFAULT;
  job->stderr_cap = stderr_cap > 0 ? stderr_cap : JOB_OUTPUT_CAP_STDERR_DEFAULT;

  /* Open tee files first so we fail before forking on disk errors. */
  job->out_file = fopen(out_path, "wb");
  job->err_file = fopen(err_path, "wb");
  if (!job->out_file || !job->err_file) goto fail;

  if (make_pipe_pair(p_out) != 0) goto fail;
  if (make_pipe_pair(p_err) != 0) goto fail;
  if (kind == JOB_AGENT && make_pipe_pair(p_progress) != 0) goto fail;
  if (private_credentials) {
    if (!action_id || !*action_id || action_secret_broker_pair(p_secret) != 0)
      goto fail;
    job->secret_broker =
        (ActionSecretBroker *)calloc(1, sizeof(*job->secret_broker));
    if (!job->secret_broker)
      goto fail;
    {
      int broker_fd = p_secret[0];
      p_secret[0] = -1;
      if (action_secret_broker_init(job->secret_broker, broker_fd,
                                    action_id) != 0)
        goto fail;
    }
  }

  pid = fork();
  if (pid < 0) goto fail;
  if (pid == 0) {
    /* Child. Replace stdin with in_path (or /dev/null), stdout/stderr with
     * the pipe write ends. */
    int fd_in;
    char progress_fd_text[32];
    char secret_fd_text[32];
    if (in_path && *in_path) fd_in = open(in_path, O_RDONLY);
    else                      fd_in = open("/dev/null", O_RDONLY);
    if (fd_in < 0) _exit(126);
    if (dup2(fd_in, 0) < 0) _exit(126);
    if (fd_in != 0) close(fd_in);
    if (dup2(p_out[1], 1) < 0) _exit(126);
    if (dup2(p_err[1], 2) < 0) _exit(126);
    close(p_out[0]); close(p_out[1]);
    close(p_err[0]); close(p_err[1]);
    if (kind == JOB_AGENT) {
      close(p_progress[0]);
      /* Keep the actual pipe descriptor open across exec. It is deliberately
       * separate from stdout/stderr, whose complete artifact contracts must
       * remain unchanged. */
      snprintf(progress_fd_text, sizeof(progress_fd_text), "%d", p_progress[1]);
      (void)setenv("FCLAW_PROGRESS_FD", progress_fd_text, 1);
    } else {
      (void)unsetenv("FCLAW_PROGRESS_FD");
    }
    if (private_credentials && p_secret[1] >= 0) {
      if (job->secret_broker && job->secret_broker->fd >= 0)
        close(job->secret_broker->fd);
      snprintf(secret_fd_text, sizeof(secret_fd_text), "%d", p_secret[1]);
      (void)setenv("FCLAW_SECRET_FD", secret_fd_text, 1);
    } else {
      (void)unsetenv("FCLAW_SECRET_FD");
    }
    if (workspace_root && *workspace_root)
      (void)setenv("FCLAW_WORKSPACE_ROOT", workspace_root, 1);
    /* Secrets, scoped by construction. The action never names its own
     * namespace, so it cannot reach another action's: we build the
     * action:<id>: prefix here and hand the child only the bare names
     * stored under it, uppercased (client_id -> $CLIENT_ID). */
    if (action_id && *action_id && !private_credentials)
      inject_action_secrets(action_id);
    /* Declared config, already resolved by the parent from the
     * deployment's config file. Applied after secrets so a config name
     * can never shadow a credential. */
    if (env_extra) {
      size_t i;
      for (i = 0; env_extra[i]; ++i) {
        char *eq = strchr(env_extra[i], '=');
        if (!eq) continue;
        *eq = '\0';
        (void)setenv(env_extra[i], eq + 1, 1);
        *eq = '=';
      }
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  /* Parent. Close write ends; keep read ends. */
  close(p_out[1]); p_out[1] = -1;
  close(p_err[1]); p_err[1] = -1;
  if (p_progress[1] >= 0) {
    close(p_progress[1]);
    p_progress[1] = -1;
  }
  if (p_secret[1] >= 0) {
    close(p_secret[1]);
    p_secret[1] = -1;
  }
  job->pid = pid;
  job->stdout_fd = p_out[0];
  job->stderr_fd = p_err[0];
  job->progress_fd = kind == JOB_AGENT ? p_progress[0] : -1;
  job->next = r->jobs;
  r->jobs = job;
  if (out_job) *out_job = job;
  return 0;

fail:
  if (p_out[0] >= 0) close(p_out[0]);
  if (p_out[1] >= 0) close(p_out[1]);
  if (p_err[0] >= 0) close(p_err[0]);
  if (p_err[1] >= 0) close(p_err[1]);
  if (p_progress[0] >= 0) close(p_progress[0]);
  if (p_progress[1] >= 0) close(p_progress[1]);
  if (p_secret[0] >= 0) close(p_secret[0]);
  if (p_secret[1] >= 0) close(p_secret[1]);
  if (job->out_file) { fclose(job->out_file); job->out_file = NULL; }
  if (job->err_file) { fclose(job->err_file); job->err_file = NULL; }
  if (job->secret_broker) {
    action_secret_broker_close(job->secret_broker);
    free(job->secret_broker);
    job->secret_broker = NULL;
  }
  free(job);
  return -1;
}

static void send_sigterm(RtJob *job, uint64_t now_ms) {
  if (job->pid <= 0 || job->sigterm_at_ms != 0) return;
  (void)kill(job->pid, SIGTERM);
  job->sigterm_at_ms = now_ms;
}

static void send_sigkill(RtJob *job) {
  if (job->pid <= 0) return;
  (void)kill(job->pid, SIGKILL);
}

/* Drain one pipe nonblocking into the tee FILE. Returns:
 *   0  if there is more to come (got bytes or EAGAIN)
 *   1  if EOF — caller closes the fd and marks the eof flag
 *  -1  on hard error
 */
static int drain_pipe(int fd, FILE *out, size_t *drained, size_t cap, int *over_cap) {
  unsigned char buf[4096];
  if (over_cap) *over_cap = 0;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (out) {
        (void)fwrite(buf, 1, (size_t)n, out);
        (void)fflush(out);
      }
      if (drained) *drained += (size_t)n;
      if (drained && cap > 0 && *drained > cap) {
        if (over_cap) *over_cap = 1;
        /* keep draining — we still want EOF, but signal so the caller can
         * SIGTERM. */
      }
      continue;
    }
    if (n == 0) return 1; /* EOF */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (errno == EINTR) continue;
    return -1;
  }
}

static void queue_progress(RtJobRunner *r, RtJob *job,
                           const char *record, size_t record_len) {
  size_t slot;
  RtJobProgress *p;
  if (!r || !job || !record || record_len == 0) return;
  if (record_len >= JOB_PROGRESS_RECORD_CAP ||
      r->progress_count >= JOB_PROGRESS_QUEUE_CAP) {
    job->progress_dropped = 1;
    r->progress_dropped++;
    return;
  }
  slot = (r->progress_head + r->progress_count) % JOB_PROGRESS_QUEUE_CAP;
  p = &r->progress_queue[slot];
  memset(p, 0, sizeof(*p));
  snprintf(p->job_id, sizeof(p->job_id), "%s", job->job_id);
  snprintf(p->run_id, sizeof(p->run_id), "%s", job->run_id);
  snprintf(p->origin_event_id, sizeof(p->origin_event_id), "%s",
           job->origin_event_id);
  memcpy(p->record, record, record_len);
  p->record[record_len] = '\0';
  r->progress_count++;
}

static void progress_consume_bytes(RtJobRunner *r, RtJob *job,
                                   const char *bytes, size_t len) {
  size_t i = 0;
  if (!r || !job || !bytes) return;
  while (i < len) {
    const char *nl = memchr(bytes + i, '\n', len - i);
    size_t part = nl ? (size_t)(nl - (bytes + i)) : len - i;
    if (job->progress_partial_len + part >=
        sizeof(job->progress_partial)) {
      job->progress_partial_len = 0;
      job->progress_dropped = 1;
      r->progress_dropped++;
      if (!nl) return;
      i += part + 1;
      continue;
    }
    memcpy(job->progress_partial + job->progress_partial_len,
           bytes + i, part);
    job->progress_partial_len += part;
    if (!nl) return;
    if (job->progress_partial_len > 0) {
      queue_progress(r, job, job->progress_partial,
                     job->progress_partial_len);
    }
    job->progress_partial_len = 0;
    i += part + 1;
  }
}

static int drain_progress_pipe(RtJobRunner *r, RtJob *job) {
  char buf[4096];
  for (;;) {
    ssize_t n = read(job->progress_fd, buf, sizeof(buf));
    if (n > 0) {
      progress_consume_bytes(r, job, buf, (size_t)n);
      continue;
    }
    if (n == 0) return 1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (errno == EINTR) continue;
    return -1;
  }
}

static void drain_pending(RtJobRunner *r, RtJob *job) {
  int over = 0;
  if (job->stdout_fd >= 0 && !job->stdout_eof) {
    int rc = drain_pipe(job->stdout_fd, job->out_file, &job->stdout_drained, job->stdout_cap, &over);
    if (over) job->killed_for_output_limit = 1;
    if (rc == 1) {
      close(job->stdout_fd); job->stdout_fd = -1;
      job->stdout_eof = 1;
      if (job->out_file) { fflush(job->out_file); fclose(job->out_file); job->out_file = NULL; }
    } else if (rc < 0) {
      close(job->stdout_fd); job->stdout_fd = -1;
      job->stdout_eof = 1;
      if (job->out_file) { fflush(job->out_file); fclose(job->out_file); job->out_file = NULL; }
    }
  }
  if (job->stderr_fd >= 0 && !job->stderr_eof) {
    int rc = drain_pipe(job->stderr_fd, job->err_file, &job->stderr_drained, job->stderr_cap, &over);
    if (over) job->killed_for_output_limit = 1;
    if (rc == 1) {
      close(job->stderr_fd); job->stderr_fd = -1;
      job->stderr_eof = 1;
      if (job->err_file) { fflush(job->err_file); fclose(job->err_file); job->err_file = NULL; }
    } else if (rc < 0) {
      close(job->stderr_fd); job->stderr_fd = -1;
      job->stderr_eof = 1;
      if (job->err_file) { fflush(job->err_file); fclose(job->err_file); job->err_file = NULL; }
    }
  }
  if (job->progress_fd >= 0 && !job->progress_eof) {
    int rc = drain_progress_pipe(r, job);
    if (rc != 0) {
      close(job->progress_fd);
      job->progress_fd = -1;
      job->progress_eof = 1;
      /* A final unterminated record is invalid and is discarded. */
      if (job->progress_partial_len > 0) {
        job->progress_partial_len = 0;
        job->progress_dropped = 1;
        r->progress_dropped++;
      }
    }
  }
}

int rt_jobrunner_tick(RtJobRunner *r, uint64_t now_ms) {
  RtJob *job;
  if (!r) return -1;
  for (job = r->jobs; job; job = job->next) {
    if (job->state != JOB_RUNNING) continue;

    /* Embedded owners drive the scheduler directly instead of entering the
     * gateway reactor. Service the private action channel here as well as in
     * on_fd so the job contract is identical in both ownership modes. */
    if (job->secret_broker && !job->secret_broker->closed &&
        job->secret_broker->fd >= 0) {
      struct pollfd pfd;
      int poll_rc;
      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = job->secret_broker->fd;
      pfd.events = action_secret_broker_events(job->secret_broker);
      do {
        poll_rc = poll(&pfd, 1, 0);
      } while (poll_rc < 0 && errno == EINTR);
      if (poll_rc > 0 && pfd.revents)
        (void)action_secret_broker_on_events(job->secret_broker, pfd.revents);
      else if (poll_rc < 0)
        action_secret_broker_close(job->secret_broker);
    }

    /* Drain whatever is sitting in the pipes right now. */
    drain_pending(r, job);

    /* deadline */
    if (job->deadline_ms > 0 && now_ms >= job->deadline_ms && job->sigterm_at_ms == 0) {
      job->killed_for_timeout = 1;
      send_sigterm(job, now_ms);
    }
    /* output cap escalation */
    if (job->killed_for_output_limit && job->sigterm_at_ms == 0) {
      send_sigterm(job, now_ms);
    }
    /* escalate SIGTERM -> SIGKILL after grace */
    if (job->sigterm_at_ms != 0 && now_ms >= job->sigterm_at_ms + r->kill_grace_ms) {
      send_sigkill(job);
    }
    /* reap (only once) */
    if (!job->exited) {
      int status;
      pid_t w = waitpid(job->pid, &status, WNOHANG);
      if (w == job->pid) {
        if (WIFEXITED(status)) {
          job->exit_status = WEXITSTATUS(status);
          job->signaled = 0;
        } else if (WIFSIGNALED(status)) {
          job->signaled = 1;
          job->term_signal = WTERMSIG(status);
          job->exit_status = 128 + job->term_signal;
        }
        job->exited = 1;
        /* Child gone — give the pipes one more nonblocking pass; remaining
         * bytes will surface as either readable data or immediate EOF. */
        drain_pending(r, job);
      }
    }

    if (job->exited && job->stdout_eof && job->stderr_eof &&
        job->progress_eof &&
        (!job->secret_broker || job->secret_broker->closed)) {
      job->state = (job->exit_status == 0 &&
                    !job->killed_for_timeout &&
                    !job->killed_for_output_limit)
                       ? JOB_FINISHED
                       : JOB_FAILED;
    }
  }
  return 0;
}

int rt_jobrunner_collect_fds(RtJobRunner *r, FcPollSet *ps, FcReactorModule *owner) {
  RtJob *j;
  if (!r || !ps) return -1;
  for (j = r->jobs; j; j = j->next) {
    if (j->state != JOB_RUNNING) continue;
    if (j->stdout_fd >= 0 && !j->stdout_eof) (void)fc_pollset_add(ps, j->stdout_fd, POLLIN, owner, j);
    if (j->stderr_fd >= 0 && !j->stderr_eof) (void)fc_pollset_add(ps, j->stderr_fd, POLLIN, owner, j);
    if (j->progress_fd >= 0 && !j->progress_eof)
      (void)fc_pollset_add(ps, j->progress_fd, POLLIN, owner, j);
    if (j->secret_broker && !j->secret_broker->closed &&
        j->secret_broker->fd >= 0)
      (void)fc_pollset_add(ps, j->secret_broker->fd,
                          action_secret_broker_events(j->secret_broker),
                          owner, j);
  }
  return 0;
}

int rt_jobrunner_on_fd(RtJobRunner *r, int fd, int events, void *tag) {
  RtJob *job = (RtJob *)tag;
  (void)r;
  if (!job) return 0;
  if (!(events & (POLLIN | POLLOUT | POLLHUP | POLLERR))) return 0;
  /* drain_pending touches both fds; cheap and idempotent. */
  if (fd == job->stdout_fd || fd == job->stderr_fd ||
      fd == job->progress_fd)
    drain_pending(r, job);
  if (job->secret_broker && fd == job->secret_broker->fd)
    (void)action_secret_broker_on_events(job->secret_broker, (short)events);
  return 0;
}

int rt_jobrunner_count_active(const RtJobRunner *r, JobKind kind) {
  const RtJob *j;
  int n = 0;
  if (!r) return 0;
  for (j = r->jobs; j; j = j->next) {
    if (j->state == JOB_RUNNING && j->kind == kind) n++;
  }
  return n;
}

int rt_jobrunner_count_active_all(const RtJobRunner *r) {
  const RtJob *j;
  int n = 0;
  if (!r) return 0;
  for (j = r->jobs; j; j = j->next) {
    if (j->state == JOB_RUNNING) n++;
  }
  return n;
}

int rt_jobrunner_progress_pop(RtJobRunner *r, RtJobProgress *out) {
  if (!r || !out || r->progress_count == 0) return 1;
  *out = r->progress_queue[r->progress_head];
  memset(&r->progress_queue[r->progress_head], 0,
         sizeof(r->progress_queue[r->progress_head]));
  r->progress_head = (r->progress_head + 1) % JOB_PROGRESS_QUEUE_CAP;
  r->progress_count--;
  return 0;
}

unsigned long long rt_jobrunner_progress_dropped(const RtJobRunner *r) {
  return r ? r->progress_dropped : 0;
}

int rt_jobrunner_cancel_run(RtJobRunner *r, const char *run_id,
                            uint64_t now_ms) {
  RtJob *j;
  int n = 0;
  if (!r || !run_id || !*run_id) return 0;
  for (j = r->jobs; j; j = j->next) {
    if (j->state != JOB_RUNNING || strcmp(j->run_id, run_id) != 0)
      continue;
    send_sigterm(j, now_ms);
    n++;
  }
  return n;
}

RtJob *rt_jobrunner_pop_finished(RtJobRunner *r) {
  RtJob *j;
  if (!r) return NULL;
  for (j = r->jobs; j; j = j->next) {
    if (j->state == JOB_FINISHED || j->state == JOB_FAILED) {
      unlink_job(r, j);
      return j;
    }
  }
  return NULL;
}

int rt_jobrunner_shutdown_all(RtJobRunner *r, uint64_t grace_ms) {
  RtJob *j;
  uint64_t deadline;
  if (!r) return -1;
  if (grace_ms == 0) grace_ms = r->kill_grace_ms;
  deadline = fc_now_ms() + grace_ms;

  /* SIGTERM every running job. */
  for (j = r->jobs; j; j = j->next) {
    if (j->state == JOB_RUNNING && j->pid > 0) {
      (void)kill(j->pid, SIGTERM);
      j->sigterm_at_ms = fc_now_ms();
    }
  }
  /* Reap until grace expires. */
  while (fc_now_ms() < deadline) {
    int still_running = 0;
    (void)rt_jobrunner_tick(r, fc_now_ms());
    for (j = r->jobs; j; j = j->next) if (j->state == JOB_RUNNING) { still_running = 1; break; }
    if (!still_running) break;
    {
      struct timespec ts = {0, 25 * 1000000L};
      nanosleep(&ts, NULL);
    }
  }
  /* SIGKILL whatever is still up, reap. */
  for (j = r->jobs; j; j = j->next) {
    if (j->state == JOB_RUNNING && j->pid > 0) (void)kill(j->pid, SIGKILL);
  }
  for (int retries = 0; retries < 50; ++retries) {
    int still_running = 0;
    (void)rt_jobrunner_tick(r, fc_now_ms());
    for (j = r->jobs; j; j = j->next) if (j->state == JOB_RUNNING) { still_running = 1; break; }
    if (!still_running) break;
    {
      struct timespec ts = {0, 10 * 1000000L};
      nanosleep(&ts, NULL);
    }
  }
  /* Drain everything from the list. */
  while ((j = rt_jobrunner_pop_finished(r)) != NULL) rt_jobrunner_release(r, j);
  return 0;
}
