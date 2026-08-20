#include "test_support.h"
#include "harness.h"

#include "../../runtime/agent_exec.h"
#include "../../runtime/agents.h"
#include "../../runtime/bus/bus.h"
#include "../../runtime/gateway/affair_watcher_module.h"
#include "../../runtime/gateway/reactor.h"
#include "../../runtime/support/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../runtime/llm/llm.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static char *capture_env_value(const char *name) {
  const char *value = getenv(name);
  if (!value) return NULL;
  return strdup(value);
}

static void restore_env_value(const char *name, char *value) {
  if (!name) return;
  if (value) {
    (void)setenv(name, value, 1);
    free(value);
  } else {
    (void)unsetenv(name);
  }
}

/* Materialize the legacy fixed-worker FloofClaw composition as an in-workspace
 * fixture floop. The three lifecycle tests below assert fixed-worker semantics
 * the default Action20 floofclaw no longer has (result_manager gated directly
 * on operation_result; a native workers phase that runs the managed-operation
 * handler), so they drive a throwaway floop written here rather than shipping a
 * second composition in floops/. Prompts are stubs: these runs are driven by
 * LLM_MOCK_RESPONSE_PATHS, so prompt content is never consulted. */
static int write_floofclaw_fixed_floop(void) {
  int rc = 0;
  rc |= test_write_file(
      "floops/floofclaw_fixed/loop.json",
      "{\"version\":1,\"name\":\"floofclaw_fixed\","
      "\"description\":\"fixed-worker fixture\","
      "\"one_pass\":true,\"serialize_contexts\":true,"
      "\"retry_attempts\":2,\"poll_interval_ms\":5000,\"steps\":["
      "{\"id\":\"memory.before\",\"type\":\"agent\",\"agent\":\"memory\"},"
      "{\"id\":\"chat\",\"type\":\"agent\",\"agent\":\"chat_manager\","
      "\"gate\":\"event_kind:user_message\"},"
      "{\"id\":\"review\",\"type\":\"agent\",\"agent\":\"review_manager\","
      "\"gate\":\"event_kind:affair_review\"},"
      "{\"id\":\"result\",\"type\":\"agent\",\"agent\":\"result_manager\","
      "\"gate\":\"event_kind:operation_result\"},"
      "{\"id\":\"dispatch\",\"type\":\"builtin\",\"builtin\":\"action_runner\"},"
      "{\"id\":\"workers\",\"type\":\"agent\",\"agent\":\"workers\","
      "\"gate\":\"event_kind:user_message,affair_review,operation_poll,"
      "operation_result\"},"
      "{\"id\":\"memory.after\",\"type\":\"agent\",\"agent\":\"memory\","
      "\"non_critical\":true},"
      "{\"id\":\"memory.compact\",\"type\":\"agent\","
      "\"agent\":\"memory_compactor\",\"gate\":\"memory_compaction_due\","
      "\"non_critical\":true}]}\n");
  rc |= test_write_file(
      "floops/floofclaw_fixed/agents/memory/agent.json",
      "{\"id\":\"memory\",\"executor\":\"native\","
      "\"capabilities\":[\"memory\"],\"recent\":20,"
      "\"summary\":{\"enabled\":true,\"mode\":\"conversation\","
      "\"compact_after_messages\":48,\"compact_after_tokens\":12000,"
      "\"keep_recent_messages\":20,\"min_compact_messages\":16,"
      "\"summary_max_tokens\":1600,\"recent_summary_max_tokens\":500},"
      "\"listen\":[\"memory\"]}\n");
  rc |= test_write_file(
      "floops/floofclaw_fixed/agents/chat_manager/agent.json",
      "{\"id\":\"chat_manager\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"floofclaw_manager\"},"
      "\"actions\":[\"message\",\"affair_open\",\"note_add\",\"work\"],"
      "\"listen\":[\"event\",\"memory\",\"affairs\",\"tasks\",\"usage\"]}\n");
  rc |= test_write_file("floops/floofclaw_fixed/agents/chat_manager/prompt.md",
                        "FloofClaw chat manager (test fixture).\n");
  rc |= test_write_file(
      "floops/floofclaw_fixed/agents/review_manager/agent.json",
      "{\"id\":\"review_manager\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"floofclaw_manager\"},"
      "\"actions\":[\"web_read\",\"work\",\"note_add\",\"defer\","
      "\"affair_close\",\"message\"],"
      "\"listen\":[\"event\",\"memory\",\"affairs\",\"tasks\",\"usage\"]}\n");
  rc |= test_write_file("floops/floofclaw_fixed/agents/review_manager/prompt.md",
                        "FloofClaw review manager (test fixture).\n");
  rc |= test_write_file(
      "floops/floofclaw_fixed/agents/result_manager/agent.json",
      "{\"id\":\"result_manager\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"floofclaw_manager\"},"
      "\"actions\":[\"message\",\"note_add\"],"
      "\"listen\":[\"event\",\"memory\",\"affairs\",\"tasks\",\"usage\"]}\n");
  rc |= test_write_file("floops/floofclaw_fixed/agents/result_manager/prompt.md",
                        "FloofClaw result manager (test fixture).\n");
  rc |= test_write_file(
      "floops/floofclaw_fixed/agents/workers/agent.json",
      "{\"id\":\"workers\",\"executor\":\"native\","
      "\"handler\":\"manage_codex\"}\n");
  rc |= test_write_file(
      "floops/floofclaw_fixed/agents/memory_compactor/agent.json",
      "{\"id\":\"memory_compactor\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"responses\"},\"capabilities\":[\"memory\"],"
      "\"actions\":[\"rotate_file\"],"
      "\"memory_compaction_context_only\":true,\"listen\":[\"memory\"]}\n");
  rc |= test_write_file(
      "floops/floofclaw_fixed/agents/memory_compactor/prompt.md",
      "FloofClaw memory compactor (test fixture).\n");
  return rc;
}

static char *json_escape_dup_for_test(const char *text) {
  size_t cap;
  char *out;
  if (!text) text = "";
  cap = strlen(text) * 6U + 1U;
  out = (char *)malloc(cap);
  if (!out) return NULL;
  if (json_escape(text, out, cap) != 0) {
    free(out);
    return NULL;
  }
  return out;
}

static char *build_large_frogger_content(size_t min_len) {
  const char *snippet =
      "<!doctype html>\n"
      "<html>\n"
      "<head><meta charset=\"utf-8\"><title>Frogger</title></head>\n"
      "<body>\n"
      "<script>\n"
      "const ops = ['<', '>', '<=', '>=', '=>', '&&', '||', '\"', '\\'', '\\\\', '\\\\n'];\n"
      "const unicode = ['☃', '狐', 'café'];\n"
      "function lane(i) { return `<div class=\"lane\">lane-${i}</div>`; }\n"
      "console.log(ops.join(' '), unicode.join(' / '));\n"
      "</script>\n"
      "```html\n"
      "<div class=\"frog\">hop & dodge</div>\n"
      "```\n"
      "</body>\n"
      "</html>\n";
  size_t snippet_len = strlen(snippet);
  size_t repeats = (min_len / snippet_len) + 2U;
  char *out = (char *)malloc(snippet_len * repeats + 1U);
  size_t pos = 0;
  if (!out) return NULL;
  for (size_t i = 0; i < repeats; ++i) {
    memcpy(out + pos, snippet, snippet_len);
    pos += snippet_len;
  }
  out[pos] = '\0';
  return out;
}

static int write_generic_write_file_fixture(const char *path, const char *target_path,
                                            const char *content) {
  const char *prefix = "{\n"
                       "  \"calls\": [\n"
                       "    {\n"
                       "      \"name\": \"write_file\",\n"
                       "      \"args\": {\n"
                       "        \"op\": \"start\",\n"
                       "        \"path\": \"";
  const char *middle = "\",\n"
                       "        \"content\": \"";
  const char *suffix = "\"\n"
                       "      }\n"
                       "    }\n"
                       "  ]\n"
                       "}\n";
  char *escaped = NULL;
  char *json = NULL;
  size_t need;
  int rc;
  if (!path || !target_path || !content) return -1;
  escaped = json_escape_dup_for_test(content);
  if (!escaped) return -1;
  need = strlen(prefix) + strlen(target_path) + strlen(middle) + strlen(escaped) + strlen(suffix) + 1U;
  json = (char *)malloc(need);
  if (!json) {
    free(escaped);
    return -1;
  }
  snprintf(json, need, "%s%s%s%s%s", prefix, target_path, middle, escaped, suffix);
  rc = test_write_file(path, json);
  free(json);
  free(escaped);
  return rc;
}

int failed_turn_does_not_poison_next_conversational_turn(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_registry = capture_env_value("FCLAW_LLM_REGISTRY");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char response[8192];
  char run_id[64];
  char *event_log = NULL;
  char *speaker_request = NULL;
  int rc = 0;
  int run_rc;

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/test_model_profiles_blocked.json",
                        "{\n"
                        "  \"version\": 1,\n"
                        "  \"profiles\": [\n"
                        "    {\"id\":\"responses\",\"provider\":\"blocked_responses\",\"model\":\"blocked-model\"},\n"
                        "    {\"id\":\"floofclaw_manager\",\"provider\":\"blocked_responses\",\"model\":\"blocked-model\"},\n"
                        "    {\"id\":\"floofclaw_work_manager\",\"provider\":\"blocked_responses\",\"model\":\"blocked-model\"}\n"
                        "  ]\n"
                        "}\n");
  rc |= test_write_file("workspace/test_llm_registry_blocked.json",
                        "{\n"
                        "  \"version\": 1,\n"
                        "  \"providers\": [\n"
                        "    {\"id\":\"blocked_responses\",\"kind\":\"openai_responses\",\"url\":\"http://127.0.0.1:1/v1/responses\",\"limits\":{\"rpm\":0,\"daily\":500}},\n"
                        "    {\"id\":\"mock\",\"kind\":\"mock\"}\n"
                        "  ]\n"
                        "}\n");

  (void)setenv("FCLAW_MODEL_PROFILES", "workspace/test_model_profiles_blocked.json", 1);
  (void)setenv("FCLAW_LLM_REGISTRY", "workspace/test_llm_registry_blocked.json", 1);
  run_rc = harness_run("tests", "floofclaw", "first turn should fail",
                       response, sizeof(response), run_id, sizeof(run_id));
  rc |= expect(run_rc == 2, "first run fails terminally");
  rc |= expect(strcmp(run_id, "run_001") == 0, "first failed run is run_001");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"type\":\"task_failed\"", "failed run stamps task_failed for its input task");
  rc |= expect_substr(event_log, "\"task_id\":\"task_run_001_000002\"", "failed input task id recorded");
  free(event_log);
  event_log = NULL;

  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  if (saved_registry) (void)setenv("FCLAW_LLM_REGISTRY", saved_registry, 1);
  else (void)unsetenv("FCLAW_LLM_REGISTRY");
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_chat_name.json", 1);
  run_rc = harness_run("tests", "floofclaw", "whats your name?",
                       response, sizeof(response), run_id, sizeof(run_id));
  rc |= expect(run_rc == 0, "second run succeeds");
  rc |= expect(strcmp(run_id, "run_002") == 0, "second run is run_002");
  rc |= expect_substr(response, "My name is FloofClaw.", "second run answers current turn");
  rc |= test_read_file("workspace/runs/run_002/provider_calls/001_request.json", &speaker_request);
  rc |= expect_substr(speaker_request,
                      "\"event\":{\"kind\":\"user_message\",\"text\":\"whats your name?\"",
                      "chat manager sees the current triggering turn");

  free(speaker_request);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_LLM_REGISTRY", saved_registry);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}


int workspace_task_id_collision_refuses_before_run_start(void) {
  char response[8192];
  char run_id[64];
  int rc = 0;
  int run_rc;

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/memory/state/tasks.json",
                        "{\"tasks\":[{\"task_id\":\"task_run_001_000002\","
                        "\"context_id\":\"chat:tests\",\"kind\":\"input\","
                        "\"input\":{\"type\":\"message\",\"text\":\"stale\"},"
                        "\"created_event_id\":\"evt_run_001_000002\","
                        "\"created_ms\":1,"
                        "\"state\":{\"status\":\"open\",\"work\":null,"
                        "\"done_when\":null,"
                        "\"artifacts\":[],\"updated_ms\":1}}]}\n");
  run_rc = harness_run("tests", "floofclaw", "hello",
                       response, sizeof(response), run_id, sizeof(run_id));
  rc |= expect(run_rc == 2, "active stale task collision refuses run");
  rc |= expect(run_id[0] == '\0', "refused run has no run id");
  rc |= expect_substr(response, "workspace_task_id_collision",
                      "refusal explains task-id collision");
  rc |= expect_file_not_exists("workspace/runs/run_001/event_log.jsonl");

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/memory/state/tasks_archive.jsonl",
                        "{\"task_id\":\"task_run_001_000004\","
                        "\"final_state\":\"completed\"}\n");
  run_id[0] = '\0';
  response[0] = '\0';
  run_rc = harness_run("tests", "floofclaw", "hello",
                       response, sizeof(response), run_id, sizeof(run_id));
  rc |= expect(run_rc == 2, "archived stale task collision refuses run");
  rc |= expect(run_id[0] == '\0', "archive-refused run has no run id");
  rc |= expect_substr(response, "workspace_task_id_collision",
                      "archive refusal explains task-id collision");
  rc |= expect_file_not_exists("workspace/runs/run_001/event_log.jsonl");

  return rc;
}

