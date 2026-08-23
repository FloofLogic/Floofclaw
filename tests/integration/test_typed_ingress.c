/* Generic typed-event ingress — the fourth way into the bus.
 *
 * A local producer publishes one custom kind with an opaque JSON object.
 * The kernel assigns identity, routes it into a context, and hands the
 * kind and payload to the floop unchanged. Everything asserted here is a
 * boundary contract: what the runtime admits, what it preserves verbatim,
 * what it refuses, and what it must NOT do on its own (no chat memory, no
 * task, no correlation, no model call). */

#include "test_support.h"
#include "harness.h"

#include "../../runtime/bus/bus.h"
#include "../../runtime/runtime.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCAN_KIND  "device_scan_requested"
#define OTHER_KIND "build_observation_changed"
#define SCAN_PAYLOAD                                                    \
  "{\"scope\":\"installed_applications\",\"deep\":{\"list\":[1,\"two\"," \
  "{\"three\":true}],\"null\":null},\"count\":42}"

static int write_exec(const char *path, const char *text) {
  if (test_write_file(path, text) != 0) return -1;
  return chmod(path, 0755);
}

/* One synchronous product-shaped action, so the fixture can prove the
 * spec's motivating shape: a deterministic agent maps a typed fact to an
 * approved action AND a user-facing message, with no provider call. */
static int write_probe_action(void) {
  int rc = 0;
  rc |= test_write_file(
      "actions/testfx/tif_probe/action.json",
      "{\"id\":\"tif_probe\",\"description\":\"typed-ingress fixture action\","
      "\"outside_world\":false,"
      "\"args_schema\":{\"type\":\"object\",\"additionalProperties\":false,"
      "\"properties\":{\"scope\":{\"type\":\"string\"}}},"
      "\"exec\":[\"bash\",\"run.sh\"],\"timeout_ms\":30000}\n");
  rc |= write_exec("actions/testfx/tif_probe/run.sh",
                   "#!/usr/bin/env bash\n"
                   "cat >/dev/null\n"
                   "printf '%s\\n' '{\"events\":[],\"result\":"
                   "{\"text\":\"probe ran\"},\"error\":null}'\n");
  return rc;
}

/* `allow_probe` decides whether the handler may call tif_probe. The
 * handler that may not call it still tries: that is the negative half of
 * "action authority remains explicit". */
static int write_handler(const char *floop, const char *agent,
                         const char *reply, int allow_probe) {
  char path[PATH_MAX], body[2048];
  int rc = 0;
  snprintf(path, sizeof(path), "floops/%s/agents/%s/agent.json", floop, agent);
  snprintf(body, sizeof(body),
           "{\"id\":\"%s\",\"executor\":\"script\","
           "\"actions\":[\"message\"%s],\"listen\":[\"event\"]}\n",
           agent, allow_probe ? ",\"tif_probe\"" : "");
  rc |= test_write_file(path, body);
  snprintf(path, sizeof(path), "floops/%s/agents/%s/run.sh", floop, agent);
  snprintf(body, sizeof(body),
           "#!/usr/bin/env bash\n"
           "cat > workspace/%s_input.json\n"
           "echo '{\"calls\":[{\"name\":\"tif_probe\",\"args\":"
           "{\"scope\":\"installed_applications\"}},"
           "{\"name\":\"message\",\"args\":{\"message\":\"%s\"}}]}'\n",
           agent, reply);
  rc |= write_exec(path, body);
  return rc;
}

/* Fixture floop. Two exact event-kind gates plus the two work-correlation
 * gates, so one publication proves both that the right step opens and that
 * the correlation gates stay shut for a product-owned payload. */
static int write_fixture_floop(const char *floop) {
  char path[PATH_MAX], body[2048];
  int rc = 0;
  snprintf(path, sizeof(path), "floops/%s/loop.json", floop);
  snprintf(body, sizeof(body),
           "{\"version\":1,\"name\":\"%s\","
           "\"description\":\"typed-ingress fixture\","
           "\"one_pass\":true,\"serialize_contexts\":true,\n"
           " \"steps\":[\n"
           "  {\"id\":\"consequence\",\"type\":\"agent\",\"agent\":\"tif_other\","
           "\"gate\":\"work_consequence\"},\n"
           "  {\"id\":\"outcome\",\"type\":\"agent\",\"agent\":\"tif_other\","
           "\"gate\":\"terminal_work_outcome\"},\n"
           "  {\"id\":\"scan\",\"type\":\"agent\",\"agent\":\"tif_scan\","
           "\"gate\":\"event_kind:" SCAN_KIND "\"},\n"
           "  {\"id\":\"other\",\"type\":\"agent\",\"agent\":\"tif_other\","
           "\"gate\":\"event_kind:" OTHER_KIND "\"},\n"
           "  {\"id\":\"dispatch\",\"type\":\"builtin\","
           "\"builtin\":\"action_runner\"}\n"
           " ]}\n", floop);
  rc |= test_write_file(path, body);
  rc |= write_probe_action();
  rc |= write_handler(floop, "tif_scan", "scan ack", 1);
  rc |= write_handler(floop, "tif_other", "other ack", 0);
  return rc;
}

static int remove_fixture_floop(const char *floop) {
  char path[PATH_MAX];
  int rc = 0;
  snprintf(path, sizeof(path), "floops/%s", floop);
  rc |= test_remove_path(path);
  rc |= test_remove_path("actions/testfx/tif_probe");
  return rc;
}

/* Run one `fclaw bus ...` invocation in-process. Captures stdout+stderr
 * and the exact exit code so the documented validation/conflict/publish
 * codes are asserted, not just "nonzero". */
static int bus_cli(char **argv, int argc, char **out, int *code) {
  return harness_cli_bus(argc, argv, out, code);
}

/* Same, with stdin redirected from `text` so --payload-stdin is exercised
 * through the real read path. */
