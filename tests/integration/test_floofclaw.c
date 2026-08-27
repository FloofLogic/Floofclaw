/* the floofclaw floop acceptance tests.
 *
 * Two layers:
 *
 *  1. Generic boundary proof — an arbitrary action bound to
 *     interchangeable fake managed-operation handlers runs the full
 *     detached lifecycle (start -> running(handle) -> poll ->
 *     finished(result) -> correlated operation_result run) with zero
 *     engine knowledge of the handler. Rebinding the handler is a
 *     config change only. Opaque payloads survive untouched.
 *     Idempotency, timer dedup, no-restart-loop, and per-context
 *     serialization are proven with fakes and integers only.
 *
 *  2. Background blocker proof — the floofclaw floop end to end with
 *     a mocked manager LLM and a fake codex binary: request -> ack ->
 *     detached start -> async follow-up revision -> polls -> rev-2
 *     attempt fed the prior result -> worker blocker text -> proactive
 *     delivery. No user activity after the follow-up.
 */

#include "../test_support.h"
#include "../harness.h"

#include "../../runtime/action_internal.h"
#include "../../runtime/agent_exec.h"
#include "../../runtime/ports.h"
#include "../../runtime/publication_outbox.h"
#include "../../runtime/gateway/affair_watcher_module.h"
#include "../../runtime/gateway/janitor_module.h"
#include "../../runtime/gateway/reactor.h"
#include "../../runtime/bus/bus.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Opt-in debug dumps. FCLAW_TEST_DEBUG_DUMP names an absolute destination
 * directory OUTSIDE the isolated copy (the copy is removed on exit either
 * way, so in-copy dumps would vanish). Filenames carry the pid so parallel
 * or repeated runs can never overwrite each other's evidence; each retained
 * path is printed. A non-absolute value is refused loudly. */
static void debug_dump_write(const char *name, const char *content) {
  const char *dir = getenv("FCLAW_TEST_DEBUG_DUMP");
  char path[512];
  if (!dir || !*dir) return;
  if (dir[0] != '/') {
    fprintf(stderr,
            "debug dump skipped: FCLAW_TEST_DEBUG_DUMP must be an absolute "
            "directory, got '%s'\n", dir);
    return;
  }
  snprintf(path, sizeof(path), "%s/fclaw_%ld_%s", dir, (long)getpid(), name);
  if (test_write_file(path, content ? content : "") == 0)
    fprintf(stderr, "debug dump retained: %s\n", path);
  else
    fprintf(stderr, "debug dump FAILED to write: %s\n", path);
}

/* ===== env save/restore (mirrors test_run_fail_cleanup.c) ========= */

static char *fx_capture_env(const char *name) {
  const char *v = getenv(name);
  return v ? strdup(v) : NULL;
}

static void fx_restore_env(const char *name, char *saved) {
  if (saved) {
    setenv(name, saved, 1);
    free(saved);
  } else {
    unsetenv(name);
  }
}

/* ===== fixture writers ============================================ */

/* test_reset_workspace removes only the runtime-owned paths; the fake
 * adapters keep their scratch state under workspace/ too. */
static int fx_reset(void) {
  int rc = test_reset_workspace();
  rc |= test_remove_path("workspace/fakeop_one");
  rc |= test_remove_path("workspace/fakeop_two");
  rc |= test_remove_path("workspace/fixtures");
  rc |= test_remove_path("workspace/flaky_ran");
  rc |= test_remove_path("workspace/a3_a3_fake_a");
  rc |= test_remove_path("workspace/a3_a3_fake_b");
  rc |= test_remove_path("workspace/a3_a3_missing_poll");
  return rc;
}

static int write_exec(const char *path, const char *text) {
  if (test_write_file(path, text) != 0) return -1;
  return chmod(path, 0755);
}

/* actions/testfx: two interchangeable fake managed-operation adapters.
 * fake_one/fake_two differ only in their state dir, handle prefix, and
 * result wording — proving handler substitution is config-only. Each
 * start detaches a fake worker that submits its completion through the
 * public `fclaw operation complete` helper using the trusted env
 * capability, exercising the full worker-side callback path. */
static int write_fake_adapter(const char *id, const char *statedir, const char *prefix,
                              const char *wording) {
  char path[512], body[4096];
  snprintf(path, sizeof(path), "actions/testfx/%s/action.json", id);
  snprintf(body, sizeof(body),
           "{\n"
           "  \"id\": \"%s\",\n"
           "  \"description\": \"Test managed-operation adapter.\",\n"
           "  \"outside_world\": false,\n"
           "  \"contract\": \"managed_operation\",\n"
           "  \"default_timeout_ms\": 60000,\n"
           "  \"args_schema\": {\n"
           "    \"type\": \"object\",\n"
           "    \"required\": [\"op\"],\n"
           "    \"additionalProperties\": false,\n"
           "    \"properties\": {\n"
           "      \"op\": {\"type\": \"string\", \"enum\": [\"start\"],"
           " \"description\": \"start\"},\n"
           "      \"task\": {\"type\": \"string\", \"description\": \"work text\"}\n"
           "    }\n"
           "  },\n"
           "  \"exec\": [\"bash\", \"run.sh\"],\n"
           "  \"timeout_ms\": 30000\n"
           "}\n", id);
  if (test_write_file(path, body) != 0) return -1;
  snprintf(path, sizeof(path), "actions/testfx/%s/run.sh", id);
  snprintf(body, sizeof(body),
           "#!/usr/bin/env bash\n"
           "set -euo pipefail\n"
           "input=\"$(cat)\"\n"
           "root=\"${FCLAW_WORKSPACE_ROOT:-workspace}\"\n"
           "d=\"$root/%s\"\n"
           "mkdir -p \"$d\"\n"
           "h=\"$(python3 - \"$input\" <<'PY'\n"
           "import json, os, sys\n"
           "req = json.loads(sys.argv[1])\n"
           "args = req.get(\"args\", {})\n"
           "root = os.environ.get(\"FCLAW_WORKSPACE_ROOT\", \"workspace\")\n"
           "d = os.path.join(root, \"%s\")\n"
           "c = os.path.join(d, \"starts\")\n"
           "n = int(open(c).read()) if os.path.exists(c) else 0\n"
           "n += 1\n"
           "open(c, \"w\").write(str(n))\n"
           "h = \"%s_%%03d\" %% n\n"
           "open(os.path.join(d, h + \".task\"), \"w\").write(args.get(\"task\", \"\"))\n"
           "print(h)\n"
           "PY\n"
           ")\"\n"
           "task=\"$(cat \"$d/$h.task\")\"\n"
           "# Detached fake worker: submits the completion claim through the\n"
           "# public helper. The WHOLE subshell redirects away from the\n"
           "# action's pipes so the job's stdout closes the moment this\n"
           "# script exits, letting the driver record the worker handle\n"
           "# before the claim lands.\n"
           "( sleep 0.45; \"${FCLAW_BIN:-fclaw}\" operation complete -a \\\n"
           "    --status succeeded --text \"%s: $task\" ) \\\n"
           "    >>\"$d/$h.completion.log\" 2>&1 </dev/null &\n"
           "printf '{\"events\":[],\"result\":{\"status\":\"running\","
           "\"handle\":\"%%s\"},\"error\":null}\\n' \"$h\"\n",
           statedir, statedir, prefix, wording);
  return write_exec(path, body);
}

/* Writing the directory is the registration: actions/ is scanned recursively,
 * so anything under actions/testfx is discovered with no list to update. */
static int setup_testfx_actions(void) {
  int rc = 0;
  rc |= write_fake_adapter("fake_one", "fakeop_one", "fake1", "fake_one finished");
  rc |= write_fake_adapter("fake_two", "fakeop_two", "fake2", "fake_two finished");
  return rc;
}

/* Generic one-shot floop bound to a configurable handler. The manager
 * is a plain script: user_message with work in it -> create work;
 * anything else -> no calls. */
static int setup_opgeneric_floop(const char *floop, const char *handler) {
  char path[512], body[2048];
  int rc = 0;
  snprintf(path, sizeof(path), "floops/%s/loop.json", floop);
  snprintf(body, sizeof(body),
           "{\n"
           "  \"version\": 1,\n"
           "  \"name\": \"%s\",\n"
           "  \"description\": \"generic managed-operation lifecycle fixture\",\n"
           "  \"one_pass\": true,\n"
           "  \"serialize_contexts\": true,\n"
           "  \"steps\": [\n"
           "    {\"id\": \"manage\", \"type\": \"agent\", \"agent\": \"opg_manager\",\n"
           "     \"gate\": \"event_kind:user_message,operation_result\"},\n"
           "    {\"id\": \"dispatch\", \"type\": \"builtin\", \"builtin\": \"action_runner\"},\n"
           "    {\"id\": \"workers\", \"type\": \"agent\", \"agent\": \"workers\",\n"
           "     \"gate\": \"event_kind:user_message,operation_result\"}\n"
           "  ]\n"
           "}\n", floop);
  rc |= test_write_file(path, body);
  snprintf(path, sizeof(path), "floops/%s/agents/opg_manager/agent.json", floop);
  rc |= test_write_file(path,
      "{\n"
      "  \"id\": \"opg_manager\",\n"
      "  \"executor\": \"script\",\n"
      "  \"actions\": [\"work\", \"message\"],\n"
      "  \"listen\": [\"event\", \"tasks\"]\n"
      "}\n");
  snprintf(path, sizeof(path), "floops/%s/agents/opg_manager/run.sh", floop);
  rc |= write_exec(path,
      "#!/usr/bin/env bash\n"
      "input=\"$(cat)\"\n"
      "if grep -q '\"kind\":\"user_message\"' <<<\"$input\" && \\\n"
      "   grep -q 'alpha work please' <<<\"$input\"; then\n"
      "  echo '{\"calls\":[{\"name\":\"work\",\"args\":{\"work\":\"update playlist X\",\"done_when\":\"instruction executed\"}},{\"name\":\"task.update\",\"args\":{\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}'\n"
      "else\n"
      "  echo '{\"calls\":[]}'\n"
      "fi\n");
  snprintf(path, sizeof(path), "floops/%s/agents/workers/agent.json", floop);
  snprintf(body, sizeof(body),
           "{\n"
           "  \"id\": \"workers\",\n"
           "  \"executor\": \"native\",\n"
           "  \"handler\": \"%s\"\n"
           "}\n", handler);
  rc |= test_write_file(path, body);
  return rc;
}

/* A3 deterministic completion fixture: start records a worker handle
 * and returns running (or finished with FCLAW_A3_FINISH_IMMEDIATE=1).
 * It never self-completes — tests submit completion claims
 * deterministically through the runtime's own publisher, so no timing
 * or polling participates. It also records the completion capability it
 * received through the trusted env so tests can prove and use it. */
static int write_a3_adapter_with_deadline(const char *id, const char *prefix,
                                          int default_timeout_ms) {
  char path[512], body[8192];
  snprintf(path, sizeof(path), "actions/testfx/%s/action.json", id);
  snprintf(body, sizeof(body),
           "{\n"
           "  \"id\": \"%s\",\n"
           "  \"description\": \"A3 deterministic completion fixture.\",\n"
           "  \"outside_world\": false,\n"
           "  \"contract\": \"managed_operation\",\n"
           "  \"default_timeout_ms\": %d,\n"
           "  \"args_schema\": {\n"
           "    \"type\": \"object\",\n"
           "    \"required\": [\"op\"],\n"
           "    \"additionalProperties\": false,\n"
           "    \"properties\": {\n"
           "      \"op\": {\"type\":\"string\",\"enum\":[\"start\"]},\n"
           "      \"task\": {\"type\":\"string\"}\n"
           "    }\n"
           "  },\n"
           "  \"exec\": [\"bash\", \"run.sh\"],\n"
           "  \"timeout_ms\": 30000\n"
           "}\n",
           id, default_timeout_ms);
  if (test_write_file(path, body) != 0) return -1;
  snprintf(path, sizeof(path), "actions/testfx/%s/run.sh", id);
  snprintf(body, sizeof(body),
           "#!/usr/bin/env bash\n"
           "set -euo pipefail\n"
           "input=\"$(cat)\"\n"
           "root=\"${FCLAW_WORKSPACE_ROOT:-workspace}\"\n"
           "d=\"$root/a3_%s\"\n"
           "mkdir -p \"$d\"\n"
           "bump() {\n"
           "  local path=\"$d/$1\" n=0\n"
           "  if [[ -f \"$path\" ]]; then read -r n < \"$path\"; fi\n"
           "  n=$((n + 1))\n"
           "  printf '%%s' \"$n\" > \"$path\"\n"
           "  printf '%%s' \"$n\"\n"
           "}\n"
           "if [[ \"$input\" == *'\"op\":\"start\"'* ]]; then\n"
           "  if [[ -n \"${FCLAW_A3_START_DELAY:-}\" ]]; then "
           "sleep \"$FCLAW_A3_START_DELAY\"; fi\n"
           "  n=\"$(bump starts)\"\n"
           "  printf -v h '%s_%%03d' \"$n\"\n"
           "  printf '%%s' \"$h\" > \"$d/handle\"\n"
           "  printf '%%s' \"${FCLAW_OPERATION_ID:-}\" > \"$d/operation_id\"\n"
           "  printf '%%s' \"${FCLAW_COMPLETION_TOKEN:-}\" > \"$d/token\"\n"
           "  if [[ \"${FCLAW_A3_FINISH_IMMEDIATE:-}\" == 1 ]]; then\n"
           "    printf '{\"events\":[],\"result\":{\"status\":\"finished\","
           "\"handle\":\"%%s\",\"text\":\"A3 immediate complete\"},"
           "\"error\":null}\\n' \"$h\"\n"
           "  else\n"
           "    printf '{\"events\":[],\"result\":{\"status\":\"running\","
           "\"handle\":\"%%s\"},\"error\":null}\\n' \"$h\"\n"
           "  fi\n"
           "else\n"
           "  printf '%%s\\n' '{\"events\":[],\"result\":{},"
           "\"error\":{\"message\":\"bad op\"}}'\n"
           "fi\n",
           id, prefix);
  return write_exec(path, body);
}

static int write_a3_adapter(const char *id, const char *prefix) {
  return write_a3_adapter_with_deadline(id, prefix, 60000);
}

static int write_a3_controller_catalog(const char *floop,
                                       const char *first,
                                       const char *second) {
  char path[512], body[1024];
  snprintf(path, sizeof(path),
           "floops/%s/agents/a3_controller/agent.json", floop);
  snprintf(body, sizeof(body),
           "{\"id\":\"a3_controller\",\"executor\":\"script\","
           "\"bind_task\":\"open_work\",\"listen\":[\"event\"],"
           "\"actions\":[\"%s\"%s%s%s]}\n",
           first,
           second && *second ? ",\"" : "",
           second && *second ? second : "",
           second && *second ? "\"" : "");
  return test_write_file(path, body);
}

static int write_a3_floop(const char *floop, const char *action,
                          int controller_enabled) {
  char path[512], body[4096];
  const char *controller_gate =
      controller_enabled ? "event_kind:user_message" : "event_kind:disabled";
  int rc = 0;
  snprintf(path, sizeof(path), "floops/%s/loop.json", floop);
  snprintf(body, sizeof(body),
           /* Looping, not one-pass. These scenarios advance a task to a new
            * revision while an action is in flight and require the
            * controller to select again for it inside the same run --
            * that is the "cycle and let the agents observe the new state"
            * behavior, which only a looping profile has. */
           "{\n"
           "  \"version\":1,\"name\":\"%s\",\"description\":\"A3 completion fixture\","
           "\"one_pass\":false,\"serialize_contexts\":true,\n"
           "  \"steps\":[\n"
           "    {\"id\":\"admit\",\"type\":\"agent\",\"agent\":\"a3_admit\","
           "\"gate\":\"event_kind:user_message\"},\n"
           "    {\"id\":\"admit.dispatch\",\"type\":\"builtin\","
           "\"builtin\":\"action_runner\"},\n"
           "    {\"id\":\"select\",\"type\":\"agent\",\"agent\":\"a3_controller\","
           "\"gate\":\"%s\"},\n"
           "    {\"id\":\"select.dispatch\",\"type\":\"builtin\","
           "\"builtin\":\"action_runner\"}\n"
           "  ]\n"
           "}\n",
           floop, controller_gate);
  rc |= test_write_file(path, body);
  snprintf(path, sizeof(path), "floops/%s/agents/a3_admit/agent.json", floop);
  rc |= test_write_file(path,
      "{\"id\":\"a3_admit\",\"executor\":\"script\","
      "\"actions\":[\"work\"],\"listen\":[\"event\"]}\n");
  snprintf(path, sizeof(path), "floops/%s/agents/a3_admit/run.sh", floop);
  rc |= write_exec(path,
      "#!/usr/bin/env bash\n"
      "input=\"$(cat)\"\n"
      "if grep -q 'create A3 work' <<<\"$input\"; then\n"
      "  echo '{\"calls\":[{\"name\":\"work\",\"args\":{\"work\":\"A3 initial\","
      "\"done_when\":\"fixture completes\"}}]}'\n"
      "elif grep -q 'revise A3 work' <<<\"$input\"; then\n"
      "  echo '{\"calls\":[{\"name\":\"work\",\"args\":{\"work\":\"A3 revised\","
      "\"done_when\":\"new revision completes\"}}]}'\n"
      "else\n"
      "  echo '{\"calls\":[]}'\n"
      "fi\n");
  rc |= write_a3_controller_catalog(
      floop, action,
      strcmp(action, "a3_fake_b") == 0 ? "a3_fake_a" : "a3_fake_b");
  snprintf(path, sizeof(path),
           "floops/%s/agents/a3_controller/run.sh", floop);
  snprintf(body, sizeof(body),
           "#!/usr/bin/env bash\n"
           "cat >/dev/null\n"
           "echo '{\"calls\":[{\"name\":\"%s\","
           "\"args\":{\"op\":\"start\",\"task\":\"do A3 work\"}}]}'\n",
           action);
  rc |= write_exec(path, body);
  return rc;
}

static int write_a4_sync_action(const char *id, const char *text) {
  char path[512], body[4096];
  snprintf(path, sizeof(path), "actions/testfx/%s/action.json", id);
  snprintf(body, sizeof(body),
           "{\"id\":\"%s\",\"description\":\"A4 synchronous fixture\","
           "\"outside_world\":false,"
           "\"args_schema\":{\"type\":\"object\","
           "\"additionalProperties\":false,\"properties\":{"
           "\"label\":{\"type\":\"string\"}}},"
           "\"exec\":[\"bash\",\"run.sh\"],\"timeout_ms\":30000}\n",
           id);
  if (test_write_file(path, body) != 0) return -1;
  snprintf(path, sizeof(path), "actions/testfx/%s/run.sh", id);
  snprintf(body, sizeof(body),
           "#!/usr/bin/env bash\n"
           "cat >/dev/null\n"
           "printf '%%s\\n' '{\"events\":[],\"result\":{\"text\":\"%s\"},"
           "\"error\":null}'\n",
           text);
  return write_exec(path, body);
}

static int setup_a4_actions(void) {
  int rc = 0;
  rc |= write_a4_sync_action("a4_note", "first durable note");
  rc |= write_a4_sync_action("a4_check", "second durable check");
  return rc;
}

static int write_a4_floop(const char *floop) {
  char path[512];
  int rc = 0;
  snprintf(path, sizeof(path), "floops/%s/loop.json", floop);
  rc |= test_write_file(
      path,
      "{\n"
      "  \"version\":1,\"name\":\"a4flow\","
      "\"description\":\"A4 product-neutral consequence routing fixture\","
      "\"one_pass\":true,\"serialize_contexts\":true,\n"
      "  \"steps\":[\n"
      "    {\"id\":\"admit\",\"type\":\"agent\",\"agent\":\"a4_admit\","
      "\"gate\":\"event_kind:user_message,affair_review\"},\n"
      "    {\"id\":\"admit.dispatch\",\"type\":\"builtin\","
      "\"builtin\":\"action_runner\"},\n"
      "    {\"id\":\"work.select\",\"type\":\"agent\","
      "\"agent\":\"a4_controller\",\"gate\":\"work_consequence\"},\n"
      "    {\"id\":\"work.dispatch\",\"type\":\"builtin\","
      "\"builtin\":\"action_runner\"},\n"
      "    {\"id\":\"result\",\"type\":\"agent\",\"agent\":\"a4_result\","
      "\"gate\":\"terminal_work_outcome\"},\n"
      "    {\"id\":\"result.dispatch\",\"type\":\"builtin\","
      "\"builtin\":\"action_runner\"}\n"
      "  ]\n"
      "}\n");
  snprintf(path, sizeof(path), "floops/%s/agents/a4_admit/agent.json", floop);
  rc |= test_write_file(
      path,
      "{\"id\":\"a4_admit\",\"executor\":\"script\","
      "\"actions\":[\"work\"],\"listen\":[\"event\"]}\n");
  snprintf(path, sizeof(path), "floops/%s/agents/a4_admit/run.sh", floop);
  rc |= write_exec(
      path,
      "#!/usr/bin/env bash\n"
      "input=\"$(cat)\"\n"
      "if [[ \"$input\" == *'make two A4 tasks'* ]]; then\n"
      "  echo '{\"calls\":["
      "{\"name\":\"work\",\"args\":{\"work\":\"A4 first duplicate\","
      "\"done_when\":\"done\"}},"
      "{\"name\":\"work\",\"args\":{\"work\":\"A4 second duplicate\","
      "\"done_when\":\"done\"}}]}'\n"
      "elif [[ \"$input\" == *'do A4 two steps'* ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"A4 two-step work\","
      "\"done_when\":\"both durable steps complete\"}}]}'\n"
      "elif [[ \"$input\" == *'block A4 work'* ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"A4 blocked work\","
      "\"done_when\":\"missing input arrives\"}}]}'\n"
      "elif [[ \"$input\" == *'message A4 work'* ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"A4 message work\","
      "\"done_when\":\"the explicit update is sent and work completes\"}}]}'\n"
      "else\n"
      "  echo '{\"calls\":[]}'\n"
      "fi\n");
  snprintf(path, sizeof(path),
           "floops/%s/agents/a4_controller/agent.json", floop);
  rc |= test_write_file(
      path,
      "{\"id\":\"a4_controller\",\"executor\":\"script\","
      "\"bind_task\":\"open_work\",\"listen\":[\"event\"],"
      "\"actions\":[\"a4_note\",\"a4_check\",\"message\",\"manage_codex\","
      "\"work_complete\",\"work_blocked\"]}\n");
  snprintf(path, sizeof(path),
           "floops/%s/agents/a4_controller/run.sh", floop);
  rc |= write_exec(
      path,
      "#!/usr/bin/env bash\n"
      "input=\"$(cat)\"\n"
      "count=\"$(grep -o 'result_event_id' <<<\"$input\" "
      "| wc -l | tr -d ' ')\"\n"
      "if [[ \"$input\" == *'A4 blocked work'* ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"work_blocked\",\"args\":{"
      "\"blocker\":\"calendar id is missing\","
      "\"needed\":\"provide the calendar id\"}}]}'\n"
      "elif [[ \"$input\" == *'A4 message work'* && \"$count\" == 0 ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"A4 explicit intermediate\"}}]}'\n"
      "elif [[ \"$input\" == *'A4 message work'* ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"work_complete\",\"args\":{"
      "\"summary\":\"A4 explicit update sent\"}}]}'\n"
      "elif [[ \"$count\" == 0 ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"a4_note\","
      "\"args\":{\"label\":\"first\"}}]}'\n"
      "elif [[ \"$count\" == 1 ]]; then\n"
      "  echo '{\"calls\":[{\"name\":\"a4_check\","
      "\"args\":{\"label\":\"second\"}}]}'\n"
      "else\n"
      "  echo '{\"calls\":[{\"name\":\"work_complete\","
      "\"args\":{\"summary\":\"both A4 steps are durable\"}}]}'\n"
      "fi\n");
  snprintf(path, sizeof(path), "floops/%s/agents/a4_result/agent.json", floop);
  rc |= test_write_file(
      path,
      "{\"id\":\"a4_result\",\"executor\":\"script\","
      "\"actions\":[\"message\"],\"listen\":[\"event\"]}\n");
  snprintf(path, sizeof(path), "floops/%s/agents/a4_result/run.sh", floop);
  rc |= write_exec(
      path,
      "#!/usr/bin/env bash\n"
      "cat >/dev/null\n"
      "echo '{\"calls\":[{\"name\":\"message\","
      "\"args\":{\"message\":\"A4 terminal handled\"}}]}'\n");
  return rc;
}

static int a4_direct_context(RtContext *ctx, const char *run_id) {
  if (!ctx || !run_id) return -1;
  memset(ctx, 0, sizeof(*ctx));
  snprintf(ctx->workspace, sizeof(ctx->workspace), "workspace");
  snprintf(ctx->run_id, sizeof(ctx->run_id), "%s", run_id);
  snprintf(ctx->run_dir, sizeof(ctx->run_dir),
           "workspace/runs/%s", run_id);
  snprintf(ctx->context_id, sizeof(ctx->context_id), "chat:tests");
  snprintf(ctx->event_kind, sizeof(ctx->event_kind), "user_message");
  snprintf(ctx->event_payload_json, sizeof(ctx->event_payload_json),
           "{\"text\":\"multiple work\"}");
  ctx->next_event = 1;
  return test_mkdir_p(ctx->run_dir);
}

static int a4_append_direct_work(RtContext *ctx, const char *task_id,
                                 const char *work) {
  char payload[RT_XL], etask[RT_MED], ework[RT_LARGE];
  if (!ctx || !task_id || !work ||
      json_escape(task_id, etask, sizeof(etask)) != 0 ||
      json_escape(work, ework, sizeof(ework)) != 0)
    return -1;
  return rt_append_event_format(
      ctx, "task_created", "a4_fixture", payload, sizeof(payload), 0,
      "{\"task_id\":\"%s\",\"context_id\":\"chat:tests\","
      "\"kind\":\"work\",\"work_rev\":1,\"input\":null,"
      "\"created_event_id\":\"created_%s\",\"created_ms\":1000,"
      "\"state\":{\"status\":\"open\",\"work\":\"%s\","
      "\"done_when\":\"done\",\"work_rev\":1,\"artifacts\":[],"
      "\"updated_ms\":1000}}",
      etask, etask, ework);
}