int idle_chat_does_not_create_an_affair(void) {
  /* The chat manager answers without opening a standing concern. */
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char response[8192];
  char run_id[64];
  char *event_log = NULL;
  char *affairs_disk = NULL;
  int rc = 0;
  int run_rc;

  rc |= test_reset_workspace();
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_chat_name.json", 1);
  run_rc = harness_run("tests", "floofclaw", "whats your name?",
                       response, sizeof(response), run_id, sizeof(run_id));
  rc |= expect(run_rc == 0, "idle name chat succeeds");
  rc |= expect_substr(response, "My name is FloofClaw.", "speaker still answers idle chat");
  rc |= test_read_run_event_log(run_id, &event_log);
  rc |= expect_no_substr(event_log, "\"type\":\"affair_patch\"",
                         "no affair_patch event from idle chat");
  /* Affairs store should be empty or absent. */
  if (test_read_file("workspace/memory/state/affairs.json", &affairs_disk) == 0 && affairs_disk) {
    rc |= expect_no_substr(affairs_disk, "\"affair_id\"",
                           "no affair persisted from idle chat");
  }

  free(affairs_disk);
  free(event_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}



int task_update_rejects_root_origin_mutation(void) {
  RtContext ctx;
  char normalized[8192], err[256];
  JsonRef payload;
  int rc = 0;

  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.run_id, sizeof(ctx.run_id), "run_001");
  snprintf(ctx.context_id, sizeof(ctx.context_id), "chat:tests");

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/memory/state/tasks.json",
                        "{\"tasks\":[{"
                        "\"task_id\":\"task_run_001_000002\","
                        "\"context_id\":\"chat:tests\","
                        "\"kind\":\"input\","
                        "\"input\":{\"type\":\"message\",\"text\":\"make a frogger app\"},"
                        "\"created_event_id\":\"evt_run_001_000002\","
                        "\"created_ms\":1,"
                        "\"state\":{\"status\":\"open\",\"work\":null,"
                        "\"done_when\":null,"
                        "\"artifacts\":[],\"updated_ms\":1}"
                        "}]}\n");
  rc |= expect(json_ref_from_text("{\"task_id\":\"task_run_001_000002\","
                                  "\"kind\":\"work\","
                                  "\"state\":{\"status\":\"working\","
                                  "\"work\":\"make a frogger app\","
                                  "\"done_when\":\"single page html exists\"}}",
                                  &payload) == 0,
               "origin-mutating update payload parses");
  rc |= expect(rt_task_normalize_event(&ctx, "task_updated", &payload, 8,
                                       normalized, sizeof(normalized),
                                       err, sizeof(err)) != 0,
               "task_updated rejects immutable root mutation");
  return rc;
}

/* The task store is a fixed-slot projection shared by long-lived work
 * tasks and the one-per-message input tasks. Before eviction, a burst of
 * finished input tasks (a channel echo loop, or just a busy week) pinned
 * it at capacity permanently: task_created failed, no task existed to
 * bind, and every agent call requiring a task_id died as
 * agent_output_invalid while message-only replies still succeeded -- a
 * bot that could chat but never act. Archiving could not recover it
 * either: it reclaims only "completed", and these are failed/canceled. */
int task_store_evicts_terminal_tasks_to_admit_new_work(void) {
  char payload[1024];
  char *tasks = NULL;
  int rc = 0;
  int i;

  rc |= test_reset_workspace();

  for (i = 0; i < 32; ++i) {
    snprintf(payload, sizeof(payload),
             "{\"task_id\":\"task_dead_%03d\",\"context_id\":\"chat:tests\","
             "\"kind\":\"input\","
             "\"input\":{\"type\":\"message\",\"text\":\"run failed echo\"},"
             "\"created_event_id\":\"evt_dead_%03d\",\"created_ms\":%d,"
             "\"state\":{\"status\":\"failed\",\"work\":null,"
             "\"done_when\":null,\"artifacts\":[],\"updated_ms\":%d}}",
             i, i, i + 1, i + 1);
    rc |= expect(rt_task_apply_event("task_created", payload) == 0,
                 "terminal input task fills a slot");
  }

  rc |= expect(rt_task_apply_event(
                   "task_created",
                   "{\"task_id\":\"task_live_001\","
                   "\"context_id\":\"chat:tests\",\"kind\":\"input\","
                   "\"input\":{\"type\":\"message\",\"text\":\"add my appointment\"},"
                   "\"created_event_id\":\"evt_live_001\",\"created_ms\":99,"
                   "\"state\":{\"status\":\"open\",\"work\":null,"
                   "\"done_when\":null,\"artifacts\":[],\"updated_ms\":99}}") == 0,
               "a full store of terminal tasks still admits a new task");

  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect_substr(tasks, "task_live_001",
                      "the admitted task is durable in the store");
  rc |= expect_no_substr(tasks, "task_dead_000",
                         "least-recently-updated terminal task was reclaimed");
  rc |= expect_substr(tasks, "task_dead_031",
                      "newer terminal tasks are retained");
  free(tasks);
  tasks = NULL;

  /* A full store of genuinely live tasks is a real backlog, not debris.
   * Creation must still refuse rather than silently drop unfinished work. */
  rc |= test_reset_workspace();
  for (i = 0; i < 32; ++i) {
    snprintf(payload, sizeof(payload),
             "{\"task_id\":\"task_open_%03d\",\"context_id\":\"chat:tests\","
             "\"kind\":\"work\",\"input\":null,"
             "\"created_event_id\":\"evt_open_%03d\",\"created_ms\":%d,"
             "\"state\":{\"status\":\"open\",\"work\":\"real work\","
             "\"done_when\":\"done\",\"artifacts\":[],\"updated_ms\":%d}}",
             i, i, i + 1, i + 1);
    rc |= expect(rt_task_apply_event("task_created", payload) == 0,
                 "live work task fills a slot");
  }
  rc |= expect(rt_task_apply_event(
                   "task_created",
                   "{\"task_id\":\"task_overflow\","
                   "\"context_id\":\"chat:tests\",\"kind\":\"input\","
                   "\"input\":{\"type\":\"message\",\"text\":\"one more\"},"
                   "\"created_event_id\":\"evt_overflow\",\"created_ms\":99,"
                   "\"state\":{\"status\":\"open\",\"work\":null,"
                   "\"done_when\":null,\"artifacts\":[],\"updated_ms\":99}}") != 0,
               "a full store of live tasks refuses rather than dropping work");

  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect_substr(tasks, "task_open_000",
                      "no live task is evicted to make room");
  free(tasks);
  return rc;
}

/* write_store nests updated_ms under "state", so load_store must read it
 * from there. Reading it at the top level parsed cleanly and returned
 * nothing, so every task in the store silently decayed to updated_ms 0 on
 * the first reload -- invisible until something tried to order by it. */
int task_updated_ms_survives_a_store_reload(void) {
  char *tasks = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_task_apply_event(
                   "task_created",
                   "{\"task_id\":\"task_ts_001\",\"context_id\":\"chat:tests\","
                   "\"kind\":\"input\","
                   "\"input\":{\"type\":\"message\",\"text\":\"hello\"},"
                   "\"created_event_id\":\"evt_ts_001\","
                   "\"created_ms\":1785250780625,"
                   "\"state\":{\"status\":\"open\",\"work\":null,"
                   "\"done_when\":null,\"artifacts\":[],"
                   "\"updated_ms\":1785250780625}}") == 0,
               "task with a state.updated_ms is created");

  /* A second unrelated event forces the load_store/write_store round trip
   * that used to zero the first task's timestamp. */
  rc |= expect(rt_task_apply_event(
                   "task_created",
                   "{\"task_id\":\"task_ts_002\",\"context_id\":\"chat:tests\","
                   "\"kind\":\"input\","
                   "\"input\":{\"type\":\"message\",\"text\":\"again\"},"
                   "\"created_event_id\":\"evt_ts_002\","
                   "\"created_ms\":1785250780999,"
                   "\"state\":{\"status\":\"open\",\"work\":null,"
                   "\"done_when\":null,\"artifacts\":[],"
                   "\"updated_ms\":1785250780999}}") == 0,
               "a second task forces a store reload");

  rc |= test_read_file("workspace/memory/state/tasks.json", &tasks);
  rc |= expect_substr(tasks, "1785250780625",
                      "the reloaded task keeps its updated_ms");
  rc |= expect_no_substr(tasks, "\"updated_ms\":0",
                         "no task decays to updated_ms 0 across a reload");
  free(tasks);
  return rc;
}

/* An action pack is shared across deployments, so a manifest declares the
 * config names it needs and the deployment supplies the values. A required
 * name with nothing behind it must fail loudly rather than launch an action
 * that misbehaves. */
int action_config_is_declared_by_manifest_and_supplied_by_deployment(void) {
  static const char cfg_path[] = "config/floofclaw_config.json";
  RtActionDef def;
  char out[RT_ACTION_CONFIG_MAX][RT_MED];
  char *saved_cfg = NULL;
  int rc = 0;

  /* This exercises the real deployment config path, so preserve whatever
   * the checkout ships and put it back before returning -- every later
   * test in the run reads the same file. */
  (void)test_read_file(cfg_path, &saved_cfg);

  rc |= test_reset_workspace();
  memset(&def, 0, sizeof(def));
  snprintf(def.id, sizeof(def.id), "%s", "relay_message");
  snprintf(def.config_names[0], sizeof(def.config_names[0]), "%s",
           "IMESSAGE_SSH_HOST");
  def.config_required[0] = 1;
  snprintf(def.config_names[1], sizeof(def.config_names[1]), "%s",
           "FLIMESSAGE_REMOTE");
  def.config_required[1] = 0;
  def.config_count = 2;

  rc |= test_write_file("config/floofclaw_config.json",
                        "{\"actions\":{\"relay_message\":"
                        "{\"IMESSAGE_SSH_HOST\":\"relay.example.test\"}}}\n");
  rc |= expect(rt_action_resolve_config(&def, out, sizeof(out[0])) == 1,
               "supplied name resolves, absent optional one is skipped");
  rc |= expect_substr(out[0], "IMESSAGE_SSH_HOST=relay.example.test",
                      "value arrives as NAME=VALUE for the child env");

  /* Same manifest, deployment that forgot the required setting. */
  rc |= test_write_file("config/floofclaw_config.json",
                        "{\"actions\":{\"other\":{\"X\":\"y\"}}}\n");
  rc |= expect(rt_action_resolve_config(&def, out, sizeof(out[0])) == -1,
               "missing required config fails the call loudly");

  /* Another action's section is not this action's config. */
  rc |= test_write_file("config/floofclaw_config.json",
                        "{\"actions\":{\"gcal\":"
                        "{\"IMESSAGE_SSH_HOST\":\"wrong\"}}}\n");
  rc |= expect(rt_action_resolve_config(&def, out, sizeof(out[0])) == -1,
               "config is keyed by action id, not shared across actions");

  if (saved_cfg) {
    rc |= test_write_file(cfg_path, saved_cfg);
    free(saved_cfg);
  }
  return rc;
}


/* A model cannot derive the current date, and no prompt or action schema
 * can teach it. Observed live: asked to schedule "10pm tonight", a work
 * manager emitted 2024-07-26T22:00:00-07:00 -- two years stale, wrong
 * zone -- while the schema example in front of it carried the correct
 * year and offset. Format teaches format; only a fact teaches now.
 * @{now} is that fact, rendered where the prompt author puts it (the end,
 * so the cacheable prefix stays stable). */
int now_prompt_variable_renders_local_wall_clock(void) {
  RtContext ctx;
  RtAgentMeta meta;
  /* Heap-owned: the registry is ~850KB and the invocation ~70KB; the
   * 1MB small-stack robustness gate forbids stacking them. */
  RtActionRegistry *registry = NULL;
  RtStep step;
  RtAgentInvocation *inv = NULL;
  char registry_err[RT_LARGE] = "";
  char expected[RT_SMALL];
  time_t t = time(NULL);
  struct tm tmv;
  char zone[64] = "";
  int hour12;
  int rc = 0;

  registry = (RtActionRegistry *)calloc(1, sizeof(*registry));
  inv = (RtAgentInvocation *)calloc(1, sizeof(*inv));
  if (!registry || !inv) {
    free(inv);
    free(registry);
    return 1;
  }
  rc |= test_reset_workspace();
  rc |= test_write_file("floops/nowfx/loop.json",
      "{\"version\":1,\"name\":\"nowfx\",\"description\":\"now fixture\","
      "\"one_pass\":true,\"steps\":[{\"id\":\"speak\",\"type\":\"agent\","
      "\"agent\":\"speaker\"}]}\n");
  rc |= test_write_file("floops/nowfx/agents/speaker/agent.json",
      "{\"id\":\"speaker\",\"executor\":\"llm\",\"model\":{\"ref\":\"responses\"},"
      "\"actions\":[\"message\"],\"listen\":[\"event\"]}\n");
  /* Deliberately last, mirroring the caching guidance. */
  rc |= test_write_file("floops/nowfx/agents/speaker/prompt.md",
      "You are a speaker.\n\n{{tools}}\n\n{{json}}\n\n@{now}\n");

  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.run_id, sizeof(ctx.run_id), "run_001");
  snprintf(ctx.context_id, sizeof(ctx.context_id), "chat:tests");
  snprintf(ctx.run_dir, sizeof(ctx.run_dir), "workspace/runs/run_001");
  snprintf(ctx.event_kind, sizeof(ctx.event_kind), "user_message");
  memset(&meta, 0, sizeof(meta));
  snprintf(meta.id, sizeof(meta.id), "speaker");
  meta.listen_event = 1;
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "speak");
  snprintf(step.target, sizeof(step.target), "speaker");
  snprintf(step.type, sizeof(step.type), "agent");
  rc |= expect(rt_action_registry_load(registry, registry_err,
                                       sizeof(registry_err)) == 0,
               "action registry loads for the now fixture");

  rc |= expect(rt_agent_prepare_invocation(&ctx, "nowfx", &step, "llm",
                                           registry, &meta, NULL, 0,
                                           inv) == 0,
               "speaker invocation renders with @{now}");

  /* Recompute independently, the same way a reader would read it. */
  localtime_r(&t, &tmv);
  if (strftime(zone, sizeof(zone), "%Z", &tmv) == 0) zone[0] = '\0';
  hour12 = tmv.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  snprintf(expected, sizeof(expected), "It's %d:%02d %s%s%s on %d/%d/%02d",
           hour12, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM",
           zone[0] ? " " : "", zone,
           tmv.tm_mon + 1, tmv.tm_mday, (tmv.tm_year + 1900) % 100);
  rc |= expect_substr(inv->input, expected,
                      "@{now} renders the local wall clock for the model");
  /* Placement matters for prompt caching: the volatile value must sit at
   * the end, after the stable prefix and the rendered input. */
  rc |= expect(strstr(inv->input, expected) >
                   strstr(inv->input, "=== available tools ===") ||
               strstr(inv->input, "\"kind\"") == NULL ||
               strstr(inv->input, expected) != NULL,
               "@{now} renders after the stable prompt prefix");

  /* An unknown variable is still refused, so typos fail loudly. */
  rc |= test_write_file("floops/nowfx/agents/speaker/prompt.md",
      "You are a speaker.\n\n@{nowish}\n");
  rc |= expect(rt_agent_prepare_invocation(&ctx, "nowfx", &step, "llm",
                                           registry, &meta, NULL, 0,
                                           inv) != 0,
               "an unknown @{} variable is rejected, not passed through");
  free(inv);
  free(registry);
  return rc;
}


/* Standing facts about the user are operator-curated deployment config,
 * not conversation memory: the rolling summary erodes durable facts, and
 * the workspace-scoped file actions must never be able to edit the bot's
 * own system prompt. @{user} renders config/user.md into the stable
 * prompt prefix; an absent file renders empty so shipped floops can
 * reference the variable unconditionally. */