static int bus_cli_stdin(char **argv, int argc, const char *text,
                         char **out, int *code) {
  char tmp[] = "/tmp/fclaw_typed_stdin_XXXXXX";
  int fd = mkstemp(tmp);
  int saved, rc;
  if (fd < 0) return -1;
  if (write(fd, text, strlen(text)) != (ssize_t)strlen(text) ||
      lseek(fd, 0, SEEK_SET) != 0) {
    close(fd); unlink(tmp); return -1;
  }
  fflush(stdin);
  saved = dup(STDIN_FILENO);
  if (saved < 0 || dup2(fd, STDIN_FILENO) < 0) {
    if (saved >= 0) close(saved);
    close(fd); unlink(tmp); return -1;
  }
  rc = harness_cli_bus(argc, argv, out, code);
  dup2(saved, STDIN_FILENO);
  close(saved);
  close(fd);
  unlink(tmp);
  return rc;
}

static int publish_scan(const char *context_id, const char *ref,
                        char **out, int *code) {
  char *argv[20];
  int argc = 0;
  argv[argc++] = (char *)"publish";
  argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";       argv[argc++] = (char *)SCAN_KIND;
  argv[argc++] = (char *)"--channel";    argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--adapter-id"; argv[argc++] = (char *)"local_product_android";
  if (context_id) { argv[argc++] = (char *)"--context-id"; argv[argc++] = (char *)context_id; }
  if (ref)        { argv[argc++] = (char *)"--ref";        argv[argc++] = (char *)ref; }
  argv[argc++] = (char *)"--payload";    argv[argc++] = (char *)SCAN_PAYLOAD;
  return bus_cli(argv, argc, out, code);
}

static int count_dir_entries(const char *path, const char *prefix) {
  DIR *d = opendir(path);
  struct dirent *e;
  int n = 0;
  if (!d) return 0;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    if (prefix && strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
    n++;
  }
  closedir(d);
  return n;
}

/* Hand-write one inbox envelope. Admission-boundary tests need shapes the
 * CLI refuses to produce, which is exactly the point: the runtime must
 * refuse them too, whatever wrote the file. */
static int write_envelope(const char *id, const char *body) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "workspace/bus/inbox/%s.json", id);
  return test_write_file(path, body);
}

/* Drive one intake tick and report whether the envelope was refused: 0 =
 * a run was created, 1 = rejected with `reason` named in the log. */
static int expect_admission_rejected(const char *id, const char *body,
                                     const char *reason) {
  HarnessGateway *g;
  char *log = NULL;
  int rc = 0, runs_before, runs_after;
  /* Only the probe envelope may be pending, so "no new run" means this
   * envelope, not some leftover the same tick would have admitted. */
  if (test_remove_path("workspace/bus/inbox") != 0 ||
      write_envelope(id, body) != 0)
    return expect(0, "write test envelope");
  runs_before = count_dir_entries("workspace/runs", "run_");
  g = harness_gateway_init("tif");
  if (!g) return expect(0, "init gateway for admission probe");
  harness_gateway_drive_one_tick(g);
  harness_gateway_close(g);
  runs_after = count_dir_entries("workspace/runs", "run_");
  rc |= expect(runs_after == runs_before, reason);
  if (test_read_file("workspace/logs/rejected_events.jsonl", &log) == 0 && log) {
    rc |= expect_substr(log, reason, reason);
    free(log);
  } else {
    rc |= expect(0, "rejected_events.jsonl exists");
  }
  return rc;
}