/* The outer isolation regression enables this only for one focused run. It
 * pauses after the real fixture writer has modified the copied shipping
 * registry and created action/floop fixtures, then reports the exact C PID and
 * physical scratch root so the parent can SIGKILL it and verify cleanup. */
static int pause_for_fixture_isolation_probe(void) {
  const char *ready_path = getenv("FCLAW_TEST_FIXTURE_ISOLATION_READY_FILE");
  char cwd[4096];
  char ready[4352];
  if (!ready_path || !*ready_path) return 0;
  if (ready_path[0] != '/' || !getcwd(cwd, sizeof(cwd))) return -1;
  snprintf(ready, sizeof(ready), "%ld\n%s\n", (long)getpid(), cwd);
  if (test_write_file(ready_path, ready) != 0) return -1;
  for (;;) pause();
}

/* ===== log inspection ============================================= */

static char *read_all_run_logs(void) {
  DIR *d = opendir("workspace/runs");
  struct dirent *ent;
  char *all = NULL;
  size_t used = 0, cap = 0;
  if (!d) return strdup("");
  while ((ent = readdir(d)) != NULL) {
    char path[512], *text = NULL;
    size_t n;
    if (ent->d_name[0] == '.') continue;
    snprintf(path, sizeof(path), "workspace/runs/%s/event_log.jsonl", ent->d_name);
    if (test_read_file(path, &text) != 0 || !text) continue;
    n = strlen(text);
    if (used + n + 1 > cap) {
      size_t nc = cap ? cap : 8192;
      while (nc < used + n + 1) nc *= 2;
      all = realloc(all, nc);
      cap = nc;
    }
    memcpy(all + used, text, n);
    used += n;
    all[used] = '\0';
    free(text);
  }
  closedir(d);
  if (!all) return strdup("");
  return all;
}

static char *read_all_agent_artifacts_with_suffix(const char *suffix) {
  DIR *runs = opendir("workspace/runs");
  struct dirent *run;
  char *all = NULL;
  size_t used = 0, cap = 0;
  size_t suffix_len = suffix ? strlen(suffix) : 0;
  if (!runs || !suffix_len) {
    if (runs) closedir(runs);
    return strdup("");
  }
  while ((run = readdir(runs)) != NULL) {
    char dir_path[512];
    DIR *artifacts;
    struct dirent *entry;
    if (run->d_name[0] == '.') continue;
    snprintf(dir_path, sizeof(dir_path),
             "workspace/runs/%s/agent_outputs", run->d_name);
    artifacts = opendir(dir_path);
    if (!artifacts) continue;
    while ((entry = readdir(artifacts)) != NULL) {
      char path[1024], *text = NULL;
      size_t name_len = strlen(entry->d_name), n;
      if (name_len < suffix_len ||
          strcmp(entry->d_name + name_len - suffix_len, suffix) != 0)
        continue;
      snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
      if (test_read_file(path, &text) != 0 || !text) continue;
      n = strlen(text);
      if (used + n + 2U > cap) {
        size_t next_cap = cap ? cap : 8192;
        char *next;
        while (next_cap < used + n + 2U) next_cap *= 2U;
        next = (char *)realloc(all, next_cap);
        if (!next) {
          free(text);
          closedir(artifacts);
          closedir(runs);
          free(all);
          return NULL;
        }
        all = next;
        cap = next_cap;
      }
      memcpy(all + used, text, n);
      used += n;
      all[used++] = '\n';
      all[used] = '\0';
      free(text);
    }
    closedir(artifacts);
  }
  closedir(runs);
  return all ? all : strdup("");
}

static int count_substr(const char *hay, const char *needle) {
  int n = 0;
  const char *p = hay;
  size_t len = strlen(needle);
  if (!hay || !needle || !len) return 0;
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += len;
  }
  return n;
}

static int count_lines_with_both(const char *text,
                                 const char *first,
                                 const char *second) {
  const char *line = text;
  int count = 0;
  if (!text || !first || !second) return 0;
  while (*line) {
    const char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    const char *a = strstr(line, first);
    const char *b = strstr(line, second);
    if (a && b && (size_t)(a - line) < len &&
        (size_t)(b - line) < len)
      count++;
    if (!end) break;
    line = end + 1;
  }
  return count;
}

static int count_run_dirs(void) {
  DIR *d = opendir("workspace/runs");
  struct dirent *ent;
  int n = 0;
  if (!d) return 0;
  while ((ent = readdir(d)) != NULL)
    if (ent->d_name[0] != '.') n++;
  closedir(d);
  return n;
}

static int count_regular_entries(const char *path) {
  DIR *d = opendir(path);
  struct dirent *entry;
  int count = 0;
  if (!d) return 0;
  while ((entry = readdir(d)) != NULL)
    if (entry->d_name[0] != '.') count++;
  closedir(d);
  return count;
}

static int count_provider_request_artifacts(void) {
  DIR *runs = opendir("workspace/runs");
  struct dirent *run;
  int count = 0;
  if (!runs) return 0;
  while ((run = readdir(runs)) != NULL) {
    char path[1024];
    DIR *calls;
    struct dirent *entry;
    size_t suffix_len = strlen("_raw_request.json");
    if (run->d_name[0] == '.') continue;
    snprintf(path, sizeof(path), "workspace/runs/%s/provider_calls",
             run->d_name);
    calls = opendir(path);
    if (!calls) continue;
    while ((entry = readdir(calls)) != NULL) {
      size_t len = strlen(entry->d_name);
      if (len >= suffix_len &&
          strcmp(entry->d_name + len - suffix_len,
                 "_raw_request.json") == 0)
        count++;
    }
    closedir(calls);
  }
  closedir(runs);
  return count;
}

static int drive_until_file_contains(HarnessGateway *g, const char *path,
                                     const char *needle, int timeout_ms);

static int read_first_file_with_suffix(const char *dir_path, const char *suffix,
                                       char **text_out) {
  DIR *d;
  struct dirent *ent;
  size_t suffix_len;
  int rc = -1;
  if (text_out) *text_out = NULL;
  if (!dir_path || !suffix || !text_out) return -1;
  d = opendir(dir_path);
  if (!d) return -1;
  suffix_len = strlen(suffix);
  while ((ent = readdir(d)) != NULL) {
    char path[1024];
    size_t name_len = strlen(ent->d_name);
    if (name_len < suffix_len ||
        strcmp(ent->d_name + name_len - suffix_len, suffix) != 0)
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
    rc = test_read_file(path, text_out);
    break;
  }
  closedir(d);
  return rc;
}

/* Shipped core web_fetch is an immediate managed operation. Its raw HTTP
 * status must not collide with the driver's reserved lifecycle status, and
 * exactly one correlated terminal envelope must be published. */
int core_web_fetch_terminal_publishes_one_correlated_operation_result(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  char *saved_path = fx_capture_env("PATH");
  char *saved_args = fx_capture_env("FCLAW_WEB_FETCH_CURL_ARGS");
  HarnessGateway *g = NULL;
  char cwd[4096], path_env[8192], args_path[8192];
  char *logs = NULL, *bus = NULL, *ops = NULL, *curl_args = NULL, *body = NULL;
  char *raw_output = NULL;
  char handle[128] = "", handle_needle[160];
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/fixtures/webfetch_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/webfetch_start.json",
      "{\"calls\":[{\"name\":\"web_fetch\",\"args\":{"
      "\"op\":\"start\",\"task_id\":\"__TASK_ID__\","
      "\"url\":\"https://fixture.invalid/page\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/webfetch_result.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{\"message\":\"web fetch observed\"}},"
      "{\"name\":\"task.update\",\"args\":{\"task_id\":\"__TASK_ID__\","
      "\"state\":\"completed\"}}]}\n");
  rc |= write_exec("workspace/fixtures/curl",
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "printf '%s\\n' \"$*\" > \"${FCLAW_WEB_FETCH_CURL_ARGS:?}\"\n"
      "printf 'fixture body from local curl\\n206'\n");
  rc |= expect(getcwd(cwd, sizeof(cwd)) != NULL, "resolve web_fetch fixture root");
  snprintf(args_path, sizeof(args_path), "%s/workspace/fixtures/curl.args", cwd);
  snprintf(path_env, sizeof(path_env), "%s/workspace/fixtures:%s",
           cwd, saved_path ? saved_path : "/usr/bin:/bin");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/webfetch_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/webfetch_start.json:"
               "workspace/fixtures/webfetch_result.json", 1);
  (void)setenv("FCLAW_WEB_FETCH_CURL_ARGS", args_path, 1);
  (void)setenv("PATH", path_env, 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "openclaw floop loads for web_fetch");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "fetch the local fixture") == 0,
               "publish web_fetch request");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/bus.jsonl",
                                         "\"type\":\"operation_result\"",
                                         8000) == 0,
               "web_fetch publishes a terminal operation_result");
  rc |= expect(drive_until_file_contains(g,
                                         "workspace/runs/run_002/event_log.jsonl",
                                         "\"type\":\"run_done\"", 8000) == 0,
               "web_fetch result turn retires");
  harness_gateway_close(g);
  g = NULL;

  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus);
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  rc |= test_read_file(args_path, &curl_args);
  rc |= read_first_file_with_suffix("workspace/runs/run_001/action_calls",
                                    ".body.html", &body);
  rc |= read_first_file_with_suffix("workspace/runs/run_001/action_calls",
                                    ".output.json", &raw_output);
  rc |= expect(logs && bus && ops && curl_args && body && raw_output,
               "web_fetch proof artifacts exist");
  rc |= expect_substr(raw_output, "\"http_status\": \"206\"",
                      "raw web_fetch output records HTTP status");
  if (ops) {
    const char *start = strstr(ops, "\"handle\":\"");
    if (start) {
      const char *end;
      start += strlen("\"handle\":\"");
      end = strchr(start, '"');
      if (end && (size_t)(end - start) < sizeof(handle)) {
        memcpy(handle, start, (size_t)(end - start));
        handle[end - start] = '\0';
      }
    }
  }
  rc |= expect(strcmp(handle, "op_actionreq_run_001_000005") == 0,
               "web_fetch uses its preallocated runtime operation id");
  snprintf(handle_needle, sizeof(handle_needle), "\"handle\":\"%s\"", handle);
  rc |= expect_substr(logs, handle_needle,
                      "action terminal and event log share the handle");
  rc |= expect_substr(bus, handle_needle,
                      "bus terminal shares the runtime handle");
  rc |= expect_substr(logs, "\"status\":\"finished\"",
                      "managed wrapper supplies finished status");
  rc |= expect_substr(logs, "\"http_status\": \"206\"",
                      "HTTP status survives under a non-reserved field");
  rc |= expect_no_substr(logs, "\"status\":\"206\"",
                         "HTTP status cannot impersonate lifecycle status");
  rc |= expect(count_substr(bus, "\"type\":\"operation_result\"") == 1,
               "terminal result is published to the bus exactly once");
  rc |= expect(count_substr(logs, "\"type\":\"operation_result\"") == 2,
               "driver append and one result-run intake are durable");
  rc |= expect_substr(bus, "\"action\":\"web_fetch\"",
                      "terminal result names web_fetch");
  rc |= expect_substr(bus, "\"task_id\":\"task_run_001_000002\"",
                      "terminal result preserves task correlation");
  rc |= expect_substr(bus, "\"context_id\":\"chat:tests\"",
                      "terminal result preserves context correlation");
  rc |= expect_substr(bus, "\"channel\":\"tests\"",
                      "terminal result preserves channel correlation");
  rc |= expect_substr(bus, "\"origin_event_id\":\"bus_000001\"",
                      "terminal result preserves origin correlation");
  rc |= expect_substr(bus, "\"ref\":null",
                      "terminal result preserves null ref correlation");
  rc |= expect_substr(bus, "HTTP 206 from https://fixture.invalid/page",
                      "result turn receives HTTP status text");
  rc |= expect_substr(bus, "fixture body from local curl",
                      "result turn receives bounded body text");
  rc |= expect_substr(curl_args, "https://fixture.invalid/page",
                      "web_fetch invokes the hermetic curl boundary");
  rc |= expect_substr(body, "fixture body from local curl",
                      "full response body reaches its artifact");
  rc |= expect_no_substr(logs, "unknown status",
                         "web_fetch no longer violates managed status");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "web_fetch lifecycle has no failed run");

done:
  if (g) harness_gateway_close(g);
  free(raw_output);
  free(body);
  free(curl_args);
  free(ops);
  free(bus);
  free(logs);
  fx_restore_env("FCLAW_WEB_FETCH_CURL_ARGS", saved_args);
  fx_restore_env("PATH", saved_path);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* Publish a raw completion-claim envelope directly into the bus inbox
 * (workers normally use the helper; tests use it to inject forged,
 * duplicate, and stale claims deterministically). */
static int publish_claim_envelope(const char *operation_id,
                                  const char *token,
                                  const char *status,
                                  const char *text) {
  static int seq = 0;
  char path[512], body[1024];
  seq++;
  snprintf(path, sizeof(path), "workspace/bus/inbox/bus_9%05d.json", seq);
  snprintf(body, sizeof(body),
           "{\"event_id\":\"bus_9%05d\",\"ts\":\"2026-07-11T00:00:00Z\","
           "\"channel\":\"operations\",\"type\":\"operation_completed\","
           "\"payload\":{\"operation_id\":\"%s\",\"completion_token\":\"%s\","
           "\"cause\":\"worker\",\"result\":{\"status\":\"%s\","
           "\"text\":\"%s\"}}}\n",
           seq, operation_id, token, status, text);
  return test_write_file(path, body);
}

/* Drive the gateway until a file contains a needle (or timeout). */
static int drive_until_file_contains(HarnessGateway *g, const char *path,
                                     const char *needle, int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    char *text = NULL;
    harness_gateway_drive_one_tick(g);
    if (test_read_file(path, &text) == 0 && text) {
      int hit = strstr(text, needle) != NULL;
      free(text);
      if (hit) return 0;
    }
    usleep(10000);
    waited += 10;
  }
  return -1;
}

/* Wait for a detached fixture to publish one of its own terminal artifacts.
 * No gateway tick is needed while the child process is doing the work. */
static int wait_until_file_contains(const char *path, const char *needle,
                                    int timeout_ms) {
  int waited = 0;
  while (waited < timeout_ms) {
    char *text = NULL;
    if (test_read_file(path, &text) == 0 && text) {
      int hit = strstr(text, needle) != NULL;
      free(text);
      if (hit) return 0;
    }
    usleep(10000);
    waited += 10;
  }
  return -1;
}

static int drive_for_ms(HarnessGateway *g, int ms) {
  int waited = 0;
  while (waited < ms) {
    harness_gateway_drive_one_tick(g);
    usleep(10000);
    waited += 10;
  }
  return 0;
}

int deterministic_completion_claims_use_durable_operation_contract(void) {
  HarnessGateway *g = NULL;
  RtActionRegistry *registry = NULL;
  RtOperationSnapshot snapshot;
  const RtActionDef *def;
  char registry_err[RT_LARGE] = "";
  char *logs = NULL, *ops = NULL;
  char op_id[RT_SMALL] = "";
  char token[RT_OPERATION_TOKEN_HEX + 1] = "";
  int rc = 0;

  /* Registry boundary: the polling contract is gone; the completion
   * contract requires an action-owned deadline. */
  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= write_a3_adapter("a3_fake_a", "a3a");
  rc |= write_a3_adapter("a3_fake_b", "a3b");
  rc |= write_a3_floop("a3poll", "a3_fake_a", 1);
  registry = (RtActionRegistry *)calloc(1, sizeof(*registry));
  rc |= expect(registry != NULL, "allocate A3 action registry fixture");
  if (!registry) return 1;
  rc |= expect(rt_action_registry_load(registry, registry_err,
                                       sizeof(registry_err)) == 0,
               "A3 fixture action registry loads");
  def = rt_action_registry_find(registry, "manage_codex");
  rc |= expect(def && def->default_timeout_ms > 0 &&
                   def->max_timeout_ms >= def->default_timeout_ms,
               "manage_codex declares its completion deadline policy");
  def = rt_action_registry_find(registry, "manage_claude");
  rc |= expect(def && def->default_timeout_ms > 0,
               "manage_claude declares its completion deadline policy");
  def = rt_action_registry_find(registry, "manage_hermes");
  rc |= expect(def && def->default_timeout_ms > 0,
               "manage_hermes declares its completion deadline policy");
  rc |= test_write_file("actions/testfx/a3_legacy_poll/action.json",
      "{\"id\":\"a3_legacy_poll\",\"description\":\"legacy poll metadata\","
      "\"outside_world\":false,\"contract\":\"managed_operation\","
      "\"default_timeout_ms\":60000,\"poll_handle_arg\":\"op_id\","
      "\"args_schema\":{\"type\":\"object\",\"properties\":{"
      "\"op\":{\"type\":\"string\",\"enum\":[\"start\"]},"
      "\"op_id\":{\"type\":\"string\"}}},"
      "\"exec\":[\"bash\",\"run.sh\"],\"timeout_ms\":1}\n");
  registry_err[0] = '\0';
  rc |= expect(rt_action_registry_load(registry, registry_err,
                                       sizeof(registry_err)) != 0,
               "poll_handle_arg fails registry load loudly");
  rc |= expect_substr(registry_err, "poll_handle_arg is no longer supported",
                      "legacy poll metadata error names the removal");
  rc |= test_remove_path("actions/testfx/a3_legacy_poll");
  rc |= test_write_file("actions/testfx/a3_no_deadline/action.json",
      "{\"id\":\"a3_no_deadline\",\"description\":\"missing deadline\","
      "\"outside_world\":false,\"contract\":\"managed_operation\","
      "\"args_schema\":{\"type\":\"object\",\"properties\":{"
      "\"op\":{\"type\":\"string\",\"enum\":[\"start\"]}}},"
      "\"exec\":[\"bash\",\"run.sh\"],\"timeout_ms\":1}\n");
  registry_err[0] = '\0';
  rc |= expect(rt_action_registry_load(registry, registry_err,
                                       sizeof(registry_err)) != 0,
               "managed contract without default_timeout_ms fails loudly");
  rc |= expect_substr(registry_err, "default_timeout_ms",
                      "missing deadline error names the required field");
  rc |= test_remove_path("actions/testfx/a3_no_deadline");
  rc |= test_write_file("actions/testfx/a3_bad_max/action.json",
      "{\"id\":\"a3_bad_max\",\"description\":\"inverted deadline bounds\","
      "\"outside_world\":false,\"contract\":\"managed_operation\","
      "\"default_timeout_ms\":60000,\"max_timeout_ms\":1000,"
      "\"args_schema\":{\"type\":\"object\",\"properties\":{"
      "\"op\":{\"type\":\"string\",\"enum\":[\"start\"]}}},"
      "\"exec\":[\"bash\",\"run.sh\"],\"timeout_ms\":1}\n");
  registry_err[0] = '\0';
  rc |= expect(rt_action_registry_load(registry, registry_err,
                                       sizeof(registry_err)) != 0,
               "max below default fails registry load loudly");
  rc |= expect_substr(registry_err, "max_timeout_ms must be >=",
                      "inverted bounds error names the constraint");
  rc |= test_remove_path("actions/testfx/a3_bad_max");
  free(registry);
  registry = NULL;

  /* Preallocated identity: the start detaches; the durable record is
   * keyed by the runtime operation id, carries correlation and a
   * deadline, and holds a bound completion token that reached the
   * worker only through trusted env. */
  g = harness_gateway_init("a3poll");
  rc |= expect(g != NULL, "A3 completion floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "create A3 work") == 0,
               "publish A3 work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"worker_handle\":\"a3a_001\"", 8000) == 0,
               "operation is durably running with its worker handle");
  harness_gateway_close(g);
  g = NULL;
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  rc |= expect(ops != NULL, "operation store exists");
  if (ops) {
    const char *start = strstr(ops, "\"handle\":\"");
    if (start) {
      const char *end;
      start += strlen("\"handle\":\"");
      end = strchr(start, '"');
      if (end && (size_t)(end - start) < sizeof(op_id)) {
        memcpy(op_id, start, (size_t)(end - start));
        op_id[end - start] = '\0';
      }
    }
    free(ops);
    ops = NULL;
  }
  rc |= expect(strncmp(op_id, "op_actionreq_", 13) == 0,
               "operation id is the runtime-owned preallocated identity");
  rc |= expect(rt_operation_snapshot(op_id, &snapshot) == 0,
               "durable operation snapshot resolves by operation id");
  rc |= expect(strcmp(snapshot.action, "a3_fake_a") == 0,
               "snapshot owns the selected action");
  rc |= expect(strcmp(snapshot.worker_handle, "a3a_001") == 0,
               "snapshot records the worker handle as metadata");
  rc |= expect(snapshot.task_id[0] && snapshot.work_rev == 1,
               "snapshot owns the exact task revision");
  rc |= expect(strcmp(snapshot.status, "running") == 0,
               "snapshot owns running status");
  rc |= expect(snapshot.deadline_ms > 0,
               "operation carries a durable completion deadline");
  rc |= expect(strcmp(snapshot.context_id, "chat:tests") == 0 &&
               snapshot.origin_event_id[0],
               "snapshot round-trips context and origin event");
  rc |= expect(rt_operation_token_of(op_id, token, sizeof(token)) == 0 &&
               token[0],
               "completion token is durably bound before the worker runs");
  {
    char *recorded = NULL;
    rc |= test_read_file("workspace/a3_a3_fake_a/operation_id", &recorded);
    rc |= expect(recorded && strcmp(recorded, op_id) == 0,
                 "worker receives the operation id through trusted env");
    free(recorded);
    recorded = NULL;
    rc |= test_read_file("workspace/a3_a3_fake_a/token", &recorded);
    rc |= expect(recorded && strcmp(recorded, token) == 0,
                 "worker receives the completion token through trusted env");
    free(recorded);
  }

  /* Forged claims are rejected before any run exists; a valid claim
   * opens exactly one result run; a duplicate is inert. */
  g = harness_gateway_init("a3poll");
  rc |= expect(g != NULL, "A3 floop reloads for claim admission");
  if (!g) return 1;
  rc |= expect(publish_claim_envelope(op_id, "forged-token",
                                      "succeeded", "forged text") == 0,
               "publish forged completion claim");
  rc |= expect(publish_claim_envelope("op_unknown_404", token,
                                      "succeeded", "stray text") == 0,
               "publish unknown-operation claim");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/rejected_events.jsonl",
                   "completion_token_mismatch", 8000) == 0,
               "forged token is rejected inspectably");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/rejected_events.jsonl",
                   "unknown_operation", 8000) == 0,
               "unknown operation id is rejected inspectably");
  {
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"event_id\":\"bus_988888\",\"ts\":\"2026-07-11T00:00:00Z\","
             "\"channel\":\"operations\",\"type\":\"operation_completed\","
             "\"payload\":{\"operation_id\":\"%s\",\"completion_token\":\"%s\","
             "\"task_id\":\"task_forged\",\"cause\":\"worker\","
             "\"result\":{\"status\":\"succeeded\",\"text\":\"forged corr\"}}}\n",
             op_id, token);
    rc |= expect(test_write_file("workspace/bus/inbox/bus_988888.json",
                                 body) == 0,
                 "publish claim carrying caller-authored correlation");
  }
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/rejected_events.jsonl",
                   "claim_carries_runtime_owned_field", 8000) == 0,
               "caller-authored correlation is rejected, not stripped");
  rc |= expect(rt_operation_snapshot(op_id, &snapshot) == 0 &&
               strcmp(snapshot.status, "running") == 0,
               "forged claims cannot terminalize the operation");
  rc |= expect(rt_operation_publish_worker_claim(
                   op_id, token, "succeeded", "A3 fixture complete") == 0,
               "publish the genuine completion claim");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"status\":\"finished\"", 8000) == 0,
               "genuine claim terminalizes the operation");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/tasks.json",
                   "A3 fixture complete", 8000) == 0,
               "task external carries the completion excerpt");
  rc |= expect(publish_claim_envelope(op_id, token,
                                      "succeeded", "duplicate text") == 0,
               "publish duplicate completion claim");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/rejected_events.jsonl",
                   "operation_already_terminal", 8000) == 0,
               "duplicate claim is rejected as already terminal");
  drive_for_ms(g, 300);
  harness_gateway_close(g);
  g = NULL;
  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"type\":\"operation_result\"") == 1,
               "exactly one canonical operation_result exists");
  rc |= expect(count_substr(logs, "\"type\":\"operation_started\"") == 1,
               "exactly one operation_started exists");
  rc |= expect_no_substr(logs, "operation_poll",
                         "no polling artifact appears anywhere");
  rc |= expect_substr(logs, "A3 fixture complete",
                      "claim text reaches the result run unchanged");
  rc |= expect_substr(logs, "\"status\":\"succeeded\"",
                      "canonical result carries its terminal status");
  rc |= expect_no_substr(logs, "forged text",
                         "forged claim text never becomes behavioral truth");
  rc |= expect_no_substr(logs, "duplicate text",
                         "duplicate claim text never becomes behavioral truth");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "claim admission has no failed run");
  rc |= expect(count_provider_request_artifacts() == 0,
               "claim admission creates no provider request");
  free(logs);
  logs = NULL;

  /* In-flight revision capture: the preallocated record keeps the
   * selected revision even when durable task state advances while the
   * start subprocess is still in flight. */
  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= write_a3_adapter("a3_fake_a", "a3a");
  rc |= write_a3_adapter("a3_fake_b", "a3b");
  rc |= write_a3_floop("a3poll", "a3_fake_a", 1);
  (void)setenv("FCLAW_A3_START_DELAY", "0.30", 1);
  g = harness_gateway_init("a3poll");
  rc |= expect(g != NULL, "in-flight revision fixture loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "create A3 work") == 0,
               "publish delayed A3 work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/runs/run_001/event_log.jsonl",
                   "\"action\":\"a3_fake_a\"", 8000) == 0,
               "delayed selected action starts");
  {
    char task_id[RT_SMALL] = "", work[RT_LARGE] = "", done[RT_LARGE] = "";
    char external[RT_LARGE] = "", etask[RT_MED], update[RT_LARGE];
    long long task_rev = 0;
    rc |= expect(rt_task_open_work_snapshot(
                     "chat:tests", task_id, sizeof(task_id),
                     work, sizeof(work), done, sizeof(done), &task_rev,
                     external, sizeof(external)) == 0 &&
                 task_rev == 1,
                 "delayed action began from revision 1");
    if (json_escape(task_id, etask, sizeof(etask)) == 0) {
      snprintf(update, sizeof(update),
               "{\"task_id\":\"%s\",\"state\":{\"status\":\"open\","
               "\"work\":\"A3 raced revision\",\"done_when\":\"new done\","
               "\"updated_ms\":1785088000000}}",
               etask);
      rc |= expect(rt_task_apply_event("task_updated", update) == 0,
                   "durable task revision advances while action is in flight");
      rc |= expect(rt_task_work_rev_of(task_id, &task_rev) == 0 &&
                   task_rev == 2,
                   "task store now owns revision 2");
    } else {
      rc |= expect(0, "escape delayed task id");
    }
  }
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"worker_handle\":\"a3a_001\"", 8000) == 0,
               "delayed action records its worker handle");
  harness_gateway_close(g);
  g = NULL;
  unsetenv("FCLAW_A3_START_DELAY");
  op_id[0] = '\0';
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  if (ops) {
    const char *start = strstr(ops, "\"handle\":\"");
    if (start) {
      const char *end;
      start += strlen("\"handle\":\"");
      end = strchr(start, '"');
      if (end && (size_t)(end - start) < sizeof(op_id)) {
        memcpy(op_id, start, (size_t)(end - start));
        op_id[end - start] = '\0';
      }
    }
    free(ops);
    ops = NULL;
  }
  rc |= expect(op_id[0] && rt_operation_snapshot(op_id, &snapshot) == 0 &&
               snapshot.work_rev == 1,
               "operation snapshot keeps selected revision 1");
  rc |= expect(count_provider_request_artifacts() == 0,
               "in-flight revision proof uses no provider");

  /* Stale revision: completing the old running attempt after a newer
   * revision exists remains durable evidence but cannot restart work or
   * advance the newer revision. */
  rc |= write_a3_floop("a3poll", "a3_fake_a", 0);
  g = harness_gateway_init("a3poll");
  rc |= expect(g != NULL, "controller-disabled stale-revision fixture loads");
  if (!g) return 1;
  rc |= expect(rt_operation_token_of(op_id, token, sizeof(token)) == 0 &&
               token[0],
               "stale operation still holds its completion capability");
  rc |= expect(rt_operation_publish_worker_claim(
                   op_id, token, "succeeded", "stale revision result") == 0,
               "publish stale-revision completion claim");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"status\":\"finished\"", 8000) == 0,
               "stale-revision claim terminalizes its own operation");
  drive_for_ms(g, 400);
  harness_gateway_close(g);
  g = NULL;
  {
    char *tasks = NULL, *starts = NULL;
    rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
    rc |= expect(tasks != NULL, "stale-revision task projection exists");
    if (tasks) {
      rc |= expect_substr(tasks, "\"work_rev\":2",
                          "task remains on its newer revision");
      rc |= expect_substr(tasks, "\"status\":\"open\"",
                          "stale completion cannot complete newer work");
      free(tasks);
    }
    rc |= test_read_file("workspace/a3_a3_fake_a/starts", &starts);
    rc |= expect(starts && strcmp(starts, "1") == 0,
                 "stale completion restarts nothing");
    free(starts);
  }
  rc |= expect(rt_operation_snapshot(op_id, &snapshot) == 0 &&
               snapshot.work_rev == 1 &&
               strcmp(snapshot.status, "finished") == 0,
               "stale revision completion remains durable evidence");

  /* Exercise the shipped manage_codex runtime adapter end to end: the
   * supervising runner itself verifies and submits completion — no poll
   * envelope, no poller agent, no test-side claim. */
  {
    char *saved_codex = fx_capture_env("FCLAW_CODEX_BIN");
    char *saved_sync = fx_capture_env("FCLAW_CODEX_START_SYNC_MS");

    rc |= fx_reset();
    rc |= setup_testfx_actions();
    rc |= write_a3_adapter("a3_fake_b", "a3b");
    rc |= write_a3_floop("a3codex", "manage_codex", 1);
    rc |= write_a3_controller_catalog(
        "a3codex", "manage_codex", "working_memory_append");
    rc |= write_exec(
        "floops/a3codex/agents/a3_controller/run.sh",
        "#!/usr/bin/env bash\n"
        "cat >/dev/null\n"
        "echo '{\"calls\":[{\"name\":\"manage_codex\",\"args\":{"
        "\"op\":\"start\",\"task\":\"do A3 work\"}},{\"name\":"
        "\"working_memory_append\",\"args\":{\"working_memory\":"
        "\"A3 shared discovery\"}}]}'\n");
    rc |= write_exec(
        "workspace/fixtures/a3_fake_codex.sh",
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        "out=\"\"\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  case \"$1\" in\n"
        "    -o) out=\"$2\"; shift 2 ;;\n"
        "    -C) shift 2 ;;\n"
        "    *) shift ;;\n"
        "  esac\n"
        "done\n"
        "prompt=\"$(cat)\"\n"
        "printf '%s' \"$prompt\" > \"$FCLAW_ACTION_ROOT/workspace/fixtures/a3_prompt.txt\"\n"
        "printf '%s' \"${FCLAW_BOUND_TASK_ID:-}\" > \"$FCLAW_ACTION_ROOT/workspace/fixtures/a3_bound_task.txt\"\n"
        "printf '%s' \"${FCLAW_COMPLETION_TOKEN:-}\" > \"$FCLAW_ACTION_ROOT/workspace/fixtures/a3_worker_token.txt\"\n"
        "printf 'A3 actual manage_codex complete\\n' > \"$out\"\n"
        "printf '%s\\n' '{\"type\":\"result\",\"text\":\"done\"}'\n");
    (void)setenv("FCLAW_CODEX_BIN",
                 "workspace/fixtures/a3_fake_codex.sh", 1);
    (void)unsetenv("FCLAW_CODEX_START_SYNC_MS");

    g = harness_gateway_init("a3codex");
    rc |= expect(g != NULL, "actual manage_codex completion fixture loads");
    if (g) {
      rc |= expect(harness_gateway_publish(g, "tests",
                                           "create A3 work") == 0,
                   "publish actual manage_codex work");
      rc |= expect(drive_until_file_contains(
                       g, "workspace/memory/state/operations.json",
                       "\"action\":\"manage_codex\"", 8000) == 0,
                   "actual manage_codex operation is durably recorded");
      rc |= expect(drive_until_file_contains(
                       g, "workspace/memory/state/operations.json",
                       "\"status\":\"finished\"", 12000) == 0,
                   "runner-submitted completion terminalizes the operation");
      drive_for_ms(g, 300);
      harness_gateway_close(g);
      g = NULL;
    }
    {
      char *prompt = NULL, *bound_task = NULL, *worker_token = NULL;
      rc |= test_read_file("workspace/fixtures/a3_prompt.txt", &prompt);
      rc |= test_read_file("workspace/fixtures/a3_bound_task.txt", &bound_task);
      (void)test_read_file("workspace/fixtures/a3_worker_token.txt",
                           &worker_token);
      rc |= expect_substr(prompt, "A3 shared discovery",
                          "managed Codex receives current shared working_memory");
      rc |= expect_substr(prompt, "working_memory_append",
                          "managed Codex receives the base append instruction");
      rc |= expect(bound_task && strncmp(bound_task, "task_run_", 9) == 0,
                   "managed Codex action CLI inherits exact task routing");
      rc |= expect(!worker_token || worker_token[0] == '\0',
                   "the completion token never reaches the provider CLI env");
      free(worker_token);
      free(bound_task);
      free(prompt);
    }
    logs = read_all_run_logs();
    rc |= expect_substr(logs, "A3 actual manage_codex complete",
                        "runner-verified result returns through the claim");
    rc |= expect_no_substr(logs, "operation_poll",
                           "actual manage_codex completes with zero polls");
    rc |= expect_no_substr(logs, "\"op\":\"poll\"",
                           "no poll call is ever dispatched");
    rc |= expect(count_provider_request_artifacts() == 0,
                 "actual manage_codex completion creates no provider request");
    free(logs);
    logs = NULL;
    fx_restore_env("FCLAW_CODEX_START_SYNC_MS", saved_sync);
    fx_restore_env("FCLAW_CODEX_BIN", saved_codex);
  }
  test_config_restore(saved_cfg);
  return rc;
}