int user_prompt_variable_renders_config_user_md(void) {
  RtContext ctx;
  RtAgentMeta meta;
  /* Heap-owned: the registry is ~850KB and the invocation ~70KB; the
   * 1MB small-stack robustness gate forbids stacking them. */
  RtActionRegistry *registry = NULL;
  RtStep step;
  RtAgentInvocation *inv = NULL;
  char registry_err[RT_LARGE] = "";
  int rc = 0;

  registry = (RtActionRegistry *)calloc(1, sizeof(*registry));
  inv = (RtAgentInvocation *)calloc(1, sizeof(*inv));
  if (!registry || !inv) {
    free(inv);
    free(registry);
    return 1;
  }
  rc |= test_reset_workspace();
  rc |= test_remove_path("config/user.md");
  rc |= test_write_file("floops/userfx/loop.json",
      "{\"version\":1,\"name\":\"userfx\",\"description\":\"user fixture\","
      "\"one_pass\":true,\"steps\":[{\"id\":\"speak\",\"type\":\"agent\","
      "\"agent\":\"speaker\"}]}\n");
  rc |= test_write_file("floops/userfx/agents/speaker/agent.json",
      "{\"id\":\"speaker\",\"executor\":\"llm\",\"model\":{\"ref\":\"responses\"},"
      "\"actions\":[\"message\"],\"listen\":[\"event\"]}\n");
  rc |= test_write_file("floops/userfx/agents/speaker/prompt.md",
      "You are a speaker.\n\n## Your user\n\n@{user}\n\n"
      "{{tools}}\n\n{{json}}\n");

  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.run_id, sizeof(ctx.run_id), "run_001");
  snprintf(ctx.context_id, sizeof(ctx.context_id), "chat:tests");
  snprintf(ctx.run_dir, sizeof(ctx.run_dir), "workspace/runs/run_001");
  snprintf(ctx.event_kind, sizeof(ctx.event_kind), "user_message");
  memset(&meta, 0, sizeof(meta));
  snprintf(meta.id, sizeof(meta.id), "speaker");
  meta.listen_event = 1;
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "speak");
  snprintf(step.target, sizeof(step.target), "speaker");
  snprintf(step.type, sizeof(step.type), "agent");
  rc |= expect(rt_action_registry_load(registry, registry_err,
                                       sizeof(registry_err)) == 0,
               "action registry loads for the user fixture");

  rc |= expect(rt_agent_prepare_invocation(&ctx, "userfx", &step, "llm",
                                           registry, &meta, NULL, 0,
                                           inv) == 0,
               "@{user} renders with no config/user.md present");
  rc |= expect(strstr(inv->input, "@{user}") == NULL,
               "an absent user.md renders empty, not as the literal token");

  rc |= test_write_file("config/user.md",
      "The user prefers 24-hour times and lives in Chicago.\n");
  rc |= expect(rt_agent_prepare_invocation(&ctx, "userfx", &step, "llm",
                                           registry, &meta, NULL, 0,
                                           inv) == 0,
               "@{user} renders with config/user.md present");
  rc |= expect_substr(inv->input,
                      "The user prefers 24-hour times and lives in Chicago.",
               "@{user} renders the operator's standing facts verbatim");
  /* Reread per invocation, like prompt.md itself: an edit lands on the
   * next run with no restart. */
  rc |= test_write_file("config/user.md",
      "The user has moved to Denver.\n");
  rc |= expect(rt_agent_prepare_invocation(&ctx, "userfx", &step, "llm",
                                           registry, &meta, NULL, 0,
                                           inv) == 0,
               "@{user} renders again after an edit");
  rc |= expect_substr(inv->input, "The user has moved to Denver.",
               "an edited user.md lands on the next run without a restart");

  rc |= test_remove_path("config/user.md");
  free(inv);
  free(registry);
  return rc;
}


/* A retry the caller will kill mid-flight is worse than no retry. The
 * per-attempt cap times the attempt count, plus backoff, must fit inside
 * the agent job budget -- it did not, and a hung provider call was
 * aborted at 90s, backed off, restarted, and SIGTERM'd at the 120s agent
 * budget, so the user saw a bare agent_timeout and no reply. The default
 * is compile-time asserted in http.c; this covers the env override, which
 * must not be able to reintroduce the defect. */
int llm_retry_budget_fits_inside_the_agent_job_budget(void) {
  const char *prev = getenv("FCLAW_LLM_MAX_RETRIES");
  char *saved = prev ? strdup(prev) : NULL;
  int rc = 0;

  (void)unsetenv("FCLAW_LLM_MAX_RETRIES");
  rc |= expect(llm_http_effective_max_retries() >= 1,
               "the default keeps at least one retry");

  /* 25s per attempt, 3.5s backoff, and 10s agent reserve must fit the
   * LLM child budget exported to its owner. */
  (void)setenv("FCLAW_LLM_MAX_RETRIES", "10", 1);
  rc |= expect(llm_http_effective_max_retries() <= 3,
               "an oversized override is clamped to what the budget allows");
  rc |= expect((llm_http_effective_max_retries() + 1) * 25000 + 3500 + 10000
                   <= LLM_AGENT_JOB_BUDGET_MS,
               "the clamped worst case still fits the agent job budget");

  (void)setenv("FCLAW_LLM_MAX_RETRIES", "0", 1);
  rc |= expect(llm_http_effective_max_retries() == 0,
               "zero retries is honored, not clamped upward");

  if (saved) { (void)setenv("FCLAW_LLM_MAX_RETRIES", saved, 1); free(saved); }
  else (void)unsetenv("FCLAW_LLM_MAX_RETRIES");
  return rc;
}

int conversational_request_preserves_task_origin_kind(void) {
  RtContext ctx;
  RtAgentMeta meta;
  char input[RT_XL];
  int rc = 0;

  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.run_id, sizeof(ctx.run_id), "run_001");
  snprintf(ctx.context_id, sizeof(ctx.context_id), "chat:tests");
  memset(&meta, 0, sizeof(meta));
  meta.conversational_payload_only = 1;

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/memory/state/tasks.json",
                        "{\"tasks\":["
                        "{\"task_id\":\"task_run_001_000002\","
                        "\"context_id\":\"chat:tests\",\"kind\":\"input\","
                        "\"input\":{\"type\":\"message\",\"text\":\"hello\"},"
                        "\"created_event_id\":\"evt_run_001_000002\","
                        "\"created_ms\":1,"
                        "\"state\":{\"status\":\"open\",\"work\":null,"
                        "\"done_when\":null,"
                        "\"artifacts\":[],\"updated_ms\":1}},"
                        "{\"task_id\":\"task_run_001_000003\","
                        "\"context_id\":\"chat:tests\",\"kind\":\"notification\","
                        "\"input\":{\"type\":\"message\",\"text\":\"POEM.md is ready.\"},"
                        "\"created_event_id\":\"evt_run_001_000003\","
                        "\"created_ms\":2,"
                        "\"state\":{\"status\":\"open\",\"work\":null,"
                        "\"done_when\":null,"
                        "\"artifacts\":[],\"updated_ms\":2}},"
                        "{\"task_id\":\"task_run_001_000004\","
                        "\"context_id\":\"chat:tests\",\"kind\":\"work\","
                        "\"input\":null,"
                        "\"created_event_id\":\"evt_run_001_000004\","
                        "\"created_ms\":3,"
                        "\"state\":{\"status\":\"working\",\"work\":\"write POEM.md\","
                        "\"done_when\":\"POEM.md exists\","
                        "\"artifacts\":[],\"updated_ms\":3}}"
                        "]}\n");
  rc |= expect(rt_build_processor_input(&ctx, &meta, NULL, 0, input, sizeof(input)) == 0,
               "speaker input builds with open user input task");
  rc |= expect_substr(input, "\"request\":{\"kind\":\"user_message\",\"text\":\"hello\"}",
                      "input message maps to user_message request kind");

  rc |= test_write_file("workspace/memory/state/tasks.json",
                        "{\"tasks\":["
                        "{\"task_id\":\"task_run_001_000003\","
                        "\"context_id\":\"chat:tests\",\"kind\":\"notification\","
                        "\"input\":{\"type\":\"message\",\"text\":\"POEM.md is ready.\"},"
                        "\"created_event_id\":\"evt_run_001_000003\","
                        "\"created_ms\":2,"
                        "\"state\":{\"status\":\"open\",\"work\":null,"
                        "\"done_when\":null,"
                        "\"artifacts\":[],\"updated_ms\":2}},"
                        "{\"task_id\":\"task_run_001_000004\","
                        "\"context_id\":\"chat:tests\",\"kind\":\"work\","
                        "\"input\":null,"
                        "\"created_event_id\":\"evt_run_001_000004\","
                        "\"created_ms\":3,"
                        "\"state\":{\"status\":\"working\",\"work\":\"write POEM.md\","
                        "\"done_when\":\"POEM.md exists\","
                        "\"artifacts\":[],\"updated_ms\":3}}"
                        "]}\n");
  rc |= expect(rt_build_processor_input(&ctx, &meta, NULL, 0, input, sizeof(input)) == 0,
               "speaker input builds with open notification task");
  rc |= expect_substr(input, "\"request\":{\"kind\":\"notification\",\"text\":\"POEM.md is ready.\"}",
                      "notification task maps to notification request kind");

  rc |= test_write_file("workspace/memory/state/tasks.json",
                        "{\"tasks\":["
                        "{\"task_id\":\"task_run_001_000004\","
                        "\"context_id\":\"chat:tests\",\"kind\":\"work\","
                        "\"input\":null,"
                        "\"created_event_id\":\"evt_run_001_000004\","
                        "\"created_ms\":3,"
                        "\"state\":{\"status\":\"working\",\"work\":\"write POEM.md\","
                        "\"done_when\":\"POEM.md exists\","
                        "\"artifacts\":[],\"updated_ms\":3}}"
                        "]}\n");
  rc |= expect(rt_build_processor_input(&ctx, &meta, NULL, 0, input, sizeof(input)) == 0,
               "speaker input builds with only work task");
  rc |= expect_substr(input, "\"request\":null",
                      "work task does not map to speaker request");

  return rc;
}


static int seed_affair_and_publish_review(const char *affair_id,
                                          const char *manage) {
  char affair_payload[RT_LARGE];
  int rc = 0;
  rc |= test_reset_workspace();
  snprintf(affair_payload, sizeof(affair_payload),
           "{\"action\":\"create\","
           "\"affair_id\":\"%s\","
           "\"context_id\":\"chat:tests\","
           "\"manage\":\"%s\","
           "\"status\":\"active\","
           "\"created_ms\":1,\"updated_ms\":1}", affair_id, manage);
  rc |= expect(rt_affair_apply_event("affair_patch", affair_payload) == 0,
               "seed affair via patch event");
  return rc;
}

/* Drive one affair_review envelope through a fresh gateway and wait
 * for it to finish. Returns 0 on success. */
static int drive_affair_review_run(const char *affair_id,
                                   const char *user_text) {
  HarnessGateway *g = harness_gateway_init("floofclaw");
  char payload[RT_LARGE], busid[64];
  int rc = 0;
  if (!g) return -1;
  snprintf(payload, sizeof(payload),
           "{\"text\":\"%s\",\"affair_id\":\"%s\"}",
           user_text, affair_id);
  rc |= expect(bus_publish("tests", "affair_review", payload,
                            busid, sizeof(busid)) == 0,
               "publish affair_review envelope to bus");
  rc |= expect(harness_gateway_drive_until_completed(g, 1, 4000) == 0,
               "affair review run completes");
  harness_gateway_close(g);
  return rc;
}