int typed_event_gates_replies_and_stays_inspectable(void) {
  HarnessGateway *g = NULL;
  char *out = NULL, *log = NULL, *input = NULL, *runstate = NULL;
  char *argv[16];
  int code = -1, rc = 0, argc;

  rc |= test_reset_workspace();
  rc |= write_fixture_floop("tif");

  /* 1. Inline publication through the public CLI. */
  rc |= expect(publish_scan("device", "{\"session_id\":\"session_123\"}",
                            &out, &code) == 0 && code == 0,
               "inline custom publication succeeds");
  rc |= expect_substr(out ? out : "", "\"event_id\":\"bus_000001\"",
                      "agent-mode success names the runtime-issued id");
  rc |= expect_substr(out ? out : "", "\"type\":\"" SCAN_KIND "\"",
                      "agent-mode success echoes the custom type");
  rc |= expect_substr(out ? out : "", "\"channel\":\"local_product\"",
                      "agent-mode success echoes the channel");
  rc |= expect_substr(out ? out : "", "\"context_id\":\"device\"",
                      "agent-mode success echoes the supplied context");
  free(out); out = NULL;

  /* 1b. The same surface over stdin, for a kind gated to the other step. */
  argc = 0;
  argv[argc++] = (char *)"publish";  argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)OTHER_KIND;
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--context-id"; argv[argc++] = (char *)"device";
  argv[argc++] = (char *)"--payload-stdin";
  rc |= expect(bus_cli_stdin(argv, argc, "{\"observed\":\"green\"}",
                             &out, &code) == 0 && code == 0,
               "stdin custom publication succeeds");
  free(out); out = NULL;

  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "init typed-ingress gateway");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 2, 20000) == 0,
                 "both typed events complete");
    harness_gateway_close(g);
  }

  /* 2. Opaque preservation: the caller's object, byte-for-byte, as the
   *    run's first behavioral event. */
  log = harness_read_event_log("run_001");
  rc |= expect(log != NULL, "run_001 event log exists");
  if (log) {
    rc |= expect_substr(log, "\"type\":\"" SCAN_KIND "\"",
                        "the triggering event keeps the custom type");
    rc |= expect_substr(log, "\"payload\":" SCAN_PAYLOAD,
                        "the triggering payload is preserved verbatim");
    /* 3. No text required, and none invented. */
    rc |= expect_no_substr(log, "\"type\":\"user_message\"",
                           "a custom kind never becomes a user message");
    /* 5/15. The deterministic handler emitted its approved action AND a
     *       user-facing message, all without a model call. */
    rc |= expect_substr(log, "\"action\":\"tif_probe\"",
                        "the allowlisted product action runs");
    rc |= expect_substr(log, "\"text\":\"probe ran\"",
                        "the approved action produced its result");
    rc |= expect_substr(log, "\"action\":\"message\"",
                        "the allowlisted message action runs");
    rc |= expect_no_substr(log, "\"reason\":\"not_allowed\"",
                           "nothing the handler declared was refused");
    rc |= expect_substr(log, "\"type\":\"run_done\"", "run_001 completed");
    free(log); log = NULL;
  }
  /* 5. The other handler asked for the same action without declaring it:
   *    publication grants no authority the allowlist withheld. */
  log = harness_read_event_log("run_002");
  if (log) {
    rc |= expect_substr(log, "\"reason\":\"not_allowed\"",
                        "an action outside the allowlist is refused");
    rc |= expect_no_substr(log, "\"result\":{\"text\":\"probe ran\"}",
                           "the refused action never ran");
    rc |= expect_substr(log, "\"type\":\"run_done\"",
                        "a refused call does not derail the run");
    free(log); log = NULL;
  }

  /* 2b. The agent's own projection carries kind + payload unchanged. */
  rc |= test_read_file("workspace/tif_scan_input.json", &input) == 0
            ? 0 : expect(0, "scan handler recorded its input");
  if (input) {
    rc |= expect_substr(input, "\"kind\":\"" SCAN_KIND "\"",
                        "event projection names the custom kind");
    rc |= expect_substr(input, "\"payload\":" SCAN_PAYLOAD,
                        "event projection carries the opaque payload");
    free(input); input = NULL;
  }

  /* 4. Exact gating: the scan step opened, the unrelated event-kind step
   *    and both work-correlation gates stayed shut. */
  rc |= expect_agent_output_exists("run_001", "scan", ".json");
  rc |= expect_agent_output_not_exists("run_001", "other", ".json");
  rc |= expect_agent_output_not_exists("run_001", "consequence", ".json");
  rc |= expect_agent_output_not_exists("run_001", "outcome", ".json");
  rc |= expect_agent_output_exists("run_002", "other", ".json");
  rc |= expect_agent_output_not_exists("run_002", "scan", ".json");

  /* 6. Return route: the delivery record carries the originating channel
   *    and the caller's opaque ref. */
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &log) == 0
            ? 0 : expect(0, "deliveries.jsonl exists");
  if (log) {
    rc |= expect_substr(log, "\"channel\":\"local_product\"",
                        "delivery routes back to the producer's channel");
    rc |= expect_substr(log, "\"ref\":{\"session_id\":\"session_123\"}",
                        "delivery carries the caller's opaque ref");
    rc |= expect_substr(log, "\"adapter_id\":\"local_product_android\"",
                        "delivery carries the producer's adapter id");
    rc |= expect_substr(log, "\"text\":\"scan ack\"",
                        "delivery carries the handler's message");
    free(log); log = NULL;
  }

  /* 14. Observability through the existing tools. */
  argc = 0;
  argv[argc++] = (char *)"log";  argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type"; argv[argc++] = (char *)SCAN_KIND;
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "bus log filters on the custom type");
  if (out) {
    rc |= expect_substr(out, "\"event_id\":\"bus_000001\"",
                        "bus log shows the committed custom envelope");
    rc |= expect_substr(out, "\"routing\":{\"adapter_id\":"
                             "\"local_product_android\",\"context_id\":"
                             "\"device\",\"ref\":{\"session_id\":"
                             "\"session_123\"}}",
                        "routing rides beside the payload, not inside it");
    rc |= expect_no_substr(out, OTHER_KIND,
                           "bus log --type excludes other kinds");
    free(out); out = NULL;
  }
  runstate = harness_read_runstate("run_001");
  rc |= expect(runstate != NULL, "run_001 runstate exists");
  if (runstate) {
    rc |= expect_substr(runstate,
                        "\"created_from\":{\"event_id\":\"bus_000001\","
                        "\"type\":\"" SCAN_KIND "\"}",
                        "runstate records the custom triggering identity");
    free(runstate); runstate = NULL;
  }
  argc = 0;
  argv[argc++] = (char *)"-a"; argv[argc++] = (char *)"run_001";
  rc |= expect(harness_cli_view(argc, argv, &out, &code) == 0 && code == 0,
               "view -a renders the custom run");
  if (out) {
    rc |= expect_substr(out, "\"type\":\"" SCAN_KIND "\"",
                        "view shows the custom triggering event");
    rc |= expect_substr(out, "\"payload\":" SCAN_PAYLOAD,
                        "view shows the opaque payload");
    free(out); out = NULL;
  }

  /* 14. Replay reduces the custom run cleanly and invents no user text. */
  rc |= expect(harness_cli_replay("workspace/runs/run_001/event_log.jsonl",
                                  &out) == 0,
               "replay reduces the custom run");
  if (out) {
    rc |= expect_substr(out, "\"user_text\":\"\"",
                        "replay leaves user_text empty for a custom kind");
    rc |= expect_substr(out, "\"latest_action\":\"message\"",
                        "replay still reconstructs the run's action outcome");
    free(out); out = NULL;
  }

  /* 15. Zero-model proof: a deterministic floop handled both events. */
  rc |= expect_file_not_exists("workspace/runs/run_001/provider_calls");
  rc |= expect_file_not_exists("workspace/runs/run_002/provider_calls");

  rc |= remove_fixture_floop("tif");
  return rc;
}

