/* `fclaw internal ...` subcommands.
 *
 *   agent-llm   — child entrypoint spawned by the gateway's run scheduler.
 *                 Reads agent.json for model.ref, calls llm_call, writes
 *                 the response to --out.
 *   now-ms      — print monotonic milliseconds for elapsed-time math.
 *                 Used by shell smokes for elapsed-time math instead of
 *                 spawning python.
 *   free-port   — bind 0.0.0.0:0, print kernel-assigned port. Same
 *                 reason: smokes need a fresh port without python. */

#include "runtime.h"

#include "agent_exec.h"
#include "gateway/reactor.h"
#include "llm/llm.h"
#include "llm/media.h"
#include "support/timing.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *opt(int *i, int argc, char **argv, const char *flag) {
  if (strcmp(argv[*i], flag) != 0) return NULL;
  if (*i + 1 >= argc) return NULL;
  (*i)++;
  return argv[*i];
}

static int agent_llm_main(int argc, char **argv) {
  const char *agent = NULL, *floop = NULL, *run_dir = NULL, *step_id = NULL;
  const char *in_path = NULL, *out_path = NULL, *media_path = NULL;
  char *step_input = NULL;
  LlmMedia *media = NULL;
  size_t media_count = 0U;
  uint64_t media_bytes = 0U;
  char media_err[RT_LARGE];
  char response[LLM_RESP_MAX];
  char model_ref[RT_SMALL] = "";
  LlmProfile profile;
  int rc;

  for (int i = 0; i < argc; ++i) {
    const char *v;
    if ((v = opt(&i, argc, argv, "--agent"))     != NULL) agent     = v;
    else if ((v = opt(&i, argc, argv, "--floop")) != NULL) floop    = v;
    else if ((v = opt(&i, argc, argv, "--run-dir")) != NULL) run_dir  = v;
    else if ((v = opt(&i, argc, argv, "--step-id")) != NULL) step_id  = v;
    else if ((v = opt(&i, argc, argv, "--in"))      != NULL) in_path  = v;
    else if ((v = opt(&i, argc, argv, "--out"))     != NULL) out_path = v;
    else if ((v = opt(&i, argc, argv, "--media"))   != NULL) media_path = v;
    else {
      fprintf(stderr, "internal agent-llm: unknown arg %s\n", argv[i]);
      return 2;
    }
  }
  if (!agent || !run_dir || !step_id || !in_path || !out_path) {
    fprintf(stderr,
            "usage: fclaw internal agent-llm --agent <id> --run-dir <dir> "
            "--step-id <id> --in <path> --out <path> [--floop <name>] "
            "[--media <manifest>]\n");
    return 2;
  }

  /* Read agent.json to pick up model.ref. */
  if (rt_agent_read_meta(floop ? floop : rt_default_loop(), agent, NULL, 0,
                         model_ref, sizeof(model_ref), NULL, 0) != 0)
    model_ref[0] = '\0';
  if (!model_ref[0]) {
    fprintf(stderr, "internal agent-llm: agent %s missing model.ref\n", agent);
    return 1;
  }
  {
    char profile_err[RT_LARGE];
    if (llm_load_profile_with_error(model_ref, &profile, profile_err,
                                    sizeof(profile_err)) != 0) {
      fprintf(stderr, "internal agent-llm: %s\n",
              profile_err[0] ? profile_err : "model profile load failed");
      return 1;
    }
  }

  /* Read agent step input. */
  if (fs_read_text(in_path, &step_input, FS_READ_TEXT_DEFAULT_CAP) != 0 || !step_input) {
    fprintf(stderr, "internal agent-llm: could not read %s\n", in_path);
    return 1;
  }
  (void)step_id;
  if (media_path &&
      llm_media_load(run_dir, media_path, &media, &media_count, &media_bytes,
                     media_err, sizeof(media_err)) != 0) {
    fprintf(stderr, "internal agent-llm: media load failed: %s\n",
            media_err[0] ? media_err : "unknown media error");
    free(step_input);
    return 1;
  }
  {
    LlmRequest request = {step_input, media, media_count};
    rc = llm_call_request(run_dir, floop ? floop : rt_default_loop(), agent,
                          &profile, &request,
                          response, sizeof(response));
  }
  (void)media_bytes;
  llm_media_dispose(media);
  free(step_input);
  if (rc != 0) {
    if (response[0] == '{')
      fprintf(stderr, "internal agent-llm: llm_call failed for %s: %.240s\n",
              agent, response);
    else
      fprintf(stderr, "internal agent-llm: llm_call failed for %s\n", agent);
    return 1;
  }
  if (fs_write_text_atomic(out_path, response) != 0) {
    fprintf(stderr, "internal agent-llm: could not write %s\n", out_path);
    return 1;
  }
  return 0;
}

static int now_ms_main(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("%llu\n", (unsigned long long)fc_now_ms());
  return 0;
}