/* The deadline, gateway-down durability, orphaned-claim reconciliation,
 * and the completion-beats-timeout race — the crash-window half of the
 * completion contract. */
int operation_completion_deadline_and_recovery_contract(void) {
  HarnessGateway *g = NULL;
  RtOperationSnapshot snapshot;
  char *logs = NULL, *ops = NULL, *bus = NULL;
  char op_id[RT_SMALL] = "";
  char token[RT_OPERATION_TOKEN_HEX + 1] = "";
  int rc = 0;

  /* 1. A worker that never completes produces exactly one timeout
   * consequence at its deadline and no periodic runs. */
  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= write_a3_adapter_with_deadline("a3_fake_a", "a3a", 400);
  rc |= write_a3_adapter("a3_fake_b", "a3b");
  rc |= write_a3_floop("a3timeout", "a3_fake_a", 1);
  g = harness_gateway_init("a3timeout");
  rc |= expect(g != NULL, "deadline fixture loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "create A3 work") == 0,
               "publish deadline work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"status\":\"finished\"", 8000) == 0,
               "deadline expiry terminalizes the silent operation");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/tasks.json",
                   "reached its completion deadline", 8000) == 0,
               "timeout consequence projects into the owning task");
  drive_for_ms(g, 600);
  harness_gateway_close(g);
  g = NULL;
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus);
  rc |= expect(count_substr(logs, "\"type\":\"operation_result\"") == 1,
               "deadline produces exactly one canonical result");
  rc |= expect_substr(logs, "\"status\":\"timed_out\"",
                      "timeout result carries the timed_out status");
  rc |= expect_substr(logs, "was not retried",
                      "timeout text preserves the ambiguity posture");
  rc |= expect(bus &&
               count_substr(bus, "\"type\":\"operation_completed\"") == 1,
               "the idempotent timeout claim publishes exactly once");
  rc |= expect(count_run_dirs() <= 3,
               "a silent operation creates no periodic runs");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "deadline path has no failed run");
  free(bus);
  bus = NULL;
  free(logs);
  logs = NULL;

  /* 2. A completion written while the gateway is stopped survives in
   * the bus; a claim orphaned by a crash between the destructive intake
   * rename and the run claim is reconciled back into intake. */
  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= write_a3_adapter("a3_fake_a", "a3a");
  rc |= write_a3_adapter("a3_fake_b", "a3b");
  rc |= write_a3_floop("a3poll", "a3_fake_a", 1);
  g = harness_gateway_init("a3poll");
  rc |= expect(g != NULL, "recovery fixture loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "create A3 work") == 0,
               "publish recovery work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"worker_handle\":\"a3a_001\"", 8000) == 0,
               "recovery operation is durably running");
  harness_gateway_close(g);
  g = NULL;
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  if (ops) {
    const char *start = strstr(ops, "\"handle\":\"");
    if (start) {
      const char *end;
      start += strlen("\"handle\":\"");
      end = strchr(start, '"');
      if (end && (size_t)(end - start) < sizeof(op_id)) {
        memcpy(op_id, start, (size_t)(end - start));
        op_id[end - start] = '\0';
      }
    }
    free(ops);
    ops = NULL;
  }
  rc |= expect(op_id[0] &&
               rt_operation_token_of(op_id, token, sizeof(token)) == 0 &&
               token[0],
               "recovery operation exposes its capability to the test");
  /* The helper's publication path works with no gateway alive. */
  rc |= expect(rt_operation_publish_worker_claim(
                   op_id, token, "succeeded",
                   "completed while the gateway slept") == 0,
               "completion publishes while the gateway is stopped");
  rc |= expect(count_regular_entries("workspace/bus/inbox") == 1,
               "stopped-gateway completion is durable in the inbox");
  /* Crash window 4: the envelope was renamed out of the inbox but no
   * run ever claimed it. */
  {
    DIR *d = opendir("workspace/bus/inbox");
    struct dirent *ent;
    char from[PATH_MAX], to[PATH_MAX];
    int moved = 0;
    rc |= expect(d != NULL, "inbox opens for the orphan simulation");
    rc |= expect(test_mkdir_p("workspace/bus/processed") == 0,
                 "processed dir exists for the orphan simulation");
    if (d) {
      while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "bus_", 4) != 0) continue;
        snprintf(from, sizeof(from), "workspace/bus/inbox/%s", ent->d_name);
        snprintf(to, sizeof(to), "workspace/bus/processed/%s", ent->d_name);
        if (rename(from, to) == 0) moved = 1;
      }
      closedir(d);
    }
    rc |= expect(moved, "claim is orphaned into processed with no run");
  }
  g = harness_gateway_init("a3poll");
  rc |= expect(g != NULL, "gateway restarts over the orphaned claim");
  if (!g) return 1;
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"status\":\"finished\"", 8000) == 0,
               "reconciled claim terminalizes the operation");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/tasks.json",
                   "completed while the gateway slept", 8000) == 0,
               "reconciled completion reaches the owning task");
  drive_for_ms(g, 300);
  harness_gateway_close(g);
  g = NULL;
  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"type\":\"operation_result\"") == 1,
               "orphan reconciliation yields exactly one result");
  rc |= expect_no_substr(logs, "\"status\":\"timed_out\"",
                         "the recovered completion is not a timeout");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "orphan recovery has no failed run");
  {
    char *narration = NULL;
    rc |= test_read_file("workspace/logs/narration.jsonl", &narration);
    rc |= expect(narration &&
                 strstr(narration, "operation completion reconciled") != NULL,
                 "the reconciler narrates the requeue");
    free(narration);
  }
  free(logs);
  logs = NULL;

  /* 3. Completion racing an already-expired deadline has one
   * deterministic winner: the queued completion. */
  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= write_a3_adapter_with_deadline("a3_fake_a", "a3a", 300);
  rc |= write_a3_adapter("a3_fake_b", "a3b");
  rc |= write_a3_floop("a3race", "a3_fake_a", 1);
  g = harness_gateway_init("a3race");
  rc |= expect(g != NULL, "race fixture loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "create A3 work") == 0,
               "publish race work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"worker_handle\":\"a3a_001\"", 8000) == 0,
               "race operation is durably running");
  harness_gateway_close(g);
  g = NULL;
  op_id[0] = token[0] = '\0';
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  if (ops) {
    const char *start = strstr(ops, "\"handle\":\"");
    if (start) {
      const char *end;
      start += strlen("\"handle\":\"");
      end = strchr(start, '"');
      if (end && (size_t)(end - start) < sizeof(op_id)) {
        memcpy(op_id, start, (size_t)(end - start));
        op_id[end - start] = '\0';
      }
    }
    free(ops);
    ops = NULL;
  }
  rc |= expect(op_id[0] &&
               rt_operation_token_of(op_id, token, sizeof(token)) == 0,
               "race operation exposes its capability");
  rc |= expect(rt_operation_publish_worker_claim(
                   op_id, token, "succeeded", "the worker was faster") == 0,
               "queue the completion while the gateway is down");
  usleep(500000); /* the 300ms deadline is now unambiguously expired */
  g = harness_gateway_init("a3race");
  rc |= expect(g != NULL, "gateway restarts with claim and expired deadline");
  if (!g) return 1;
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/operations.json",
                   "\"status\":\"finished\"", 8000) == 0,
               "the race resolves to one terminal transition");
  drive_for_ms(g, 800);
  harness_gateway_close(g);
  g = NULL;
  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"type\":\"operation_result\"") == 1,
               "race yields exactly one canonical result");
  rc |= expect_substr(logs, "the worker was faster",
                      "the queued completion wins the race");
  rc |= expect_no_substr(logs, "\"status\":\"timed_out\"",
                         "the expired deadline cannot double-terminalize");
  rc |= expect(rt_operation_snapshot(op_id, &snapshot) == 0 &&
               strcmp(snapshot.status, "finished") == 0,
               "race operation is durably terminal exactly once");
  free(logs);
  return rc;
}

/* Discovery is the directory tree: there is no registration list, an action's
 * identity is action.json's id, and a leading underscore is how a directory
 * stays in the tree without being loaded. */
int action_discovery_is_the_directory_tree(void) {
  RtActionRegistry *registry = NULL;
  char err[RT_LARGE] = "";
  int rc = 0;

  rc |= fx_reset();
  registry = (RtActionRegistry *)calloc(1, sizeof(*registry));
  rc |= expect(registry != NULL, "allocate discovery registry fixture");
  if (!registry) return 1;

  /* Writing the directory is the whole install. Nothing else is edited, and
   * actions/registry.json does not exist. */
  rc |= expect_file_not_exists("actions/registry.json");
  rc |= test_write_file("actions/discofx/nested/deep/action.json",
      "{\"id\":\"disco_deep\",\"description\":\"Nested discovery fixture.\","
      "\"outside_world\":false,\"args_schema\":{\"type\":\"object\","
      "\"required\":[],\"additionalProperties\":false,\"properties\":{}},"
      "\"exec\":[\"runtime\",\"fc_message()\"],\"timeout_ms\":0}\n");
  rc |= expect(rt_action_registry_load(registry, err, sizeof(err)) == 0,
               "a copied action directory loads with no registration list");
  rc |= expect(rt_action_registry_find(registry, "disco_deep") != NULL,
               "an action nested below the top level is discovered");
  rc |= expect(rt_action_registry_find(registry, "message") != NULL,
               "shipped core actions still load alongside a copied one");

  /* action.json's id is the identity, so a second action claiming it is a
   * loud failure naming both manifests -- not a silent shadowing. */
  rc |= test_write_file("actions/discofx/collide/action.json",
      "{\"id\":\"disco_deep\",\"description\":\"Colliding id fixture.\","
      "\"outside_world\":false,\"args_schema\":{\"type\":\"object\","
      "\"required\":[],\"additionalProperties\":false,\"properties\":{}},"
      "\"exec\":[\"runtime\",\"fc_message()\"],\"timeout_ms\":0}\n");
  err[0] = '\0';
  rc |= expect(rt_action_registry_load(registry, err, sizeof(err)) != 0,
               "a duplicate action id fails the scan");
  rc |= expect_substr(err, "actions/discofx/collide/action.json",
                      "duplicate error names the second manifest");
  rc |= expect_substr(err, "actions/discofx/nested/deep/action.json",
                      "duplicate error names the first manifest");

  /* A leading underscore unloads a directory without deleting it, which is
   * also what keeps build tooling such as web_read's _pluck out of the scan. */
  rc |= expect(rename("actions/discofx/collide",
                      "actions/discofx/_collide") == 0,
               "park the colliding action behind a leading underscore");
  err[0] = '\0';
  rc |= expect(rt_action_registry_load(registry, err, sizeof(err)) == 0,
               "an underscore-prefixed directory is skipped, resolving the collision");
  rc |= expect(rt_action_registry_find(registry, "disco_deep") != NULL,
               "the remaining action still loads");

  rc |= test_remove_path("actions/discofx");
  free(registry);
  return rc;
}

/* The default (Action20) FloofClaw floop must carry an admitted work item all
 * the way to a result-manager reply, not only the chat acknowledgement. Drive
 * the shipped floofclaw floop over the real bus/intake loop: chat admits work
 * and acks, the bound work_manager completes it, and result_manager narrates
 * the terminal outcome to the channel. The lookup/poem smoke only reaches the
 * chat ack (fclaw run returns on the synchronous ack), so this witnesses the
 * default floop's terminal/reply tail that the smoke cannot. */
int default_floofclaw_admitted_work_reaches_result_manager_reply(void) {
  HarnessGateway *g = NULL;
  char *logs = NULL, *deliveries = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file(
      "workspace/fc_chat_admit.json",
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"note the greeting in a durable record\","
      "\"done_when\":\"the greeting is acknowledged\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"on it\"}}]}\n");
  rc |= test_write_file(
      "workspace/fc_work_complete.json",
      "{\"calls\":[{\"name\":\"work_complete\","
      "\"args\":{\"summary\":\"greeting acknowledged\"}}]}\n");
  rc |= test_write_file(
      "workspace/fc_result_reply.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"FLOOFCLAW_TERMINAL_REPLY_OK\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fc_chat_admit.json:"
               "workspace/fc_work_complete.json:"
               "workspace/fc_result_reply.json", 1);

  g = harness_gateway_init("floofclaw");
  rc |= expect(g != NULL, "default floofclaw floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "hi there") == 0,
               "publish user message to default floofclaw");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "FLOOFCLAW_TERMINAL_REPLY_OK", 12000) == 0,
               "admitted work reaches a result-manager reply on the channel");
  drive_for_ms(g, 300);
  harness_gateway_close(g);
  g = NULL;

  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(logs && deliveries,
               "default floofclaw terminal proof artifacts exist");
  if (logs) {
    rc |= expect(count_substr(logs, "\"source\":\"work_manager\"") >= 1,
                 "bound work_manager runs on the admitted work");
    rc |= expect(count_substr(logs, "\"type\":\"work_completed\"") == 1,
                 "work completion is one durable terminal source");
    rc |= expect(count_substr(logs, "\"type\":\"work_outcome\"") == 1,
                 "terminal work outcome is intaken exactly once");
    rc |= expect(count_substr(logs, "\"source\":\"result_manager\"") == 1,
                 "result manager runs exactly once after the terminal outcome");
    rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                           "default floofclaw terminal path has no failed run");
  }
  if (deliveries)
    rc |= expect(count_substr(deliveries, "FLOOFCLAW_TERMINAL_REPLY_OK") == 1,
                 "the terminal reply is delivered exactly once");
  free(deliveries);
  free(logs);
  return rc;
}

/* A failed attempt is evidence for the next controller turn, not authority
 * to block the task. The mock responses pin the semantic choices while the
 * shipped floop proves their durable action/result routing end to end. */