int typed_event_adds_no_chat_memory_or_correlation(void) {
  HarnessGateway *g = NULL;
  char *out = NULL, *state = NULL;
  char *argv[20];
  int code = -1, rc = 0, argc;
  /* Correlation-shaped keys are ordinary product data inside a custom
   * payload. Publishing them must not bind work, tasks, or a revision. */
  static const char kForgedCorrelation[] =
      "{\"task_id\":\"task_run_001_000002\",\"work_rev\":7,"
      "\"request_id\":\"actionreq_run_001_000003\","
      "\"source_event_id\":\"evt_run_001_000001\","
      "\"context_id\":\"chat:tests\",\"text\":\"pretend chat\"}";

  rc |= test_reset_workspace();
  rc |= write_fixture_floop("tif");

  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)SCAN_KIND;
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--context-id"; argv[argc++] = (char *)"device";
  argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)kForgedCorrelation;
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "publish a custom event carrying correlation-shaped keys");
  free(out); out = NULL;

  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "init gateway for the memory/correlation probe");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 1, 20000) == 0,
                 "the custom event completes");
    harness_gateway_close(g);
  }

  /* 8. No fake chat memory. The conversational record is written from
   *    user_text, which a custom kind never sets. */
  rc |= expect_file_not_exists("workspace/memory/memory.jsonl");

  /* No reducer-owned projection moved on its own. */
  if (test_read_file("workspace/memory/state/tasks.json", &state) == 0 && state) {
    rc |= expect_substr(state, "\"tasks\":[]",
                        "a custom event creates no input task");
    free(state); state = NULL;
  }
  if (test_read_file("workspace/memory/state/work_steps.json", &state) == 0 &&
      state) {
    rc |= expect_no_substr(state, "run_001",
                           "a custom event opens no work-controller lineage");
    free(state); state = NULL;
  }

  /* The correlation gates never opened, and the payload's task_id bound
   *  nothing: the run is an ordinary custom run. */
  rc |= expect_agent_output_exists("run_001", "scan", ".json");
  rc |= expect_agent_output_not_exists("run_001", "consequence", ".json");
  rc |= expect_agent_output_not_exists("run_001", "outcome", ".json");
  if (test_read_file("workspace/runs/run_001/event_log.jsonl", &state) == 0 &&
      state) {
    rc |= expect_no_substr(state, "\"type\":\"task_created\"",
                           "no task event from a custom payload key");
    rc |= expect_no_substr(state, "\"type\":\"work_step_result\"",
                           "no work correlation from a custom payload key");
    /* The payload's own context_id key is data; routing decided the run. */
    free(state); state = NULL;
  }
  state = harness_read_runstate("run_001");
  if (state) {
    rc |= expect_substr(state, "\"context_id\":\"event:local_product:device\"",
                        "routing, not a payload key, chose the context");
    free(state); state = NULL;
  }

  rc |= remove_fixture_floop("tif");
  return rc;
}

int ungated_typed_event_completes_as_a_noop_run(void) {
  HarnessGateway *g = NULL;
  char *out = NULL, *log = NULL;
  char *argv[16];
  int code = -1, rc = 0, argc;

  rc |= test_reset_workspace();
  rc |= write_fixture_floop("tif");

  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"nobody_listens";
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)"{\"fact\":true}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "an ungated but valid custom kind is admitted");
  free(out); out = NULL;

  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "init gateway for the ungated probe");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 1, 20000) == 0,
                 "the ungated custom event completes");
    harness_gateway_close(g);
  }

  /* 13. Inspectable no-op: a real run, no default chat behavior. */
  log = harness_read_event_log("run_001");
  rc |= expect(log != NULL, "the ungated event still created a run");
  if (log) {
    rc |= expect_substr(log, "\"type\":\"nobody_listens\"",
                        "the run records the custom kind");
    rc |= expect_substr(log, "\"type\":\"run_done\"",
                        "the ungated run terminates cleanly");
    rc |= expect_no_substr(log, "\"type\":\"action_request\"",
                           "an ungated custom kind requests no action");
    rc |= expect_no_substr(log, "\"type\":\"task_created\"",
                           "an ungated custom kind creates no task");
    free(log); log = NULL;
  }
  rc |= expect_agent_output_not_exists("run_001", "scan", ".json");
  rc |= expect_agent_output_not_exists("run_001", "other", ".json");
  rc |= expect_file_not_exists("workspace/logs/deliveries.jsonl");

  rc |= remove_fixture_floop("tif");
  return rc;
}

