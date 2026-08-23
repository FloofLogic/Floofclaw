#include "channel.h"

#include "../bus/bus.h"
#include "../gateway/gateway.h"
#include "../gateway/reactor.h"
#include "../runtime_kernel.h"
#include "../runtime.h"
#include "../support/color.h"
#include "../support/cli_output.h"
#include "../support/fsutil.h"
#include "../support/heap_guard.h"
#include "../support/json.h"
#include "../support/timing.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHANNEL_LINE_MAX 8192
#define CHANNEL_DELIVERIES_LOG "workspace/logs/deliveries.jsonl"
#define CHANNEL_DELIVERY_TIMEOUT_MS 180000
#define CHANNEL_DELIVERY_POLL_US 50000
#define CHANNEL_EMBEDDED_NAP_US  250

static void channel_log(const char *channel, const char *direction, const char *text) {
  char path[256];
  char ts[64];
  char escaped[CHANNEL_LINE_MAX];
  char line[CHANNEL_LINE_MAX + 256];
  time_t now;
  struct tm tmv;
  if (fs_mkdir_p(CHANNEL_LOG_DIR) != 0) return;
  snprintf(path, sizeof(path), "%s/%s.log", CHANNEL_LOG_DIR, channel);
  now = time(NULL);
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  if (json_escape(text ? text : "", escaped, sizeof(escaped)) != 0) escaped[0] = '\0';
  snprintf(line, sizeof(line),
           "{\"ts\":\"%s\",\"direction\":\"%s\",\"text\":\"%s\"}\n",
           ts, direction, escaped);
  (void)fs_append_text(path, line);
}

static long long delivery_log_size(void) {
  struct stat st;
  if (stat(CHANNEL_DELIVERIES_LOG, &st) != 0) return 0;
  return (long long)st.st_size;
}

static long long monotonic_ms(void) {
  return (long long)timing_monotonic_ms();
}

/* Top-level request_id stamped on the failure delivery emitted by the
 * scheduler when a run retires in RT_RUN_FAILED. Used by the CLI to
 * tell run_done deliveries from run_failed deliveries. */
#define CHANNEL_FAILURE_REQUEST_ID "run_failed"

static int delivery_line_matches(const char *line, const char *origin_event_id,
                                 const char *channel,
                                 char *response_out, size_t response_out_len,
                                 char *run_id_out, size_t run_id_out_len,
                                 int *is_failure_out) {
  JsonRef root, delivery;
  char got_origin[RT_SMALL] = "";
  char got_channel[RT_SMALL] = "";
  char request_id[RT_SMALL] = "";
  char text[RT_LARGE] = "";
  if (!line || !channel) return 0;
  if (json_ref_first_object(line, &root) != 0) return 0;
  if (run_id_out && run_id_out_len > 0) {
    if (json_ref_object_get_string(&root, "run_id", run_id_out, run_id_out_len) != 0)
      run_id_out[0] = '\0';
  }
  if (json_ref_object_get_object(&root, "delivery", &delivery) != 0) return 0;
  if (origin_event_id &&
      json_ref_object_get_string(&delivery, "origin_event_id", got_origin, sizeof(got_origin)) != 0) return 0;
  if (json_ref_object_get_string(&delivery, "channel", got_channel, sizeof(got_channel)) != 0) return 0;
  if ((origin_event_id && strcmp(got_origin, origin_event_id) != 0) ||
      strcmp(got_channel, channel) != 0) return 0;
  if (json_ref_object_get_string(&delivery, "text", text, sizeof(text)) != 0) return 0;
  snprintf(response_out, response_out_len, "%s", text);
  if (is_failure_out) {
    if (json_ref_object_get_string(&root, "request_id", request_id, sizeof(request_id)) != 0)
      request_id[0] = '\0';
    *is_failure_out = (strcmp(request_id, CHANNEL_FAILURE_REQUEST_ID) == 0);
  }
  return 1;
}