int work_manager_tries_justified_alternative_after_failure(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *logs = NULL, *steps = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/evidence/verified.txt",
                        "verified answer recovered by the second method\n");
  rc |= test_write_file(
      "workspace/dgu_admit.json",
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"Recover and verify the requested answer.\","
      "\"done_when\":\"A verified answer has been inspected.\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"working\"}}]}\n");
  rc |= test_write_file(
      "workspace/dgu_first_fails.json",
      "{\"calls\":[{\"name\":\"bash\",\"args\":{"
      "\"op\":\"start\",\"cmd\":\"printf 'first method failed\\\\n' "
      ">&2; exit 7\"}}]}\n");
  rc |= test_write_file(
      "workspace/dgu_try_alternative.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"evidence/verified.txt\"}}]}\n");
  rc |= test_write_file(
      "workspace/dgu_complete.json",
      "{\"calls\":[{\"name\":\"work_complete\",\"args\":{"
      "\"summary\":\"verified answer recovered through the second method\"}}]}\n");
  rc |= test_write_file(
      "workspace/dgu_result_complete.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"PERSISTENCE_ALTERNATIVE_COMPLETE\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/dgu_admit.json:"
               "workspace/dgu_first_fails.json:"
               "workspace/dgu_try_alternative.json:"
               "workspace/dgu_complete.json:"
               "workspace/dgu_result_complete.json", 1);

  g = harness_gateway_init("floofclaw");
  rc |= expect(g != NULL, "persistence alternative fixture loads");
  if (g) {
    rc |= expect(harness_gateway_publish(
                     g, "tests", "recover the answer after one failed method") == 0,
                 "publish persistence alternative work");
    rc |= expect(drive_until_file_contains(
                     g, "workspace/logs/deliveries.jsonl",
                     "PERSISTENCE_ALTERNATIVE_COMPLETE", 12000) == 0,
                 "justified alternative reaches completion");
    drive_for_ms(g, 200);
    harness_gateway_close(g);
    g = NULL;
  }

  logs = read_all_run_logs();
  rc |= test_read_file("workspace/memory/state/work_steps.json", &steps);
  rc |= expect(logs && steps,
               "persistence alternative artifacts exist");
  if (logs) {
    rc |= expect(count_lines_with_both(
                     logs, "\"type\":\"action_failed\"",
                     "\"action\":\"bash\"") == 1,
                 "first method fails exactly once");
    rc |= expect(count_lines_with_both(
                     logs, "\"type\":\"action_request\"",
                     "\"action\":\"read_file\"") == 1,
                 "controller selects one different justified method");
    rc |= expect(count_substr(logs, "\"type\":\"work_blocked\"") == 0,
                 "failed first method does not prematurely block work");
    rc |= expect(count_substr(logs, "\"type\":\"work_completed\"") == 1,
                 "verified alternative produces one completion");
  }
  if (steps) {
    rc |= expect_substr(steps, "\"action\":\"bash\"",
                        "ledger retains the failed first method");
    rc |= expect_substr(steps, "\"action\":\"read_file\"",
                        "ledger retains the justified alternative");
  }

  if (g) harness_gateway_close(g);
  free(steps);
  free(logs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* Persistence remains conservative: when the failed attempt establishes a
 * concrete external dead end and the controller has no justified authorized
 * alternative, work_blocked is still the correct terminal decision. */
int work_manager_blocks_when_no_justified_alternative_remains(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *logs = NULL, *tasks = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file(
      "workspace/dgu_admit.json",
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"Submit the record to the required upstream endpoint.\","
      "\"done_when\":\"The upstream endpoint confirms receipt.\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"working\"}}]}\n");
  rc |= test_write_file(
      "workspace/dgu_first_fails.json",
      "{\"calls\":[{\"name\":\"bash\",\"args\":{"
      "\"op\":\"start\",\"cmd\":\"printf 'required endpoint is offline; "
      "operator restoration required\\\\n' >&2; exit 7\"}}]}\n");
  rc |= test_write_file(
      "workspace/dgu_block.json",
      "{\"calls\":[{\"name\":\"work_blocked\",\"args\":{"
      "\"blocker\":\"the required upstream endpoint is unavailable and no "
      "authorized alternative can satisfy the task\","
      "\"needed\":\"the operator must restore the required upstream endpoint\"}}]}\n");
  rc |= test_write_file(
      "workspace/dgu_result_blocked.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"PERSISTENCE_JUSTIFIED_BLOCK\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/dgu_admit.json:"
               "workspace/dgu_first_fails.json:"
               "workspace/dgu_block.json:"
               "workspace/dgu_result_blocked.json", 1);

  g = harness_gateway_init("floofclaw");
  rc |= expect(g != NULL, "conservative block fixture loads");
  if (g) {
    rc |= expect(harness_gateway_publish(
                     g, "tests", "submit through the required endpoint") == 0,
                 "publish conservative block work");
    rc |= expect(drive_until_file_contains(
                     g, "workspace/logs/deliveries.jsonl",
                     "PERSISTENCE_JUSTIFIED_BLOCK", 12000) == 0,
                 "justified blocker reaches terminal narration");
    drive_for_ms(g, 200);
    harness_gateway_close(g);
    g = NULL;
  }

  logs = read_all_run_logs();
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect(logs && tasks, "conservative block artifacts exist");
  if (logs) {
    rc |= expect(count_lines_with_both(
                     logs, "\"type\":\"action_failed\"",
                     "\"action\":\"bash\"") == 1,
                 "dead-end attempt fails exactly once");
    rc |= expect(count_substr(logs, "\"type\":\"work_blocked\"") == 1,
                 "dead end emits one conservative block");
    rc |= expect_substr(
        logs,
        "the required upstream endpoint is unavailable and no authorized "
        "alternative can satisfy the task",
        "block records why no justified next step remains");
    rc |= expect_substr(
        logs, "the operator must restore the required upstream endpoint",
        "block records what must change before progress resumes");
    rc |= expect(count_lines_with_both(
                     logs, "\"type\":\"action_request\"",
                     "\"action\":\"read_file\"") == 0,
                 "controller does not invent an irrelevant alternative");
    rc |= expect(count_substr(logs, "\"type\":\"work_completed\"") == 0,
                 "dead end is never reported complete");
  }
  if (tasks)
    rc |= expect_substr(tasks, "\"status\":\"blocked\"",
                        "task projection remains conservatively blocked");

  if (g) harness_gateway_close(g);
  free(tasks);
  free(logs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int work_consequence_routes_intermediate_and_terminal_results(void) {
  HarnessGateway *g = NULL;
  char wake_id[BUS_ID_MAX] = "";
  char *logs = NULL, *deliveries = NULL, *steps = NULL;
  char *controller_inputs = NULL, *result_inputs = NULL;
  int rc = 0;

  /* One gateway proves that ordinary chat and review turns cannot wake an
   * older work task, then drives two synchronous consequences and one
   * terminal consequence through the real bus/intake loop. */
  rc |= fx_reset();
  rc |= setup_a4_actions();
  rc |= write_a4_floop("a4flow");
  g = harness_gateway_init("a4flow");
  rc |= expect(g != NULL, "A4 consequence-routing floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "hello without work") == 0,
               "publish message-only A4 turn");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/runs/run_001/event_log.jsonl",
                   "\"type\":\"run_done\"", 8000) == 0,
               "message-only turn retires");
  rc |= expect(bus_publish(
                   "tests", "affair_review",
                   "{\"affair_id\":\"affair_a4\","
                   "\"text\":\"review without work\"}",
                   wake_id, sizeof(wake_id)) == 0,
               "publish review-without-work A4 turn");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/runs/run_002/event_log.jsonl",
                   "\"type\":\"run_done\"", 8000) == 0,
               "review-without-work turn retires");
  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"source\":\"a4_controller\"") == 0,
               "message and review without work invoke controller zero times");
  free(logs);
  logs = NULL;

  rc |= expect(harness_gateway_publish(g, "tests", "do A4 two steps") == 0,
               "publish two-step A4 work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "A4 terminal handled", 12000) == 0,
               "terminal outcome reaches result manager");
  drive_for_ms(g, 300);
  harness_gateway_close(g);
  g = NULL;

  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= test_read_file("workspace/memory/state/work_steps.json", &steps);
  controller_inputs =
      read_all_agent_artifacts_with_suffix("_work.select.input.json");
  debug_dump_write("a4_happy_logs.jsonl", logs);
  debug_dump_write("a4_happy_steps.json", steps);
  debug_dump_write("a4_happy_controller_inputs.jsonl", controller_inputs);
  rc |= expect(logs && deliveries && steps && controller_inputs,
               "A4 durable proof artifacts exist");
  if (logs) {
    rc |= expect(count_substr(logs, "\"source\":\"a4_controller\"") == 3,
                 "controller runs initially and after each intermediate result");
    rc |= expect(count_substr(logs, "\"source\":\"a4_result\"") == 1,
                 "result manager runs exactly once after terminal outcome");
    rc |= expect(count_substr(logs, "\"type\":\"work_step_result\"") == 2,
                 "two correlated synchronous step results enter new runs");
    rc |= expect(count_substr(logs, "\"type\":\"work_completed\"") == 1,
                 "work completion is one durable terminal source");
    rc |= expect(count_substr(logs, "\"type\":\"work_outcome\"") == 1,
                 "terminal work outcome is intaken exactly once");
    rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                           "happy-path consequence routing has no failed run");
  }
  if (deliveries)
    rc |= expect(count_substr(deliveries, "A4 terminal handled") == 1,
                 "intermediate steps deliver no premature user message");
  if (steps) {
    rc |= expect_substr(steps, "\"action\":\"a4_note\"",
                        "ledger records first synchronous selection");
    rc |= expect_substr(steps, "\"action\":\"a4_check\"",
                        "ledger records second synchronous selection");
    rc |= expect_substr(steps, "first durable note",
                        "ledger keeps first exact result evidence");
    rc |= expect_substr(steps, "second durable check",
                        "ledger keeps second exact result evidence");
  }
  if (controller_inputs) {
    rc |= expect_substr(controller_inputs, "\"action\":\"a4_note\"",
                        "later controller input projects first prior step");
    rc |= expect_substr(controller_inputs, "\"action\":\"a4_check\"",
                        "third controller input projects second prior step");
    rc |= expect(count_substr(controller_inputs,
                              "\"result_event_id\"") >= 3,
                 "controller inputs accumulate bounded prior-result references");
  }
  rc |= expect(count_provider_request_artifacts() == 0,
               "A4 script fixture creates no provider request");
  free(controller_inputs);
  free(steps);
  free(deliveries);
  free(logs);

  /* Blocking is a terminal outcome too: one controller turn, one result
   * turn, and one delivery without an intermediate-result detour. */
  controller_inputs = steps = deliveries = logs = NULL;
  rc |= fx_reset();
  rc |= setup_a4_actions();
  rc |= write_a4_floop("a4flow");
  g = harness_gateway_init("a4flow");
  rc |= expect(g != NULL, "A4 blocking fixture loads");
  if (!g) return 1;
  rc |= expect(bus_publish(
                   "tests", "affair_review",
                   "{\"affair_id\":\"affair_a4_block\","
                   "\"text\":\"block A4 work\"}",
                   wake_id, sizeof(wake_id)) == 0,
               "publish affair-correlated blocked A4 work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "A4 terminal handled", 8000) == 0,
               "blocked outcome reaches one result turn");
  harness_gateway_close(g);
  g = NULL;
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  result_inputs =
      read_all_agent_artifacts_with_suffix("_result.input.json");
  rc |= expect(count_substr(logs, "\"source\":\"a4_controller\"") == 1,
               "blocked work invokes controller exactly once");
  rc |= expect(count_substr(logs, "\"type\":\"work_blocked\"") == 1,
               "blocked work emits one terminal source");
  rc |= expect(count_substr(logs, "\"source\":\"a4_result\"") == 1,
               "blocked work invokes result manager exactly once");
  rc |= expect(deliveries &&
               count_substr(deliveries, "A4 terminal handled") == 1,
               "blocked terminal narration is delivered once");
  rc |= expect_substr(result_inputs,
                      "\"affair_id\":\"affair_a4_block\"",
                      "terminal wake preserves exact affair correlation");
  rc |= expect_substr(result_inputs,
                      "\"blocker\":\"calendar id is missing\"",
                      "terminal wake projects the concrete blocker");
  rc |= expect_substr(result_inputs,
                      "\"needed\":\"provide the calendar id\"",
                      "terminal wake projects the exact unblocking input");
  free(result_inputs);
  free(deliveries);
  free(logs);
  result_inputs = NULL;

  /* A controller-selected core message is an intentional work action, not
   * an accidental result-manager shortcut. Its runtime-synchronous terminal
   * publishes one correlated step wake, which permits a later semantic
   * completion. Both the explicit update and the final narration deliver
   * exactly once. */
  deliveries = logs = NULL;
  rc |= fx_reset();
  rc |= setup_a4_actions();
  rc |= write_a4_floop("a4flow");
  g = harness_gateway_init("a4flow");
  rc |= expect(g != NULL, "A4 intrinsic-message fixture loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "message A4 work") == 0,
               "publish intrinsic-message A4 work");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "A4 terminal handled", 8000) == 0,
               "intrinsic message result permits later completion");
  drive_for_ms(g, 200);
  harness_gateway_close(g);
  g = NULL;
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(count_substr(logs, "\"source\":\"a4_controller\"") == 2,
               "message work invokes controller before and after its result");
  rc |= expect(count_lines_with_both(
                   logs, "\"type\":\"work_step_result\"",
                   "\"action\":\"message\"") == 1,
               "runtime-synchronous message returns one correlated step wake");
  rc |= expect(count_substr(logs, "\"type\":\"work_completed\"") == 1 &&
               count_substr(logs, "\"source\":\"a4_result\"") == 1,
               "message work reaches one terminal result-manager turn");
  rc |= expect(deliveries &&
               count_substr(deliveries, "A4 explicit intermediate") == 1 &&
               count_substr(deliveries, "A4 terminal handled") == 1,
               "explicit intermediate and final narration each deliver once");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "intrinsic-message work completes without failure");
  free(deliveries);
  free(logs);

  /* The selector itself distinguishes two distinct current-run task
   * consequences before any agent can be invoked. Repeated revisions of one
   * task are deliberately deduplicated elsewhere; these are separate ids. */
  {
    RtContext direct;
    char selected[RT_SMALL] = "", trigger[RT_SMALL] = "";
    long long rev = 0;
    rc |= fx_reset();
    rc |= expect(a4_direct_context(&direct, "run_a4_multiple") == 0,
                 "initialize direct multiple-consequence context");
    rc |= expect(a4_append_direct_work(
                     &direct, "task_a4_first", "first distinct work") == 0 &&
                 a4_append_direct_work(
                     &direct, "task_a4_second", "second distinct work") == 0,
                 "append two distinct work consequences");
    rc |= expect(rt_task_select_work_consequence(
                     &direct, selected, sizeof(selected), &rev,
                     trigger, sizeof(trigger)) == 2,
                 "multiple distinct work consequences return loud ambiguity");
    rc |= expect(selected[0] == '\0' && rev == 0 && trigger[0] == '\0',
                 "ambiguous selector exposes no first-task binding");
  }

  rc |= test_reset_workspace();
  return rc;
}

int work_controller_keeps_managed_actions_ordinary_and_stale_results_inert(void) {
  RtContext ctx, result_ctx;
  RtStep step;
  RtAgentMeta meta;
  char *normalized = NULL, *payload = NULL;
  char source_event_id[RT_SMALL];
  char selected[RT_SMALL] = "", trigger[RT_SMALL] = "";
  char *logs = NULL, *steps = NULL;
  long long rev = 0;
  int rc = 0;

  normalized = (char *)calloc(1, RT_XL);
  payload = (char *)calloc(1, RT_XL);
  rc |= expect(normalized && payload,
               "allocate managed-controller large JSON fixtures");
  if (!normalized || !payload) {
    free(payload);
    free(normalized);
    return 1;
  }
  rc |= fx_reset();
  rc |= setup_a4_actions();
  rc |= expect(a4_direct_context(&ctx, "run_a4_managed") == 0 &&
               a4_append_direct_work(
                   &ctx, "task_a4_managed", "managed work") == 0,
               "create managed-action consequence fixture");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "work.select");
  snprintf(step.target, sizeof(step.target), "a4_controller");
  snprintf(step.gate, sizeof(step.gate), "work_consequence");
  memset(&meta, 0, sizeof(meta));
  snprintf(meta.id, sizeof(meta.id), "a4_controller");
  meta.bind_task_open_work = 1;

  rc |= expect(rt_agent_normalize_output(
                   &ctx, &step, &meta, "task_a4_managed", 1,
                   "{\"calls\":[{\"name\":\"manage_codex\","
                   "\"args\":{\"op\":\"start\",\"task\":\"first\"}}]}",
                   normalized, RT_XL) == 0,
               "manage_codex is an ordinary valid first controller choice");
  rc |= expect_substr(normalized, "\"action\":\"manage_codex\"",
                      "first managed choice keeps registered action name");
  rc |= expect_substr(normalized,
                      "\"trigger_event_id\":\"evt_run_a4_managed_000001\"",
                      "first managed choice receives exact consequence trigger");

  rc |= expect(rt_append_event_format(
                   &ctx, "action_request", "a4_controller",
                   payload, RT_XL, 0,
                   "{\"request_id\":\"managed_first\","
                   "\"action\":\"manage_codex\","
                   "\"args\":{\"op\":\"start\",\"task\":\"first\"},"
                   "\"task_id\":\"task_a4_managed\",\"work_rev\":1,"
                   "\"context_id\":\"chat:tests\","
                   "\"trigger_event_id\":\"evt_run_a4_managed_000001\"}") == 0,
               "record first managed semantic selection");
  rc |= expect(rt_next_event_id(
                   &ctx, source_event_id, sizeof(source_event_id)) == 0,
               "reserve managed result source id");
  snprintf(payload, RT_XL,
           "{\"handle\":\"managed_a4_001\","
           "\"action\":\"manage_codex\",\"request_id\":\"managed_first\","
           "\"trigger_event_id\":\"evt_run_a4_managed_000001\","
           "\"task_id\":\"task_a4_managed\","
           "\"context_id\":\"chat:tests\",\"channel\":\"tests\","
           "\"adapter_id\":\"\",\"origin_event_id\":\"bus_managed\","
           "\"ref\":null,\"work_rev\":1,\"source_event_id\":\"%s\","
           "\"text\":\"first managed result\"}",
           source_event_id);
  rc |= expect(rt_append_event_sync(
                   &ctx, "operation_result", "kernel", payload) == 0,
               "record exact managed intermediate result");

  result_ctx = ctx;
  snprintf(result_ctx.event_kind, sizeof(result_ctx.event_kind),
           "operation_result");
  snprintf(result_ctx.event_payload_json,
           sizeof(result_ctx.event_payload_json),
           "{\"handle\":\"managed_a4_001\","
           "\"action\":\"manage_codex\",\"request_id\":\"managed_first\","
           "\"trigger_event_id\":\"evt_run_a4_managed_000001\","
           "\"task_id\":\"task_a4_managed\",\"work_rev\":1,"
           "\"source_event_id\":\"%s\",\"text\":\"first managed result\"}",
           source_event_id);
  rc |= expect(rt_task_select_work_consequence(
                   &result_ctx, selected, sizeof(selected), &rev,
                   trigger, sizeof(trigger)) == 0 &&
               strcmp(selected, "task_a4_managed") == 0 && rev == 1 &&
               strcmp(trigger, source_event_id) == 0,
               "managed result is an exact later controller consequence");
  rc |= expect(rt_agent_normalize_output(
                   &result_ctx, &step, &meta, "task_a4_managed", 1,
                   "{\"calls\":[{\"name\":\"manage_codex\","
                   "\"args\":{\"op\":\"start\",\"task\":\"later\"}}]}",
                   normalized, RT_XL) == 0,
               "manage_codex is also an ordinary valid later choice");
  rc |= expect_substr(normalized, source_event_id,
                      "later managed choice binds exact result trigger");

  rc |= expect(rt_task_apply_event(
                   "task_updated",
                   "{\"task_id\":\"task_a4_managed\","
                   "\"state\":{\"status\":\"open\","
                   "\"work\":\"new revision\",\"done_when\":\"new done\","
                   "\"updated_ms\":2000}}") == 0,
               "revise work while old managed result exists");
  selected[0] = trigger[0] = '\0';
  rev = 0;
  rc |= expect(rt_task_select_work_consequence(
                   &result_ctx, selected, sizeof(selected), &rev,
                   trigger, sizeof(trigger)) == 3,
               "old managed result cannot select newer work revision");
  rc |= test_read_file("workspace/memory/state/work_steps.json", &steps);
  rc |= test_read_file(
      "workspace/runs/run_a4_managed/event_log.jsonl", &logs);
  rc |= expect_substr(steps, "first managed result",
                      "stale prior-revision result remains ledger truth");
  rc |= expect_substr(logs, "\"type\":\"operation_result\"",
                      "stale prior-revision result remains event-log truth");
  free(logs);
  free(steps);
  logs = steps = NULL;

  free(payload);
  free(normalized);
  rc |= test_reset_workspace();
  return rc;
}

int work_publication_recovers_once_after_source_append_crash(void) {
  RtContext ctx;
  HarnessGateway *g = NULL;
  char payload[RT_XL], wake[4096], source_event_id[RT_SMALL];
  char *logs = NULL, *deliveries = NULL;
  const char *marker = "workspace/a4_source_appended";
  pid_t child = -1;
  int status = 0;
  int rc = 0;

  rc |= fx_reset();
  rc |= setup_a4_actions();
  rc |= write_a4_floop("a4flow");
  rc |= expect(a4_direct_context(&ctx, "run_001") == 0 &&
               a4_append_direct_work(
                   &ctx, "task_a4_crash", "recover one wake") == 0,
               "create crash-recovery work consequence");
  snprintf(ctx.origin_event_id, sizeof(ctx.origin_event_id), "bus_crash");
  snprintf(ctx.origin_channel, sizeof(ctx.origin_channel), "tests");
  snprintf(ctx.origin_ref_json, sizeof(ctx.origin_ref_json), "null");
  rc |= expect(rt_append_event_format(
                   &ctx, "action_request", "a4_controller",
                   payload, sizeof(payload), 0,
                   "{\"request_id\":\"crash_selected\","
                   "\"action\":\"a4_note\","
                   "\"args\":{\"label\":\"before crash\"},"
                   "\"task_id\":\"task_a4_crash\",\"work_rev\":1,"
                   "\"context_id\":\"chat:tests\","
                   "\"trigger_event_id\":\"evt_run_001_000001\"}") == 0,
               "record pre-crash semantic selection");
  rc |= expect(rt_next_event_id(
                   &ctx, source_event_id, sizeof(source_event_id)) == 0,
               "reserve exact pre-crash source id");
  snprintf(payload, sizeof(payload),
           "{\"request_id\":\"crash_selected\",\"action\":\"a4_note\","
           "\"run_id\":\"run_001\",\"job_id\":null,"
           "\"task_id\":\"task_a4_crash\","
           "\"result\":{\"text\":\"source survived SIGKILL\"},"
           "\"error\":null,\"work_rev\":1,"
           "\"context_id\":\"chat:tests\","
           "\"trigger_event_id\":\"evt_run_001_000001\","
           "\"source_event_id\":\"%s\"}",
           source_event_id);
  snprintf(wake, sizeof(wake),
           "{\"task_id\":\"task_a4_crash\",\"work_rev\":1,"
           "\"request_id\":\"crash_selected\",\"action\":\"a4_note\","
           "\"source_event_id\":\"%s\",\"status\":\"succeeded\","
           "\"detail\":\"source survived SIGKILL\","
           "\"text\":\"source survived SIGKILL\","
           "\"origin_event_id\":\"bus_crash\",\"adapter_id\":\"\","
           "\"ref\":null}",
           source_event_id);
  child = fork();
  rc |= expect(child >= 0, "fork source-append crash fixture");
  if (child == 0) {
    if (rt_publication_outbox_prepare(
            &ctx, source_event_id, "action_succeeded", payload,
            "tests", "work_step_result", wake, NULL, 0) != 0)
      _exit(21);
    if (rt_append_event_sync(
            &ctx, "action_succeeded", "action_runner", payload) != 0)
      _exit(22);
    if (test_write_file(marker, "source appended\n") != 0)
      _exit(23);
    for (;;) pause();
  }
  if (child > 0) {
    rc |= expect(wait_until_file_contains(
                     marker, "source appended", 5000) == 0,
                 "child reaches source-appended publication gap");
    if (kill(child, SIGKILL) != 0) {
      rc |= expect(0, "SIGKILL source-appended child");
    }
    rc |= expect(waitpid(child, &status, 0) == child &&
                 WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
                 "source producer is killed at exact publication gap");
    child = -1;
  }
  rc |= expect(count_regular_entries("workspace/bus/outbox") == 1,
               "prepared wake survives killed producer");
  rc |= expect(count_regular_entries("workspace/bus/inbox") == 0,
               "killed producer published no wake before restart");

  g = harness_gateway_init("a4flow");
  rc |= expect(g != NULL, "gateway restarts with prepared source wake");
  if (!g) return 1;
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "A4 terminal handled", 12000) == 0,
               "restart reconciles source and completes work");
  drive_for_ms(g, 300);
  harness_gateway_close(g);
  g = NULL;
  g = harness_gateway_init("a4flow");
  rc |= expect(g != NULL, "gateway restarts again after claimed wake");
  if (g) {
    drive_for_ms(g, 300);
    harness_gateway_close(g);
    g = NULL;
  }

  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(count_lines_with_both(
                   logs, "\"type\":\"work_step_result\"",
                   "\"request_id\":\"crash_selected\"") == 1,
               "crash source has exactly one inbound wake copy");
  rc |= expect(count_substr(logs,
                            "\"type\":\"work_step_result\"") == 2,
               "recovered first result plus later synchronous result each intake once");
  rc |= expect(count_substr(logs, "\"source\":\"a4_result\"") == 1,
               "crash recovery reaches one terminal result-manager turn");
  rc |= expect(deliveries &&
               count_substr(deliveries, "A4 terminal handled") == 1,
               "crash recovery produces one terminal user delivery");
  rc |= expect(count_regular_entries("workspace/bus/outbox") == 0,
               "terminal result claims clear all publication records");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "source-append crash recovery has no failed run");
  free(deliveries);
  free(logs);
  if (child > 0) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
  }
  rc |= test_reset_workspace();
  return rc;
}