int typed_event_context_namespace_is_the_callers_choice(void) {
  HarnessGateway *g = NULL;
  char *out = NULL, *state = NULL;
  char *argv[20];
  int code = -1, rc = 0, argc;

  rc |= test_reset_workspace();
  rc |= write_fixture_floop("tif");

  /* Two events in one supplied context: same namespaced id, and the
   * second waits for the first exactly like any other same-context run. */
  rc |= expect(publish_scan("device", NULL, &out, &code) == 0 && code == 0,
               "publish the first same-context event");
  free(out); out = NULL;
  rc |= expect(publish_scan("device", NULL, &out, &code) == 0 && code == 0,
               "publish the second same-context event");
  free(out); out = NULL;

  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "init gateway for the context probe");
  if (g) {
    /* One tick admits both and advances only the older run. */
    harness_gateway_drive_one_tick(g);
    rc |= expect(test_file_contains("workspace/runs/run_001/event_log.jsonl",
                                    "\"type\":\"run_started\"") == 0,
                 "the older same-context run advances");
    rc |= expect(test_file_contains("workspace/runs/run_002/event_log.jsonl",
                                    "\"type\":\"run_started\"") != 0,
                 "the newer same-context run is serialized behind it");
    rc |= expect(harness_gateway_drive_until_completed(g, 2, 20000) == 0,
                 "both same-context events complete");
    harness_gateway_close(g);
  }
  state = harness_read_runstate("run_001");
  if (state) {
    rc |= expect_substr(state, "\"context_id\":\"event:local_product:device\"",
                        "a supplied context id gets the event: namespace");
    free(state); state = NULL;
  }
  state = harness_read_runstate("run_002");
  if (state) {
    rc |= expect_substr(state, "\"context_id\":\"event:local_product:device\"",
                        "the second event lands in the same context");
    free(state); state = NULL;
  }

  /* No context id: the adapter id, then the channel alone. */
  argc = 0;
  argv[argc++] = (char *)"publish";      argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";       argv[argc++] = (char *)SCAN_KIND;
  argv[argc++] = (char *)"--channel";    argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--adapter-id"; argv[argc++] = (char *)"android";
  argv[argc++] = (char *)"--payload";    argv[argc++] = (char *)"{\"a\":1}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "publish without a context id");
  free(out); out = NULL;
  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)SCAN_KIND;
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)"{\"a\":2}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "publish without a context id or adapter id");
  free(out); out = NULL;
  /* A fully-qualified caller id is preserved verbatim, so a product may
   * deliberately run an event inside an existing conversation. */
  argc = 0;
  argv[argc++] = (char *)"publish";      argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";       argv[argc++] = (char *)SCAN_KIND;
  argv[argc++] = (char *)"--channel";    argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--context-id"; argv[argc++] = (char *)"chat:tests:room1";
  argv[argc++] = (char *)"--payload";    argv[argc++] = (char *)"{\"a\":3}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "publish into an explicit chat context");
  free(out); out = NULL;

  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "init gateway for the fallback probe");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 3, 20000) == 0,
                 "the fallback-context events complete");
    harness_gateway_close(g);
  }
  state = harness_read_runstate("run_003");
  if (state) {
    rc |= expect_substr(state, "\"context_id\":\"event:local_product:android\"",
                        "the adapter id is the second fallback");
    free(state); state = NULL;
  }
  state = harness_read_runstate("run_004");
  if (state) {
    rc |= expect_substr(state, "\"context_id\":\"event:local_product\"",
                        "the channel alone is the last fallback");
    free(state); state = NULL;
  }
  state = harness_read_runstate("run_005");
  if (state) {
    rc |= expect_substr(state, "\"context_id\":\"chat:tests:room1\"",
                        "a fully-qualified caller context is preserved");
    free(state); state = NULL;
  }

  /* The runtime-owned action: namespace is refused at both boundaries. */
  argc = 0;
  argv[argc++] = (char *)"publish";      argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";       argv[argc++] = (char *)SCAN_KIND;
  argv[argc++] = (char *)"--channel";    argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--context-id"; argv[argc++] = (char *)"action:cli:bus_000001";
  argv[argc++] = (char *)"--payload";    argv[argc++] = (char *)"{\"a\":4}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
               "the CLI refuses a runtime-owned action: context id");
  free(out); out = NULL;
  rc |= expect_admission_rejected(
      "bus_900001",
      "{\"event_id\":\"bus_900001\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"" SCAN_KIND "\","
      "\"routing\":{\"context_id\":\"action:cli:forged\"},"
      "\"payload\":{\"a\":5}}\n",
      "invalid_field: routing.context_id");

  rc |= remove_fixture_floop("tif");
  return rc;
}

int typed_event_survives_a_restart_before_intake(void) {
  HarnessGateway *g = NULL;
  char *out = NULL, *log = NULL;
  int code = -1, rc = 0;

  rc |= test_reset_workspace();
  rc |= write_fixture_floop("tif");
  rc |= expect(publish_scan("device", NULL, &out, &code) == 0 && code == 0,
               "publish before the owner ever runs");
  free(out); out = NULL;

  /* 9. The inbox file is the commit. An owner that starts and stops
   *    without draining leaves it exactly where it was. */
  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "start the first owner");
  if (g) harness_gateway_close(g);
  rc |= expect(count_dir_entries("workspace/bus/inbox", "bus_") == 1,
               "the committed envelope survives the restart");
  rc |= expect(count_dir_entries("workspace/runs", "run_") == 0,
               "no run was created before intake");

  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "start the second owner");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 1, 20000) == 0,
                 "the surviving envelope completes after restart");
    harness_gateway_close(g);
  }
  rc |= expect(count_dir_entries("workspace/runs", "run_") == 1,
               "the surviving envelope creates exactly one run");
  rc |= expect(count_dir_entries("workspace/bus/inbox", "bus_") == 0,
               "the inbox is drained");
  log = harness_read_event_log("run_001");
  if (log) {
    rc |= expect_substr(log, "\"type\":\"" SCAN_KIND "\"",
                        "the recovered run keeps the custom kind");
    free(log); log = NULL;
  }

  rc |= remove_fixture_floop("tif");
  return rc;
}