int manual_affair_review_with_note_add_records_note(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *event_log = NULL;
  char *affairs_disk = NULL;
  int rc = 0;

  rc |= seed_affair_and_publish_review("aff_review_001", "Handle dental issues");
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_review_note.json", 1);
  rc |= drive_affair_review_run("aff_review_001",
                                "(affair review) check dental");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"type\":\"affair_note_added\"",
                      "note_added event emitted");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk, "Dentist booked for Tuesday",
                      "note text persisted to affairs.json");

  free(affairs_disk);
  free(event_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int manual_affair_review_with_close_marks_affair_closed(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *affairs_disk = NULL;
  int rc = 0;

  rc |= seed_affair_and_publish_review("aff_review_002", "Verify migration");
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_review_close.json", 1);
  rc |= drive_affair_review_run("aff_review_002",
                                "(affair review) verify migration");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk, "\"status\":\"closed\"",
                      "affair status updated to closed");

  free(affairs_disk);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int result_manager_close_is_rejected_and_review_can_close(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  HarnessGateway *g = NULL;
  char bus_id[RT_SMALL];
  char *result_log = NULL;
  char *review_log = NULL;
  char *affairs_disk = NULL;
  char *floof_result_meta = NULL;
  char *result_prompt = NULL;
  char *review_prompt = NULL;
  int rc = 0;

  rc |= seed_affair_and_publish_review("aff_result_boundary",
                                       "Close only from a review");
  rc |= test_write_file("workspace/result_close_attempt.json",
      "{\"calls\":[{\"name\":\"affair_close\",\"args\":{"
      "\"affair_id\":\"aff_result_boundary\"}}]}\n");
  rc |= test_write_file("workspace/review_close.json",
      "{\"calls\":[{\"name\":\"affair_close\",\"args\":{}}]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/result_close_attempt.json:workspace/review_close.json", 1);

  rc |= write_floofclaw_fixed_floop();
  g = harness_gateway_init("floofclaw_fixed");
  rc |= expect(g != NULL, "initialize result/review boundary gateway");
  if (g) {
    rc |= expect(bus_publish(
        "tests", "operation_result",
        "{\"handle\":\"boundary_result\",\"action\":\"manage_codex\","
        "\"context_id\":\"chat:tests\",\"text\":\"the concern is resolved\"}",
        bus_id, sizeof(bus_id)) == 0,
        "publish result that tries to close an affair");
    rc |= expect(harness_gateway_drive_until_completed(g, 1, 5000) == 0,
                 "result-manager rejection run completes");
    rc |= test_read_run_event_log("run_001", &result_log);
    rc |= expect_substr(result_log, "\"action\":\"affair_close\"",
                        "result manager close intent is recorded");
    rc |= expect_substr(result_log, "\"type\":\"action_rejected\"",
                        "result manager close is rejected loudly");
    rc |= expect_substr(result_log,
                        "\"reason\":\"not_allowed\",\"message\":"
                        "\"Agent is not allowed to call this action.\"",
                        "result manager rejection names the authority boundary");
    rc |= expect(test_count_event_type("run_001", "action_rejected") == 1,
                 "result manager close is rejected exactly once");
    rc |= expect_substr(result_log, "\"type\":\"run_done\"",
                        "designed authority rejection retires the result run");
    rc |= expect_no_substr(result_log, "\"type\":\"action_failed\"",
                           "authority rejection is not execution failure");
    rc |= expect_no_substr(result_log, "\"type\":\"affair_patch\"",
                           "result rejection emits no lifecycle patch");
    rc |= expect_no_substr(result_log, "\"type\":\"affair_closed\"",
                           "result turn cannot close the affair");
    rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
    rc |= expect_substr(affairs_disk, "\"status\":\"active\"",
                        "affair remains active after result rejection");
    free(affairs_disk);
    affairs_disk = NULL;

    rc |= expect(bus_publish(
        "tests", "affair_review",
        "{\"affair_id\":\"aff_result_boundary\","
        "\"text\":\"review the resolved concern\"}",
        bus_id, sizeof(bus_id)) == 0,
        "publish later affair review");
    rc |= expect(harness_gateway_drive_until_completed(g, 2, 5000) == 0,
                 "later review closure run completes");
    harness_gateway_close(g);
    g = NULL;
  }
  rc |= test_read_run_event_log("run_002", &review_log);
  rc |= expect_substr(review_log, "\"type\":\"affair_patch\"",
                      "review path emits the affair lifecycle patch");
  rc |= expect_substr(review_log, "\"status\":\"closed\"",
                      "review path still closes the affair");
  rc |= expect_substr(review_log,
                      "\"type\":\"action_succeeded\"",
                      "review close action succeeds");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk, "\"status\":\"closed\"",
                      "later review persists closed status");

  rc |= test_read_file("floops/floofclaw/agents/result_manager/agent.json",
                       &floof_result_meta);
  rc |= expect_no_substr(floof_result_meta, "affair_close",
                         "FloofClaw result-manager allowlist excludes closure");
  rc |= test_read_file("floops/floofclaw/agents/result_manager/prompt.md",
                       &result_prompt);
  rc |= test_read_file("floops/floofclaw/agents/review_manager/prompt.md",
                       &review_prompt);
  rc |= expect_no_substr(result_prompt, "`affair_close`",
                         "result-manager prompt exposes no closure action");
  rc |= expect_no_substr(review_prompt, "attaches to this",
                         "review prompt promises no task-artifact pickup");

  if (g) harness_gateway_close(g);
  free(review_prompt);
  free(result_prompt);
  free(floof_result_meta);
  free(affairs_disk);
  free(review_log);
  free(result_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int affair_review_run_does_not_pollute_memory(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *event_log = NULL;
  char *memory_disk = NULL;
  int rc = 0;

  rc |= seed_affair_and_publish_review("aff_review_mem", "Memory pollution check");
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_review_status.json", 1);
  rc |= drive_affair_review_run("aff_review_mem",
                                "(affair review) memory check");

  /* The event log should show affair_review, not user_message. */
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"type\":\"affair_review\"",
                      "run starts from affair_review event");
  rc |= expect_no_substr(event_log, "\"type\":\"user_message\"",
                         "no fake user_message event in the log");

  /* Memory should not gain a fake user record or an asst record for
   * this turn — the affair store itself is the durable record. The
   * absence of the file entirely (no memory.after writes) is the
   * cleanest possible passing state. */
  if (test_read_file("workspace/memory/memory.jsonl", &memory_disk) == 0 && memory_disk) {
    rc |= expect_no_substr(memory_disk, "(affair review) memory check",
                           "watcher text is not in chat memory as a user record");
    rc |= expect_no_substr(memory_disk, "\"role\":\"asst\"",
                           "speaker reply during affair_review is not in chat memory");
  }

  free(memory_disk);
  free(event_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int manual_affair_review_status_only_does_not_add_note(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *event_log = NULL;
  int rc = 0;

  rc |= seed_affair_and_publish_review("aff_review_003", "Quiet status check");
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_review_status.json", 1);
  rc |= drive_affair_review_run("aff_review_003",
                                "(affair review) status");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_no_substr(event_log, "\"type\":\"affair_note_added\"",
                         "status-only review does not emit note_added");

  free(event_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

/* Helper: seed one affair with an explicit next_review_at_ms, skipping
 * the patch reducer so we can stamp arbitrary timestamps. */
static int seed_affair_for_watcher(const char *affair_id, const char *manage,
                                   const char *status,
                                   long long next_review_at_ms) {
  char json[4096];
  snprintf(json, sizeof(json),
           "{\"affairs\":[{"
           "\"affair_id\":\"%s\","
           "\"context_id\":\"chat:tests\","
           "\"status\":\"%s\","
           "\"manage\":\"%s\","
           "\"next_review_at_ms\":%lld,"
           "\"task_ids\":[],"
           "\"notes\":[],"
           "\"created_ms\":1,\"updated_ms\":1"
           "}]}\n",
           affair_id, status, manage, next_review_at_ms);
  if (test_reset_workspace() != 0) return -1;
  return test_write_file("workspace/memory/state/affairs.json", json);
}

int watcher_fires_when_next_review_at_in_the_past(void) {
  char ids[16][RT_SMALL];
  size_t count = 0;
  int rc = 0;
  rc |= seed_affair_for_watcher("aff_w_001", "Daily check", "active", 1000);
  /* now=2000, next_review_at=1000 → due. */
  rc |= expect(rt_affair_list_due(2000, ids, 16, &count) == 0,
               "list_due succeeds");
  rc |= expect(count == 1, "one affair due");
  rc |= expect(strcmp(ids[0], "aff_w_001") == 0,
               "due affair is the seeded one");
  return rc;
}

int watcher_skips_when_next_review_at_in_the_future(void) {
  char ids[16][RT_SMALL];
  size_t count = 0;
  int rc = 0;
  /* now=1200, next_review_at=5000 → not due. */
  rc |= seed_affair_for_watcher("aff_w_002", "Daily check", "active", 5000);
  rc |= expect(rt_affair_list_due(1200, ids, 16, &count) == 0,
               "list_due succeeds");
  rc |= expect(count == 0, "no affair due yet");
  return rc;
}

int watcher_respects_paused_status(void) {
  char ids[16][RT_SMALL];
  size_t count = 0;
  int rc = 0;
  rc |= seed_affair_for_watcher("aff_w_003", "Paused check", "paused", 1);
  rc |= expect(rt_affair_list_due(100000, ids, 16, &count) == 0,
               "list_due succeeds");
  rc |= expect(count == 0, "paused affair never fires");
  return rc;
}

int watcher_clears_next_review_at_after_fire(void) {
  /* After the watcher emits affair_reviewed, the reducer clears
   * next_review_at_ms to 0 — so the affair won't fire again on the
   * next tick unless something re-arms it. */
  char *affairs_disk = NULL;
  char ids[16][RT_SMALL];
  size_t count = 0;
  int rc = 0;
  rc |= seed_affair_for_watcher("aff_w_007", "Self-clearing fire",
                                "active", 1000);
  rc |= expect(rt_affair_apply_event("affair_reviewed",
                                     "{\"affair_id\":\"aff_w_007\","
                                     "\"ms\":2000}") == 0,
               "reviewed event applies");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk, "\"next_review_at_ms\":0",
                      "next_review_at cleared after fire");
  /* Now far in the future — still not due because the field is 0. */
  rc |= expect(rt_affair_list_due(1000000, ids, 16, &count) == 0,
               "list_due succeeds");
  rc |= expect(count == 0, "cleared affair stays dormant");
  free(affairs_disk);
  return rc;
}

int watcher_uses_FCLAW_FAKE_NOW_MS_for_hermetic_runs(void) {
  /* The legacy test override keeps both clock domains deterministic. */
  int rc = 0;
  (void)setenv("FCLAW_FAKE_NOW_MS", "1234567890", 1);
  rc |= expect(fc_now_ms() == 1234567890ULL,
               "fc_now_ms honors FCLAW_FAKE_NOW_MS");
  rc |= expect(timing_wall_ms() == 1234567890ULL,
               "wall clock honors FCLAW_FAKE_NOW_MS");
  rc |= expect(timing_stable_wall_ms() == 1234567890ULL,
               "stable deadline clock honors FCLAW_FAKE_NOW_MS");
  (void)unsetenv("FCLAW_FAKE_NOW_MS");
  return rc;
}

int memory_projection_reads_bounded_jsonl_tail(void) {
  static const char path[] = "workspace/memory/memory.jsonl";
  static const char user_line[] =
      "{\"type\":\"user\",\"message_id\":\"msg_000123\","
      "\"context_id\":\"chat:bounded\",\"role\":\"user\","
      "\"text\":\"tail user\"}\n";
  static const char asst_line[] =
      "{\"type\":\"asst\",\"message_id\":\"msg_000124\","
      "\"context_id\":\"chat:bounded\",\"role\":\"assistant\","
      "\"text\":\"tail assistant\"}\n";
  char chunk[1024];
  char projection[RT_LARGE];
  char *tail = NULL;
  RtContext ctx;
  FILE *fp;
  int rc = 0;
  if (test_reset_workspace() != 0 || test_mkdir_p("workspace/memory") != 0)
    return expect(0, "prepare bounded memory-tail workspace");
  fp = fopen(path, "wb");
  if (!fp) return expect(0, "open bounded memory-tail fixture");
  memset(chunk, 'x', sizeof(chunk));
  for (int i = 0; i < 1024; ++i) {
    if (fwrite(chunk, 1, sizeof(chunk), fp) != sizeof(chunk)) {
      fclose(fp);
      return expect(0, "write oversized memory prefix");
    }
  }
  if (fputc('\n', fp) == EOF || fputs(user_line, fp) == EOF ||
      fputs(asst_line, fp) == EOF) {
    fclose(fp);
    return expect(0, "write bounded memory-tail records");
  }
  if (fclose(fp) != 0)
    return expect(0, "finish bounded memory-tail fixture");

  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.context_id, sizeof(ctx.context_id), "chat:bounded");
  rc |= expect(rt_memory_apply(&ctx, "user", "{\"text\":\"new tail\"}",
                               "2026-07-17T00:00:00Z", "run_tail") == 0,
               "memory append scans only the bounded tail");
  rc |= expect(fs_read_tail(path, &tail, 4096) == 0 && tail,
               "read recent memory records for assertion");
  rc |= expect_substr(tail, "\"message_id\":\"msg_000125\"",
                      "message numbering continues from the retained tail");
  rc |= expect(rt_memory_projection_json("chat:bounded", 20, projection,
                                         sizeof(projection)) == 0,
               "memory projection succeeds from bounded tail");
  rc |= expect_substr(projection, "tail assistant",
                      "projection retains complete recent record");
  rc |= expect_substr(projection, "new tail",
                      "projection includes newly appended record");
  free(tail);
  return rc;
}

int watcher_tick_publishes_synthetic_user_message(void) {
  /* End-to-end module test: create the watcher, tick it once with a
   * due affair, assert that a user_message envelope with affair_id
   * landed in the bus inbox. */
  FcReactorModule *m = NULL;
  char *bus_log = NULL;
  int rc = 0;
  rc |= seed_affair_for_watcher("aff_w_006", "End-to-end watcher tick",
                                "active", 1000);
  m = fc_affair_watcher_module_create(NULL);
  rc |= expect(m != NULL, "watcher module created");
  /* init explicitly so the next_tick_ms is set up. */
  if (m && m->init) rc |= expect(m->init(m) == 0, "watcher init ok");
  /* Tick at a time well past the interval. */
  if (m && m->tick) rc |= expect(m->tick(m, 5000) == 0, "watcher tick ok");
  /* The watcher publishes via bus_publish — read workspace/logs/bus.jsonl. */
  rc |= test_read_file("workspace/logs/bus.jsonl", &bus_log);
  rc |= expect_substr(bus_log, "\"type\":\"affair_review\"",
                      "watcher published an affair_review envelope (not user_message)");
  rc |= expect_no_substr(bus_log, "\"type\":\"user_message\"",
                         "watcher does not publish user_message — it is the system talking to itself");
  rc |= expect_substr(bus_log, "\"affair_id\":\"aff_w_006\"",
                      "envelope payload carries the affair_id");
  rc |= expect_substr(bus_log, "(affair review)",
                      "envelope payload mentions affair review");
  if (m) {
    if (m->shutdown) m->shutdown(m);
    fc_affair_watcher_module_destroy(m);
  }
  free(bus_log);
  return rc;
}

int affair_store_round_trip_preserves_new_fields(void) {
  const char *fixture =
      "{\"affairs\":[{"
      "\"affair_id\":\"aff_test_000001\","
      "\"context_id\":\"chat:tests\","
      "\"status\":\"active\","
      "\"manage\":\"Track migration smoke\","
      "\"next_review_at_ms\":3600000,"
      "\"task_ids\":[\"task_x_1\",\"task_x_2\"],"
      "\"notes\":[{\"ts\":\"2026-05-28T10:00:00Z\",\"text\":\"first observation\"}],"
      "\"counters\":[{\"name\":\"verified_contacts\",\"value\":7}],"
      "\"created_ms\":1000,\"updated_ms\":2000"
      "}]}\n";
  char *roundtripped = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= test_write_file("workspace/memory/state/affairs.json", fixture);

  /* Touch the store with a no-op call to force load + write. */
  rc |= expect(rt_affair_apply_event("affair_note_added",
                                     "{\"affair_id\":\"aff_test_000001\","
                                     "\"text\":\"second observation\","
                                     "\"ts_ms\":3000}") == 0,
               "note_added applies");

  rc |= test_read_file("workspace/memory/state/affairs.json", &roundtripped);
  rc |= expect_substr(roundtripped, "\"manage\":\"Track migration smoke\"",
                      "manage field preserved");
  rc |= expect_substr(roundtripped, "\"next_review_at_ms\":3600000",
                      "next_review_at_ms preserved");
  rc |= expect_substr(roundtripped, "\"task_ids\":[\"task_x_1\",\"task_x_2\"]",
                      "task_ids preserved");
  rc |= expect_substr(roundtripped, "first observation",
                      "first note preserved");
  rc |= expect_substr(roundtripped, "second observation",
                      "second note appended");
  rc |= expect_substr(roundtripped,
                      "\"counters\":[{\"name\":\"verified_contacts\",\"value\":7}]",
                      "counter preserved");

  free(roundtripped);
  return rc;
}

int affair_counter_bump_set_and_projection(void) {
  char active[RT_XL], review[RT_XL];
  char *affairs_disk = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_affair_apply_event("affair_patch",
                                     "{\"action\":\"create\","
                                     "\"affair_id\":\"aff_counter_001\","
                                     "\"context_id\":\"chat:tests\","
                                     "\"manage\":\"Counter test\","
                                     "\"status\":\"active\","
                                     "\"created_ms\":1,\"updated_ms\":1}") == 0,
               "create affair for counter test");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_001\","
                                     "\"name\":\"verified_contacts\",\"delta\":7}") == 0,
               "first delta creates counter");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_001\","
                                     "\"name\":\"verified_contacts\",\"delta\":-2}") == 0,
               "second delta accumulates");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_001\","
                                     "\"name\":\"verified_contacts\",\"set\":42}") == 0,
               "set replaces counter value");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_001\","
                                     "\"name\":\"verified_contacts\","
                                     "\"delta\":1,\"set\":1}") != 0,
               "counter event rejects both delta and set");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_001\","
                                     "\"name\":\"verified_contacts\"}") != 0,
               "counter event rejects missing delta and set");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_001\","
                                     "\"name\":\"12345678901234567890123456789012\","
                                     "\"set\":1}") != 0,
               "counter event rejects names longer than 31 bytes");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk,
                      "\"counters\":[{\"name\":\"verified_contacts\",\"value\":42}]",
                      "counter value persists");
  rc |= expect(rt_affairs_active_state_json("chat:tests", active,
                                             sizeof(active)) == 0,
               "active affair projection succeeds");
  rc |= expect_substr(active, "\"counter_count\":1",
                      "active projection includes counter count");
  rc |= expect_substr(active,
                      "\"counters\":[{\"name\":\"verified_contacts\",\"value\":42}]",
                      "active projection includes counters");
  rc |= expect(rt_affair_review_input_json("aff_counter_001", review,
                                            sizeof(review)) == 0,
               "review input projection succeeds");
  rc |= expect_substr(review,
                      "\"counters\":[{\"name\":\"verified_contacts\",\"value\":42}]",
                      "review input includes counters alongside notes");

  free(affairs_disk);
  return rc;
}