int work_publication_pending_claim_resumes_same_run(void) {
  RtContext ctx;
  /* Heap-owned: an RtRun is ~190KB; the small-stack gate forbids
   * stacking it beside the context and payload buffers. */
  RtRun *claim = NULL;
  RtProfile profile;
  HarnessGateway *g = NULL;
  char payload[RT_XL], wake[4096], source_event_id[RT_SMALL];
  char wake_id[BUS_ID_MAX] = "";
  char inbox_path[PATH_MAX], processed_path[PATH_MAX];
  char newer_processed_path[PATH_MAX];
  char *run_two = NULL, *logs = NULL, *deliveries = NULL;
  int rc = 0;

  claim = (RtRun *)calloc(1, sizeof(*claim));
  if (!claim) return 1;

  /* A byte-for-byte matching first event is still a conflicting claim when
   * its event source is not the validated envelope channel. */
  rc |= fx_reset();
  memset(claim, 0, sizeof(*claim));
  snprintf(claim->ctx.workspace, sizeof(claim->ctx.workspace), "workspace");
  snprintf(claim->ctx.run_id, sizeof(claim->ctx.run_id),
           "run_a4_wrong_source");
  snprintf(claim->ctx.run_dir, sizeof(claim->ctx.run_dir),
           "workspace/runs/run_a4_wrong_source");
  snprintf(claim->ctx.origin_channel, sizeof(claim->ctx.origin_channel),
           "tests");
  snprintf(claim->ctx.event_payload_json,
           sizeof(claim->ctx.event_payload_json),
           "{\"task_id\":\"task_wrong_source\",\"work_rev\":1,"
           "\"request_id\":\"wrong_source\",\"action\":\"a4_note\","
           "\"source_event_id\":\"evt_source_wrong\"}");
  snprintf(claim->created_from_event_id,
           sizeof(claim->created_from_event_id), "bus_wrong_source");
  snprintf(claim->created_from_type, sizeof(claim->created_from_type),
           "work_step_result");
  snprintf(wake, sizeof(wake),
           "{\"event_id\":\"evt_run_a4_wrong_source_000001\","
           "\"ts\":\"2026-07-26T00:00:00Z\","
           "\"type\":\"work_step_result\",\"source\":\"forged\","
           "\"run_id\":\"run_a4_wrong_source\",\"payload\":%s}\n",
           claim->ctx.event_payload_json);
  snprintf(inbox_path, sizeof(inbox_path), "%s/event_log.jsonl",
           claim->ctx.run_dir);
  rc |= expect(test_mkdir_p(claim->ctx.run_dir) == 0 &&
               test_write_file(inbox_path, wake) == 0,
               "write exact first inbound with wrong source");
  rc |= expect(rt_ports_inbound_commit_state(claim) == -1,
               "wrong-source first inbound is a durable claim conflict");

  /* Retention must pin the claimed result run as well as the source run.
   * The runstates below carry only the real serializer key, so the pin —
   * not a stale field the janitor could not read — is what keeps the
   * claimed run alive under retention pressure. */
  rc |= fx_reset();
  rc |= expect(a4_direct_context(&ctx, "source_a4_prune") == 0,
               "initialize terminal result-run prune fixture");
  rc |= expect(rt_next_event_id(
                   &ctx, source_event_id, sizeof(source_event_id)) == 0,
               "reserve prune-fixture source id");
  rc |= expect(rt_publication_outbox_prepare(
                   &ctx, source_event_id, "action_succeeded",
                   "{\"context_id\":\"chat:tests\"}",
                   "tests", "work_step_result", "{}",
                   wake_id, sizeof(wake_id)) == 0,
               "prepare prune-fixture wake");
  rc |= expect(rt_append_event_sync(
                   &ctx, "action_succeeded", "action_runner",
                   "{\"context_id\":\"chat:tests\"}") == 0,
               "commit prune-fixture source");
  rc |= expect(rt_publication_outbox_reconcile() == 0,
               "publish prune-fixture wake from exact source");
  snprintf(inbox_path, sizeof(inbox_path),
           "workspace/bus/inbox/%s.json", wake_id);
  snprintf(processed_path, sizeof(processed_path),
           "workspace/bus/processed/%s.json", wake_id);
  rc |= expect(test_mkdir_p("workspace/bus/processed") == 0 &&
               rename(inbox_path, processed_path) == 0,
               "claim prune-fixture wake as processed");
  rc |= expect(test_mkdir_p("workspace/runs/run_001") == 0,
               "create claimed terminal result run");
  snprintf(payload, sizeof(payload),
           "{\"event_id\":\"evt_run_001_000001\","
           "\"ts\":\"2026-07-26T00:00:00Z\","
           "\"type\":\"work_step_result\",\"source\":\"tests\","
           "\"run_id\":\"run_001\",\"payload\":{}}\n");
  rc |= expect(test_write_file(
                   "workspace/runs/run_001/event_log.jsonl", payload) == 0,
               "commit exact first event in terminal result run");
  snprintf(payload, sizeof(payload),
           "{\"run_id\":\"run_001\",\"context_id\":\"chat:tests\","
           "\"profile\":\"a4flow\",\"status\":\"done\","
           "\"advance\":0,\"created_from\":{\"event_id\":\"%s\","
           "\"type\":\"work_step_result\"}}\n",
           wake_id);
  rc |= expect(test_write_file(
                   "workspace/runs/run_001/runstate.json", payload) == 0 &&
               test_mkdir_p("workspace/runs/run_002") == 0 &&
               test_write_file(
                   "workspace/runs/run_002/runstate.json",
                   "{\"run_id\":\"run_002\",\"status\":\"done\","
                   "\"created_from\":{"
                   "\"event_id\":\"bus_unrelated\",\"type\":\"tests\"}}\n") == 0,
               "create pinned older and unpinned newer terminal runs");
  rc |= expect(rt_publication_outbox_run_pending("run_001") == 1,
               "outbox identifies claimed result run as pinned");
  rc |= expect(fc_janitor_test_runs_prune(
                   "workspace/runs", 1) == 0,
               "run janitor under keep-one pressure");
  rc |= expect_file_exists("workspace/runs/run_001/runstate.json");
  rc |= expect_file_not_exists("workspace/runs/run_002");

  rc |= fx_reset();
  rc |= setup_a4_actions();
  rc |= write_a4_floop("a4flow");
  rc |= expect(a4_direct_context(&ctx, "run_001") == 0 &&
               a4_append_direct_work(
                   &ctx, "task_a4_pending", "resume pending claim") == 0,
               "create pending-claim work consequence");
  rc |= expect(rt_append_event_format(
                   &ctx, "action_request", "a4_controller",
                   payload, sizeof(payload), 0,
                   "{\"request_id\":\"pending_selected\","
                   "\"action\":\"a4_note\","
                   "\"args\":{\"label\":\"pending\"},"
                   "\"task_id\":\"task_a4_pending\",\"work_rev\":1,"
                   "\"context_id\":\"chat:tests\","
                   "\"trigger_event_id\":\"evt_run_001_000001\"}") == 0,
               "record pending-claim semantic selection");
  rc |= expect(rt_next_event_id(
                   &ctx, source_event_id, sizeof(source_event_id)) == 0,
               "reserve pending-claim source id");
  snprintf(payload, sizeof(payload),
           "{\"request_id\":\"pending_selected\",\"action\":\"a4_note\","
           "\"run_id\":\"run_001\",\"job_id\":null,"
           "\"task_id\":\"task_a4_pending\","
           "\"result\":{\"text\":\"pending source\"},\"error\":null,"
           "\"work_rev\":1,\"context_id\":\"chat:tests\","
           "\"trigger_event_id\":\"evt_run_001_000001\","
           "\"source_event_id\":\"%s\"}",
           source_event_id);
  snprintf(wake, sizeof(wake),
           "{\"task_id\":\"task_a4_pending\",\"work_rev\":1,"
           "\"request_id\":\"pending_selected\",\"action\":\"a4_note\","
           "\"source_event_id\":\"%s\",\"status\":\"succeeded\","
           "\"detail\":\"pending source\",\"text\":\"pending source\","
           "\"origin_event_id\":\"bus_pending\",\"adapter_id\":\"\","
           "\"ref\":null}",
           source_event_id);
  rc |= expect(rt_publication_outbox_prepare(
                   &ctx, source_event_id, "action_succeeded", payload,
                   "tests", "work_step_result", wake,
                   wake_id, sizeof(wake_id)) == 0,
               "prepare pending-claim wake");
  rc |= expect(rt_append_event_sync(
                   &ctx, "action_succeeded", "action_runner", payload) == 0,
               "commit pending-claim source");
  rc |= expect(rt_publication_outbox_reconcile() == 0,
               "publish pending-claim wake");
  snprintf(inbox_path, sizeof(inbox_path),
           "workspace/bus/inbox/%s.json", wake_id);
  snprintf(processed_path, sizeof(processed_path),
           "workspace/bus/processed/%s.json", wake_id);
  rc |= expect(test_mkdir_p("workspace/bus/processed") == 0 &&
               rename(inbox_path, processed_path) == 0,
               "simulate intake rename before durable first event");
  snprintf(newer_processed_path, sizeof(newer_processed_path),
           "workspace/bus/processed/bus_999999.json");
  rc |= expect(test_write_file(newer_processed_path, "{}\n") == 0,
               "add newer processed envelope for retention pressure");
  rc |= expect(fc_janitor_test_bus_processed_prune(
                   "workspace/bus/processed", 1) == 0,
               "processed janitor runs with keep one");
  rc |= expect_file_exists(processed_path);
  rc |= expect_file_exists(newer_processed_path);

  memset(claim, 0, sizeof(*claim));
  memset(&profile, 0, sizeof(profile));
  snprintf(profile.name, sizeof(profile.name), "a4flow");
  claim->profile = &profile;
  snprintf(claim->ctx.workspace, sizeof(claim->ctx.workspace), "workspace");
  snprintf(claim->ctx.run_id, sizeof(claim->ctx.run_id), "run_002");
  snprintf(claim->ctx.run_dir, sizeof(claim->ctx.run_dir),
           "workspace/runs/run_002");
  snprintf(claim->ctx.context_id, sizeof(claim->ctx.context_id), "chat:tests");
  snprintf(claim->context_id, sizeof(claim->context_id), "chat:tests");
  snprintf(claim->created_from_event_id,
           sizeof(claim->created_from_event_id), "%s", wake_id);
  snprintf(claim->created_from_type, sizeof(claim->created_from_type),
           "work_step_result");
  claim->state = RT_RUN_WAITING_EVENT;
  claim->ctx.next_event = 1;
  rc |= expect(test_mkdir_p(claim->ctx.run_dir) == 0 &&
               test_mkdir_p(
                   "workspace/runs/run_002/agent_outputs") == 0 &&
               test_mkdir_p(
                   "workspace/runs/run_002/action_calls") == 0 &&
               rt_run_persist_state(claim) == 0,
               "persist pending exact result-run claim");
  rc |= expect_file_not_exists(
      "workspace/runs/run_002/event_log.jsonl");

  g = harness_gateway_init("a4flow");
  rc |= expect(g != NULL, "gateway restores pending result-run claim");
  if (!g) {
    free(claim);
    return 1;
  }
  rc |= test_read_file("workspace/runs/run_002/event_log.jsonl", &run_two);
  rc |= expect(run_two &&
               count_lines_with_both(
                   run_two, "\"event_id\":\"evt_run_002_000001\"",
                   "\"request_id\":\"pending_selected\"") == 1,
               "startup recommits exact first inbound in claimed run_002");
  rc |= expect(count_regular_entries("workspace/bus/outbox") == 1,
               "active result run retains its source publication record");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "A4 terminal handled", 12000) == 0,
               "pending claim resumes through terminal result");
  drive_for_ms(g, 300);
  harness_gateway_close(g);
  g = NULL;

  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(count_lines_with_both(
                   logs, "\"type\":\"work_step_result\"",
                   "\"request_id\":\"pending_selected\"") == 1,
               "pending wake has one exact claim and no duplicate run");
  rc |= expect(count_regular_entries("workspace/bus/outbox") == 0,
               "terminal claim releases publication pin");
  rc |= expect(deliveries &&
               count_substr(deliveries, "A4 terminal handled") == 1,
               "resumed pending claim produces one terminal delivery");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "pending exact claim resumes without failure");
  free(deliveries);
  free(logs);
  free(run_two);
  free(claim);
  rc |= test_reset_workspace();
  return rc;
}

int publication_outbox_rejects_conflicting_sources_and_enforces_cap(void) {
  RtContext ctx;
  char source_id[RT_SMALL], wake_id[BUS_ID_MAX];
  char payload[RT_LARGE], wake[RT_LARGE], path[PATH_MAX], record[2048];
  int rc = 0;

  /* A source id is owned by exactly one complete event in the named run.
   * A top-level run mismatch and a duplicate id are both durable conflicts,
   * never permission to publish. */
  rc |= fx_reset();
  rc |= expect(a4_direct_context(
                   &ctx, "run_a4_source_conflict") == 0,
               "initialize source run-id conflict fixture");
  snprintf(source_id, sizeof(source_id),
           "evt_run_a4_source_conflict_000001");
  snprintf(payload, sizeof(payload),
           "{\"task_id\":\"task_source_conflict\",\"work_rev\":1,"
           "\"request_id\":\"source_conflict\","
           "\"action\":\"a4_note\",\"context_id\":\"chat:tests\","
           "\"trigger_event_id\":\"trigger_source\","
           "\"source_event_id\":\"%s\",\"result\":{}}",
           source_id);
  snprintf(wake, sizeof(wake),
           "{\"task_id\":\"task_source_conflict\",\"work_rev\":1,"
           "\"request_id\":\"source_conflict\","
           "\"action\":\"a4_note\",\"source_event_id\":\"%s\"}",
           source_id);
  rc |= expect(rt_publication_outbox_prepare(
                   &ctx, source_id, "action_succeeded", payload,
                   "tests", "work_step_result", wake,
                   wake_id, sizeof(wake_id)) == 0,
               "prepare run-id conflict publication");
  snprintf(record, sizeof(record),
           "{\"event_id\":\"%s\",\"ts\":\"2026-07-26T00:00:00Z\","
           "\"type\":\"action_succeeded\",\"source\":\"fixture\","
           "\"run_id\":\"run_wrong\",\"payload\":%s}\n",
           source_id, payload);
  snprintf(path, sizeof(path), "%s/event_log.jsonl", ctx.run_dir);
  rc |= expect(test_write_file(path, record) == 0,
               "write same source id under wrong top-level run");
  rc |= expect(rt_publication_outbox_reconcile() != 0,
               "wrong source run id fails reconciliation loudly");
  rc |= expect(count_regular_entries("workspace/bus/outbox") == 1 &&
               count_regular_entries("workspace/bus/inbox") == 0,
               "run-id conflict is retained and never published");
  rc |= expect(rt_publication_outbox_validate(
                   wake_id, "tests", "work_step_result", wake) != 1,
               "run-id conflict cannot validate as internal wake");

  rc |= fx_reset();
  rc |= expect(a4_direct_context(&ctx, "run_a4_source_duplicate") == 0,
               "initialize duplicate-source fixture");
  snprintf(source_id, sizeof(source_id),
           "evt_run_a4_source_duplicate_000001");
  snprintf(payload, sizeof(payload),
           "{\"task_id\":\"task_source_duplicate\",\"work_rev\":1,"
           "\"request_id\":\"source_duplicate\","
           "\"action\":\"a4_note\",\"context_id\":\"chat:tests\","
           "\"trigger_event_id\":\"trigger_source\","
           "\"source_event_id\":\"%s\",\"result\":{}}",
           source_id);
  snprintf(wake, sizeof(wake),
           "{\"task_id\":\"task_source_duplicate\",\"work_rev\":1,"
           "\"request_id\":\"source_duplicate\","
           "\"action\":\"a4_note\",\"source_event_id\":\"%s\"}",
           source_id);
  rc |= expect(rt_publication_outbox_prepare(
                   &ctx, source_id, "action_succeeded", payload,
                   "tests", "work_step_result", wake,
                   wake_id, sizeof(wake_id)) == 0,
               "prepare duplicate-source publication");
  snprintf(record, sizeof(record),
           "{\"event_id\":\"%s\",\"ts\":\"2026-07-26T00:00:00Z\","
           "\"type\":\"action_succeeded\",\"source\":\"fixture\","
           "\"run_id\":\"run_a4_source_duplicate\",\"payload\":%s}\n"
           "{\"event_id\":\"%s\",\"ts\":\"2026-07-26T00:00:01Z\","
           "\"type\":\"action_succeeded\",\"source\":\"fixture\","
           "\"run_id\":\"run_a4_source_duplicate\",\"payload\":%s}\n",
           source_id, payload, source_id, payload);
  snprintf(path, sizeof(path), "%s/event_log.jsonl", ctx.run_dir);
  rc |= expect(test_write_file(path, record) == 0,
               "write duplicate complete source ids");
  rc |= expect(rt_publication_outbox_reconcile() != 0,
               "duplicate source id fails reconciliation loudly");
  rc |= expect(count_regular_entries("workspace/bus/outbox") == 1 &&
               count_regular_entries("workspace/bus/inbox") == 0,
               "duplicate source is retained and never published");

  /* Irrelevant directory noise does not consume the canonical-record cap.
   * Exactly 1024 canonical records are accepted; the next prepare is loud
   * ENOSPC, and reconciliation still processes the bounded canonical batch. */
  rc |= fx_reset();
  rc |= expect(test_mkdir_p("workspace/bus/outbox") == 0,
               "create outbox cap fixture directory");
  rc |= test_write_file("workspace/bus/outbox/.partial", "{}\n");
  rc |= test_write_file("workspace/bus/outbox/README", "noise\n");
  rc |= test_write_file(
      "workspace/bus/outbox/bus_not_numeric.json", "{}\n");
  for (int i = 0; i < 1023; ++i) {
    char id[BUS_ID_MAX], run_id[RT_SMALL], event_id[RT_SMALL];
    snprintf(id, sizeof(id), "bus_8%05d", i);
    snprintf(run_id, sizeof(run_id), "run_cap_%04d", i);
    snprintf(event_id, sizeof(event_id), "evt_cap_%04d", i);
    snprintf(path, sizeof(path),
             "workspace/bus/outbox/%s.json", id);
    snprintf(record, sizeof(record),
             "{\"wake_id\":\"%s\",\"source_run_id\":\"%s\","
             "\"source_event_id\":\"%s\","
             "\"source_type\":\"action_succeeded\","
             "\"source_payload\":{},\"channel\":\"tests\","
             "\"wake_type\":\"work_step_result\",\"wake_payload\":{}}\n",
             id, run_id, event_id);
    if (test_write_file(path, record) != 0) {
      rc |= expect(0, "write canonical outbox cap record");
      break;
    }
  }
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.run_id, sizeof(ctx.run_id), "run_cap_source");
  rc |= expect(rt_publication_outbox_prepare(
                   &ctx, "evt_cap_source_1", "action_succeeded",
                   "{\"context_id\":\"chat:tests\"}",
                   "tests", "work_step_result", "{}",
                   wake_id, sizeof(wake_id)) == 0,
               "1024th canonical publication record is accepted");
  errno = 0;
  rc |= expect(rt_publication_outbox_prepare(
                   &ctx, "evt_cap_source_2", "action_succeeded",
                   "{\"context_id\":\"chat:tests\"}",
                   "tests", "work_step_result", "{}",
                   NULL, 0) != 0 && errno == ENOSPC,
               "canonical publication record 1025 is loud ENOSPC");
  rc |= expect(rt_publication_outbox_reconcile() == 0,
               "canonical cap batch reconciles despite irrelevant files");
  rc |= expect(count_regular_entries("workspace/bus/inbox") == 0,
               "absent cap-fixture sources publish no wakes");

  rc |= test_reset_workspace();
  return rc;
}

/* ===== 1. generic lifecycle + opaque payloads ===================== */

static int run_generic_lifecycle(const char *floop, const char *handle_prefix,
                                 const char *wording, const char *statedir) {
  HarnessGateway *g;
  char needle[256];
  char *ops = NULL, *logs = NULL, *tasks = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= setup_opgeneric_floop(floop, strcmp(handle_prefix, "fake1") == 0 ? "fake_one" : "fake_two");
  if (rc == 0) rc |= pause_for_fixture_isolation_probe();
  g = harness_gateway_init(floop);
  rc |= expect(g != NULL, "generic floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "alpha work please") == 0,
               "publish user message");
  /* Full lifecycle: start -> running(worker_handle) -> detached worker
   * submits its claim through the public helper -> one result run.
   * Wait for the store to go terminal. */
  snprintf(needle, sizeof(needle), "\"status\":\"finished\"");
  rc |= expect(drive_until_file_contains(g, "workspace/memory/state/operations.json",
                                         needle, 8000) == 0,
               "operation reaches finished in the store");
  /* Let the published operation_result run retire. */
  rc |= expect(drive_until_file_contains(g, "workspace/memory/state/tasks.json",
                                         "\"result_excerpt\"", 8000) == 0,
               "task external carries the result");
  drive_for_ms(g, 300);
  harness_gateway_close(g);

  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  rc |= expect(ops != NULL, "operation store exists");
  if (ops) {
    snprintf(needle, sizeof(needle), "\"worker_handle\":\"%s_001\"", handle_prefix);
    rc |= expect_substr(ops, needle, "worker handle uses the configured adapter");
    rc |= expect(count_substr(ops, "\"handle\":") == 1, "exactly one operation record");
  }
  logs = read_all_run_logs();
  /* Exactly one driver-published terminal result for the handle. */
  rc |= expect(count_substr(logs, "\"type\":\"operation_started\"") == 1,
               "one operation_started event");
  {
    int result_events = count_substr(logs, "\"type\":\"operation_result\"");
    /* the admitted claim IS the result run's one canonical event */
    rc |= expect(result_events == 1, "one canonical operation_result exists");
  }
  rc |= expect_no_substr(logs, "operation_poll",
                         "no polling artifact appears in any run");
  /* Opaque payload proof: the instruction text reached the handler and
   * its result text reached the event log unchanged. */
  snprintf(needle, sizeof(needle), "%s: update playlist X", wording);
  rc |= expect_substr(logs, needle, "arbitrary payload text survives untouched");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"", "no run fails");
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  if (tasks) {
    rc |= expect_substr(tasks, "\"work_rev\":1", "work task stays at rev 1");
    rc |= expect_substr(tasks, "\"status\":\"open\"",
                        "task remains open after terminal attempt");
  }
  {
    char starts_path[256], *starts = NULL;
    snprintf(starts_path, sizeof(starts_path), "workspace/%s/starts", statedir);
    rc |= test_read_file(starts_path, &starts);
    rc |= expect(starts && strcmp(starts, "1") == 0, "adapter started exactly once");
    free(starts);
  }
  free(tasks);
  free(logs);
  free(ops);
  return rc;
}

int opgeneric_fake_one_full_lifecycle(void) {
  return run_generic_lifecycle("opgeneric", "fake1", "fake_one finished", "fakeop_one");
}

/* Rebinding alpha to fake_two is a one-line config change; the
 * lifecycle, event kinds, and correlation are identical. */
int opgeneric_rebind_fake_two_config_only(void) {
  int rc = run_generic_lifecycle("opgeneric2", "fake2", "fake_two finished", "fakeop_two");
  /* fake_one was never touched by this floop. */
  rc |= expect_file_not_exists("workspace/fakeop_one/starts");
  return rc;
}

/* ===== 2. timer dedup + no restart loop =========================== */

int opgeneric_stale_claim_after_terminal_is_noop(void) {
  HarnessGateway *g;
  char *logs = NULL, *ops = NULL;
  char op_id[RT_SMALL] = "";
  int rc = 0, before_results, before_starts;

  rc |= run_generic_lifecycle("opgeneric", "fake1", "fake_one finished", "fakeop_one");
  logs = read_all_run_logs();
  before_results = count_substr(logs, "\"type\":\"operation_result\"");
  before_starts = count_substr(logs, "\"type\":\"operation_started\"");
  free(logs);
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  if (ops) {
    const char *start = strstr(ops, "\"handle\":\"");
    if (start) {
      const char *end;
      start += strlen("\"handle\":\"");
      end = strchr(start, '"');
      if (end && (size_t)(end - start) < sizeof(op_id)) {
        memcpy(op_id, start, (size_t)(end - start));
        op_id[end - start] = '\0';
      }
    }
    free(ops);
    ops = NULL;
  }
  rc |= expect(op_id[0] != '\0', "terminal operation id is readable");

  g = harness_gateway_init("opgeneric");
  rc |= expect(g != NULL, "gateway restarts on same workspace");
  if (!g) return 1;
  rc |= expect(publish_claim_envelope(op_id, "whatever-token",
                                      "succeeded", "stale duplicate") == 0,
               "stale claim envelope published");
  drive_for_ms(g, 600);
  harness_gateway_close(g);

  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"type\":\"operation_result\"") == before_results,
               "stale claim publishes no second terminal result");
  rc |= expect(count_substr(logs, "\"type\":\"operation_started\"") == before_starts,
               "stale claim starts nothing");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "stale claim leaves no failed run");
  {
    char *rejected = NULL;
    rc |= test_read_file("workspace/logs/rejected_events.jsonl", &rejected);
    rc |= expect(rejected &&
                 strstr(rejected, "operation_already_terminal") != NULL,
                 "stale claim is rejected as already terminal");
    free(rejected);
  }
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  if (ops)
    rc |= expect(count_substr(ops, "\"handle\":") == 1, "store still has one record");
  {
    char *starts = NULL;
    rc |= test_read_file("workspace/fakeop_one/starts", &starts);
    rc |= expect(starts && strcmp(starts, "1") == 0,
                 "adapter never re-invoked to start");
    free(starts);
  }
  free(ops);
  free(logs);
  return rc;
}

int opgeneric_terminal_attempt_never_restarts_itself(void) {
  HarnessGateway *g;
  char *starts = NULL, *logs = NULL;
  int rc = 0;

  rc |= run_generic_lifecycle("opgeneric", "fake1", "fake_one finished", "fakeop_one");
  g = harness_gateway_init("opgeneric");
  rc |= expect(g != NULL, "gateway restarts on same workspace");
  if (!g) return 1;
  /* A contentless follow-up: the manager script emits no calls; the
   * task is open with a terminal attempt at the current revision. The
   * attempt rule must not start anything. */
  rc |= expect(harness_gateway_publish(g, "tests", "thanks!") == 0,
               "publish thanks");
  drive_for_ms(g, 800);
  harness_gateway_close(g);
  rc |= test_read_file("workspace/fakeop_one/starts", &starts);
  rc |= expect(starts && strcmp(starts, "1") == 0,
               "terminal attempt at current rev never restarts");
  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"type\":\"operation_started\"") == 1,
               "no second operation_started");
  free(logs);
  free(starts);
  return rc;
}

/* ===== 3. retry reuses committed calls ============================ */

int opgeneric_retry_reuses_committed_calls(void) {
  HarnessGateway *g;
  char *logs = NULL, *deliveries = NULL, *tasks = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= setup_testfx_actions();
  /* Floop: manage emits work + a user-facing message; a flaky step
   * fails once after they commit; retry rewinds to phase zero. */
  rc |= test_write_file("floops/opflaky/loop.json",
      "{\n"
      "  \"version\": 1,\n"
      "  \"name\": \"opflaky\",\n"
      "  \"description\": \"bounded-retry idempotency fixture\",\n"
      "  \"one_pass\": true,\n"
      "  \"retry_attempts\": 1,\n"
      "  \"steps\": [\n"
      "    {\"id\": \"manage\", \"type\": \"agent\", \"agent\": \"flk_manager\"},\n"
      "    {\"id\": \"dispatch\", \"type\": \"builtin\", \"builtin\": \"action_runner\"},\n"
      "    {\"id\": \"flaky\", \"type\": \"agent\", \"agent\": \"flk_flaky\"}\n"
      "  ]\n"
      "}\n");
  rc |= test_write_file("floops/opflaky/agents/flk_manager/agent.json",
      "{\"id\":\"flk_manager\",\"executor\":\"script\","
      "\"actions\":[\"work\",\"message\"],\"listen\":[\"event\",\"tasks\"]}\n");
  rc |= write_exec("floops/opflaky/agents/flk_manager/run.sh",
      "#!/usr/bin/env bash\n"
      "cat >/dev/null\n"
      "echo '{\"calls\":[{\"name\":\"work\",\"args\":{\"work\":\"flaky work\",\"done_when\":\"done\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"Queued the flaky work.\"}}]}'\n");
  rc |= test_write_file("floops/opflaky/agents/flk_flaky/agent.json",
      "{\"id\":\"flk_flaky\",\"executor\":\"script\","
      "\"actions\":[],\"listen\":[\"event\"]}\n");
  rc |= write_exec("floops/opflaky/agents/flk_flaky/run.sh",
      "#!/usr/bin/env bash\n"
      "cat >/dev/null\n"
      "if [ ! -f workspace/flaky_ran ]; then\n"
      "  echo ran > workspace/flaky_ran\n"
      "  echo 'transient failure' >&2\n"
      "  exit 1\n"
      "fi\n"
      "echo '{\"calls\":[]}'\n");
  g = harness_gateway_init("opflaky");
  rc |= expect(g != NULL, "opflaky floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "please do the flaky work") == 0,
               "publish user message");
  rc |= expect(harness_gateway_drive_until_completed(g, 1, 8000) == 0,
               "run completes after retry");
  harness_gateway_close(g);

  logs = read_all_run_logs();
  rc |= expect_substr(logs, "retrying event (attempt 1 of 1)",
                      "bounded retry engaged");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "retry pass succeeds");
  /* The retry pass re-emitted both calls (two work action_requests) but
   * the committed effects did not repeat: one work task, one delivery. */
  rc |= expect(count_substr(logs, "\"action\":\"work\"") >= 4,
               "work call re-emitted on the retry pass");
  rc |= expect(count_substr(logs, "\"kind\":\"work\"") == 1,
               "re-emitted work call reuses the recorded create, no second task");
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(deliveries != NULL, "deliveries log exists");
  if (deliveries)
    rc |= expect(count_substr(deliveries, "Queued the flaky work.") == 1,
                 "acknowledgement delivered exactly once across retry");
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  if (tasks)
    rc |= expect(count_substr(tasks, "\"kind\":\"work\"") == 1,
                 "exactly one work task across retry");
  free(tasks);
  free(deliveries);
  free(logs);
  return rc;
}