int typed_publication_is_stable_and_conflicts_loudly(void) {
  HarnessGateway *g = NULL;
  char *out = NULL;
  char reserved[BUS_ID_MAX] = "";
  char *argv[20];
  int code = -1, rc = 0, argc;

  rc |= test_reset_workspace();
  rc |= write_fixture_floop("tif");

  argc = 0;
  argv[argc++] = (char *)"reserve"; argv[argc++] = (char *)"-a";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "reserve issues a runtime-owned envelope id");
  if (out) {
    rc |= expect_substr(out, "\"event_id\":\"bus_000001\"",
                        "reserve returns the next ordinary bus identity");
    snprintf(reserved, sizeof(reserved), "bus_000001");
    free(out); out = NULL;
  }
  rc |= expect(count_dir_entries("workspace/bus/inbox", "bus_") == 0,
               "reserving publishes nothing");

  argc = 0;
  argv[argc++] = (char *)"publish";    argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";     argv[argc++] = (char *)SCAN_KIND;
  argv[argc++] = (char *)"--channel";  argv[argc++] = (char *)"local_product";
  argv[argc++] = (char *)"--context-id"; argv[argc++] = (char *)"device";
  argv[argc++] = (char *)"--event-id"; argv[argc++] = reserved;
  argv[argc++] = (char *)"--payload";  argv[argc++] = (char *)SCAN_PAYLOAD;
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "the reserved identity publishes once");
  free(out); out = NULL;
  /* 10. The identical republish an interrupted producer would retry. */
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "an identical republish is absorbed");
  free(out); out = NULL;
  rc |= expect(count_dir_entries("workspace/bus/inbox", "bus_") == 1,
               "the identical republish created no second envelope");

  /* A different payload under the same identity is a loud conflict. */
  argv[argc - 1] = (char *)"{\"scope\":\"different\"}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 3,
               "a conflicting payload fails with the conflict code");
  free(out); out = NULL;
  argv[argc - 1] = (char *)SCAN_PAYLOAD;
  /* So is different routing metadata. */
  argv[7] = (char *)"other_device";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 3,
               "conflicting routing fails with the conflict code");
  free(out); out = NULL;
  argv[7] = (char *)"device";
  rc |= expect(count_dir_entries("workspace/bus/inbox", "bus_") == 1,
               "a rejected conflict leaves the committed envelope alone");

  g = harness_gateway_init("tif");
  rc |= expect(g != NULL, "init gateway for the stable-publish probe");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 1, 20000) == 0,
                 "the stable envelope completes");
    harness_gateway_close(g);
  }
  rc |= expect(count_dir_entries("workspace/runs", "run_") == 1,
               "stable publication created exactly one run");
  /* Republishing after intake still matches the processed envelope. */
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "republishing a processed envelope is still idempotent");
  free(out); out = NULL;
  rc |= expect(count_dir_entries("workspace/bus/inbox", "bus_") == 0,
               "the absorbed republish did not requeue a second run");

  rc |= remove_fixture_floop("tif");
  return rc;
}