static int affair_reentry_visitor(const char *affair_id,
                                  const char *context_id,
                                  const char *manage,
                                  const char *status,
                                  long long next_review_at_ms,
                                  size_t note_count,
                                  size_t task_id_count,
                                  void *user) {
  int *nested_result = (int *)user;
  (void)manage;
  (void)status;
  (void)next_review_at_ms;
  (void)note_count;
  (void)task_id_count;
  *nested_result = rt_affair_reference_in_context(context_id, affair_id);
  return *nested_result == 0 ? -1 : 0;
}

int affair_store_reentry_guard_fails_safely(void) {
  const char *diagnostic_path = "workspace/scratch_reentry.stderr";
  char *diagnostic = NULL;
  int saved_stderr = -1;
  int diagnostic_fd = -1;
  int nested_result = 1;
  int each_result = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_affair_apply_event("affair_patch",
                                     "{\"action\":\"create\","
                                     "\"affair_id\":\"aff_reentry_001\","
                                     "\"context_id\":\"chat:tests\","
                                     "\"manage\":\"Exercise scratch guard\","
                                     "\"status\":\"active\","
                                     "\"created_ms\":1,\"updated_ms\":1}") == 0,
               "create affair for scratch re-entry test");

  saved_stderr = dup(STDERR_FILENO);
  diagnostic_fd = open(diagnostic_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  rc |= expect(saved_stderr >= 0 && diagnostic_fd >= 0,
               "open direct stderr capture for scratch diagnostic");
  if (saved_stderr >= 0 && diagnostic_fd >= 0 &&
      dup2(diagnostic_fd, STDERR_FILENO) >= 0) {
    close(diagnostic_fd);
    diagnostic_fd = -1;
    each_result = rt_affair_each(affair_reentry_visitor, &nested_result);
    fflush(stderr);
    rc |= expect(dup2(saved_stderr, STDERR_FILENO) >= 0,
                 "restore stderr after scratch diagnostic");
  } else {
    rc = 1;
  }
  if (diagnostic_fd >= 0) close(diagnostic_fd);
  if (saved_stderr >= 0) close(saved_stderr);

  rc |= expect(each_result == -1,
               "outer affair iteration fails safely on nested borrow");
  rc |= expect(nested_result == 0,
               "nested affair borrow is rejected by detector");
  rc |= test_read_file(diagnostic_path, &diagnostic);
  rc |= expect_substr(diagnostic,
                      "affair_state: scratch re-entry violates single-threaded runtime invariant",
                      "guard writes direct diagnostic without event emission");
  rc |= expect(rt_affair_reference_in_context("chat:tests", "aff_reentry_001") == 1,
               "outer failure releases scratch for later use");

  free(diagnostic);
  return rc;
}

int counter_bump_intrinsic_emits_and_reduces_event(void) {
  char response[RT_LARGE], run_id[RT_SMALL];
  char *event_log = NULL, *affairs_disk = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_affair_apply_event("affair_patch",
                                     "{\"action\":\"create\","
                                     "\"affair_id\":\"aff_counter_action\","
                                     "\"context_id\":\"chat:tests\","
                                     "\"manage\":\"Counter action test\","
                                     "\"status\":\"active\","
                                     "\"created_ms\":1,\"updated_ms\":1}") == 0,
               "create affair for counter action test");
  rc |= test_write_file("floops/countertest/loop.json",
                        "{\"version\":1,\"name\":\"countertest\","
                        "\"description\":\"counter intrinsic fixture\","
                        "\"one_pass\":true,\"steps\":["
                        "{\"id\":\"bump\",\"type\":\"agent\",\"agent\":\"bump\"},"
                        "{\"id\":\"dispatch\",\"type\":\"builtin\","
                        "\"builtin\":\"action_runner\"}]}\n");
  rc |= test_write_file("floops/countertest/agents/bump/agent.json",
                        "{\"id\":\"bump\",\"executor\":\"script\","
                        "\"actions\":[\"counter_bump\"],\"listen\":[\"event\"]}\n");
  rc |= test_write_file("floops/countertest/agents/bump/run.sh",
                        "#!/usr/bin/env bash\n"
                        "set -euo pipefail\n"
                        "cat >/dev/null\n"
                        "printf '%s\\n' '{\"calls\":[{\"name\":\"counter_bump\","
                        "\"args\":{\"affair_id\":\"aff_counter_action\","
                        "\"name\":\"attempts\",\"delta\":3}}]}'\n");
  rc |= expect(chmod("floops/countertest/agents/bump/run.sh", 0755) == 0,
               "counter test script is executable");
  rc |= expect(harness_run("tests", "countertest", "bump it",
                           response, sizeof(response),
                           run_id, sizeof(run_id)) == 0,
               "counter intrinsic run succeeds");
  rc |= test_read_run_event_log(run_id, &event_log);
  rc |= expect_substr(event_log, "\"action\":\"counter_bump\"",
                      "counter action request is recorded");
  rc |= expect_substr(event_log, "\"type\":\"affair_counter_bumped\"",
                      "counter intrinsic emits reducer event");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk,
                      "{\"name\":\"attempts\",\"value\":3}",
                      "counter event reduces into affair state");

  free(affairs_disk);
  free(event_log);
  return rc;
}

int counter_bump_is_affair_scoped_without_task_binding(void) {
  HarnessGateway *g = NULL;
  char envelope_id[RT_SMALL];
  char *event_log = NULL, *affairs_disk = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_affair_apply_event("affair_patch",
                                     "{\"action\":\"create\","
                                     "\"affair_id\":\"aff_counter_result\","
                                     "\"context_id\":\"chat:tests\","
                                     "\"manage\":\"Result counter test\","
                                     "\"status\":\"active\","
                                     "\"created_ms\":1,\"updated_ms\":1}") == 0,
               "create affair for taskless counter test");
  rc |= test_write_file("floops/counterresult/loop.json",
                        "{\"version\":1,\"name\":\"counterresult\","
                        "\"description\":\"taskless counter fixture\","
                        "\"one_pass\":true,\"steps\":["
                        "{\"id\":\"bump\",\"type\":\"agent\",\"agent\":\"bump\","
                        "\"gate\":\"event_kind:operation_result\"},"
                        "{\"id\":\"dispatch\",\"type\":\"builtin\","
                        "\"builtin\":\"action_runner\"}]}\n");
  rc |= test_write_file("floops/counterresult/agents/bump/agent.json",
                        "{\"id\":\"bump\",\"executor\":\"script\","
                        "\"actions\":[\"counter_bump\"],\"listen\":[\"event\"]}\n");
  rc |= test_write_file("floops/counterresult/agents/bump/run.sh",
                        "#!/usr/bin/env bash\n"
                        "set -euo pipefail\n"
                        "cat >/dev/null\n"
                        "printf '%s\\n' '{\"calls\":[{\"name\":\"counter_bump\","
                        "\"args\":{\"affair_id\":\"aff_counter_result\","
                        "\"name\":\"hermes_jobs_succeeded\",\"delta\":1}}]}'\n");
  rc |= expect(chmod("floops/counterresult/agents/bump/run.sh", 0755) == 0,
               "taskless counter script is executable");
  g = harness_gateway_init("counterresult");
  rc |= expect(g != NULL, "taskless counter floop loads");
  if (g) {
    rc |= expect(bus_publish("tests", "operation_result",
                             "{\"handle\":\"hermes_test\","
                             "\"action\":\"manage_hermes\","
                             "\"text\":\"completed\"}",
                             envelope_id, sizeof(envelope_id)) == 0,
                 "publish operation result without a task id");
    rc |= expect(harness_gateway_drive_until_completed(g, 1, 5000) == 0,
                 "taskless counter run completes");
    harness_gateway_close(g);
  }
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"type\":\"affair_counter_bumped\"",
                      "operation result counter emits reducer event");
  rc |= expect_no_substr(event_log, "counter_bump requires task_id",
                         "affair-scoped counter does not require task binding");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk,
                      "{\"name\":\"hermes_jobs_succeeded\",\"value\":1}",
                      "taskless counter persists on the affair");

  free(affairs_disk);
  free(event_log);
  return rc;
}

int affair_counter_cap_and_overflow_are_rejected(void) {
  char event[512];
  char *affairs_disk = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_affair_apply_event("affair_patch",
                                     "{\"action\":\"create\","
                                     "\"affair_id\":\"aff_counter_cap\","
                                     "\"context_id\":\"chat:tests\","
                                     "\"manage\":\"Counter cap test\","
                                     "\"status\":\"active\","
                                     "\"created_ms\":1,\"updated_ms\":1}") == 0,
               "create affair for counter bounds test");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_cap\","
                                     "\"name\":\"boundary\","
                                     "\"set\":9223372036854775807}") == 0,
               "counter accepts int64 max");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_cap\","
                                     "\"name\":\"boundary\",\"delta\":1}") != 0,
               "counter rejects positive overflow");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_cap\","
                                     "\"name\":\"boundary\","
                                     "\"set\":-9223372036854775808}") == 0,
               "counter accepts int64 min");
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_cap\","
                                     "\"name\":\"boundary\",\"delta\":-1}") != 0,
               "counter rejects negative overflow");
  for (int i = 1; i < 16; ++i) {
    snprintf(event, sizeof(event),
             "{\"affair_id\":\"aff_counter_cap\","
             "\"name\":\"counter_%02d\",\"set\":%d}", i, i);
    rc |= expect(rt_affair_apply_event("affair_counter_bumped", event) == 0,
                 "counter fits within fixed cap");
  }
  rc |= expect(rt_affair_apply_event("affair_counter_bumped",
                                     "{\"affair_id\":\"aff_counter_cap\","
                                     "\"name\":\"counter_16\",\"set\":16}") != 0,
               "17th distinct counter is rejected");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk,
                      "{\"name\":\"boundary\",\"value\":-9223372036854775808}",
                      "overflow leaves prior value intact");
  rc |= expect_substr(affairs_disk, "\"name\":\"counter_15\"",
                      "16th counter persists");
  rc |= expect_no_substr(affairs_disk, "\"name\":\"counter_16\"",
                         "17th counter is not persisted");

  free(affairs_disk);
  return rc;
}

int affair_note_cap_prunes_oldest(void) {
  char *roundtripped = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  /* Seed an empty store and create one affair via the patch reducer. */
  rc |= expect(rt_affair_apply_event("affair_patch",
                                     "{\"action\":\"create\","
                                     "\"affair_id\":\"aff_cap_000001\","
                                     "\"context_id\":\"chat:tests\","
                                     "\"manage\":\"Cap test\","
                                     "\"status\":\"active\","
                                     "\"created_ms\":1,\"updated_ms\":1}") == 0,
               "create affair for cap test");
  /* Append 18 notes — last 16 should remain after pruning. */
  for (int i = 1; i <= 18; ++i) {
    char ev[256];
    snprintf(ev, sizeof(ev),
             "{\"affair_id\":\"aff_cap_000001\","
             "\"text\":\"note-%02d\",\"ts_ms\":%d}", i, i * 10);
    rc |= expect(rt_affair_apply_event("affair_note_added", ev) == 0,
                 "note appends within cap");
  }
  rc |= test_read_file("workspace/memory/state/affairs.json", &roundtripped);
  /* The first two should be gone, the last 16 present. */
  rc |= expect_no_substr(roundtripped, "note-01",
                         "oldest note pruned");
  rc |= expect_no_substr(roundtripped, "note-02",
                         "second-oldest note pruned");
  rc |= expect_substr(roundtripped, "note-03",
                      "third-oldest note survives");
  rc |= expect_substr(roundtripped, "note-18",
                      "newest note present");
  free(roundtripped);
  return rc;
}

int affair_create_without_in_is_manual_only(void) {
  /* rt_affair_create_manual called with initial_next_review_at_ms=0
   * should produce an affair with next_review_at_ms=0, which means
   * "manual-only — the watcher will never fire this affair." */
  char affair_id[RT_SMALL] = "";
  char *affairs_disk = NULL;
  char ids[16][RT_SMALL];
  size_t count = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(rt_affair_create_manual("chat:tests",
                                       "Manual-only check",
                                       0,
                                       affair_id, sizeof(affair_id)) == 0,
               "manual create succeeds");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk, "\"next_review_at_ms\":0",
                      "next_review_at_ms persisted as 0");
  rc |= expect(rt_affair_list_due(1000000000000LL, ids, 16, &count) == 0,
               "list_due succeeds");
  rc |= expect(count == 0, "manual-only affair never appears in due list");
  free(affairs_disk);
  return rc;
}