/* ===== 4. per-context serialization =============================== */

int opgeneric_serialize_contexts_orders_runs(void) {
  HarnessGateway *g;
  char *log1 = NULL, *log2 = NULL, *log3 = NULL;
  long long a1_done = 0, a2_started = 0, b1_started = 0;
  int rc = 0;

  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= test_write_file("floops/opslow/loop.json",
      "{\n"
      "  \"version\": 1,\n"
      "  \"name\": \"opslow\",\n"
      "  \"description\": \"serialization fixture\",\n"
      "  \"one_pass\": true,\n"
      "  \"serialize_contexts\": true,\n"
      "  \"steps\": [\n"
      "    {\"id\": \"manage\", \"type\": \"agent\", \"agent\": \"slow_manager\"}\n"
      "  ]\n"
      "}\n");
  rc |= test_write_file("floops/opslow/agents/slow_manager/agent.json",
      "{\"id\":\"slow_manager\",\"executor\":\"script\","
      "\"actions\":[],\"listen\":[\"event\"]}\n");
  rc |= write_exec("floops/opslow/agents/slow_manager/run.sh",
      "#!/usr/bin/env bash\n"
      "cat >/dev/null\n"
      "sleep 0.4\n"
      "echo '{\"calls\":[]}'\n");
  g = harness_gateway_init("opslow");
  rc |= expect(g != NULL, "opslow floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "A1") == 0, "publish A1");
  rc |= expect(harness_gateway_publish(g, "tests", "A2") == 0, "publish A2");
  rc |= expect(harness_gateway_publish(g, "other", "B1") == 0, "publish B1");
  rc |= expect(harness_gateway_drive_until_completed(g, 3, 10000) == 0,
               "all three runs complete");
  harness_gateway_close(g);

  log1 = harness_read_event_log("run_001");
  log2 = harness_read_event_log("run_002");
  log3 = harness_read_event_log("run_003");
  rc |= expect(log1 && log2 && log3, "three run logs exist");
  if (log1 && log2 && log3) {
    const char *p;
    rc |= expect_substr(log1, "\"text\":\"A1\"", "run_001 is A1");
    rc |= expect_substr(log2, "\"text\":\"A2\"", "run_002 is A2");
    rc |= expect_substr(log3, "\"text\":\"B1\"", "run_003 is B1");
    p = strstr(log1, "\"type\":\"run_done\"");
    if (p) p = strstr(p, "\"monotonic_ms\":");
    if (p) a1_done = atoll(p + strlen("\"monotonic_ms\":"));
    p = strstr(log2, "\"type\":\"run_started\"");
    if (p) p = strstr(p, "\"monotonic_ms\":");
    if (p) a2_started = atoll(p + strlen("\"monotonic_ms\":"));
    p = strstr(log3, "\"type\":\"run_started\"");
    if (p) p = strstr(p, "\"monotonic_ms\":");
    if (p) b1_started = atoll(p + strlen("\"monotonic_ms\":"));
    rc |= expect(a1_done > 0 && a2_started > 0 && b1_started > 0,
                 "lifecycle timestamps parsed");
    rc |= expect(a2_started >= a1_done,
                 "same-context follow-up stays queued until run 1 retires");
    rc |= expect(b1_started < a1_done,
                 "a different context advances while run 1 is active");
  }
  free(log3);
  free(log2);
  free(log1);
  return rc;
}

/* ===== 5. non-critical maintenance ================================ */

int opgeneric_non_critical_step_failure_keeps_run_done(void) {
  HarnessGateway *g;
  char *logs = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= setup_testfx_actions();
  rc |= test_write_file("floops/opnc/loop.json",
      "{\n"
      "  \"version\": 1,\n"
      "  \"name\": \"opnc\",\n"
      "  \"description\": \"non-critical maintenance fixture\",\n"
      "  \"one_pass\": true,\n"
      "  \"steps\": [\n"
      "    {\"id\": \"manage\", \"type\": \"agent\", \"agent\": \"nc_manager\"},\n"
      "    {\"id\": \"maintain\", \"type\": \"agent\", \"agent\": \"nc_broken\", \"non_critical\": true}\n"
      "  ]\n"
      "}\n");
  rc |= test_write_file("floops/opnc/agents/nc_manager/agent.json",
      "{\"id\":\"nc_manager\",\"executor\":\"script\",\"actions\":[\"message\"],\"listen\":[\"event\"]}\n");
  rc |= write_exec("floops/opnc/agents/nc_manager/run.sh",
      "#!/usr/bin/env bash\ncat >/dev/null\n"
      "echo '{\"calls\":[{\"name\":\"message\",\"args\":{\"message\":\"done before maintenance\"}}]}'\n");
  rc |= test_write_file("floops/opnc/agents/nc_broken/agent.json",
      "{\"id\":\"nc_broken\",\"executor\":\"script\",\"actions\":[],\"listen\":[\"event\"]}\n");
  rc |= write_exec("floops/opnc/agents/nc_broken/run.sh",
      "#!/usr/bin/env bash\ncat >/dev/null\necho 'maintenance broke' >&2\nexit 1\n");
  g = harness_gateway_init("opnc");
  rc |= expect(g != NULL, "opnc floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests", "hello") == 0, "publish");
  rc |= expect(harness_gateway_drive_until_completed(g, 1, 8000) == 0, "run completes");
  harness_gateway_close(g);
  logs = read_all_run_logs();
  rc |= expect_substr(logs, "\"type\":\"run_done\"",
                      "non-critical failure still retires done");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "non-critical failure does not fail the event");
  {
    char *deliveries = NULL;
    rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
    if (deliveries)
      rc |= expect(count_substr(deliveries, "done before maintenance") == 1,
                   "committed delivery not replayed");
    free(deliveries);
  }
  free(logs);
  return rc;
}

/* ===== 6. floofclaw background blocker, end to end ============== */

/* A one-pass floop declares "one event -> one run -> one complete pass".
 * finish_advance() honors that at end-of-profile; drain_pending_effect()
 * did not honor it when parking on a subprocess, and rewound to phase
 * zero instead. Every step upstream of the wait then re-ran: an
 * event_kind:-gated handler fired twice (event_kind is fixed for a run's
 * lifetime, so its gate cannot close), and the steps downstream of the
 * wait never ran in the first pass at all. On jet this made the chat
 * agent answer the same user message twice, the second time with no
 * record of the first because the memory step sits downstream. */
static int write_passtest_floop(const char *name, int one_pass) {
  char path[512], body[2048];
  int rc = 0;
  snprintf(path, sizeof(path), "floops/%s/loop.json", name);
  snprintf(body, sizeof(body),
      "{\n"
      "  \"version\": 1,\n"
      "  \"name\": \"%s\",\n"
      "  \"description\": \"pass-semantics fixture\",\n"
      "  \"one_pass\": %s,\n"
      "  \"serialize_contexts\": true,\n"
      "  \"retry_attempts\": 0,\n"
      "  \"steps\": [\n"
      "    {\"id\": \"handler\", \"type\": \"agent\", \"agent\": \"handler\",\n"
      "     \"gate\": \"event_kind:user_message\"},\n"
      "    {\"id\": \"dispatch\", \"type\": \"builtin\", \"builtin\": \"action_runner\"},\n"
      "    {\"id\": \"tail\", \"type\": \"agent\", \"agent\": \"memory\",\n"
      "     \"non_critical\": true}\n"
      "  ]\n"
      "}\n",
      name, one_pass ? "true" : "false");
  rc |= test_write_file(path, body);
  snprintf(path, sizeof(path), "floops/%s/agents/handler/agent.json", name);
  rc |= test_write_file(path,
      "{\"id\":\"handler\",\"executor\":\"llm\",\"model\":{\"ref\":\"responses\"},"
      "\"actions\":[\"passtest_park\"],\"listen\":[\"event\"]}\n");
  snprintf(path, sizeof(path), "floops/%s/agents/memory/agent.json", name);
  rc |= test_write_file(path,
      "{\"id\":\"memory\",\"executor\":\"native\",\"capabilities\":[\"memory\"],"
      "\"recent\":20,\"listen\":[\"memory\"]}\n");
  return rc;
}

/* A real subprocess action. It must fork/exec so waiting_job_count > 0 and
 * the run actually parks -- a runtime intrinsic completes inline and never
 * reaches the rewind. */
static int write_passtest_action(void) {
  int rc = test_write_file("actions/testfx/passtest_park/action.json",
      "{\"id\":\"passtest_park\",\"description\":\"parks the run in a subprocess\","
      "\"outside_world\":false,"
      "\"args_schema\":{\"type\":\"object\",\"additionalProperties\":false,"
      "\"properties\":{\"label\":{\"type\":\"string\"}}},"
      "\"exec\":[\"bash\",\"run.sh\"],\"timeout_ms\":30000}\n");
  rc |= write_exec("actions/testfx/passtest_park/run.sh",
      "#!/usr/bin/env bash\n"
      "cat >/dev/null\n"
      "printf '%s\\n' '{\"events\":[],\"result\":{\"text\":\"parked\"},\"error\":null}'\n");
  /* Keep every shipped area enabled. This file is shared by the whole
   * suite; dropping an area here silently breaks later tests whose floops
   * reference actions from it. */
  rc |= test_write_file("actions/registry.json",
      "{\"version\":1,\"areas\":["
      "{\"id\":\"core\",\"path\":\"core\",\"enabled\":true,"
      "\"description\":\"Runtime core actions shipped with FloofClaw.\"},"
      "{\"id\":\"common\",\"path\":\"common\",\"enabled\":true,"
      "\"description\":\"Common actions shipped with FloofClaw.\"},"
      "{\"id\":\"testfx\",\"path\":\"testfx\",\"enabled\":true,"
      "\"description\":\"Pass-semantics fixtures.\"}"
      "]}\n");
  return rc;
}

static int count_agent_outputs_matching(const char *needle) {
  DIR *runs = opendir("workspace/runs");
  struct dirent *run;
  int count = 0;
  if (!runs) return 0;
  while ((run = readdir(runs)) != NULL) {
    char dir[1024];
    DIR *outs;
    struct dirent *f;
    if (run->d_name[0] == '.') continue;
    snprintf(dir, sizeof(dir), "workspace/runs/%s/agent_outputs", run->d_name);
    outs = opendir(dir);
    if (!outs) continue;
    while ((f = readdir(outs)) != NULL) {
      /* Exactly "<seq>_<step>.json" — the committed output. Skip the
       * sibling .input.json / .llm_response.json / .stderr artifacts,
       * which would otherwise inflate the count per invocation. */
      char want[256];
      const char *underscore = strchr(f->d_name, '_');
      snprintf(want, sizeof(want), "%s.json", needle);
      if (underscore && strcmp(underscore + 1, want) == 0) count++;
    }
    closedir(outs);
  }
  closedir(runs);
  return count;
}

int one_pass_floop_resumes_instead_of_rewinding_on_a_parked_subprocess(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *trace = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= write_passtest_action();
  rc |= write_passtest_floop("passtest", 1);
  rc |= test_write_file("workspace/fixtures/passtest_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  /* First handler invocation asks for the parking action. A second
   * invocation would consume this empty fixture -- which is exactly the
   * defect, so its presence proves the handler ran twice. */
  rc |= test_write_file("workspace/fixtures/passtest_park.json",
      "{\"calls\":[{\"name\":\"passtest_park\",\"args\":{\"label\":\"one\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/passtest_empty.json",
      "{\"calls\":[]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/passtest_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/passtest_park.json:"
               "workspace/fixtures/passtest_empty.json", 1);

  g = harness_gateway_init("passtest");
  rc |= expect(g != NULL, "one-pass fixture floop loads");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "park please") == 0,
               "publish the parking request");
  rc |= expect(drive_until_file_contains(g, "workspace/runs/run_001/event_log.jsonl",
                                         "\"type\":\"run_done\"", 8000) == 0,
               "parked run retires");
  harness_gateway_close(g);
  g = NULL;

  rc |= test_read_file("workspace/runs/run_001/trace.jsonl", &trace);
  rc |= expect(trace != NULL, "trace exists");

  /* The defect, stated directly. */
  rc |= expect(count_agent_outputs_matching("handler") == 1,
               "an event_kind-gated handler runs exactly once in a one-pass run");
  rc |= expect_substr(trace ? trace : "", "\"phase\":\"tail\"",
                      "steps downstream of the parked subprocess run in the same pass");
  rc |= expect_no_substr(trace ? trace : "", "\"pass\":2",
                         "a one-pass run never begins a second pass");
  free(trace);
  trace = NULL;

done:
  if (g) harness_gateway_close(g);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* The control: the change must be driven by the floop's declaration, not
 * by the wait itself. A looping floop still rewinds to phase zero. */
int looping_floop_still_rewinds_on_a_parked_subprocess(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *trace = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= write_passtest_action();
  rc |= write_passtest_floop("looptest", 0);
  rc |= test_write_file("workspace/fixtures/passtest_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/passtest_park.json",
      "{\"calls\":[{\"name\":\"passtest_park\",\"args\":{\"label\":\"one\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/passtest_empty.json",
      "{\"calls\":[]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/passtest_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/passtest_park.json:"
               "workspace/fixtures/passtest_empty.json", 1);

  g = harness_gateway_init("looptest");
  rc |= expect(g != NULL, "looping fixture floop loads");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "park please") == 0,
               "publish the parking request");
  rc |= expect(drive_until_file_contains(g, "workspace/runs/run_001/event_log.jsonl",
                                         "\"type\":\"run_done\"", 8000) == 0,
               "looping run retires");
  harness_gateway_close(g);
  g = NULL;

  rc |= test_read_file("workspace/runs/run_001/trace.jsonl", &trace);
  rc |= expect_substr(trace ? trace : "", "\"pass\":2",
                      "a looping floop still re-walks the profile after the wait");
  free(trace);
  trace = NULL;

done:
  if (g) harness_gateway_close(g);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

static int write_fatest_floop(void) {
  int rc = 0;
  rc |= test_write_file("floops/fatest/loop.json",
      "{\n"
      "  \"version\": 1,\n"
      "  \"name\": \"fatest\",\n"
      "  \"description\": \"floofclaw with a fast poll interval\",\n"
      "  \"one_pass\": true,\n"
      "  \"serialize_contexts\": true,\n"
      "  \"retry_attempts\": 2,\n"
      "  \"steps\": [\n"
      "    {\"id\": \"memory.before\", \"type\": \"agent\", \"agent\": \"memory\"},\n"
      "    {\"id\": \"manage\", \"type\": \"agent\", \"agent\": \"floofclaw_manager\",\n"
      "     \"gate\": \"event_kind:user_message,affair_review,operation_result\"},\n"
      "    {\"id\": \"dispatch\", \"type\": \"builtin\", \"builtin\": \"action_runner\"},\n"
      "    {\"id\": \"workers\", \"type\": \"agent\", \"agent\": \"workers\",\n"
      "     \"gate\": \"event_kind:user_message,affair_review,operation_result\"},\n"
      "    {\"id\": \"memory.after\", \"type\": \"agent\", \"agent\": \"memory\", \"non_critical\": true}\n"
      "  ]\n"
      "}\n");
  rc |= test_write_file("floops/fatest/agents/memory/agent.json",
      "{\"id\":\"memory\",\"executor\":\"native\",\"capabilities\":[\"memory\"],"
      "\"recent\":20,\"listen\":[\"memory\"]}\n");
  rc |= test_write_file("floops/fatest/agents/floofclaw_manager/agent.json",
      "{\"id\":\"floofclaw_manager\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"floofclaw_manager\"},"
      "\"actions\":[\"work\",\"message\",\"affair_open\",\"note_add\",\"defer\","
      "\"affair_close\",\"web_read\"],"
      "\"autocomplete_message_task_on_send\":true,"
      "\"listen\":[\"event\",\"memory\",\"affairs\",\"tasks\"]}\n");
  rc |= test_write_file("floops/fatest/agents/workers/agent.json",
      "{\"id\":\"workers\",\"executor\":\"native\",\"handler\":\"manage_codex\"}\n");
  return rc;
}

static int write_manager_mocks(void) {
  int rc = 0;
  rc |= test_write_file("workspace/fixtures/model_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"floofclaw_manager\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/m1_user_create.json",
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"Sync the YouTube playlist to Spotify.\","
      "\"done_when\":\"The playlist is synchronized.\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"I'll work on that.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/m2_user_revise.json",
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"Sync the NEW playlist URL to Spotify REV2MARK; drop the old one.\","
      "\"done_when\":\"The new playlist is fully mirrored.\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"Swapped it in.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/m3_result_stale.json",
      "{\"calls\":[]}\n");
  rc |= test_write_file("workspace/fixtures/m4_result_blocker.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{\"message\":"
      "\"I could not continue: SPOTIFY_API_KEY is unavailable. Set it in the "
      "FloofClaw environment and tell me when it is ready.\"}}]}\n");
  rc |= write_exec("workspace/fixtures/fake_codex.sh",
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "out=\"\"\n"
      "while [ \"$#\" -gt 0 ]; do case \"$1\" in\n"
      "  -o) out=\"$2\"; shift 2 ;;\n"
      "  -C) shift 2 ;;\n"
      "  *) shift ;;\n"
      "esac; done\n"
      "prompt=\"$(cat)\"\n"
      "mkdir -p \"$(dirname \"$out\")\"\n"
      "if grep -q REV2MARK <<<\"$prompt\"; then\n"
      "  sleep 0.2\n"
      "  printf 'I could not continue because SPOTIFY_API_KEY is unavailable.\\n' > \"$out\"\n"
      "else\n"
      "  sleep 1.4\n"
      "  printf 'Handled only the original revision.\\n' > \"$out\"\n"
      "fi\n");
  return rc;
}

int floofclaw_background_blocker_end_to_end(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = fx_capture_env("FCLAW_CODEX_BIN");
  HarnessGateway *g;
  char *ops = NULL, *tasks = NULL, *deliveries = NULL, *logs = NULL;
  int rc = 0;

  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= fx_reset();
  rc |= write_fatest_floop();
  rc |= write_manager_mocks();
  (void)setenv("FCLAW_MODEL_PROFILES", "workspace/fixtures/model_profiles.json", 1);
  (void)setenv("FCLAW_CODEX_BIN", "workspace/fixtures/fake_codex.sh", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/m1_user_create.json:"
               "workspace/fixtures/m2_user_revise.json:"
               "workspace/fixtures/m3_result_stale.json:"
               "workspace/fixtures/m4_result_blocker.json", 1);

  g = harness_gateway_init("fatest");
  rc |= expect(g != NULL, "fatest floop loads");
  if (!g) return 1;

  /* Request -> ack -> detached start; the initiating run retires while
   * the fake worker (1.4s) is still running. */
  rc |= expect(harness_gateway_publish(g, "tests",
               "put this youtube playlist on my spotify") == 0, "publish request");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "I'll work on that.", 8000) == 0,
               "acknowledgement delivered");
  rc |= expect(drive_until_file_contains(g, "workspace/memory/state/operations.json",
                                         "\"status\":\"running\"", 4000) == 0,
               "detached attempt recorded while worker still runs");

  /* Async follow-up while attempt 1 is running: same task revised to
   * rev 2, no duplicate start. The user disconnects after this. */
  rc |= expect(harness_gateway_publish(g, "tests",
               "actually use the NEW playlist instead") == 0, "publish follow-up");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "Swapped it in.", 8000) == 0,
               "follow-up acknowledged before attempt 1 finishes");
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect(tasks != NULL, "task store exists");
  if (tasks) {
    rc |= expect(count_substr(tasks, "\"kind\":\"work\"") == 1,
                 "follow-up revises the same task, no second task");
    rc |= expect_substr(tasks, "\"work_rev\":2", "task is at rev 2");
    free(tasks);
    tasks = NULL;
  }
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  if (ops) {
    rc |= expect(count_substr(ops, "\"handle\":") == 1,
                 "no duplicate start while attempt 1 runs");
    free(ops);
    ops = NULL;
  }

  /* Background: attempt 1 finishes with rev-1-only text; the attempt
   * rule starts one rev-2 attempt (fed the prior result); the rev-2
   * attempt returns the missing-key text; the manager relays it. */
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "SPOTIFY_API_KEY is unavailable", 20000) == 0,
               "blocker text proactively delivered to the original channel");
  if (getenv("FCLAW_TEST_DEBUG_DUMP")) {
    char *dbg_logs = read_all_run_logs();
    char *dbg_ops = NULL, *dbg_tasks = NULL;
    (void)test_read_file("workspace/memory/state/operations.json", &dbg_ops);
    (void)test_read_file("workspace/memory/state/tasks.json", &dbg_tasks);
    debug_dump_write("blocker.log", dbg_logs);
    debug_dump_write("blocker.ops", dbg_ops);
    debug_dump_write("blocker.tasks", dbg_tasks);
    free(dbg_tasks);
    free(dbg_ops);
    free(dbg_logs);
  }
  drive_for_ms(g, 400);
  harness_gateway_close(g);

  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  rc |= expect(ops != NULL, "operation store exists");
  if (ops) {
    rc |= expect(count_substr(ops, "\"handle\":") == 2,
                 "exactly two attempts across both revisions");
    rc |= expect(count_substr(ops, "\"status\":\"finished\"") == 2,
                 "both attempts terminal");
  }
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  if (tasks) {
    rc |= expect_substr(tasks, "\"status\":\"open\"",
                        "the work task remains open after the blocker");
    rc |= expect_substr(tasks, "\"work_rev\":2", "no further revision");
  }
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  if (deliveries) {
    rc |= expect(count_substr(deliveries, "SPOTIFY_API_KEY is unavailable") == 1,
                 "blocker delivered exactly once");
    rc |= expect(count_substr(deliveries, "\"channel\":\"tests\"") == 3,
                 "all deliveries route to the original channel");
  }
  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"type\":\"operation_started\"") == 2,
               "two operation_started events, one per attempt");
  /* The manager never announces completion from the stale rev-1 result
   * (fixture m3 returns no calls) and never runs on mechanical polls. */
  rc |= expect(count_substr(logs, "Handled only the original revision") >= 1,
               "stale rev-1 result text is preserved as evidence");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "no run reports a scheduler failure");
  rc |= expect(count_run_dirs() >= 4,
               "each completion returns as its own result run");
  free(logs);
  free(deliveries);
  free(tasks);
  free(ops);
  fx_restore_env("FCLAW_CODEX_BIN", saved_codex);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  test_config_restore(saved_cfg);
  return rc;
}

/* ===== 7. manager retry consumes the event once =================== */

int floofclaw_manager_retry_no_duplicate_effects(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = fx_capture_env("FCLAW_CODEX_BIN");
  HarnessGateway *g;
  char *deliveries = NULL, *tasks = NULL, *logs = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= write_fatest_floop();
  /* The subject here is the floop-level bounded retry, so opt this one
   * agent out of in-run decision repair. With the default budget the
   * malformed decision is repaired inside the run and the floop retry
   * never engages -- which is the point of that feature, and is proved
   * by floofclaw_agent_repairs_a_normalizer_rejection. */
  rc |= test_write_file("floops/fatest/agents/floofclaw_manager/agent.json",
      "{\"id\":\"floofclaw_manager\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"floofclaw_manager\"},"
      "\"repair_attempts\":0,"
      "\"actions\":[\"work\",\"message\",\"affair_open\",\"note_add\",\"defer\","
      "\"affair_close\",\"web_read\"],"
      "\"autocomplete_message_task_on_send\":true,"
      "\"listen\":[\"event\",\"memory\",\"affairs\",\"tasks\"]}\n");
  rc |= write_manager_mocks();
  rc |= test_write_file("workspace/fixtures/m0_malformed.json",
                        "{\"calls\":[{\"kind\":\"nonsense\"}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES", "workspace/fixtures/model_profiles.json", 1);
  (void)setenv("FCLAW_CODEX_BIN", "workspace/fixtures/fake_codex.sh", 1);
  /* Attempt 1 returns malformed output; the bounded retry consumes the
   * valid fixture. The event is processed exactly once. */
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/m0_malformed.json:"
               "workspace/fixtures/m1_user_create.json", 1);

  g = harness_gateway_init("fatest");
  rc |= expect(g != NULL, "fatest floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests",
               "put this youtube playlist on my spotify") == 0, "publish request");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "I'll work on that.", 8000) == 0,
               "acknowledgement delivered after manager retry");
  drive_for_ms(g, 200);
  harness_gateway_close(g);

  logs = read_all_run_logs();
  rc |= expect_substr(logs, "retrying event (attempt 1 of 2)",
                      "manager failure engaged the bounded retry");
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  if (deliveries)
    rc |= expect(count_substr(deliveries, "I'll work on that.") == 1,
                 "single acknowledgement after retry");
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  if (tasks)
    rc |= expect(count_substr(tasks, "\"kind\":\"work\"") == 1,
                 "single work task after retry");
  free(tasks);
  free(deliveries);
  free(logs);
  fx_restore_env("FCLAW_CODEX_BIN", saved_codex);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* ===== 8. Milestone 2: standing concerns (affair reviews) ========= */