int typed_ingress_rejects_reserved_and_malformed_publications(void) {
  static const char *kReserved[] = {
      "user_message", "user_note", "action_exec", "action_result",
      "affair_review", "affair_patch", "operation_completed",
      "operation_started", "work_outcome", "work_started", "task_created",
      "run_done", "memory_compacted", NULL};
  static const char *kBadTokens[] = {
      "9leading_digit", "has space", "has/slash", "_leading_underscore",
      "", NULL};
  char *out = NULL;
  char *argv[20];
  char *big = NULL;
  int code = -1, rc = 0, argc, i;

  rc |= test_reset_workspace();
  rc |= write_fixture_floop("tif");

  /* 11. The custom path cannot forge a runtime-owned kind. */
  for (i = 0; kReserved[i]; ++i) {
    argc = 0;
    argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
    argv[argc++] = (char *)"--type";    argv[argc++] = (char *)kReserved[i];
    argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)"{\"forged\":true}";
    rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
                 "the CLI refuses a runtime-owned kind");
    if (out) {
      rc |= expect_substr(out, "\"code\":\"reserved_type\"",
                          "the refusal names the reserved-type boundary");
      free(out); out = NULL;
    }
  }
  /* 12. Token grammar. */
  for (i = 0; kBadTokens[i]; ++i) {
    argc = 0;
    argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
    argv[argc++] = (char *)"--type";    argv[argc++] = (char *)kBadTokens[i];
    argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)"{}";
    rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
                 "the CLI refuses a type outside the token grammar");
    if (out) {
      rc |= expect_substr(out, "\"code\":\"invalid_type\"",
                          "the refusal names the invalid type");
      free(out); out = NULL;
    }
  }
  /* Namespace separators inside the grammar are accepted. */
  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"wishware.build:observed-v1";
  argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)"{}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "documented namespace separators are admissible");
  free(out); out = NULL;

  /* 12. Payload shape and bounds, each named, each leaving no envelope. */
  {
    struct { const char *payload; const char *code_name; } cases[] = {
        { "not json",     "invalid_payload" },
        { "[1,2,3]",      "invalid_payload" },
        { "\"text\"",     "invalid_payload" },
        { "{\"a\":1",     "invalid_payload" },
        { NULL, NULL } };
    for (i = 0; cases[i].payload; ++i) {
      argc = 0;
      argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
      argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"boundary_probe";
      argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)cases[i].payload;
      rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
                   "a payload that is not one JSON object is refused");
      if (out) {
        rc |= expect_substr(out, cases[i].code_name,
                            "the refusal names the payload boundary");
        free(out); out = NULL;
      }
    }
  }
  big = (char *)malloc(BUS_PAYLOAD_MAX + 64);
  rc |= expect(big != NULL, "allocate the oversized payload probe");
  if (big) {
    size_t filler = BUS_PAYLOAD_MAX + 8;
    memset(big, 'x', filler);
    memcpy(big, "{\"a\":\"", 6);
    big[filler] = '\0';
    memcpy(big + filler - 2, "\"}", 2);
    argc = 0;
    argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
    argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"boundary_probe";
    argv[argc++] = (char *)"--payload"; argv[argc++] = big;
    rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
                 "an oversized payload is refused before publication");
    if (out) {
      rc |= expect_substr(out, "payload_too_large",
                          "the refusal names the payload bound");
      free(out); out = NULL;
    }
    /* Oversized routing metadata, same treatment. */
    memset(big, 'c', BUS_ROUTING_CONTEXT_MAX + 4);
    big[BUS_ROUTING_CONTEXT_MAX + 4] = '\0';
    argc = 0;
    argv[argc++] = (char *)"publish";      argv[argc++] = (char *)"-a";
    argv[argc++] = (char *)"--type";       argv[argc++] = (char *)"boundary_probe";
    argv[argc++] = (char *)"--context-id"; argv[argc++] = big;
    argv[argc++] = (char *)"--payload";    argv[argc++] = (char *)"{}";
    rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
                 "an oversized context id is refused");
    if (out) {
      rc |= expect_substr(out, "invalid_context_id",
                          "the refusal names the context-id bound");
      free(out); out = NULL;
    }
    memset(big, 'a', BUS_ROUTING_ADAPTER_MAX + 4);
    big[BUS_ROUTING_ADAPTER_MAX + 4] = '\0';
    argc = 0;
    argv[argc++] = (char *)"publish";      argv[argc++] = (char *)"-a";
    argv[argc++] = (char *)"--type";       argv[argc++] = (char *)"boundary_probe";
    argv[argc++] = (char *)"--adapter-id"; argv[argc++] = big;
    argv[argc++] = (char *)"--payload";    argv[argc++] = (char *)"{}";
    rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
                 "an oversized adapter id is refused");
    if (out) {
      rc |= expect_substr(out, "invalid_adapter_id",
                          "the refusal names the adapter-id bound");
      free(out); out = NULL;
    }
    free(big); big = NULL;
  }
  /* A caller-authored runtime identity that was never reserved. */
  {
    static const char *kBadIds[] = { "bus_", "run_000001", "bus_00zz",
                                     "notbus_000001", NULL };
    for (i = 0; kBadIds[i]; ++i) {
      argc = 0;
      argv[argc++] = (char *)"publish";    argv[argc++] = (char *)"-a";
      argv[argc++] = (char *)"--type";     argv[argc++] = (char *)"boundary_probe";
      argv[argc++] = (char *)"--event-id"; argv[argc++] = (char *)kBadIds[i];
      argv[argc++] = (char *)"--payload";  argv[argc++] = (char *)"{}";
      rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
                   "an unreserved runtime identity is refused");
      if (out) {
        rc |= expect_substr(out, "invalid_event_id",
                            "the refusal names the event-id boundary");
        free(out); out = NULL;
      }
    }
  }
  /* Exactly one payload source; text mode never mixes with custom mode. */
  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"boundary_probe";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
               "a custom publication without a payload source is refused");
  free(out); out = NULL;
  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"boundary_probe";
  argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)"{}";
  argv[argc++] = (char *)"--payload-stdin";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
               "two payload sources are refused");
  free(out); out = NULL;
  argc = 0;
  argv[argc++] = (char *)"publish"; argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--text";  argv[argc++] = (char *)"hello";
  argv[argc++] = (char *)"--type";  argv[argc++] = (char *)"boundary_probe";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 1,
               "text mode and custom mode are mutually exclusive");
  free(out); out = NULL;

  /* Only the namespace-separator probe should have committed. */
  rc |= expect(count_dir_entries("workspace/bus/inbox", "bus_") == 1,
               "every refusal left the inbox untouched");

  /* The same boundaries hold at admission, whatever wrote the file. */
  rc |= expect_admission_rejected(
      "bus_900101",
      "{\"event_id\":\"bus_900101\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"task_created\","
      "\"payload\":{\"forged\":true}}\n",
      "reserved_envelope_type: task_created");
  rc |= expect_admission_rejected(
      "bus_900102",
      "{\"event_id\":\"bus_900102\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"bad type\","
      "\"payload\":{}}\n",
      "invalid_field: type (bad type)");
  rc |= expect_admission_rejected(
      "bus_900103",
      "{\"event_id\":\"bus_900103\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"boundary_probe\","
      "\"payload\":[1,2,3]}\n",
      "missing_required_field: payload");
  rc |= expect_admission_rejected(
      "bus_900104",
      "{\"event_id\":\"bus_900104\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"boundary_probe\","
      "\"routing\":{\"ref\":\"not-an-object\"},\"payload\":{}}\n",
      "invalid_field: routing.ref");
  rc |= expect_admission_rejected(
      "bus_900105",
      "{\"event_id\":\"bus_900105\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"boundary_probe\","
      "\"routing\":\"not-an-object\",\"payload\":{}}\n",
      "invalid_field: routing");
  rc |= expect_admission_rejected(
      "bus_900107",
      "{\"event_id\":\"bus_900107\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"boundary_probe\","
      "\"routing\":{\"context_id\":\""
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
      "c\"},\"payload\":{}}\n",
      "invalid_field: routing.context_id");
  rc |= expect_admission_rejected(
      "bus_900108",
      "{\"event_id\":\"bus_900108\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"boundary_probe\","
      "\"routing\":{\"adapter_id\":\""
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "a\"},\"payload\":{}}\n",
      "invalid_field: routing.adapter_id");
  /* A user_message forged through the opaque path still faces the
   * built-in kind's own required fields. */
  rc |= expect_admission_rejected(
      "bus_900106",
      "{\"event_id\":\"bus_900106\",\"ts\":\"2026-01-01T00:00:00Z\","
      "\"channel\":\"local_product\",\"type\":\"user_message\","
      "\"payload\":{\"scope\":\"forged\"}}\n",
      "missing_required_field: payload.text");

  rc |= remove_fixture_floop("tif");
  return rc;
}