/* detach — run a command as a session-detached daemon child. Double
 * fork + setsid, stdio replaced with /dev/null (redirect inside the
 * command if a log is wanted), environment and cwd inherited. This is
 * the blessed detacher for subprocess managed actions that spawn a
 * completion waiter: it survives the action's own timeout kill, the
 * gateway's shutdown sweep, and a foreground Ctrl-C, exactly like the
 * intrinsic codex/claude runners (macOS ships no setsid(1)). */
static int detach_main(int argc, char **argv) {
  int start = 0;
  pid_t first;
  int pipefd[2];
  if (argc > 0 && strcmp(argv[0], "--") == 0) start = 1;
  if (argc - start < 1) {
    fprintf(stderr, "usage: fclaw internal detach -- <command> [args...]\n");
    return 2;
  }
  if (pipe(pipefd) != 0) {
    fprintf(stderr, "internal detach: pipe failed\n");
    return 1;
  }
  first = fork();
  if (first < 0) {
    close(pipefd[0]); close(pipefd[1]);
    fprintf(stderr, "internal detach: fork failed\n");
    return 1;
  }
  if (first == 0) {
    pid_t worker;
    close(pipefd[0]);
    if (setsid() < 0) _exit(111);
    worker = fork();
    if (worker < 0) _exit(112);
    if (worker > 0) {
      (void)write(pipefd[1], &worker, sizeof(worker));
      close(pipefd[1]);
      _exit(0);
    }
    close(pipefd[1]);
    {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        (void)dup2(devnull, STDIN_FILENO);
        (void)dup2(devnull, STDOUT_FILENO);
        (void)dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
      }
    }
    execvp(argv[start], &argv[start]);
    _exit(127);
  }
  close(pipefd[1]);
  {
    pid_t worker_pid = 0;
    ssize_t got = read(pipefd[0], &worker_pid, sizeof(worker_pid));
    int status = 0;
    close(pipefd[0]);
    (void)waitpid(first, &status, 0);
    if (got != (ssize_t)sizeof(worker_pid) || worker_pid <= 0) {
      fprintf(stderr, "internal detach: launch failed\n");
      return 1;
    }
    printf("{\"ok\":true,\"pid\":%lld}\n", (long long)worker_pid);
  }
  return 0;
}

static int free_port_main(int argc, char **argv) {
  (void)argc; (void)argv;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { perror("socket"); return 1; }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind"); close(s); return 1;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(s, (struct sockaddr *)&addr, &len) != 0) {
    perror("getsockname"); close(s); return 1;
  }
  printf("%u\n", (unsigned)ntohs(addr.sin_port));
  close(s);
  return 0;
}

static int pulse_json_main(int argc, char **argv) {
  const char *root = ".";
  int messages = 40, runs = 25;
  long long now_ms = (long long)timing_wall_ms();
  char *json;
  char err[RT_LARGE] = "";
  for (int i = 0; i < argc; ++i) {
    const char *v;
    if ((v = opt(&i, argc, argv, "--root")) != NULL) root = v;
    else if ((v = opt(&i, argc, argv, "--messages")) != NULL)
      messages = atoi(v);
    else if ((v = opt(&i, argc, argv, "--runs")) != NULL)
      runs = atoi(v);
    else if ((v = opt(&i, argc, argv, "--now-ms")) != NULL)
      now_ms = strtoll(v, NULL, 10);
    else {
      fprintf(stderr, "internal pulse-json: unknown arg %s\n", argv[i]);
      return 2;
    }
  }
  json = (char *)malloc(512U * 1024U);
  if (!json) return 1;
  if (rt_pulse_projection_json(root, now_ms, messages, runs,
                               json, 512U * 1024U,
                               err, sizeof(err)) != 0) {
    fprintf(stderr, "internal pulse-json: %s\n",
            err[0] ? err : "projection failed");
    free(json);
    return 1;
  }
  fputs(json, stdout);
  free(json);
  return 0;
}

int rt_internal_main(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "usage: fclaw internal <subcommand> ...\n"
                    "  subcommands: agent-llm, now-ms, free-port, pulse-json, detach\n");
    return 2;
  }
  if (strcmp(argv[0], "agent-llm") == 0) return agent_llm_main(argc - 1, argv + 1);
  if (strcmp(argv[0], "detach")    == 0) return detach_main(argc - 1, argv + 1);
  if (strcmp(argv[0], "now-ms")    == 0) return now_ms_main(argc - 1, argv + 1);
  if (strcmp(argv[0], "free-port") == 0) return free_port_main(argc - 1, argv + 1);
  if (strcmp(argv[0], "pulse-json") == 0) return pulse_json_main(argc - 1, argv + 1);
  fprintf(stderr, "internal: unknown subcommand %s\n", argv[0]);
  return 2;
}