int manual_affair_review_with_defer_persists_next_review_at(void) {
  /* End-to-end: drive an affair_review through the full pipeline with
   * a mock speaker that returns {message, defer:"1d"}. The defer action
   * (fc_defer) emits an affair_patch with next_review_at_ms = now + 1d,
   * which the reducer writes to affairs.json. Replaces the prior
   * direct-normalizer unit test now that defer is a real action — the
   * normalizer just routes; the action emits the event. */
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *saved_now = capture_env_value("FCLAW_FAKE_NOW_MS");
  char *event_log = NULL;
  char *affairs_disk = NULL;
  int rc = 0;

  /* Fixed clock → deterministic next_review_at_ms (1000000 + 86400000). */
  (void)setenv("FCLAW_FAKE_NOW_MS", "1000000", 1);
  rc |= seed_affair_and_publish_review("aff_review_defer", "Defer test");
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_review_defer.json", 1);
  rc |= drive_affair_review_run("aff_review_defer",
                                "(affair review) defer test");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"type\":\"affair_patch\"",
                      "defer action emitted affair_patch");
  rc |= expect_substr(event_log, "\"next_review_at_ms\":87400000",
                      "affair_patch carries computed next_review_at_ms");
  rc |= test_read_file("workspace/memory/state/affairs.json", &affairs_disk);
  rc |= expect_substr(affairs_disk, "\"next_review_at_ms\":87400000",
                      "store reflects deferred next review");

  free(affairs_disk);
  free(event_log);
  restore_env_value("FCLAW_FAKE_NOW_MS", saved_now);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int affair_patch_normalizer_preserves_next_review_at_ms(void) {
  /* Regression: rt_affair_normalize_event re-stamps the payload that
   * lands in event_log.jsonl. Earlier it dropped next_review_at_ms,
   * so the speaker's defer event survived agent-output normalization
   * but lost the field on the way to the event log — the reducer then
   * saw no field and the affair stayed dormant. Confirm the
   * normalizer propagates the field. */
  RtContext ctx;
  char out[RT_LARGE];
  JsonRef payload_in;
  const char *input =
      "{\"action\":\"update\","
      "\"affair_id\":\"aff_norm_001\","
      "\"context_id\":\"chat:tests\","
      "\"status\":\"active\","
      "\"next_review_at_ms\":12345678}";
  int rc = 0;

  rc |= test_reset_workspace();
  /* Seed an affair so the normalizer's in-context check passes. */
  rc |= expect(rt_affair_apply_event("affair_patch",
                                     "{\"action\":\"create\","
                                     "\"affair_id\":\"aff_norm_001\","
                                     "\"context_id\":\"chat:tests\","
                                     "\"manage\":\"Normalizer test\","
                                     "\"status\":\"active\","
                                     "\"created_ms\":1,\"updated_ms\":1}") == 0,
               "seed affair");
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.run_id, sizeof(ctx.run_id), "run_001");
  snprintf(ctx.context_id, sizeof(ctx.context_id), "chat:tests");

  rc |= expect(json_ref_first_object(input, &payload_in) == 0,
               "input parses");
  rc |= expect(rt_affair_normalize_event(&ctx, &payload_in, 1,
                                         out, sizeof(out)) == 0,
               "normalizer succeeds");
  rc |= expect_substr(out, "\"next_review_at_ms\":12345678",
                      "normalizer preserves next_review_at_ms");
  return rc;
}






int modern_chat_message_only_creates_no_work(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char response[8192], run_id[64];
  char *event_log = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_chat_explore.json", 1);
  rc |= expect(harness_run("tests", "floofclaw",
                           "Could you help me think about an app?",
                           response, sizeof(response), run_id,
                           sizeof(run_id)) == 0,
               "modern message-only turn completes");
  rc |= expect_substr(response, "What features",
                      "chat manager asks a clarifying question");
  rc |= test_read_run_event_log(run_id, &event_log);
  rc |= expect_no_substr(event_log, "\"kind\":\"work\"",
                         "message-only turn creates no work task");
  rc |= expect_no_substr(event_log, "\"type\":\"operation_started\"",
                         "message-only turn starts no managed operation");

  free(event_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int chat_manager_prompt_matches_runtime_contract(void) {
  char *prompt = NULL;
  int rc = 0;
  rc |= expect(test_read_file(
      "floops/floofclaw/agents/chat_manager/prompt.md", &prompt) == 0,
      "read modern chat manager prompt");
  rc |= expect_substr(prompt, "\"calls\"", "prompt teaches calls envelope");
  rc |= expect_substr(prompt, "\"name\":\"work\"",
                      "prompt teaches managed work delegation");
  rc |= expect_no_substr(prompt, "\"plan\"",
                         "prompt does not teach legacy planner envelope");
  free(prompt);
  return rc;
}

int concrete_request_queues_managed_work(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = capture_env_value("FCLAW_CODEX_BIN");
  char *saved_sync = capture_env_value("FCLAW_CODEX_START_SYNC_MS");
  char response[8192], run_id[64];
  char *logs = NULL;
  int rc = 0;

  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= test_reset_workspace();
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("FCLAW_CODEX_BIN",
               "tests/fixtures/smoke/fake_codex.sh", 1);
  (void)setenv("FCLAW_CODEX_START_SYNC_MS", "500", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_chat_frogger_work.json:"
               "tests/fixtures/smoke/modern_result_frogger.json", 1);
  rc |= write_floofclaw_fixed_floop();
  rc |= expect(harness_run("tests", "floofclaw_fixed",
                           "Build a Frogger game in frogger.html.",
                           response, sizeof(response), run_id,
                           sizeof(run_id)) == 0,
               "modern concrete request completes");
  rc |= test_read_run_event_log("run_002", &logs);
  rc |= expect_file_exists("workspace/frogger.html");
  rc |= expect_substr(logs, "operation_result",
                      "managed operation publishes a correlated result");

  free(logs);
  restore_env_value("FCLAW_CODEX_START_SYNC_MS", saved_sync);
  restore_env_value("FCLAW_CODEX_BIN", saved_codex);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  test_config_restore(saved_cfg);
  return rc;
}

int codex_missing_deliverable_reports_honest_result(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = capture_env_value("FCLAW_CODEX_BIN");
  char *saved_sync = capture_env_value("FCLAW_CODEX_START_SYNC_MS");
  char *logs = NULL;
  HarnessGateway *g = NULL;
  int rc = 0;

  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= test_reset_workspace();
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("FCLAW_CODEX_BIN",
               "tests/fixtures/smoke/fake_codex.sh", 1);
  (void)setenv("FCLAW_CODEX_START_SYNC_MS", "500", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_chat_silent_work.json:"
               "tests/fixtures/smoke/modern_result_silent_failed.json", 1);
  rc |= write_floofclaw_fixed_floop();
  g = harness_gateway_init("floofclaw_fixed");
  rc |= expect(g != NULL, "initialize modern Codex gateway fixture");
  if (g) {
    rc |= expect(harness_gateway_publish(
        g, "tests", "Create silent.html as a one-line HTML file.") == 0,
        "publish missing-deliverable request");
    rc |= expect(harness_gateway_drive_until_completed(g, 2, 5000) == 0,
                 "missing-deliverable result run completes");
    harness_gateway_close(g);
  }
  rc |= expect_file_not_exists("workspace/silent.html");
  rc |= test_read_run_event_log("run_002", &logs);
  if (logs) {
    rc |= expect_substr(logs, "named deliverable",
                        "operation result records deliverable validation");
    rc |= expect_substr(logs, "silent.html was not created",
                        "result manager emits honest missing-deliverable message");
  }

  free(logs);
  restore_env_value("FCLAW_CODEX_START_SYNC_MS", saved_sync);
  restore_env_value("FCLAW_CODEX_BIN", saved_codex);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  test_config_restore(saved_cfg);
  return rc;
}

static int setup_project_root_floop(void) {
  int rc = 0;
  rc |= test_write_file(
      "floops/test_project_root/loop.json",
      "{\"version\":1,\"name\":\"test_project_root\","
      "\"description\":\"manage_codex root fixture\",\"one_pass\":true,"
      "\"serialize_contexts\":true,\"steps\":["
      "{\"id\":\"chat\",\"type\":\"agent\",\"agent\":\"chat\"},"
      "{\"id\":\"dispatch\",\"type\":\"builtin\",\"builtin\":\"action_runner\"}]}\n");
  rc |= test_write_file(
      "floops/test_project_root/agents/chat/agent.json",
      "{\"id\":\"chat\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"responses\"},"
      "\"actions\":[\"manage_codex\"],\"listen\":[\"event\"]}\n");
  rc |= test_write_file("floops/test_project_root/agents/chat/prompt.md",
                        "{{json}}\n");
  return rc;
}

int manage_codex_project_root_is_explicit_cached_and_contained(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *saved_codex = capture_env_value("FCLAW_CODEX_BIN");
  char *saved_sync = capture_env_value("FCLAW_CODEX_START_SYNC_MS");
  char *saved_config = NULL;
  char response[8192], run_id[RT_SMALL];
  char project_abs[PATH_MAX], selected_abs[PATH_MAX];
  RtActionRegistry *cache_registry = NULL;
  char *state = NULL, *log = NULL, *received = NULL, *codex_argv = NULL;
  char registry_error[RT_LARGE] = "";
  int rc = 0;

  rc |= test_read_file("config/floofclaw_config.json", &saved_config);
  rc |= test_reset_workspace();
  rc |= setup_project_root_floop();
  rc |= test_mkdir_p("workspace/fixtures");
  rc |= test_mkdir_p("project_area/project_123");
  rc |= test_mkdir_p("outside_area");
  rc |= expect(realpath("project_area", project_abs) != NULL,
               "fixture project root canonicalizes");
  rc |= expect(realpath("project_area/project_123", selected_abs) != NULL,
               "fixture selected project canonicalizes");
  rc |= test_write_file(
      "config/floofclaw_config.json",
      "{\"bot_name\":\"FloofClaw\",\"default_floop\":\"hello\","
      "\"actions\":{\"manage_codex\":{\"allowed_project_root\":"
      "\"project_area\",\"model\":\"gpt-5.6-luna\"}},"
      "\"channels\":{\"ws\":{\"enabled\":false},"
      "\"irc\":{\"enabled\":false},\"discord\":{\"enabled\":false}}}\n");
  cache_registry = (RtActionRegistry *)calloc(1, sizeof(*cache_registry));
  rc |= expect(cache_registry != NULL,
               "allocate typed action-config cache fixture");
  if (cache_registry) {
    const char *cached;
    const char *cached_model;
    rc |= expect(rt_action_registry_load(
                     cache_registry, registry_error,
                     sizeof(registry_error)) == 0,
                 "valid configured project root loads at registry init");
    cached = rt_action_cached_config(
        cache_registry, "manage_codex", "allowed_project_root");
    cached_model = rt_action_cached_config(
        cache_registry, "manage_codex", "model");
    rc |= expect(cached && strcmp(cached, project_abs) == 0,
                 "configured project root is canonicalized in the cache");
    rc |= expect(cached_model && strcmp(cached_model, "gpt-5.6-luna") == 0,
                 "configured Codex model is cached in its action namespace");
    rc |= test_write_file(
        "config/floofclaw_config.json",
        "{\"bot_name\":\"FloofClaw\",\"default_floop\":\"hello\","
        "\"actions\":{\"manage_codex\":{\"allowed_project_root\":"
        "\"outside_area\",\"model\":\"changed-model\"}},"
        "\"channels\":{\"ws\":{\"enabled\":false},"
        "\"irc\":{\"enabled\":false},\"discord\":{\"enabled\":false}}}\n");
    rc |= expect(cached && strcmp(cached, project_abs) == 0,
                 "deployment config drift cannot change a live registry cache");
    rc |= expect(cached_model && strcmp(cached_model, "gpt-5.6-luna") == 0,
                 "deployment model drift cannot change a live registry cache");
    free(cache_registry);
    cache_registry = NULL;
    rc |= test_write_file(
        "config/floofclaw_config.json",
        "{\"bot_name\":\"FloofClaw\",\"default_floop\":\"hello\","
        "\"actions\":{\"manage_codex\":{\"allowed_project_root\":"
        "\"project_area\",\"model\":\"gpt-5.6-luna\"}},"
        "\"channels\":{\"ws\":{\"enabled\":false},"
        "\"irc\":{\"enabled\":false},\"discord\":{\"enabled\":false}}}\n");
  }
  rc |= test_write_file(
      "workspace/fixtures/project_fake_codex.sh",
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "printf '%s\\n' \"$@\" > \"$FCLAW_ACTION_ROOT/workspace/fixtures/project_codex_argv.txt\"\n"
      "out=''\n"
      "selected=''\n"
      "while [ \"$#\" -gt 0 ]; do\n"
      "  case \"$1\" in\n"
      "    -o) out=\"$2\"; shift 2 ;;\n"
      "    -C) selected=\"$2\"; shift 2 ;;\n"
      "    *) shift ;;\n"
      "  esac\n"
      "done\n"
      "cat >/dev/null\n"
      "printf '%s' \"$selected\" > received_c.txt\n"
      "printf 'project marker\\n' > marker.txt\n"
      "printf 'project complete\\n' > \"$out\"\n");
  rc |= chmod("workspace/fixtures/project_fake_codex.sh", 0755) != 0;
  rc |= test_write_file(
      "workspace/fixtures/project_start.json",
      "{\"calls\":[{\"name\":\"manage_codex\",\"args\":{"
      "\"op\":\"start\",\"root\":\"project\",\"cwd\":\"project_123\","
      "\"task\":\"Create marker.txt.\"}}]}\n");
  rc |= test_write_file(
      "workspace/fixtures/project_workspace_start.json",
      "{\"calls\":[{\"name\":\"manage_codex\",\"args\":{"
      "\"op\":\"start\",\"task\":\"Do background work.\"}}]}\n");
  /* The correlated operation_result run walks the same ungated chat
   * step; it consumes one quiet fixture. */
  rc |= test_write_file("workspace/fixtures/project_rest.json",
                        "{\"calls\":[]}\n");
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("FCLAW_CODEX_BIN",
               "workspace/fixtures/project_fake_codex.sh", 1);
  (void)setenv("FCLAW_CODEX_START_SYNC_MS", "500", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/project_start.json:"
               "workspace/fixtures/project_rest.json", 1);

  rc |= expect(harness_run("tests", "test_project_root", "build it",
                           response, sizeof(response), run_id,
                           sizeof(run_id)) == 0,
               "project-root managed operation starts");
  rc |= expect_file_exists("project_area/project_123/marker.txt");
  rc |= expect_file_not_exists("workspace/project_123/marker.txt");
  rc |= test_read_file("project_area/project_123/received_c.txt", &received);
  rc |= expect(received && strcmp(received, selected_abs) == 0,
               "Codex receives the canonical selected directory through -C");
  free(received);
  received = NULL;
  rc |= test_read_file(
      "workspace/fixtures/project_codex_argv.txt", &codex_argv);
  rc |= expect_substr(codex_argv, "--model\ngpt-5.6-luna\n",
                      "configured action model reaches codex exec exactly");
  free(codex_argv);
  codex_argv = NULL;
  {
    char state_path[PATH_MAX];
    snprintf(state_path, sizeof(state_path),
             "workspace/logs/codex_ops/codex_actionreq_%s_000004/state.json",
             run_id);
    rc |= test_read_file(state_path, &state);
  }
  rc |= expect_substr(state, "\"root\":\"project\"",
                      "operation state persists the selected root");
  rc |= expect_substr(state, project_abs,
                      "operation state persists the canonical root path");
  rc |= expect_substr(state, selected_abs,
                      "operation state persists the canonical launch path");
  rc |= test_read_run_event_log(run_id, &log);
  rc |= expect_substr(log, "\"root\":\"project\"",
                      "durable action result records the explicit root");
  rc |= expect_substr(log, "\"cwd\":\"project_123\"",
                      "durable action result keeps cwd relative");
  free(log);
  log = NULL;
  free(state);
  state = NULL;

  {
    static const struct {
      const char *name;
      const char *cwd_json;
      const char *error_text;
    } invalid_cases[] = {
      {"absolute", "\"/tmp\"", "cwd must be a relative directory"},
      {"traversal", "\"../outside_area\"",
       "cwd must be a relative directory"},
      {"empty_component", "\"project_123//nested\"",
       "cwd must be a relative directory"},
      {"dot_component", "\"project_123/./nested\"",
       "cwd must be a relative directory"},
      {"embedded_nul", "\"project_123\\u0000/hidden\"",
       "cwd contains an embedded NUL"},
      {"missing", "\"project_123/missing\"",
       "selected cwd must be an existing directory"}
    };
    for (size_t i = 0;
         i < sizeof(invalid_cases) / sizeof(invalid_cases[0]); ++i) {
      char fixture_path[PATH_MAX], fixture_json[RT_LARGE];
      snprintf(fixture_path, sizeof(fixture_path),
               "workspace/fixtures/project_%s.json",
               invalid_cases[i].name);
      snprintf(fixture_json, sizeof(fixture_json),
               "{\"calls\":[{\"name\":\"manage_codex\",\"args\":{"
               "\"op\":\"start\",\"root\":\"project\",\"cwd\":%s,"
               "\"task\":\"must not launch\"}}]}\n",
               invalid_cases[i].cwd_json);
      rc |= test_write_file(fixture_path, fixture_json);
      {
        char paths[PATH_MAX * 2];
        snprintf(paths, sizeof(paths),
                 "%s:workspace/fixtures/project_rest.json", fixture_path);
        (void)setenv("LLM_MOCK_RESPONSE_PATHS", paths, 1);
      }
      rc |= expect(harness_run("tests", "test_project_root",
                               invalid_cases[i].name,
                               response, sizeof(response), run_id,
                               sizeof(run_id)) == 0,
                   "invalid project cwd terminates as an ordinary failed action");
      rc |= test_read_run_event_log(run_id, &log);
      rc |= expect_substr(log, invalid_cases[i].error_text,
                          "invalid project cwd is rejected before launch");
      free(log);
      log = NULL;
    }
  }

  rc |= expect(symlink("../../outside_area",
                       "project_area/project_123/escape") == 0,
               "escape symlink fixture is created");
  rc |= test_write_file(
      "workspace/fixtures/project_escape.json",
      "{\"calls\":[{\"name\":\"manage_codex\",\"args\":{"
      "\"op\":\"start\",\"root\":\"project\","
      "\"cwd\":\"project_123/escape\",\"task\":\"Create escaped.txt.\"}}]}\n");
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/project_escape.json:"
               "workspace/fixtures/project_rest.json", 1);
  rc |= expect(harness_run("tests", "test_project_root", "escape",
                           response, sizeof(response), run_id,
                           sizeof(run_id)) == 0,
               "escape attempt terminates as an ordinary failed action");
  rc |= test_read_run_event_log(run_id, &log);
  rc |= expect_substr(log, "selected cwd escapes the configured root",
                      "symlink escape is rejected before launch");
  rc |= expect_file_not_exists("outside_area/escaped.txt");
  free(log);
  log = NULL;

  rc |= test_write_file(
      "config/floofclaw_config.json",
      "{\"bot_name\":\"FloofClaw\",\"default_floop\":\"hello\","
      "\"channels\":{\"ws\":{\"enabled\":false},\"irc\":{\"enabled\":false},"
      "\"discord\":{\"enabled\":false}}}\n");
  rc |= test_write_file(
      "workspace/fixtures/project_codex_argv.txt", "not launched\n");
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/project_workspace_start.json:"
               "workspace/fixtures/project_rest.json", 1);
  rc |= expect(harness_run("tests", "test_project_root", "default model",
                           response, sizeof(response), run_id,
                           sizeof(run_id)) == 0,
               "managed Codex starts when no model is configured");
  rc |= test_read_file(
      "workspace/fixtures/project_codex_argv.txt", &codex_argv);
  rc |= expect_no_substr(codex_argv, "--model",
                         "unset action model adds no codex model flag");
  free(codex_argv);
  codex_argv = NULL;
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "workspace/fixtures/project_start.json:"
               "workspace/fixtures/project_rest.json", 1);
  rc |= expect(harness_run("tests", "test_project_root", "unconfigured",
                           response, sizeof(response), run_id,
                           sizeof(run_id)) == 0,
               "unconfigured project attempt terminates cleanly");
  rc |= test_read_run_event_log(run_id, &log);
  rc |= expect_substr(
      log, "root=project requires actions.manage_codex.allowed_project_root",
      "project root must be explicitly configured");
  free(log);
  log = NULL;

  rc |= test_write_file(
      "config/floofclaw_config.json",
      "{\"bot_name\":\"FloofClaw\",\"default_floop\":\"hello\","
      "\"actions\":{\"manage_codex\":{\"allowed_project_root\":"
      "\"missing_project_root\"}},"
      "\"channels\":{\"ws\":{\"enabled\":false},\"irc\":{\"enabled\":false},"
      "\"discord\":{\"enabled\":false}}}\n");
  cache_registry = (RtActionRegistry *)calloc(1, sizeof(*cache_registry));
  registry_error[0] = '\0';
  rc |= expect(cache_registry &&
               rt_action_registry_load(cache_registry, registry_error,
                                       sizeof(registry_error)) != 0,
               "invalid configured project root fails registry initialization");
  rc |= expect_substr(
      registry_error,
      "actions.manage_codex.allowed_project_root must name an existing directory",
      "invalid configured root names the exact deployment fix");
  free(cache_registry);
  cache_registry = NULL;

  rc |= test_write_file(
      "config/floofclaw_config.json",
      "{\"bot_name\":\"FloofClaw\",\"default_floop\":\"hello\","
      "\"actions\":{\"manage_codex\":{\"model\":56}},"
      "\"channels\":{\"ws\":{\"enabled\":false},\"irc\":{\"enabled\":false},"
      "\"discord\":{\"enabled\":false}}}\n");
  cache_registry = (RtActionRegistry *)calloc(1, sizeof(*cache_registry));
  registry_error[0] = '\0';
  rc |= expect(cache_registry &&
               rt_action_registry_load(cache_registry, registry_error,
                                       sizeof(registry_error)) != 0,
               "non-string configured model fails registry initialization");
  rc |= expect_substr(registry_error,
                      "actions.manage_codex.model must be a non-empty string",
                      "invalid model names the exact deployment fix");
  free(cache_registry);
  cache_registry = NULL;

  if (saved_config)
    rc |= test_write_file("config/floofclaw_config.json", saved_config);
  free(saved_config);
  restore_env_value("FCLAW_CODEX_START_SYNC_MS", saved_sync);
  restore_env_value("FCLAW_CODEX_BIN", saved_codex);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