int legacy_user_message_ingress_is_unchanged(void) {
  HarnessGateway *g = NULL;
  char *out = NULL, *envelope = NULL, *state = NULL;
  char *argv[16];
  int code = -1, rc = 0, argc;

  rc |= test_reset_workspace();

  /* 16. The text shortcut's output and envelope are a published contract. */
  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"cli";
  argv[argc++] = (char *)"--text";    argv[argc++] = (char *)"hello";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "the text shortcut still publishes");
  rc |= expect(out && strcmp(out, "{\"event_id\":\"bus_000001\","
                                  "\"channel\":\"cli\"}\n") == 0,
               "the text shortcut's agent output is byte-identical");
  free(out); out = NULL;
  rc |= test_read_file("workspace/bus/inbox/bus_000001.json", &envelope) == 0
            ? 0 : expect(0, "the text envelope exists");
  if (envelope) {
    rc |= expect_substr(envelope, "\"type\":\"user_message\"",
                        "the text shortcut still publishes user_message");
    rc |= expect_substr(envelope, "\"payload\":{\"text\":\"hello\"}",
                        "the legacy payload shape is unchanged");
    rc |= expect_no_substr(envelope, "\"routing\"",
                           "a legacy envelope carries no routing block");
    free(envelope); envelope = NULL;
  }
  /* Routing-shaped flags still ride inside the legacy payload. */
  argc = 0;
  argv[argc++] = (char *)"publish";      argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--channel";    argv[argc++] = (char *)"tests";
  argv[argc++] = (char *)"--adapter-id"; argv[argc++] = (char *)"probe";
  argv[argc++] = (char *)"--ref";        argv[argc++] = (char *)"{\"handle\":\"h1\"}";
  argv[argc++] = (char *)"--text";       argv[argc++] = (char *)"routed hello";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "the text shortcut still accepts adapter-id and ref");
  free(out); out = NULL;
  rc |= test_read_file("workspace/bus/inbox/bus_000002.json", &envelope) == 0
            ? 0 : expect(0, "the routed text envelope exists");
  if (envelope) {
    rc |= expect_substr(envelope,
                        "\"payload\":{\"text\":\"routed hello\","
                        "\"adapter_id\":\"probe\",\"ref\":{\"handle\":\"h1\"}}",
                        "the legacy routed payload shape is unchanged");
    rc |= expect_no_substr(envelope, "\"routing\"",
                           "a legacy routed envelope carries no routing block");
    free(envelope); envelope = NULL;
  }

  /* And intake still treats it as chat: a chat: context and an input task. */
  g = harness_gateway_init("fast");
  rc |= expect(g != NULL, "init gateway for the legacy parity probe");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 2, 20000) == 0,
                 "the legacy user messages complete");
    harness_gateway_close(g);
  }
  state = harness_read_runstate("run_001");
  if (state) {
    rc |= expect_substr(state, "\"context_id\":\"chat:cli\"",
                        "a legacy user_message keeps its chat: context");
    free(state); state = NULL;
  }
  state = harness_read_event_log("run_001");
  if (state) {
    rc |= expect_substr(state, "\"type\":\"user_message\"",
                        "the legacy triggering event is unchanged");
    rc |= expect_substr(state, "\"type\":\"task_created\"",
                        "a legacy user_message still creates its input task");
    free(state); state = NULL;
  }
  return rc;
}

/* Typed ingress made multi-KiB envelopes ordinary, and `bus log --type` is
 * the documented way a producer watches its own kind. The filter must be
 * exact at every size — an oversized envelope that slips past it hands one
 * producer another producer's events. */
int bus_log_filters_stay_exact_at_any_envelope_size(void) {
  char *out = NULL;
  char *big_payload = NULL;
  char *argv[8];
  const size_t blob = 5000;
  int code = -1, rc = 0, argc;

  rc |= test_reset_workspace();
  big_payload = (char *)malloc(blob + 32);
  rc |= expect(big_payload != NULL, "allocate the oversized payload");
  if (!big_payload) return rc;
  memcpy(big_payload, "{\"blob\":\"", 9);
  memset(big_payload + 9, 'x', blob);
  memcpy(big_payload + 9 + blob, "\"}", 3);

  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"big_fact";
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"p_big";
  argv[argc++] = (char *)"--payload"; argv[argc++] = big_payload;
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "publish an envelope larger than the filter scratch buffer");
  free(out); out = NULL;
  argc = 0;
  argv[argc++] = (char *)"publish";   argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type";    argv[argc++] = (char *)"small_fact";
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"p_small";
  argv[argc++] = (char *)"--payload"; argv[argc++] = (char *)"{\"a\":1}";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "publish a small envelope of another kind");
  free(out); out = NULL;

  /* Filtering for the small kind must not sweep the large envelope in. */
  argc = 0;
  argv[argc++] = (char *)"log"; argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type"; argv[argc++] = (char *)"small_fact";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "bus log --type runs");
  if (out) {
    rc |= expect(test_count_substr(out, "\n") == 1,
                 "--type emits exactly the one matching envelope");
    rc |= expect_substr(out, "\"type\":\"small_fact\"",
                        "--type keeps the matching envelope");
    rc |= expect_no_substr(out, "big_fact",
                           "--type excludes the oversized envelope");
    free(out); out = NULL;
  }
  argc = 0;
  argv[argc++] = (char *)"log"; argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--channel"; argv[argc++] = (char *)"p_small";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "bus log --channel runs");
  if (out) {
    rc |= expect(test_count_substr(out, "\n") == 1,
                 "--channel emits exactly the one matching envelope");
    rc |= expect_no_substr(out, "p_big",
                           "--channel excludes the oversized envelope");
    free(out); out = NULL;
  }
  /* And the large envelope is still findable by its own kind. */
  argc = 0;
  argv[argc++] = (char *)"log"; argv[argc++] = (char *)"-a";
  argv[argc++] = (char *)"--type"; argv[argc++] = (char *)"big_fact";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "bus log --type finds the oversized envelope");
  if (out) {
    rc |= expect(test_count_substr(out, "\n") == 1,
                 "the oversized envelope matches its own kind exactly once");
    rc |= expect_substr(out, "\"type\":\"big_fact\"",
                        "the oversized envelope is still inspectable");
    free(out); out = NULL;
  }
  /* Unfiltered output is unchanged: every envelope, in order. */
  argc = 0;
  argv[argc++] = (char *)"log"; argv[argc++] = (char *)"-a";
  rc |= expect(bus_cli(argv, argc, &out, &code) == 0 && code == 0,
               "unfiltered bus log runs");
  if (out) {
    rc |= expect(test_count_substr(out, "\n") == 2,
                 "unfiltered bus log still emits every envelope");
    rc |= expect_substr(out, "\"type\":\"big_fact\"", "unfiltered keeps big");
    rc |= expect_substr(out, "\"type\":\"small_fact\"", "unfiltered keeps small");
    free(out); out = NULL;
  }
  free(big_payload);
  return rc;
}