/* Fire the affair watcher once at a time far past every armed review.
 * Mirrors the production reactor module; NULL scheduler skips the
 * busy-context check (the harness drains the published envelope). */
static int fire_affair_watcher(void) {
  FcReactorModule *m = fc_affair_watcher_module_create(NULL);
  int rc = 0;
  if (!m) return -1;
  if (m->init) rc |= m->init(m);
  if (m->tick) rc |= m->tick(m, fc_now_ms() + 365LL * 86400000LL);
  if (m->shutdown) m->shutdown(m);
  fc_affair_watcher_module_destroy(m);
  return rc;
}

/* The Kill Tony conversation, hermetically: a standing concern is
 * opened from chat, reviewed quietly (note + defer, no message), and
 * finally speaks up exactly once and closes. */
int floofclaw_standing_concern_review_lifecycle(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g;
  char *affairs = NULL, *deliveries = NULL, *tasks = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= write_fatest_floop();
  rc |= write_manager_mocks();
  rc |= test_write_file("workspace/fixtures/a1_open.json",
      "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
      "\"manage\":\"Watch for Kill Tony tickets in Austin to become available; the user wants one.\","
      "\"review_in\":\"1d\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"I'll keep an eye on it.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/a2_quiet_review.json",
      "{\"calls\":[{\"name\":\"note_add\",\"args\":{\"text\":\"Checked; tickets not on sale yet.\"}},"
      "{\"name\":\"defer\",\"args\":{\"value\":\"1d\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/a3_loud_review.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{\"message\":"
      "\"Kill Tony tickets are on sale - you should grab one now.\"}},"
      "{\"name\":\"affair_close\",\"args\":{}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES", "workspace/fixtures/model_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/a1_open.json:"
               "workspace/fixtures/a2_quiet_review.json:"
               "workspace/fixtures/a3_loud_review.json", 1);

  g = harness_gateway_init("fatest");
  rc |= expect(g != NULL, "fatest floop loads");
  if (!g) return 1;

  /* Standing concern from chat: affair armed, ack delivered, NO work
   * task — a concern is not a one-shot job. */
  rc |= expect(harness_gateway_publish(g, "tests",
               "check for kill tony tickets and let me know when they're available") == 0,
               "publish concern");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "keep an eye on it", 8000) == 0,
               "concern acknowledged");
  if (getenv("FCLAW_TEST_DEBUG_DUMP")) {
    char *dbg_logs = read_all_run_logs();
    char *dbg_rej = NULL;
    (void)test_read_file("workspace/logs/rejected_events.jsonl", &dbg_rej);
    debug_dump_write("affair.log", dbg_logs);
    debug_dump_write("affair.rej", dbg_rej);
    free(dbg_rej);
    free(dbg_logs);
  }
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  rc |= expect(affairs != NULL, "affair store exists");
  if (affairs) {
    rc |= expect_substr(affairs, "Kill Tony tickets", "affair records the concern");
    rc |= expect_substr(affairs, "\"status\":\"active\"", "affair is active");
    rc |= expect_no_substr(affairs, "\"next_review_at_ms\":0",
                           "first review is armed");
    free(affairs);
    affairs = NULL;
  }
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  if (tasks) {
    rc |= expect_no_substr(tasks, "\"kind\":\"work\"",
                           "a standing concern queues no work task");
    free(tasks);
    tasks = NULL;
  }

  /* Quiet review: note + defer, and — the important part — silence. */
  rc |= expect(fire_affair_watcher() == 0, "watcher fires review 1");
  rc |= expect(drive_until_file_contains(g, "workspace/memory/state/affairs.json",
                                         "not on sale yet", 8000) == 0,
               "review 1 recorded its observation");
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  if (deliveries) {
    rc |= expect(count_substr(deliveries, "\"delivery_id\"") == 1,
                 "quiet review sends no message");
    free(deliveries);
    deliveries = NULL;
  }
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  if (affairs) {
    rc |= expect_substr(affairs, "\"status\":\"active\"",
                        "affair stays active after a quiet review");
    rc |= expect_no_substr(affairs, "\"next_review_at_ms\":0",
                           "defer re-armed the next review");
    free(affairs);
    affairs = NULL;
  }

  /* The day it changes: one proactive message, affair closed. */
  rc |= expect(fire_affair_watcher() == 0, "watcher fires review 2");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "grab one now", 8000) == 0,
               "the change is proactively delivered");
  drive_for_ms(g, 300);
  harness_gateway_close(g);

  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  if (affairs)
    rc |= expect_substr(affairs, "\"status\":\"closed\"",
                        "affair is closed after the loud review");
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  if (deliveries) {
    rc |= expect(count_substr(deliveries, "grab one now") == 1,
                 "the change is announced exactly once");
    rc |= expect(count_substr(deliveries, "\"delivery_id\"") == 2,
                 "two deliveries total: ack + announcement");
  }
  /* A closed affair never fires again. */
  rc |= expect(fire_affair_watcher() == 0, "watcher ticks after close");
  {
    char *bus_log = NULL;
    rc |= test_read_file("workspace/logs/bus.jsonl", &bus_log);
    if (bus_log)
      rc |= expect(count_substr(bus_log, "\"type\":\"affair_review\"") == 2,
                   "no review fires for a closed affair");
    free(bus_log);
  }
  free(deliveries);
  free(affairs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* A review may escalate to real background work; the operation result
 * comes back through the normal M1 path with the review's correlation. */
int floofclaw_review_queues_background_work(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = fx_capture_env("FCLAW_CODEX_BIN");
  HarnessGateway *g;
  char *deliveries = NULL, *logs = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= write_fatest_floop();
  rc |= write_manager_mocks();
  rc |= test_write_file("workspace/fixtures/b1_open.json",
      "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
      "\"manage\":\"Watch the playlist for drift.\",\"review_in\":\"1d\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"Watching the playlist.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/b2_review_work.json",
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"Deep-check the playlist REV2MARK and reconcile.\","
      "\"done_when\":\"The playlist matches.\"}},"
      "{\"name\":\"defer\",\"args\":{\"value\":\"1d\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_002_000002\",\"state\":\"completed\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/b3_result.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{\"message\":"
      "\"The playlist drifted, so I updated it for you.\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES", "workspace/fixtures/model_profiles.json", 1);
  (void)setenv("FCLAW_CODEX_BIN", "workspace/fixtures/fake_codex.sh", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/b1_open.json:"
               "workspace/fixtures/b2_review_work.json:"
               "workspace/fixtures/b3_result.json", 1);

  g = harness_gateway_init("fatest");
  rc |= expect(g != NULL, "fatest floop loads");
  if (!g) return 1;
  rc |= expect(harness_gateway_publish(g, "tests",
               "keep my playlist synced") == 0, "publish concern");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "Watching the playlist.", 8000) == 0,
               "concern acknowledged");
  rc |= expect(fire_affair_watcher() == 0, "watcher fires the review");
  /* The review queues work; the fake worker (REV2MARK -> fast) finishes;
   * the operation result returns and the manager reports the outcome. */
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "updated it for you", 15000) == 0,
               "background result from a review is proactively delivered");
  drive_for_ms(g, 300);
  harness_gateway_close(g);

  logs = read_all_run_logs();
  rc |= expect(count_substr(logs, "\"type\":\"operation_started\"") == 1,
               "the review started one detached attempt");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"", "no run fails");
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  if (deliveries)
    rc |= expect(count_substr(deliveries, "updated it for you") == 1,
                 "one proactive outcome message");
  free(deliveries);
  free(logs);
  fx_restore_env("FCLAW_CODEX_BIN", saved_codex);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* ===== 9. the kernel stays product-free =========================== */

int kernel_modules_contain_no_product_identifiers(void) {
  static const char *files[] = {
    "runtime/scheduler.c", "runtime/run.c", "runtime/profile.c",
    "runtime/ports.c", "runtime/event_log.c",
    "runtime/operation_state.c", "runtime/operation_driver.c",
    NULL
  };
  static const char *banned[] = {
    "manage_codex", "manage_claude", "spotify", "youtube",
    "credential", "blocker", NULL
  };
  int rc = 0;
  for (int i = 0; files[i]; ++i) {
    char *text = NULL;
    rc |= expect(test_read_file(files[i], &text) == 0 && text != NULL,
                 "kernel module readable");
    if (!text) continue;
    for (int b = 0; banned[b]; ++b) {
      char msg[256];
      snprintf(msg, sizeof(msg), "%s must not mention %s", files[i], banned[b]);
      rc |= expect_no_substr(text, banned[b], msg);
    }
    free(text);
  }
  return rc;
}

/* #11: every immediate managed operation forwards useful bounded text to
 * its later operation_result turn. Covers the intrinsic path (read_file,
 * read_dir) and the subprocess-script path (bash; apply_patch shares the
 * identical stdout->text mechanism). */
int immediate_operations_forward_bounded_result_text(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *bus = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/notes/hello.txt", "text payload alpha");
  rc |= test_write_file("workspace/fixtures/optext_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/optext_read.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes/hello.txt\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/optext_dir.json",
      "{\"calls\":[{\"name\":\"read_dir\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/optext_bash.json",
      "{\"calls\":[{\"name\":\"bash\",\"args\":{"
      "\"op\":\"start\",\"cmd\":\"printf bravo-stdout\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/optext_final.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"operation text proof complete\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/optext_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/optext_read.json:"
               "workspace/fixtures/optext_dir.json:"
               "workspace/fixtures/optext_bash.json:"
               "workspace/fixtures/optext_final.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "openclaw floop loads for result-text proof");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "exercise result text") == 0,
               "publish result-text request");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/bus.jsonl",
                                         "text payload alpha", 8000) == 0,
               "read_file forwards bounded file content as text");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/bus.jsonl",
                                         "hello.txt\\n", 8000) == 0,
               "read_dir forwards a readable listing as text");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/bus.jsonl",
                                         "bravo-stdout", 8000) == 0,
               "bash forwards bounded stdout as text");
done:
  if (g) harness_gateway_close(g);
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus);
  rc |= expect(bus && count_substr(bus, "\"type\":\"operation_result\"") >= 3,
               "three correlated operation_result envelopes published");
  free(bus);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  return rc;
}

/* OpenClaw's all_actions entry injects the complete loaded registry while the
 * ordinary action_list boundary remains filtered and callable. */
int openclaw_action_list_discovers_wildcard_grant(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *first_request = NULL, *second_request = NULL;
  char *third_request = NULL, *fourth_request = NULL;
  char *logs = NULL, *deliveries = NULL;
  int rc = 0;

  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= fx_reset();
  rc |= expect(rt_task_apply_event(
                   "task_created",
                   "{\"task_id\":\"task_stale_open\","
                   "\"context_id\":\"chat:tests\",\"kind\":\"input\","
                   "\"input\":{\"type\":\"message\",\"text\":\"older unfinished input\"},"
                   "\"created_event_id\":\"evt_stale_000001\",\"created_ms\":1,"
                   "\"state\":{\"status\":\"open\",\"work\":null,"
                   "\"done_when\":null,\"artifacts\":[],\"updated_ms\":1}}") == 0,
               "seed another open input in the same chat context");
  rc |= test_write_file("workspace/notes/discovered.txt", "discovery proof\n");
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_discovery_1.json",
      "{\"calls\":["
      "{\"name\":\"working_memory_append\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\","
      "\"working_memory\":\"Discovering the action needed for this request.\"}},"
      "{\"name\":\"action_list\",\"args\":{\"op\":\"start\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_discovery_2.json",
      "{\"calls\":[{\"name\":\"action_list\",\"args\":{"
      "\"op\":\"start\",\"name\":\"gcal\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_discovery_3.json",
      "{\"calls\":[{\"name\":\"action_list\",\"args\":{"
      "\"op\":\"start\",\"name\":\"read_dir\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_discovery_4.json",
      "{\"calls\":[{\"name\":\"read_dir\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_discovery_5.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"OPENCLAW_DISCOVERY_COMPLETE\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_discovery_1.json:"
               "workspace/fixtures/openclaw_discovery_2.json:"
               "workspace/fixtures/openclaw_discovery_3.json:"
               "workspace/fixtures/openclaw_discovery_4.json:"
               "workspace/fixtures/openclaw_discovery_5.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw loads with action discovery enabled");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "discover and inspect notes") == 0,
               "publish action-discovery request");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "OPENCLAW_DISCOVERY_COMPLETE", 12000) == 0,
               "discovered action completes and main_claw replies");
  drive_for_ms(g, 200);

done:
  if (g) harness_gateway_close(g);
  rc |= test_read_file(
      "workspace/runs/run_001/provider_calls/001_request.json", &first_request);
  rc |= test_read_file(
      "workspace/runs/run_002/provider_calls/001_request.json", &second_request);
  rc |= test_read_file(
      "workspace/runs/run_003/provider_calls/001_request.json", &third_request);
  rc |= test_read_file(
      "workspace/runs/run_004/provider_calls/001_request.json", &fourth_request);
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(first_request && second_request && third_request &&
                   fourth_request && logs && deliveries,
               "action-discovery proof artifacts exist");
  rc |= expect_substr(first_request, "\"name\": \"action_list\"",
                      "bootstrap prompt includes action_list");
  rc |= expect_substr(first_request, "=== right now ===",
                      "OpenClaw prompt keeps its current-time block");
  rc |= expect_substr(first_request, "It's ",
                      "OpenClaw receives the rendered local wall clock");
  rc |= expect_no_substr(first_request, "@{now}",
                         "OpenClaw does not leak an unrendered time variable");
  rc |= expect_substr(first_request, "\"name\": \"read_dir\"",
                      "all_actions injects the read_dir schema");
  rc |= expect_substr(first_request,
                      "List entries in a workspace-relative directory",
                      "all_actions injects authoritative action descriptions");
  rc |= expect(test_count_substr(first_request,
                                 "\"name\": \"action_list\"") == 1,
               "all_actions-only config presents action_list exactly once");
  rc |= expect_substr(second_request, "Allowed action IDs",
                      "action_list result returns through operation_result");
  rc |= expect_substr(second_request, "read_dir",
                      "allowed-id discovery includes read_dir");
  rc |= expect_substr(third_request, "Action contract gcal",
                      "large selected contract reaches main_claw in one result");
  rc |= expect_substr(third_request, "Attendee email addresses.",
                      "the end of the gcal schema reaches main_claw");
  rc |= expect_no_substr(third_request, "Continue: call action_list",
                         "action contracts are not paginated");
  rc |= expect_substr(fourth_request, "Action contract read_dir",
                      "selected authoritative contract reaches main_claw");
  rc |= expect_substr(fourth_request, "List entries in a workspace-relative directory",
                      "discovery returns the manifest description");
  rc |= expect(count_lines_with_both(logs, "\"type\":\"action_succeeded\"",
                                     "\"action\":\"action_list\"") == 3,
               "three ordinary action_list calls succeed");
  rc |= expect(count_lines_with_both(logs, "\"type\":\"action_request\"",
                                     "\"task_id\":\"task_run_001_000002\"") == 6,
               "sidecar, discovery chain, and final reply stay on the triggering task");
  rc |= expect_substr(logs, "\"action\":\"read_dir\"",
                      "wildcard-authorized discovered action executes");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "action discovery has no failed run");
  free(deliveries);
  free(logs);
  free(fourth_request);
  free(third_request);
  free(second_request);
  free(first_request);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  test_config_restore(saved_cfg);
  return rc;
}

static int write_openclaw_input_poll_fixture(void) {
  int rc = 0;
  rc |= test_write_file("floops/openclaw_input_poll/loop.json",
      "{\"version\":1,\"name\":\"openclaw_input_poll\","
      "\"description\":\"OpenClaw detached input-task completion proof.\","
      "\"one_pass\":true,\"serialize_contexts\":true,"
      "\"steps\":["
      "{\"id\":\"main\",\"type\":\"agent\",\"agent\":\"main_claw\","
      "\"gate\":\"event_kind:user_message,operation_result\"},"
      "{\"id\":\"dispatch\",\"type\":\"builtin\","
      "\"builtin\":\"action_runner\"}]}\n");
  rc |= test_write_file(
      "floops/openclaw_input_poll/agents/main_claw/agent.json",
      "{\"id\":\"main_claw\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"responses\"},"
      "\"actions\":[\"message\",\"manage_codex\"],"
      "\"listen\":[\"event\",\"tasks\"]}\n");
  rc |= test_write_file(
      "floops/openclaw_input_poll/agents/main_claw/prompt.md",
      "Use the exact catalog. An action continues the input task. A final "
      "message must include task.update state completed.\n{{tools}}\n{{json}}\n");
  return rc;
}

/* one_pass is a run boundary, not a task-state transition. Even a delivered
 * message leaves its input task open unless the owning agent explicitly
 * emits task.update (or uses the separate legacy autocomplete feature). */
int one_pass_run_does_not_complete_input_task(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *log = NULL, *tasks = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= write_openclaw_input_poll_fixture();
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_unfinished.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"OPENCLAW_UNFINISHED_MESSAGE\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_unfinished.json", 1);

  g = harness_gateway_init("openclaw_input_poll");
  rc |= expect(g != NULL, "one-pass task-bound fixture loads");
  if (g) {
    rc |= expect(harness_gateway_publish(g, "tests", "do not infer completion") == 0,
                 "publish one-pass task-bound request");
    rc |= expect(drive_until_file_contains(
                     g, "workspace/logs/deliveries.jsonl",
                     "OPENCLAW_UNFINISHED_MESSAGE", 8000) == 0,
                 "message is delivered before the run retires");
    drive_for_ms(g, 100);
    harness_gateway_close(g);
  }
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &log);
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect(log && tasks, "one-pass task-separation artifacts exist");
  rc |= expect_substr(log, "\"type\":\"run_done\"",
                      "one-pass event run still retires normally");
  rc |= expect_no_substr(log, "\"type\":\"task_completed\"",
                         "run completion does not imply task completion");
  rc |= expect_no_substr(log, "\"type\":\"task_updated\"",
                         "runtime does not invent an explicit agent decision");
  rc |= expect_substr(tasks, "\"status\":\"open\"",
                      "unfinished input task remains durably open");
  free(tasks);
  free(log);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* A visible progress message and detached managed action share one model
 * response. The input task remains open, the detached worker's completion
 * returns as its own correlated result run, and only the later explicit
 * task.update completes the original task. */
int openclaw_progress_and_detached_completion_reach_final_reply(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = fx_capture_env("FCLAW_CODEX_BIN");
  char *saved_sync = fx_capture_env("FCLAW_CODEX_START_SYNC_MS");
  HarnessGateway *g = NULL;
  char *first_log = NULL, *logs = NULL, *bus = NULL;
  char *deliveries = NULL, *archive = NULL, *ops = NULL;
  int rc = 0;

  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= fx_reset();
  rc |= write_openclaw_input_poll_fixture();
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_async_1.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{\"message\":\"OPENCLAW_PROGRESS\"}},"
      "{\"name\":\"manage_codex\",\"args\":{"
      "\"op\":\"start\",\"task\":\"Return the async proof text.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_async_2.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"OPENCLAW_ASYNC_FINAL\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  rc |= write_exec("workspace/fixtures/openclaw_fake_codex.sh",
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "out=\"\"\n"
      "while [ \"$#\" -gt 0 ]; do\n"
      "  case \"$1\" in\n"
      "    -o) out=\"$2\"; shift 2 ;;\n"
      "    -C) shift 2 ;;\n"
      "    *) shift ;;\n"
      "  esac\n"
      "done\n"
      "sleep 0.35\n"
      "mkdir -p \"$(dirname \"$out\")\"\n"
      "printf 'OPENCLAW_ASYNC_TOOL_DONE\\n' > \"$out\"\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_async_1.json:"
               "workspace/fixtures/openclaw_async_2.json", 1);
  (void)setenv("FCLAW_CODEX_BIN",
               "workspace/fixtures/openclaw_fake_codex.sh", 1);
  (void)setenv("FCLAW_CODEX_START_SYNC_MS", "0", 1);

  g = harness_gateway_init("openclaw_input_poll");
  rc |= expect(g != NULL, "input-task polling fixture loads");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "run the detached proof") == 0,
               "publish detached input-task request");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "OPENCLAW_PROGRESS", 8000) == 0,
               "progress message is delivered before operation completion");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "OPENCLAW_ASYNC_FINAL", 12000) == 0,
               "the detached completion returns and main_claw replies");
  drive_for_ms(g, 200);

done:
  if (g) harness_gateway_close(g);
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &first_log);
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus);
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= test_read_file("workspace/memory/state/tasks_archive.jsonl", &archive);
  rc |= test_read_file("workspace/memory/state/operations.json", &ops);
  rc |= expect(first_log && logs && bus && deliveries && archive && ops,
               "progress/poll proof artifacts exist");
  rc |= expect_substr(first_log, "OPENCLAW_PROGRESS",
                      "progress message is durable in the initiating run");
  rc |= expect_no_substr(first_log, "\"status\":\"completed\"",
                         "message paired with an action does not finalize");
  rc |= expect_no_substr(first_log, "\"type\":\"task_completed\"",
                         "progress does not complete the input task");
  rc |= expect_substr(logs, "\"work_rev\":0",
                      "detached input operation records revision zero");
  rc |= expect_no_substr(bus, "\"type\":\"operation_poll\"",
                         "no poll envelope ever reaches the bus");
  rc |= expect_substr(bus, "\"type\":\"operation_completed\"",
                      "the detached runner submits one completion claim");
  rc |= expect_substr(logs, "OPENCLAW_ASYNC_TOOL_DONE",
                      "runner-submitted completion returns as operation_result");
  rc |= expect_substr(logs, "\"type\":\"task_updated\"",
                      "final turn carries explicit task completion intent");
  rc |= expect_substr(logs, "\"status\":\"completed\"",
                      "explicit task update completes the original input task");
  rc |= expect_no_substr(logs, "complete_input_task",
                         "removed implicit-completion envelope field is absent");
  rc |= expect(count_substr(deliveries, "OPENCLAW_PROGRESS") == 1 &&
               count_substr(deliveries, "OPENCLAW_ASYNC_FINAL") == 1,
               "progress and final messages each deliver exactly once");
  rc |= expect_substr(archive, "run the detached proof",
                      "completed original request is archived");
  rc |= expect_substr(ops, "\"status\":\"finished\"",
                      "detached operation reaches durable terminal state");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "progress and async polling have no failed run");
  free(ops);
  free(archive);
  free(deliveries);
  free(bus);
  free(logs);
  free(first_log);
  fx_restore_env("FCLAW_CODEX_START_SYNC_MS", saved_sync);
  fx_restore_env("FCLAW_CODEX_BIN", saved_codex);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  test_config_restore(saved_cfg);
  return rc;
}

/* Observed live (truly run_121, then run_130 three times in a row):
 * gemini-2.5-flash emits a perfect action call but places task_id beside
 * args instead of inside it. The binding is the same fact either way, so
 * the normalizer now accepts that placement: the call executes bound to
 * the task, with no rejection and no repair spent. */
static int openclaw_call_level_task_id_binds_without_repair(void) {
  HarnessGateway *g = NULL;
  char *logs = NULL, *deliveries = NULL, *events = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/notes/shape.txt", "shape-alpha\n");
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_shape_beside.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes/shape.txt\"},"
      "\"task_id\":\"task_run_001_000002\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_shape_final.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"SHAPE_BESIDE_COMPLETE\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_shape_beside.json:"
               "workspace/fixtures/openclaw_shape_final.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw loads for call-level task_id");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests",
                                       "read the shape file and report") == 0,
               "publish call-level task_id request");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "SHAPE_BESIDE_COMPLETE", 12000) == 0,
               "task_id beside args executes and the chain completes");
  drive_for_ms(g, 200);
done:
  if (g) harness_gateway_close(g);
  logs = read_all_run_logs();
  (void)test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= test_read_file("workspace/runs/run_001/event_log.jsonl", &events);
  rc |= expect(access("workspace/runs/run_001/agent_outputs/001_main.json.rejected.json",
                      F_OK) != 0,
               "nothing was rejected");
  rc |= expect(access("workspace/runs/run_001/provider_calls/002_request.json",
                      F_OK) != 0,
               "no repair attempt was spent");
  rc |= expect_substr(events ? events : "",
                      "\"action\":\"read_file\"",
                      "the call was executed");
  rc |= expect_substr(events ? events : "",
                      "\"task_id\":\"task_run_001_000002\"",
                      "and it was bound to the task the model named");
  rc |= expect(count_substr(logs ? logs : "",
                            "\"type\":\"run_failed\"") == 0,
               "no run failed");
  rc |= expect_substr(deliveries ? deliveries : "", "SHAPE_BESIDE_COMPLETE",
                      "the conversation reaches the user");
  free(events);
  free(deliveries);
  free(logs);
  return rc;
}

/* A genuinely stray key is still rejected, and the repair prompt names it:
 * "not the correct output format" beside the rejected response was not
 * enough for Truly's main_claw, which repeated the identical shape through
 * every repair attempt (run_130). */
static int openclaw_stray_call_key_repair_names_the_key(void) {
  HarnessGateway *g = NULL;
  char *logs = NULL, *deliveries = NULL;
  char *repair_request = NULL, *rejection = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/notes/shape.txt", "shape-alpha\n");
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_shape_stray.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes/shape.txt\"},"
      "\"rationale\":\"the user asked for the file\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_shape_clean.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes/shape.txt\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_shape_final.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"SHAPE_REPAIR_COMPLETE\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_shape_stray.json:"
               "workspace/fixtures/openclaw_shape_clean.json:"
               "workspace/fixtures/openclaw_shape_final.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw loads for stray-key repair");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests",
                                       "read the shape file and report") == 0,
               "publish stray-key request");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "SHAPE_REPAIR_COMPLETE", 12000) == 0,
               "structurally rejected decision repairs and completes");
  drive_for_ms(g, 200);