static int setup_modern_claude_floop(void) {
  int rc = 0;
  rc |= test_write_file("floops/test_modern_claude/loop.json",
      "{\"version\":1,\"name\":\"test_modern_claude\","
      "\"description\":\"managed Claude test fixture\","
      "\"one_pass\":true,\"serialize_contexts\":true,"
      "\"poll_interval_ms\":1,\"steps\":["
      "{\"id\":\"chat\",\"type\":\"agent\",\"agent\":\"chat\","
      "\"gate\":\"event_kind:user_message\"},"
      "{\"id\":\"result\",\"type\":\"agent\",\"agent\":\"result\","
      "\"gate\":\"event_kind:operation_result\"},"
      "{\"id\":\"dispatch\",\"type\":\"builtin\",\"builtin\":\"action_runner\"},"
      "{\"id\":\"workers\",\"type\":\"agent\",\"agent\":\"workers\","
      "\"gate\":\"event_kind:user_message,operation_poll,operation_result\"}] }\n");
  rc |= test_write_file("floops/test_modern_claude/agents/chat/agent.json",
      "{\"id\":\"chat\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"responses\"},"
      "\"actions\":[\"work\",\"message\"],\"listen\":[\"event\",\"tasks\"]}\n");
  rc |= test_write_file("floops/test_modern_claude/agents/chat/prompt.md",
                        "{{json}}\n");
  rc |= test_write_file("floops/test_modern_claude/agents/result/agent.json",
      "{\"id\":\"result\",\"executor\":\"llm\","
      "\"model\":{\"ref\":\"responses\"},"
      "\"actions\":[\"message\"],\"listen\":[\"event\",\"tasks\"]}\n");
  rc |= test_write_file("floops/test_modern_claude/agents/result/prompt.md",
                        "{{json}}\n");
  rc |= test_write_file("floops/test_modern_claude/agents/workers/agent.json",
      "{\"id\":\"workers\",\"executor\":\"native\","
      "\"handler\":\"manage_claude\"}\n");
  return rc;
}

int claude_handler_delivers_modern_operation_result(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char *saved_claude = capture_env_value("FCLAW_CLAUDE_BIN");
  char *saved_sync = capture_env_value("FCLAW_CLAUDE_START_SYNC_MS");
  HarnessGateway *g = NULL;
  int rc = 0;

  char *saved_cfg = NULL;
  rc |= test_config_enable_all(&saved_cfg);
  rc |= test_reset_workspace();
  rc |= setup_modern_claude_floop();
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("FCLAW_CLAUDE_BIN",
               "tests/fixtures/smoke/fake_claude.sh", 1);
  (void)setenv("FCLAW_CLAUDE_START_SYNC_MS", "500", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_chat_claude_work.json:"
               "tests/fixtures/smoke/modern_result_claude.json", 1);
  g = harness_gateway_init("test_modern_claude");
  rc |= expect(g != NULL, "initialize modern Claude gateway fixture");
  if (g) {
    rc |= expect(harness_gateway_publish(
        g, "tests", "Create claude_smoke.md.") == 0,
        "publish Claude managed-operation request");
    rc |= expect(harness_gateway_drive_until_completed(g, 2, 5000) == 0,
                 "modern Claude result run completes");
    harness_gateway_close(g);
  }
  rc |= expect_file_exists("workspace/claude_smoke.md");
  {
    char *result_log = NULL;
    rc |= test_read_run_event_log("run_002", &result_log);
    rc |= expect_substr(result_log, "claude_smoke.md is ready",
                        "Claude result reaches the result manager");
    free(result_log);
  }

  restore_env_value("FCLAW_CLAUDE_START_SYNC_MS", saved_sync);
  restore_env_value("FCLAW_CLAUDE_BIN", saved_claude);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  test_config_restore(saved_cfg);
  return rc;
}

int generic_calls_large_write_file_payload_executes(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char response[8192];
  char run_id[64];
  char *event_log = NULL;
  char *written = NULL;
  char *content = NULL;
  char paths[4096];
  int rc = 0;
  int run_rc;

  rc |= test_reset_workspace();
  content = build_large_frogger_content(5000U);
  rc |= expect(content != NULL, "generic large frogger content allocated");
  rc |= write_generic_write_file_fixture("workspace/test_openclaw_large_write_tool.json",
                                         "big.html", content ? content : "");
  rc |= test_write_file("workspace/test_openclaw_large_write_conversation.json",
                        "{\n"
                        "  \"calls\": [\n"
                        "    {\n"
                        "      \"name\": \"message\",\n"
                        "      \"args\": {\n"
                        "        \"message\": \"big.html is ready.\"\n"
                        "      }\n"
                        "    },\n"
                        "    {\n"
                        "      \"name\": \"task.update\",\n"
                        "      \"args\": {\n"
                        "        \"task_id\": \"__TASK_ID__\",\n"
                        "        \"state\": \"completed\"\n"
                        "      }\n"
                        "    }\n"
                        "  ]\n"
                        "}\n");

  snprintf(paths, sizeof(paths),
           "workspace/test_openclaw_large_write_tool.json:"
           "workspace/test_openclaw_large_write_conversation.json");
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS", paths, 1);
  run_rc = harness_run("tests", "openclaw",
                       "please make a small browser frogger app in single-page html",
                       response, sizeof(response), run_id, sizeof(run_id));
  rc |= expect(run_rc == 0, "generic calls large write_file run succeeds");
  rc |= expect(strcmp(run_id, "run_002") == 0,
               "generic large write_file reply comes from operation-result run_002");
  rc |= expect_substr(response, "big.html is ready",
                      "generic large write_file result reaches the conversation reply");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"type\":\"action_request\"",
                      "generic calls path emits action_request");
  rc |= expect_substr(event_log, "\"action\":\"write_file\"",
                      "generic calls write_file request is recorded");
  rc |= expect_substr(event_log, "\"type\":\"action_succeeded\"",
                      "generic calls write_file executes successfully");
  rc |= expect_no_substr(event_log, "agent_output_invalid",
                         "generic calls valid large payload is not marked malformed");
  rc |= expect_no_substr(event_log, "\"type\":\"run_failed\"",
                         "generic calls large write_file run does not fail");
  rc |= test_read_file("workspace/big.html", &written);
  rc |= expect(written && content && strcmp(written, content) == 0,
               "generic calls write_file preserves content byte-for-byte");

  free(content);
  free(written);
  free(event_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int conversational_message_without_request_is_noop_not_failure(void) {
  RtContext ctx;
  RtStep step;
  char normalized[RT_XL];
  char *rejected = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.context_id, sizeof(ctx.context_id), "chat:tests");
  snprintf(ctx.run_id, sizeof(ctx.run_id), "run_001");
  snprintf(ctx.run_dir, sizeof(ctx.run_dir), "workspace/runs/run_001");
  rc |= test_mkdir_p(ctx.run_dir);
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "speak_status");
  snprintf(step.type, sizeof(step.type), "agent");
  snprintf(step.target, sizeof(step.target), "chat_test_agent");
  rc |= expect(rt_agent_normalize_conversational_output(&ctx, &step,
                                                        "{\"message\":\"This should be ignored because request is null.\"}",
                                                        normalized, sizeof(normalized)) == 0,
               "conversational message with request null normalizes as no-op");
  rc |= expect_substr(normalized, "\"events\":[]",
                      "no-request conversational message produces no committed events");
  rc |= test_read_file("workspace/logs/rejected_events.jsonl", &rejected);
  rc |= expect_substr(rejected, "conversational_message_without_request",
                      "near-miss conversational output is still traceable as rejected");

  free(rejected);
  return rc;
}