static int find_delivery_since(long long *cursor, const char *origin_event_id,
                               const char *channel,
                               char *response_out, size_t response_out_len,
                               char *run_id_out, size_t run_id_out_len,
                               int *is_failure_out) {
  FILE *fp;
  char line[RT_XL];
  long pos;
  if (!cursor || !origin_event_id || !channel || !response_out) return -1;
  fp = fopen(CHANNEL_DELIVERIES_LOG, "rb");
  if (!fp) return 1;
  if (fseek(fp, *cursor, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  while (fgets(line, sizeof(line), fp)) {
    if (delivery_line_matches(line, origin_event_id, channel,
                              response_out, response_out_len,
                              run_id_out, run_id_out_len,
                              is_failure_out)) {
      pos = ftell(fp);
      if (pos >= 0) *cursor = (long long)pos;
      fclose(fp);
      return 0;
    }
  }
  pos = ftell(fp);
  if (pos >= 0) *cursor = (long long)pos;
  fclose(fp);
  return 1;
}

static int find_channel_delivery_since(long long *cursor, const char *channel,
                                       char *response_out, size_t response_out_len,
                                       char *run_id_out, size_t run_id_out_len) {
  FILE *fp;
  char line[RT_XL];
  long pos;
  if (!cursor || !channel || !response_out) return -1;
  fp = fopen(CHANNEL_DELIVERIES_LOG, "rb");
  if (!fp) return 1;
  if (fseek(fp, *cursor, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  while (fgets(line, sizeof(line), fp)) {
    if (delivery_line_matches(line, NULL, channel, response_out, response_out_len,
                              run_id_out, run_id_out_len, NULL)) {
      pos = ftell(fp);
      if (pos >= 0) *cursor = (long long)pos;
      fclose(fp);
      return 0;
    }
  }
  pos = ftell(fp);
  if (pos >= 0) *cursor = (long long)pos;
  fclose(fp);
  return 1;
}

static int publish_channel_text(const char *channel, const char *user_text,
                                char *id, size_t id_len) {
  char escaped[RT_LARGE];
  char payload[RT_LARGE + 64];
  if (!channel || !user_text) return -1;
  if (json_escape(user_text, escaped, sizeof(escaped)) != 0) return -1;
  snprintf(payload, sizeof(payload), "{\"text\":\"%s\"}", escaped);
  if (bus_publish(channel, "user_message", payload, id, id_len) != 0) return -1;
  channel_log(channel, "in", user_text);
  return 0;
}

static int refuse_if_workspace_task_collision(const char *channel,
                                              char *response_out,
                                              size_t response_out_len,
                                              char *run_id_out,
                                              size_t run_id_out_len) {
  char next_run_id[RT_SMALL] = "";
  int rc = rt_workspace_next_run_task_collision("workspace",
                                                next_run_id,
                                                sizeof(next_run_id));
  if (rc != 1) return rc < 0 ? -1 : 0;
  if (run_id_out && run_id_out_len) run_id_out[0] = '\0';
  snprintf(response_out, response_out_len,
           "Run refused: workspace_task_id_collision: next run_id %s conflicts with existing task ids. "
           "Run `./bin/fclaw clear --yes` to start fresh, or restore workspace/runs to match "
           "workspace/memory/state. See docs/getting-started/project-layout.md#starting-fresh.",
           next_run_id[0] ? next_run_id : "(unknown)");
  channel_log(channel ? channel : "cli", "error", response_out);
  return 1;
}

/* Tri-state return for the CLI delivery wait:
 *   0 -> run_done: response_out holds a real reply
 *   2 -> run_failed: response_out holds the "Run failed: …" text
 *   1 -> no terminal delivery within timeout, or setup/I-O error */
static int wait_for_delivery(long long cursor, const char *origin_event_id,
                             const char *channel,
                             char *response_out, size_t response_out_len,
                             char *run_id_out, size_t run_id_out_len) {
  long long deadline = monotonic_ms() + CHANNEL_DELIVERY_TIMEOUT_MS;
  while (monotonic_ms() < deadline) {
    int is_failure = 0;
    int rc = find_delivery_since(&cursor, origin_event_id, channel,
                                 response_out, response_out_len,
                                 run_id_out, run_id_out_len,
                                 &is_failure);
    if (rc == 0) return is_failure ? 2 : 0;
    if (rc < 0) return 1;
    usleep(CHANNEL_DELIVERY_POLL_US);
  }
  return 1;
}

/* Resolve the live runtime owner's floop at the gateway boundary. A missing
 * gateway is not an error here because synchronous ws/irc/run clients can
 * host an embedded owner. Explicit client selection is an assertion when a
 * gateway is live; it never reroutes one message to another floop. */
static int channel_live_floop(const char *channel, const char *label,
                              const char *requested,
                              char *actual, size_t actual_len) {
  char message[RT_LARGE];
  int rc = gateway_running_floop(actual, actual_len);
  if (rc == 1) return 1;
  if (rc != 0) {
    snprintf(message, sizeof(message),
             "%s: live gateway floop is unavailable; fix: restart it with "
             "`./bin/fclaw gateway reload --floop <name>`",
             label ? label : "channel");
    channel_log(channel ? channel : "cli", "error", message);
    fprintf(stderr, "%s\n", message);
    return -1;
  }
  if (requested && requested[0] && strcmp(requested, actual) != 0) {
    snprintf(message, sizeof(message),
             "%s: --floop '%s' conflicts with live gateway floop '%s'; "
             "fix: run `./bin/fclaw gateway reload --floop %s`",
             label ? label : "channel", requested, actual, requested);
    channel_log(channel ? channel : "cli", "error", message);
    fprintf(stderr, "%s\n", message);
    return CHANNEL_FLOOP_CONFLICT;
  }
  return 0;
}

static int channel_require_live_gateway(const char *channel, const char *label,
                                        const char *requested,
                                        char *actual, size_t actual_len) {
  int rc = channel_live_floop(channel, label, requested, actual, actual_len);
  if (rc != 1) return rc;
  channel_log(channel ? channel : "cli", "error", "gateway not running");
  fprintf(stderr, "%s: gateway not running; start `./bin/fclaw gateway run` or `./bin/fclaw gateway start`\n",
          label ? label : "channel");
  return -1;
}

static int embedded_owner_lock(void) {
  int fd;
  if (fs_mkdir_p(".fclaw/run") != 0) return -1;
  fd = open(GW_RUNTIME_OWNER_LOCK_PATH, O_CREAT | O_RDWR, 0644);
  if (fd < 0) return -1;
  if (flock(fd, LOCK_EX) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void embedded_owner_unlock(int fd) {
  if (fd < 0) return;
  (void)flock(fd, LOCK_UN);
  close(fd);
}

/* Tri-state matching wait_for_delivery: 0=done, 2=failed, 1=timeout/setup. */
static int embedded_owner_wait_for_delivery(const char *loop_name,
                                            long long cursor,
                                            const char *origin_event_id,
                                            const char *channel,
                                            char *response_out,
                                            size_t response_out_len,
                                            char *run_id_out,
                                            size_t run_id_out_len) {
  RtScheduler *scheduler;
  char err[RT_LARGE] = "";
  long long deadline = monotonic_ms() + CHANNEL_DELIVERY_TIMEOUT_MS;
  int rc = 1;
  int have_delivery = 0;
  char settled_without_delivery[RT_SMALL] = "";
  scheduler = (RtScheduler *)fc_xcalloc(1, sizeof(*scheduler));
  if (!scheduler) return 1;
  if (rt_scheduler_init(scheduler, loop_name, err, sizeof(err)) != 0) {
    channel_log(channel, "error", err[0] ? err : "embedded runtime owner init failed");
    fprintf(stderr, "%s\n", err[0] ? err :
            "embedded runtime owner init failed; fix: check config/floofclaw_config.json");
    fc_xfree(scheduler);
    return 1;
  }
  while (monotonic_ms() < deadline) {
    uint64_t now = fc_now_ms();
    int is_failure = 0;
    rt_scheduler_tick(scheduler, now);
    rc = find_delivery_since(&cursor, origin_event_id, channel,
                             response_out, response_out_len,
                             run_id_out, run_id_out_len,
                             &is_failure);
    if (rc == 0) have_delivery = 1;
    else if (rc < 0) break;
    if (scheduler->last_retired_origin_event_id[0] &&
        strcmp(scheduler->last_retired_origin_event_id, origin_event_id) == 0) {
      int correlated_active = 0;
      if (run_id_out && run_id_out_len > 0 && !run_id_out[0])
        snprintf(run_id_out, run_id_out_len, "%s", scheduler->last_retired_run_id);
      /* Authoritative status from the scheduler — `is_failure` from the
       * delivery line is only a hint; the run-state machine is the truth. */
      if (scheduler->last_retired_state == RT_RUN_FAILED || have_delivery) {
        rc = scheduler->last_retired_state == RT_RUN_FAILED ? 2 : 0;
        rt_scheduler_destroy(scheduler);
        fc_xfree(scheduler);
        return rc;
      }
      for (size_t i = 0; i < scheduler->run_count; ++i) {
        if (scheduler->runs[i] &&
            strcmp(scheduler->runs[i]->ctx.origin_event_id, origin_event_id) == 0) {
          correlated_active = 1;
          break;
        }
      }
      /* A done run may have published a correlated follow-up envelope
       * (managed-operation results use this path). Give intake one tick
       * to expose that run before accepting a legitimate empty reply. */
      if (!correlated_active &&
          strcmp(settled_without_delivery, scheduler->last_retired_run_id) == 0) {
        rt_scheduler_destroy(scheduler);
        fc_xfree(scheduler);
        return 0;
      }
      if (strcmp(settled_without_delivery, scheduler->last_retired_run_id) != 0)
        snprintf(settled_without_delivery, sizeof(settled_without_delivery), "%s",
                 scheduler->last_retired_run_id);
    }
    usleep(CHANNEL_EMBEDDED_NAP_US);
  }
  rt_scheduler_shutdown(scheduler, 3000);
  rt_scheduler_destroy(scheduler);
  fc_xfree(scheduler);
  return 1;
}

/* Synchronously publish a user_message and wait for a terminal delivery.
 * Returns:
 *   0 -> run_done; response_out is the reply
 *   2 -> run_failed; response_out is the "Run failed: …" message
 *   1 -> setup error or no terminal delivery within timeout
 *   3 -> explicit client floop conflicts with the live gateway floop */
int channel_synchronous(const char *channel, const char *loop_name,
                        const char *user_text,
                        char *response_out, size_t response_out_len,
                        char *run_id_out, size_t run_id_out_len) {
  char id[BUS_ID_MAX];
  long long cursor;
  int owner_fd = -1;
  int live_gateway;
  int rc;
  const char *embedded_loop;
  char live_loop[RT_SMALL] = "";
  if (!channel || !user_text || !response_out || response_out_len == 0) return 1;
  response_out[0] = '\0';
  if (run_id_out && run_id_out_len > 0) run_id_out[0] = '\0';
  rc = channel_live_floop(channel, channel, loop_name,
                          live_loop, sizeof(live_loop));
  if (rc == CHANNEL_FLOOP_CONFLICT) return CHANNEL_FLOOP_CONFLICT;
  if (rc < 0) return 1;
  live_gateway = (rc == 0);
  embedded_loop = (loop_name && loop_name[0]) ? loop_name : rt_default_loop();
  rc = refuse_if_workspace_task_collision(channel, response_out, response_out_len,
                                          run_id_out, run_id_out_len);
  if (rc > 0) return 2;
  if (rc < 0) return 1;
  if (!live_gateway) {
    owner_fd = embedded_owner_lock();
    if (owner_fd < 0) {
      channel_log(channel, "error", "embedded runtime owner lock failed");
      return 1;
    }
  }
  cursor = delivery_log_size();
  if (publish_channel_text(channel, user_text, id, sizeof(id)) != 0) {
    embedded_owner_unlock(owner_fd);
    return 1;
  }
  rc = live_gateway
       ? wait_for_delivery(cursor, id, channel, response_out, response_out_len,
                           run_id_out, run_id_out_len)
       : embedded_owner_wait_for_delivery(embedded_loop, cursor, id, channel,
                                          response_out, response_out_len,
                                          run_id_out, run_id_out_len);
  embedded_owner_unlock(owner_fd);
  if (rc == 1) {
    channel_log(channel, "error", "delivery wait failed");
    return 1;
  }
  channel_log(channel, "out", response_out);
  return rc;
}

static int channel_synchronous_gateway_only(const char *channel,
                                            const char *user_text,
                                            char *response_out, size_t response_out_len,
                                            char *run_id_out, size_t run_id_out_len) {
  char id[BUS_ID_MAX];
  char live_loop[RT_SMALL] = "";
  long long cursor;
  int rc;
  if (!channel || !user_text || !response_out || response_out_len == 0) return 1;
  response_out[0] = '\0';
  if (run_id_out && run_id_out_len > 0) run_id_out[0] = '\0';
  if (channel_require_live_gateway(channel, channel, NULL,
                                   live_loop, sizeof(live_loop)) != 0) return 1;
  rc = refuse_if_workspace_task_collision(channel, response_out, response_out_len,
                                          run_id_out, run_id_out_len);
  if (rc > 0) return 2;
  if (rc < 0) return 1;
  cursor = delivery_log_size();
  if (publish_channel_text(channel, user_text, id, sizeof(id)) != 0) return 1;
  rc = wait_for_delivery(cursor, id, channel, response_out, response_out_len,
                         run_id_out, run_id_out_len);
  if (rc == 1) {
    channel_log(channel, "error", "delivery wait failed");
    return 1;
  }
  channel_log(channel, "out", response_out);
  return rc;
}

static void print_cli_prompt(void) {
  printf(COLOR_GREEN_BRIGHT "you> " COLOR_RESET);
  fflush(stdout);
}

static void print_cli_delivery(const char *response, const char *run_id) {
  printf("\r\033[2K" COLOR_BLUE_BRIGHT "fclaw>" COLOR_RESET " %s\n", response ? response : "");
  if (run_id && run_id[0]) printf(COLOR_BRIGHT_BLACK "  [%s]" COLOR_RESET "\n", run_id);
  print_cli_prompt();
}

static void print_cli_spinner(size_t *idx) {
  static const char *frames[] = {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"};
  printf("\r" COLOR_CYAN "%s Responding..." COLOR_RESET, frames[*idx]);
  fflush(stdout);
  *idx = (*idx + 1) % (sizeof(frames) / sizeof(frames[0]));
}

static int drain_cli_deliveries(long long *cursor) {
  int printed = 0;
  while (1) {
    char response[RT_LARGE] = "";
    char run_id[RT_SMALL] = "";
    int rc = find_channel_delivery_since(cursor, "cli", response, sizeof(response),
                                         run_id, sizeof(run_id));
    if (rc == 1) return printed;
    if (rc < 0) return -1;
    channel_log("cli", "out", response);
    print_cli_delivery(response, run_id);
    printed++;
  }
}

static int stdin_line_ready(void) {
  fd_set rfds;
  struct timeval tv;
  int rc;
  FD_ZERO(&rfds);
  FD_SET(STDIN_FILENO, &rfds);
  tv.tv_sec = 0;
  tv.tv_usec = CHANNEL_DELIVERY_POLL_US;
  rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
  if (rc < 0 && errno == EINTR) return 0;
  if (rc < 0) return -1;
  return FD_ISSET(STDIN_FILENO, &rfds) ? 1 : 0;
}

static int channel_run_cli_live(const char *loop_name) {
  long long cursor;
  char line[CHANNEL_LINE_MAX];
  int spinning = 0;
  size_t spinner_idx = 0;
  printf(COLOR_CYAN "fclaw" COLOR_RESET " interactive (loop=%s). type to chat, ^D to quit.\n", loop_name);
  cursor = delivery_log_size();
  print_cli_prompt();
  while (1) {
    char id[BUS_ID_MAX];
    int ready;
    int drained;
    drained = drain_cli_deliveries(&cursor);
    if (drained < 0) {
      fprintf(stderr, "\ncli: failed to read deliveries\n");
      break;
    }
    if (drained > 0) spinning = 0;
    ready = stdin_line_ready();
    if (ready < 0) {
      fprintf(stderr, "\ncli: stdin wait failed\n");
      break;
    }
    if (!ready) {
      if (spinning) print_cli_spinner(&spinner_idx);
      continue;
    }
    if (!fgets(line, sizeof(line), stdin)) break;
    fs_trim_newline(line);
    if (line[0] == '\0') {
      print_cli_prompt();
    } else if (publish_channel_text("cli", line, id, sizeof(id)) != 0) {
      fprintf(stderr, "cli: failed to publish message\n");
      print_cli_prompt();
    } else {
      spinning = 1;
      spinner_idx = 0;
      print_cli_spinner(&spinner_idx);
    }
  }
  printf("\nbye\n");
  return 0;
}

static void print_agent_channel_result(const char *channel, int rc,
                                       const char *response,
                                       const char *run_id) {
  fputs("{\"channel\":", stdout); fc_cli_print_json_string(stdout, channel);
  fputs(",\"run_id\":", stdout); fc_cli_print_json_string(stdout, run_id ? run_id : "");
  fputs(",\"status\":", stdout);
  fc_cli_print_json_string(stdout, rc == 0 ? "done" : (rc == 2 ? "failed" : "error"));
  fputs(",\"result\":", stdout); fc_cli_print_json_string(stdout, response ? response : "");
  puts("}");
  fflush(stdout);
}

int channel_run_cli_interactive(const char *loop_name, int human) {
  char line[CHANNEL_LINE_MAX];
  char response[RT_LARGE];
  char run_id[RT_SMALL];
  char live_loop[RT_SMALL] = "";
  int interactive = isatty(fileno(stdin));
  if (channel_require_live_gateway("cli", "cli", loop_name,
                                   live_loop, sizeof(live_loop)) != 0) return 1;
  if (interactive && human) return channel_run_cli_live(live_loop);
  /* Stdout defaults to block-buffered when not a TTY (POSIX). The
   * chat-smoke harness drives fclaw through a FIFO that stays open
   * across turns, so a block-buffered stdout would hold each reply
   * until exit. Line-buffer so external readers see one reply per
   * turn — what every well-behaved CLI does. */
  setvbuf(stdout, NULL, _IOLBF, 0);
  while (1) {
    if (!fgets(line, sizeof(line), stdin)) break;
    fs_trim_newline(line);
    if (line[0] == '\0') continue;
    /* rc == 1 is the only "no response" case; rc == 2 (run_failed) still
     * carries a "Run failed: …" message that the user should see. */
    int sync_rc = channel_synchronous_gateway_only("cli", line, response,
                                                   sizeof(response), run_id,
                                                   sizeof(run_id));
    if (sync_rc == 1) {
      fprintf(stderr, "cli: failed to receive gateway delivery\n");
      if (!human)
        fc_cli_print_json_error("channel.cli", "delivery_failed",
                                "failed to receive gateway delivery");
      continue;
    }
    if (human)
      printf(COLOR_BLUE_BRIGHT "fclaw>" COLOR_RESET " %s\n", response);
    else
      print_agent_channel_result("cli", sync_rc, response, run_id);
  }
  return 0;
}

int channel_run_ws(const char *loop_name, int human) {
  char line[CHANNEL_LINE_MAX];
  char user_text[RT_LARGE];
  char response[RT_LARGE];
  char run_id[RT_SMALL];
  char live_loop[RT_SMALL] = "";
  int preflight = channel_live_floop("ws", "ws", loop_name,
                                     live_loop, sizeof(live_loop));
  if (preflight == CHANNEL_FLOOP_CONFLICT || preflight < 0) return 1;
  while (fgets(line, sizeof(line), stdin)) {
    JsonRef root;
    fs_trim_newline(line);
    if (line[0] == '\0') continue;
    if (json_ref_first_object(line, &root) != 0 ||
        json_ref_object_get_string(&root, "text", user_text, sizeof(user_text)) != 0) {
      fprintf(stderr, "ws: bad input line, expected {\"text\":\"...\"}\n");
      continue;
    }
    int sync_rc = channel_synchronous("ws", loop_name, user_text,
                                      response, sizeof(response), run_id,
                                      sizeof(run_id));
    if (sync_rc == CHANNEL_FLOOP_CONFLICT) return 1;
    if (sync_rc == 1) {
      if (human)
        printf(COLOR_RED_BRIGHT "loop failed" COLOR_RESET "\n");
      else
        fc_cli_print_json_error("channel.ws", "loop_failed", "loop failed");
      fflush(stdout);
      continue;
    }
    if (human)
      printf(COLOR_BLUE_BRIGHT "fclaw>" COLOR_RESET " %s\n", response);
    else
      print_agent_channel_result("ws", sync_rc, response, run_id);
  }
  return 0;
}

int channel_run_irc(const char *loop_name, int human) {
  /* Simple line-oriented IRC stub: each input line is a chat message; each
   * response goes back as a single output line. Real IRC transport (server
   * connection, PRIVMSG framing, channels) is intentionally deferred. */
  char line[CHANNEL_LINE_MAX];
  char response[RT_LARGE];
  char run_id[RT_SMALL];
  char live_loop[RT_SMALL] = "";
  int preflight = channel_live_floop("irc", "irc", loop_name,
                                     live_loop, sizeof(live_loop));
  if (preflight == CHANNEL_FLOOP_CONFLICT || preflight < 0) return 1;
  while (fgets(line, sizeof(line), stdin)) {
    fs_trim_newline(line);
    if (line[0] == '\0') continue;
    int sync_rc = channel_synchronous("irc", loop_name, line,
                                      response, sizeof(response), run_id,
                                      sizeof(run_id));
    if (sync_rc == CHANNEL_FLOOP_CONFLICT) return 1;
    if (sync_rc == 1) {
      if (human)
        printf(COLOR_RED_BRIGHT "loop failed" COLOR_RESET "\n");
      else
        fc_cli_print_json_error("channel.irc", "loop_failed", "loop failed");
      fflush(stdout);
      continue;
    }
    if (human)
      printf(COLOR_BLUE_BRIGHT "fclaw>" COLOR_RESET " %s\n", response);
    else
      print_agent_channel_result("irc", sync_rc, response, run_id);
  }
  return 0;
}