done:
  if (g) harness_gateway_close(g);
  logs = read_all_run_logs();
  (void)test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= test_read_file(
      "workspace/runs/run_001/provider_calls/002_request.json",
      &repair_request);
  rc |= test_read_file(
      "workspace/runs/run_001/agent_outputs/001_main.json.rejected.json",
      &rejection);
  rc |= expect_substr(repair_request ? repair_request : "",
                      "response was not the correct output format: "
                      "invalid_agent_call: call has non-intent key "
                      "\"rationale\"",
                      "repair call names the stray key");
  rc |= expect_substr(rejection ? rejection : "", "\"executed\":false",
                      "structurally rejected decision is preserved unexecuted");
  rc |= expect(count_substr(logs ? logs : "",
                            "\"type\":\"run_failed\"") == 0,
               "one structural slip does not fail the run");
  rc |= expect_substr(deliveries ? deliveries : "", "SHAPE_REPAIR_COMPLETE",
                      "repaired conversation still reaches the user");
  free(rejection);
  free(repair_request);
  free(deliveries);
  free(logs);
  return rc;
}

/* The shipped OpenClaw profile is one durable conversation loop: the same
 * main_claw selects a tool, receives its correlated result in a later run,
 * selects another tool, and finally replies. Existing task working memory
 * and the ordinary memory transcript carry the context between runs. */
int openclaw_same_main_claw_completes_multi_tool_chain(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *logs = NULL, *memory = NULL, *archive = NULL, *deliveries = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/notes/chain.txt", "chain-alpha\n");
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_chain_1.json",
      "{\"calls\":["
      "{\"name\":\"working_memory_append\",\"args\":{"
      "\"working_memory\":\"Reading the exact file before listing its directory.\"}},"
      "{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes/chain.txt\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_chain_2.json",
      "{\"calls\":["
      "{\"name\":\"working_memory_append\",\"args\":{"
      "\"working_memory\":\"The file contained chain-alpha; now confirming its directory.\"}},"
      "{\"name\":\"read_dir\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_chain_3.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"OPENCLAW_CHAIN_COMPLETE\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_chain_1.json:"
               "workspace/fixtures/openclaw_chain_2.json:"
               "workspace/fixtures/openclaw_chain_3.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "single-agent OpenClaw floop loads");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(
                   g, "tests", "read the chain file, inspect its directory, and report") == 0,
               "publish multi-tool OpenClaw request");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "OPENCLAW_CHAIN_COMPLETE", 12000) == 0,
               "same OpenClaw conversation completes after two tools");
  drive_for_ms(g, 200);

done:
  if (g) harness_gateway_close(g);
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/memory/memory.jsonl", &memory);
  rc |= test_read_file("workspace/memory/state/tasks_archive.jsonl", &archive);
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect(logs && memory && archive && deliveries,
               "multi-tool OpenClaw proof artifacts exist");
  if (logs) {
    rc |= expect(count_substr(logs, "\"source\":\"main_claw\"") >= 3 &&
                 count_provider_request_artifacts() == 3,
                 "the same main_claw owns all three provider decisions");
    rc |= expect_substr(logs, "\"action\":\"read_file\"",
                        "main_claw selects read_file");
    rc |= expect_substr(logs, "\"action\":\"read_dir\"",
                        "main_claw selects read_dir after the first result");
    rc |= expect_substr(logs, "\"action\":\"message\"",
                        "main_claw replies after the second result");
    rc |= expect(count_substr(logs, "\"type\":\"operation_result\"") == 4,
                 "two operation results are published and intaken");
    rc |= expect_no_substr(logs, "openclaw_tool_calls",
                           "removed tool-only agent is never invoked");
    rc |= expect_no_substr(logs, "openclaw_conversation",
                           "removed result-only agent is never invoked");
    rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                           "multi-tool chain has no failed run");
  }
  if (memory) {
    rc |= expect_substr(memory, "read the chain file",
                        "original request enters existing conversation memory");
    rc |= expect_substr(memory, "Tool read_file returned:",
                        "read_file consequence enters existing memory");
    rc |= expect_substr(memory, "chain-alpha",
                        "read_file memory record retains returned content");
    rc |= expect_substr(memory, "Tool read_dir returned:",
                        "read_dir consequence enters existing memory");
    rc |= expect_substr(memory, "OPENCLAW_CHAIN_COMPLETE",
                        "final reply enters existing memory");
  }
  if (archive) {
    rc |= expect_substr(archive, "Reading the exact file",
                        "first working-memory note stays on the task");
    rc |= expect_substr(archive, "The file contained chain-alpha",
                        "second working-memory note stays on the same task");
  }
  free(deliveries);
  free(archive);
  free(memory);
  free(logs);
  rc |= openclaw_call_level_task_id_binds_without_repair();
  rc |= openclaw_stray_call_key_repair_names_the_key();
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* Truly run_168 returned three individually valid calls: preserve working
 * memory, send the answer, then complete the exact task. The removed semantic
 * contract rejected that whole response solely because a final answer carried
 * the memory sidecar. The kernel should normalize calls, not classify the
 * model's overall decision as one of two product-shaped forms. */
int openclaw_finalize_with_working_memory_is_not_semantically_rejected(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *logs = NULL, *deliveries = NULL, *archive = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_final_with_memory.json",
      "{\"calls\":["
      "{\"name\":\"working_memory_append\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\","
      "\"working_memory\":\"The requested dates were confirmed.\"}},"
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"FINAL_WITH_MEMORY_OK\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\","
      "\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_final_with_memory.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw loads without a semantic output contract");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "answer and remember") == 0,
               "publish a request finalized with a memory sidecar");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "FINAL_WITH_MEMORY_OK", 10000) == 0,
               "the valid final response reaches delivery");
  drive_for_ms(g, 200);
done:
  if (g) harness_gateway_close(g);
  logs = read_all_run_logs();
  (void)test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  (void)test_read_file("workspace/memory/state/tasks_archive.jsonl", &archive);
  rc |= expect(access(
                   "workspace/runs/run_001/agent_outputs/001_main.json.rejected.json",
                   F_OK) != 0,
               "the response is not semantically rejected");
  rc |= expect(count_provider_request_artifacts() == 1,
               "the valid response needs no repair call");
  rc |= expect_substr(archive ? archive : "",
                      "The requested dates were confirmed.",
                      "working memory is committed before task completion");
  rc |= expect(count_substr(deliveries ? deliveries : "",
                            "FINAL_WITH_MEMORY_OK") == 1,
               "the final message is delivered exactly once");
  rc |= expect_no_substr(logs ? logs : "", "\"type\":\"run_failed\"",
                         "the valid response completes without failure");
  free(archive);
  free(deliveries);
  free(logs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* Until this landed, an LLM agent failed terminally as
 * agent_output_invalid on its first normalizer rejection, and floop
 * retry_attempts then re-ran the identical turn at full model price with
 * nothing said about what was wrong -- Scraps run_781 spent three clean
 * envelopes on three identical rejections and zero repair prompts. The
 * shipped floofclaw chat_manager could not repair a single slip. */
int floofclaw_agent_repairs_a_normalizer_rejection(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *logs = NULL, *deliveries = NULL;
  char *repair_request = NULL, *rejection = NULL;
  int rc = 0;

  rc |= fx_reset();
  /* A stray non-intent key beside args: a perfect call otherwise, and
   * exactly the shape gemini-2.5-flash produces intermittently. */
  rc |= test_write_file("workspace/fc_chat_stray.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"NORMALIZER_REPAIRED_REPLY\"},"
      "\"rationale\":\"the user said hello\"}]}\n");
  rc |= test_write_file("workspace/fc_chat_clean.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"NORMALIZER_REPAIRED_REPLY\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fc_chat_stray.json:"
               "workspace/fc_chat_clean.json", 1);

  g = harness_gateway_init("floofclaw");
  rc |= expect(g != NULL, "default floofclaw floop loads");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "hello there") == 0,
               "publish a message the agent answers with a stray key");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "NORMALIZER_REPAIRED_REPLY", 12000) == 0,
               "an LLM agent repairs its slip and still replies");
  drive_for_ms(g, 200);
done:
  if (g) harness_gateway_close(g);
  logs = read_all_run_logs();
  (void)test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  (void)test_read_file("workspace/runs/run_001/provider_calls/002_request.json",
                       &repair_request);
  (void)test_read_file(
      "workspace/runs/run_001/agent_outputs/001_chat.json.rejected.json",
      &rejection);
  rc |= expect(repair_request != NULL,
               "a second provider call is the repair, not a retried turn");
  rc |= expect_substr(repair_request ? repair_request : "",
                      "call has non-intent key",
                      "the repair prompt names the normalizer's rejection");
  rc |= expect_substr(repair_request ? repair_request : "",
                      "rejected decision; repair attempt 1",
                      "the repair prompt says which attempt this is");
  rc |= expect_no_substr(repair_request ? repair_request : "",
                         "CONTINUE = one substantive action",
                         "repair does not prescribe a semantic decision shape");
  rc |= expect_no_substr(rejection ? rejection : "", "\"contract\":",
                         "the rejection artifact has no dead contract field");
  rc |= expect_substr(rejection ? rejection : "", "\"executed\":false",
                      "the rejected decision is preserved unexecuted");
  rc |= expect(count_substr(logs ? logs : "", "\"type\":\"run_failed\"") == 0,
               "one slip does not fail the run");
  rc |= expect(count_substr(deliveries ? deliveries : "",
                            "NORMALIZER_REPAIRED_REPLY") == 1,
               "the repaired reply is delivered exactly once");
  free(rejection);
  free(repair_request);
  free(deliveries);
  free(logs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* The normalizer-repair budget is bounded, and exhausting it is terminal as
 * agent_output_invalid. */
int floofclaw_repair_budget_is_bounded(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *logs = NULL, *agent_json = NULL;
  char *runstate = NULL, *second_rejection = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_read_file("floops/floofclaw/agents/chat_manager/agent.json",
                       &agent_json);
  rc |= expect(agent_json != NULL, "chat_manager agent.json is readable");
  if (agent_json) {
    /* One repair, then terminal: two identical slips exhaust it. */
    char *patched = (char *)malloc(strlen(agent_json) + 64);
    if (patched) {
      const char *at = strstr(agent_json, "\"executor\": \"llm\",");
      if (at) {
        size_t head = (size_t)(at - agent_json) +
                      strlen("\"executor\": \"llm\",");
        memcpy(patched, agent_json, head);
        patched[head] = '\0';
        strcat(patched, "\n  \"repair_attempts\": 1,");
        strcat(patched, agent_json + head);
        rc |= test_write_file("floops/floofclaw/agents/chat_manager/agent.json",
                              patched);
      } else {
        rc |= expect(0, "chat_manager agent.json has the expected executor key");
      }
      free(patched);
    }
  }
  rc |= test_write_file("workspace/fc_chat_stray.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{"
      "\"message\":\"NEVER_DELIVERED\"},"
      "\"rationale\":\"the user said hello\"}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fc_chat_stray.json:"
               "workspace/fc_chat_stray.json", 1);

  g = harness_gateway_init("floofclaw");
  rc |= expect(g != NULL, "floofclaw loads with a one-repair budget");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "hello there") == 0,
               "publish a message the agent answers wrongly twice");
  rc |= expect(drive_until_file_contains(g, "workspace/runs/run_001/event_log.jsonl",
                                        "\"type\":\"run_failed\"", 12000) == 0,
               "a second identical slip inside the budget is terminal");
  drive_for_ms(g, 200);
done:
  if (g) harness_gateway_close(g);
  (void)test_read_file("workspace/runs/run_001/runstate.json", &runstate);
  (void)test_read_file(
      "workspace/runs/run_001/agent_outputs/002_chat.json.rejected.json",
      &second_rejection);
  logs = read_all_run_logs();
  rc |= expect(second_rejection != NULL,
               "the budget bought exactly one repair before terminal");
  rc |= expect_substr(runstate ? runstate : "", "\"code\":\"agent_output_invalid\"",
                      "an LLM agent exhausts as agent_output_invalid");
  rc |= expect_substr(runstate ? runstate : "", "call has non-intent key",
                      "the terminal error still names the rejection");
  rc |= expect_no_substr(logs ? logs : "", "NEVER_DELIVERED",
                         "nothing from a rejected decision is delivered");
  free(second_rejection);
  free(runstate);
  if (agent_json)
    (void)test_write_file("floops/floofclaw/agents/chat_manager/agent.json",
                          agent_json);
  free(agent_json);
  free(logs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* The operation wake uses the managed-operation excerpt budget directly.
 * A marker beyond the former 384-byte second truncation must reach both the
 * bus envelope and the next main_claw provider request. */
int openclaw_long_operation_result_reaches_same_main_claw(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char long_text[1201];
  char *bus = NULL, *request = NULL, *logs = NULL;
  int rc = 0;

  memset(long_text, 'L', sizeof(long_text) - 1U);
  memcpy(long_text + 760, "LONG_RESULT_TAIL_VISIBLE",
         strlen("LONG_RESULT_TAIL_VISIBLE"));
  long_text[sizeof(long_text) - 1U] = '\0';
  rc |= fx_reset();
  rc |= test_write_file("workspace/notes/long.txt", long_text);
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_long_1.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes/long.txt\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_long_2.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"OPENCLAW_LONG_RESULT_SEEN\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_long_1.json:"
               "workspace/fixtures/openclaw_long_2.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw floop loads for long-result proof");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "inspect the long file") == 0,
               "publish long-result OpenClaw request");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/logs/deliveries.jsonl",
                   "OPENCLAW_LONG_RESULT_SEEN", 10000) == 0,
               "main_claw replies after observing the long result");

done:
  if (g) harness_gateway_close(g);
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus);
  rc |= test_read_file(
      "workspace/runs/run_002/provider_calls/001_raw_request.json", &request);
  logs = read_all_run_logs();
  rc |= expect(bus && request && logs, "long-result proof artifacts exist");
  rc |= expect_substr(bus, "LONG_RESULT_TAIL_VISIBLE",
                      "operation_result keeps text beyond 384 bytes");
  rc |= expect_substr(request, "LONG_RESULT_TAIL_VISIBLE",
                      "the same main_claw receives the uncut marker");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "long-result continuation has no failed run");
  free(logs);
  free(request);
  free(bus);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* Ordinary managed failures and schema rejections are consequences the
 * model can reason about. Each returns through operation_result to the same
 * main_claw, which may explain, retry, or choose another tool. */
int openclaw_managed_failure_and_rejection_return_to_main_claw(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *logs = NULL, *bus = NULL, *deliveries = NULL;
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_fail_1.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"start\",\"path\":\"notes/does-not-exist.txt\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_fail_2.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"OPENCLAW_FAILURE_RECOVERED\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_fail_1.json:"
               "workspace/fixtures/openclaw_fail_2.json", 1);
  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw floop loads for failure continuation");
  if (g) {
    rc |= expect(harness_gateway_publish(g, "tests", "read the missing file") == 0,
                 "publish failing managed action request");
    rc |= expect(drive_until_file_contains(
                     g, "workspace/logs/deliveries.jsonl",
                     "OPENCLAW_FAILURE_RECOVERED", 10000) == 0,
                 "main_claw continues after an ordinary tool failure");
    harness_gateway_close(g);
    g = NULL;
  }
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus);
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect_substr(logs, "\"type\":\"action_failed\"",
                      "missing file records action_failed truth");
  rc |= expect_substr(bus, "Tool read_file failed",
                      "failure is returned as correlated operation_result text");
  rc |= expect(count_provider_request_artifacts() == 2,
               "same main_claw decides before and after failure");
  rc |= expect_substr(deliveries, "OPENCLAW_FAILURE_RECOVERED",
                      "failure continuation reaches the user");
  free(deliveries);
  free(bus);
  free(logs);
  deliveries = bus = logs = NULL;

  rc |= fx_reset();
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_reject_1.json",
      "{\"calls\":[{\"name\":\"read_file\",\"args\":{"
      "\"op\":\"wrong\",\"path\":\"notes/anything.txt\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_reject_2.json",
      "{\"calls\":["
      "{\"name\":\"message\",\"args\":{"
      "\"message\":\"OPENCLAW_REJECTION_RECOVERED\"}},"
      "{\"name\":\"task.update\",\"args\":{"
      "\"task_id\":\"task_run_001_000002\",\"state\":\"completed\"}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_reject_1.json:"
               "workspace/fixtures/openclaw_reject_2.json", 1);
  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw floop loads for rejection continuation");
  if (g) {
    rc |= expect(harness_gateway_publish(g, "tests", "try the invalid read") == 0,
                 "publish rejected managed action request");
    rc |= expect(drive_until_file_contains(
                     g, "workspace/logs/deliveries.jsonl",
                     "OPENCLAW_REJECTION_RECOVERED", 10000) == 0,
                 "main_claw continues after an ordinary tool rejection");
    harness_gateway_close(g);
    g = NULL;
  }
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus);
  rc |= test_read_file("workspace/logs/deliveries.jsonl", &deliveries);
  rc |= expect_substr(logs, "\"type\":\"action_rejected\"",
                      "invalid enum records action_rejected truth");
  rc |= expect_substr(bus, "Tool read_file was rejected",
                      "rejection is returned as correlated operation_result text");
  rc |= expect_substr(bus, "$.op",
                      "rejection consequence includes the invalid field path");
  rc |= expect_substr(bus, "Value is not in enum.",
                      "rejection consequence includes the validator message");
  rc |= expect(count_provider_request_artifacts() == 2,
               "same main_claw decides before and after rejection");
  rc |= expect_substr(deliveries, "OPENCLAW_REJECTION_RECOVERED",
                      "rejection continuation reaches the user");

  if (g) harness_gateway_close(g);
  free(deliveries);
  free(bus);
  free(logs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* Affairs stay a main-claw decision. The first ordinary turn opens a
 * concern; a later synthetic review of that exact affair is handled by the
 * same main_claw and closes it through the existing affair projection. */
int openclaw_main_claw_owns_affair_review(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char *affairs = NULL, *logs = NULL, *memory = NULL;
  char affair_id[64] = "", review_payload[256], bus_id[BUS_ID_MAX] = "";
  int rc = 0;

  rc |= fx_reset();
  rc |= test_write_file("workspace/fixtures/openclaw_profiles.json",
      "{\"version\":1,\"profiles\":["
      "{\"id\":\"default_gemini_flash\",\"provider\":\"mock\",\"model\":\"mock-1\"},"
      "{\"id\":\"responses\",\"provider\":\"mock\",\"model\":\"mock-1\"}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_affair_1.json",
      "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
      "\"manage\":\"Watch the single-claw proof.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/openclaw_affair_2.json",
      "{\"calls\":[{\"name\":\"affair_close\",\"args\":{}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "workspace/fixtures/openclaw_profiles.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/openclaw_affair_1.json:"
               "workspace/fixtures/openclaw_affair_2.json", 1);

  g = harness_gateway_init("openclaw");
  rc |= expect(g != NULL, "OpenClaw floop loads for affair-review proof");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "watch the single-claw proof") == 0,
               "publish request that opens an affair");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/affairs.json",
                   "Watch the single-claw proof", 8000) == 0,
               "main_claw opens the standing concern");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  if (affairs) {
    const char *p = strstr(affairs, "\"affair_id\":\"");
    if (p) sscanf(p + strlen("\"affair_id\":\""), "%63[^\"]", affair_id);
  }
  rc |= expect(affair_id[0] != '\0', "opened affair has a durable id");
  snprintf(review_payload, sizeof(review_payload),
           "{\"affair_id\":\"%s\",\"adapter_id\":\"\","
           "\"text\":\"(affair review)\"}", affair_id);
  rc |= expect(bus_publish("tests", "affair_review", review_payload,
                           bus_id, sizeof(bus_id)) == 0,
               "publish exact affair review");
  rc |= expect(drive_until_file_contains(
                   g, "workspace/memory/state/affairs.json",
                   "\"status\":\"closed\"", 8000) == 0,
               "main_claw closes the affair under review");

done:
  if (g) harness_gateway_close(g);
  free(affairs);
  affairs = NULL;
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  logs = read_all_run_logs();
  rc |= test_read_file("workspace/memory/memory.jsonl", &memory);
  rc |= expect(affairs && logs && memory,
               "affair-review proof artifacts exist");
  rc |= expect_substr(affairs, "\"status\":\"closed\"",
                      "affair projection records main_claw's close decision");
  rc |= expect(count_substr(logs, "\"source\":\"main_claw\"") == 2,
               "same main_claw owns the user turn and review turn");
  rc |= expect_no_substr(logs, "review_manager",
                         "no product-specific review agent makes the decision");
  rc |= expect_no_substr(memory, "(affair review)",
                         "internal affair review stays out of conversation memory");
  rc |= expect_no_substr(logs, "\"type\":\"run_failed\"",
                         "affair review has no failed run");
  free(memory);
  free(logs);
  free(affairs);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* #14: work admitted from an affair review durably carries its origin
 * affair_id through the task record — with a second affair present, the
 * correlation is exact, and it survives a gateway restart. The result
 * manager reads this exact id from the task; no semantic matching. */
int review_dispatched_work_carries_origin_affair_id(void) {
  char *saved_profiles = fx_capture_env("FCLAW_MODEL_PROFILES");
  char *saved_paths = fx_capture_env("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = fx_capture_env("FCLAW_CODEX_BIN");
  HarnessGateway *g = NULL;
  char *affairs = NULL, *tasks = NULL;
  char alpha[64] = "", beta[64] = "";
  char review_payload[256], needle[96], bus_id[64];
  int rc = 0;

  rc |= fx_reset();
  rc |= write_fatest_floop();
  rc |= write_manager_mocks();
  rc |= test_write_file("workspace/fixtures/c1_open_alpha.json",
      "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
      "\"manage\":\"Watch the ALPHA feed for changes.\",\"review_in\":\"1d\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"Watching alpha.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/c2_open_beta.json",
      "{\"calls\":[{\"name\":\"affair_open\",\"args\":{"
      "\"manage\":\"Watch the BETA feed for changes.\",\"review_in\":\"1d\"}},"
      "{\"name\":\"message\",\"args\":{\"message\":\"Watching beta.\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/c3_review_dispatch.json",
      "{\"calls\":[{\"name\":\"work\",\"args\":{"
      "\"work\":\"pull the alpha feed and summarize\","
      "\"done_when\":\"summary exists\"}}]}\n");
  rc |= test_write_file("workspace/fixtures/c4_rest.json", "{\"calls\":[]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES", "workspace/fixtures/model_profiles.json", 1);
  /* The review dispatches real work; the workers phase then starts the
   * configured manage_codex handler. NEVER let that reach the real
   * codex binary from a test. */
  (void)setenv("FCLAW_CODEX_BIN", "workspace/fixtures/fake_codex.sh", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/c1_open_alpha.json:"
               "workspace/fixtures/c2_open_beta.json:"
               "workspace/fixtures/c3_review_dispatch.json:"
               "workspace/fixtures/c4_rest.json", 1);

  g = harness_gateway_init("fatest");
  rc |= expect(g != NULL, "fatest floop loads for affair correlation");
  if (!g) goto done;
  rc |= expect(harness_gateway_publish(g, "tests", "watch the alpha feed") == 0,
               "open alpha affair");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "Watching alpha.", 8000) == 0,
               "alpha affair acknowledged");
  rc |= expect(harness_gateway_publish(g, "tests", "watch the beta feed") == 0,
               "open beta affair");
  rc |= expect(drive_until_file_contains(g, "workspace/logs/deliveries.jsonl",
                                         "Watching beta.", 8000) == 0,
               "beta affair acknowledged");

  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs);
  rc |= expect(affairs != NULL, "affair store exists");
  if (affairs) {
    const char *pa = strstr(affairs, "\"affair_id\":\"");
    if (pa) {
      sscanf(pa + strlen("\"affair_id\":\""), "%63[^\"]", alpha);
      pa = strstr(pa + 1, "\"affair_id\":\"");
      if (pa) sscanf(pa + strlen("\"affair_id\":\""), "%63[^\"]", beta);
    }
    free(affairs);
    affairs = NULL;
  }
  rc |= expect(alpha[0] && beta[0] && strcmp(alpha, beta) != 0,
               "two distinct affairs recorded");

  snprintf(review_payload, sizeof(review_payload),
           "{\"affair_id\":\"%s\",\"adapter_id\":\"tests\","
           "\"text\":\"(affair review)\"}", alpha);
  rc |= expect(bus_publish("tests", "affair_review", review_payload,
                           bus_id, sizeof(bus_id)) == 0,
               "publish review envelope for alpha");
  snprintf(needle, sizeof(needle), "\"affair_id\":\"%s\"", alpha);
  rc |= expect(drive_until_file_contains(g, "workspace/memory/state/tasks.json",
                                         needle, 8000) == 0,
               "review-dispatched work task carries the alpha affair id");
  harness_gateway_close(g);
  g = NULL;

  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect(tasks != NULL, "task store exists");
  if (tasks) {
    char beta_needle[96];
    snprintf(beta_needle, sizeof(beta_needle), "\"affair_id\":\"%s\"", beta);
    rc |= expect(count_substr(tasks, needle) >= 1,
                 "task store links the work to alpha");
    rc |= expect(count_substr(tasks, beta_needle) == 0,
                 "the beta affair is never attached to alpha's work");
    free(tasks);
    tasks = NULL;
  }

  /* Restart preservation: a cold gateway rebuilds state with the same
   * correlation intact. */
  g = harness_gateway_init("fatest");
  rc |= expect(g != NULL, "gateway restarts");
  if (g) { drive_for_ms(g, 200); harness_gateway_close(g); g = NULL; }
  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect(tasks && count_substr(tasks, needle) >= 1,
               "affair correlation survives restart");
  free(tasks);
  tasks = NULL;
done:
  if (g) harness_gateway_close(g);
  free(affairs);
  fx_restore_env("FCLAW_CODEX_BIN", saved_codex);
  fx_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  fx_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  return rc;
}