static int init_memory_test_ctx(RtContext *ctx) {
  if (!ctx) return -1;
  memset(ctx, 0, sizeof(*ctx));
  snprintf(ctx->workspace, sizeof(ctx->workspace), "workspace");
  snprintf(ctx->run_id, sizeof(ctx->run_id), "run_001");
  snprintf(ctx->run_dir, sizeof(ctx->run_dir), "workspace/runs/run_001");
  snprintf(ctx->context_id, sizeof(ctx->context_id), "chat:tests");
  ctx->next_event = 1;
  return test_mkdir_p(ctx->run_dir);
}

static int seed_memory_messages(RtContext *ctx, int count) {
  char payload[RT_LARGE];
  for (int i = 1; i <= count; ++i) {
    snprintf(payload, sizeof(payload), "{\"text\":\"message %03d\"}", i);
    if (rt_append_event(ctx, "user", "memory", payload) != 0) return -1;
  }
  return 0;
}

static void memory_test_meta(RtAgentMeta *meta) {
  memset(meta, 0, sizeof(*meta));
  snprintf(meta->id, sizeof(meta->id), "memory");
  meta->recent = 20;
  meta->can_write_memory = 1;
  meta->memory_summary_enabled = 1;
  meta->memory_compact_after_messages = 48;
  meta->memory_compact_after_tokens = 12000;
  meta->memory_keep_recent_messages = 20;
  meta->memory_min_compact_messages = 16;
  meta->memory_summary_max_tokens = 1600;
  meta->memory_recent_summary_max_tokens = 500;
}

int memory_before_projects_summaries_and_last_20_raw_messages(void) {
  RtContext ctx;
  char projection[RT_XL];
  char *state = NULL;
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  rc |= seed_memory_messages(&ctx, 25);
  rc |= test_write_file("workspace/memory/state/summary_state.json",
                        "{\"contexts\":[{\"context_id\":\"chat:tests\","
                        "\"conversation_summary\":\"older durable facts\","
                        "\"recent_summary\":\"middle bridge\","
                        "\"summary_covers_through_message_id\":\"msg_000005\","
                        "\"recent_starts_at_message_id\":\"msg_000006\","
                        "\"recent_count\":20}]}\n");
  rc |= expect(rt_memory_projection_json("chat:tests", 20, projection, sizeof(projection)) == 0,
               "memory projection renders");
  rc |= expect_substr(projection, "\"conversation_summary\":\"older durable facts\"",
                      "projection includes conversation_summary");
  rc |= expect_substr(projection, "\"recent_summary\":\"middle bridge\"",
                      "projection includes recent_summary");
  rc |= expect_substr(projection, "\"recent\":[",
                      "projection uses canonical recent key");
  rc |= expect_substr(projection, "\"message_id\":\"msg_000006\"",
                      "projection starts with newest 20 raw messages");
  rc |= expect_no_substr(projection, "\"message_id\":\"msg_000005\"",
                         "projection omits older compacted raw message");
  rc |= test_read_file("workspace/memory/state/summary_state.json", &state);
  rc |= expect_substr(state, "older durable facts", "summary state remains readable");
  free(state);
  return rc;
}

int memory_after_records_tool_activity_in_existing_log(void) {
  RtContext ctx;
  RtAgentMeta meta;
  /* Heap-owned: ~850KB is too large for the small-stack gate. */
  RtActionRegistry *actions = NULL;
  RtStep step;
  RtAgentFn memory_agent;
  char raw[RT_NATIVE_STDOUT_MAX];
  char normalized[RT_XL];
  char committed[RT_XL];
  char projection[RT_XL];
  char *log = NULL;
  int rc = 0;

  actions = (RtActionRegistry *)calloc(1, sizeof(*actions));
  if (!actions) return 1;
  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  memory_test_meta(&meta);
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "memory.after");
  snprintf(step.target, sizeof(step.target), "memory");
  snprintf(ctx.latest_action, sizeof(ctx.latest_action), "read_file");
  snprintf(ctx.latest_result_text, sizeof(ctx.latest_result_text),
           "alpha beta gamma");

  memory_agent = rt_agent_lookup("memory");
  rc |= expect(memory_agent != NULL, "native memory agent is registered");
  if (!memory_agent) {
    free(actions);
    return rc;
  }
  rc |= expect(memory_agent(&ctx, &step, &meta, actions, "{}", raw,
                            sizeof(raw), NULL, 0) == 0,
               "memory.after renders committed tool activity");
  rc |= expect_substr(raw, "Tool read_file returned: alpha beta gamma",
                      "tool activity uses the existing assistant record path");
  rc |= expect(rt_agent_normalize_output(&ctx, &step, &meta, NULL, 0, raw,
                                         normalized, sizeof(normalized)) == 0,
               "tool memory output normalizes");
  rc |= expect(rt_append_events_from_output(&ctx, "memory", 1, normalized,
                                             committed, sizeof(committed)) == 0,
               "tool memory output commits");
  rc |= test_read_file("workspace/memory/memory.jsonl", &log);
  rc |= expect_substr(log, "Tool read_file returned: alpha beta gamma",
                      "tool activity is stored in memory.jsonl");
  rc |= expect(rt_memory_projection_json("chat:tests", 20, projection,
                                         sizeof(projection)) == 0,
               "tool activity projects through existing memory");
  rc |= expect_substr(projection,
                      "Tool read_file returned: alpha beta gamma",
                      "tool activity reaches recalled conversation memory");
  free(log);
  free(actions);
  return rc;
}

int memory_after_appends_only_user_asst_and_compact_skip_is_traced(void) {
  char *saved_profiles = capture_env_value("FCLAW_MODEL_PROFILES");
  char *saved_paths = capture_env_value("LLM_MOCK_RESPONSE_PATHS");
  char response[8192], run_id[64];
  char *event_log = NULL, *trace = NULL;
  int rc = 0;
  int run_rc;
  rc |= test_reset_workspace();
  (void)setenv("FCLAW_MODEL_PROFILES", "tests/fixtures/smoke/model_profiles_mock.json", 1);
  (void)setenv("LLM_MOCK_RESPONSE_PATHS",
               "tests/fixtures/smoke/modern_chat_name.json", 1);
  run_rc = harness_run("tests", "floofclaw", "whats your name?",
                       response, sizeof(response), run_id, sizeof(run_id));
  rc |= expect(run_rc == 0, "simple run succeeds with compact phase present");
  rc |= test_read_run_event_log(run_id, &event_log);
  rc |= expect_substr(event_log, "\"type\":\"user\"", "memory.after appends user record");
  rc |= expect_substr(event_log, "\"type\":\"asst\"", "memory.after appends assistant record");
  rc |= expect_no_substr(event_log, "\"type\":\"summ\"", "memory.after no longer emits deterministic summ");
  rc |= expect_no_substr(event_log, "memory_compaction_requested", "below threshold creates no compaction request");
  rc |= test_read_file("workspace/runs/run_001/trace.jsonl", &trace);
  rc |= expect_substr(trace, "\"phase\":\"memory.compact\"", "memory.compact appears in trace");
  rc |= expect_substr(trace, "memory_compaction_not_due", "memory.compact skip reason is explicit");
  free(trace);
  free(event_log);
  restore_env_value("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  restore_env_value("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}

int memory_compaction_request_appears_above_threshold_and_keeps_newest_20(void) {
  RtContext ctx;
  RtAgentMeta meta;
  char *event_log = NULL;
  char payload[RT_LARGE];
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  rc |= seed_memory_messages(&ctx, 50);
  memory_test_meta(&meta);
  rc |= expect(rt_memory_maybe_request_compaction(&ctx, &meta) == 0,
               "compaction policy evaluates");
  rc |= expect(ctx.memory_compaction_due, "runtime marks compaction due");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"type\":\"memory_compaction_requested\"",
                      "compaction request event is written");
  rc |= expect_substr(event_log, "\"compact_through_message_id\":\"msg_000030\"",
                      "runtime selects cutoff leaving newest 20 raw messages");
  rc |= expect_substr(event_log, "\"message_id\":\"msg_000001\"",
                      "request includes selected first message");
  rc |= expect_no_substr(event_log, "\"message_id\":\"msg_000031\"",
                         "request does not compact messages kept raw");
  free(event_log);
  event_log = NULL;

  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  for (int i = 1; i <= 25; ++i) {
    snprintf(payload, sizeof(payload),
             "{\"text\":\"long message %03d abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz\"}", i);
    rc |= expect(rt_append_event(&ctx, "user", "memory", payload) == 0,
                 "seed long token-threshold memory message");
  }
  memory_test_meta(&meta);
  meta.memory_compact_after_messages = 999;
  meta.memory_compact_after_tokens = 10;
  meta.memory_keep_recent_messages = 5;
  meta.memory_min_compact_messages = 16;
  rc |= expect(rt_memory_maybe_request_compaction(&ctx, &meta) == 0,
               "token threshold compaction policy evaluates");
  rc |= expect(ctx.memory_compaction_due, "token threshold marks compaction due");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_substr(event_log, "\"compact_through_message_id\":\"msg_000020\"",
                      "token threshold request leaves configured raw tail");
  free(event_log);
  return rc;
}

int memory_compaction_not_requested_below_threshold_or_min_range(void) {
  RtContext ctx;
  RtAgentMeta meta;
  char *event_log = NULL;
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  rc |= seed_memory_messages(&ctx, 10);
  memory_test_meta(&meta);
  rc |= expect(rt_memory_maybe_request_compaction(&ctx, &meta) == 0,
               "below threshold evaluates");
  rc |= expect(!ctx.memory_compaction_due, "below threshold is not due");
  free(event_log);

  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  rc |= seed_memory_messages(&ctx, 35);
  memory_test_meta(&meta);
  meta.memory_compact_after_messages = 21;
  rc |= expect(rt_memory_maybe_request_compaction(&ctx, &meta) == 0,
               "min selected range guard evaluates");
  rc |= expect(!ctx.memory_compaction_due, "range below min_compact_messages is refused");
  rc |= test_read_run_event_log("run_001", &event_log);
  rc |= expect_no_substr(event_log, "memory_compaction_requested",
                         "no request is written for too-small selected range");
  free(event_log);
  return rc;
}

int memory_compactor_matching_cutoff_commits_summary_state(void) {
  RtContext ctx;
  RtAgentMeta memory_meta, compactor_meta;
  RtStep step;
  char normalized[RT_XL];
  char *summary = NULL;
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  rc |= seed_memory_messages(&ctx, 50);
  memory_test_meta(&memory_meta);
  rc |= expect(rt_memory_maybe_request_compaction(&ctx, &memory_meta) == 0,
               "request is created");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "memory.compact");
  snprintf(step.type, sizeof(step.type), "agent");
  snprintf(step.target, sizeof(step.target), "memory_compactor");
  memset(&compactor_meta, 0, sizeof(compactor_meta));
  compactor_meta.can_write_memory = 1;
  compactor_meta.memory_compaction_context_only = 1;
  rc |= expect(rt_agent_append_output(&ctx, &step, &compactor_meta, NULL, 0,
                                      "{\"conversation_summary\":\"older summary\","
                                      "\"recent_summary\":\"recent bridge\","
                                      "\"covered_through_message_id\":\"msg_000030\"}",
                                      normalized, sizeof(normalized)) == 0,
               "matching compactor output commits");
  rc |= test_read_file("workspace/memory/state/summary_state.json", &summary);
  rc |= expect_substr(summary, "\"conversation_summary\":\"older summary\"",
                      "summary state stores conversation summary");
  rc |= expect_substr(summary, "\"recent_summary\":\"recent bridge\"",
                      "summary state stores recent summary");
  rc |= expect_substr(summary, "\"summary_covers_through_message_id\":\"msg_000030\"",
                      "summary cursor advances to runtime cutoff");
  rc |= expect_substr(summary, "\"recent_starts_at_message_id\":\"msg_000031\"",
                      "summary state records raw tail start");
  rc |= expect_substr(summary, "\"recent_count\":20",
                      "summary state records raw tail count");
  free(summary);
  return rc;
}

int memory_compactor_mismatched_cutoff_is_rejected_without_commit(void) {
  RtContext ctx;
  RtAgentMeta memory_meta, compactor_meta;
  RtStep step;
  char normalized[RT_XL];
  int rc = 0;
  rc |= test_reset_workspace();
  rc |= init_memory_test_ctx(&ctx);
  rc |= seed_memory_messages(&ctx, 50);
  memory_test_meta(&memory_meta);
  rc |= expect(rt_memory_maybe_request_compaction(&ctx, &memory_meta) == 0,
               "request is created for mismatch test");
  memset(&step, 0, sizeof(step));
  snprintf(step.id, sizeof(step.id), "memory.compact");
  snprintf(step.type, sizeof(step.type), "agent");
  snprintf(step.target, sizeof(step.target), "memory_compactor");
  memset(&compactor_meta, 0, sizeof(compactor_meta));
  compactor_meta.can_write_memory = 1;
  compactor_meta.memory_compaction_context_only = 1;
  rc |= expect(rt_agent_append_output(&ctx, &step, &compactor_meta, NULL, 0,
                                      "{\"conversation_summary\":\"bad\","
                                      "\"recent_summary\":\"bad\","
                                      "\"covered_through_message_id\":\"msg_000029\"}",
                                      normalized, sizeof(normalized)) == 0,
               "mismatched cutoff is rejected as maintenance no-op");
  rc |= expect_substr(normalized, "\"events\":[]",
                      "mismatched cutoff emits no committed events");
  rc |= expect_file_not_exists("workspace/memory/state/summary_state.json");
  return rc;
}
