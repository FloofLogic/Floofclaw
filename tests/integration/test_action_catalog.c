#include "../test_support.h"
#include "../harness.h"

#include "../../runtime/runtime.h"
#include "../../runtime/runtime_kernel.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A1_FIXTURE_FLOOP "action20_catalog"
#define A1_FIXTURE_AGENT "catalog_agent"
#define A1_FIXTURE_AREA  "actions/action20fx"

static const char A1_COMPLEX_SCHEMA[] =
    "{\"type\":\"object\",\"required\":[\"op\",\"calendar\"],"
    "\"additionalProperties\":false,\"properties\":{"
    "\"op\":{\"type\":\"string\",\"enum\":[\"create\",\"list\"],"
    "\"description\":\"Operation.\"},"
    "\"calendar\":{\"type\":\"object\",\"required\":[\"id\"],"
    "\"additionalProperties\":false,\"properties\":{"
    "\"id\":{\"type\":\"string\",\"description\":\"Calendar id.\"},"
    "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\","
    "\"enum\":[\"work\",\"home\"]}}}},"
    "\"attendees\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
    "\"required\":[\"email\"],\"additionalProperties\":false,"
    "\"properties\":{\"email\":{\"type\":\"string\"}}}},"
    "\"note\":{\"type\":\"string\",\"description\":\"Optional note.\"}}}";

static const char *a1_capture_env(const char *name) {
  const char *value = getenv(name);
  return value ? strdup(value) : NULL;
}

static void a1_restore_env(const char *name, const char *saved) {
  if (saved) {
    (void)setenv(name, saved, 1);
    free((void *)saved);
  } else {
    (void)unsetenv(name);
  }
}

static int a1_action_id(size_t index, char *out, size_t out_len) {
  int n;
  if (!out || out_len == 0 || index >= RT_MAX_ACTIONS) return -1;
  if (index == 0) n = snprintf(out, out_len, "gcal");
  else if (index == 1) n = snprintf(out, out_len, "manage_codex");
  else if (index == 2) n = snprintf(out, out_len, "message");
  else n = snprintf(out, out_len, "action20_%02zu", index);
  return n < 0 || (size_t)n >= out_len ? -1 : 0;
}

static int a1_write_action(size_t index, const char *schema) {
  char id[RT_SMALL], path[512], body[RT_LARGE * 2];
  const char *description;
  int n;
  if (a1_action_id(index, id, sizeof(id)) != 0) return -1;
  if (index == 0) description = "Calendar-specific action.";
  else if (index == 1) description = "Complex iterative Codex work.";
  else if (index == 2) description = "Send a user-visible message.";
  else description = "Action20 registry-capacity fixture.";
  snprintf(path, sizeof(path), "%s/%s/action.json", A1_FIXTURE_AREA, id);
  n = snprintf(body, sizeof(body),
               "{\n"
               "  \"id\":\"%s\",\n"
               "  \"description\":\"%s\",\n"
               "  \"outside_world\":false,\n"
               "  \"args_schema\":%s,\n"
               "  \"exec\":[\"runtime\",\"fc_message()\"],\n"
               "  \"timeout_ms\":0\n"
               "}\n",
               id, description, schema);
  if (n < 0 || (size_t)n >= sizeof(body)) return -1;
  return test_write_file(path, body);
}

static int a1_write_actions(int oversized) {
  static const char SIMPLE_SCHEMA[] =
      "{\"type\":\"object\",\"required\":[],\"additionalProperties\":false,"
      "\"properties\":{}}";
  char large_description[2201];
  char large_schema[RT_LARGE];
  const char *schema;
  int rc = 0;
  memset(large_description, 'x', sizeof(large_description) - 1);
  large_description[sizeof(large_description) - 1] = '\0';
  if (snprintf(large_schema, sizeof(large_schema),
               "{\"type\":\"object\",\"required\":[],"
               "\"additionalProperties\":false,\"properties\":{"
               "\"payload\":{\"type\":\"string\",\"description\":\"%s\"}}}",
               large_description) >= (int)sizeof(large_schema))
    return -1;
  for (size_t i = 0; i < RT_MAX_ACTIONS; ++i) {
    schema = oversized ? large_schema : (i == 0 ? A1_COMPLEX_SCHEMA : SIMPLE_SCHEMA);
    rc |= a1_write_action(i, schema);
  }
  return rc;
}

/* This fixture fills the registry to RT_MAX_ACTIONS by itself and reuses the
 * ids gcal/manage_codex/message, so the shipped actions must not be scanned
 * alongside it. Discovery is the directory tree, so unloading them is a
 * rename: a leading underscore keeps a directory in the tree and out of the
 * scan. Safe to do in place because the suite runs in an isolated copy
 * (tests/test_runner.c: require_isolated_test_root). */
static const char *const A1_SHIPPED_AREAS[] = {
  "common", "core"
};

static int a1_set_shipped_areas_loaded(int loaded) {
  int rc = 0;
  for (size_t i = 0; i < sizeof(A1_SHIPPED_AREAS) / sizeof(A1_SHIPPED_AREAS[0]); ++i) {
    char live[256], parked[256];
    snprintf(live, sizeof(live), "actions/%s", A1_SHIPPED_AREAS[i]);
    snprintf(parked, sizeof(parked), "actions/_%s", A1_SHIPPED_AREAS[i]);
    /* A missing source is not a failure: restore also runs if setup
     * half-ran in an intentionally reduced fixture. */
    if (rename(loaded ? parked : live, loaded ? live : parked) != 0 &&
        errno != ENOENT)
      rc = -1;
  }
  return rc;
}

static int a1_write_floop(void) {
  int rc = 0;
  rc |= test_write_file(
      "floops/" A1_FIXTURE_FLOOP "/loop.json",
      "{\n"
      "  \"version\":1,\n"
      "  \"name\":\"" A1_FIXTURE_FLOOP "\",\n"
      "  \"description\":\"Action20 catalog contract fixture.\",\n"
      "  \"one_pass\":true,\n"
      "  \"steps\":[\n"
      "    {\"id\":\"catalog\",\"type\":\"agent\",\"agent\":\""
      A1_FIXTURE_AGENT "\"},\n"
      "    {\"id\":\"dispatch\",\"type\":\"builtin\","
      "\"builtin\":\"action_runner\"}\n"
      "  ]\n"
      "}\n");
  rc |= test_write_file(
      "floops/" A1_FIXTURE_FLOOP "/agents/" A1_FIXTURE_AGENT "/prompt.md",
      "Configured action catalog:\n{{tools}}\n"
      "Current input:\n{{json}}\n"
      "Advance only through the configured action catalog.\n");
  rc |= test_write_file(
      "floops/" A1_FIXTURE_FLOOP "/empty_response.json",
      "{\"calls\":[]}\n");
  rc |= test_write_file(
      "floops/" A1_FIXTURE_FLOOP "/disallowed_response.json",
      "{\"calls\":[{\"name\":\"message\",\"args\":{}}]}\n");
  return rc;
}

static int a1_write_agent(const char *const *ids, size_t count) {
  char body[RT_LARGE];
  size_t pos = 0;
  int n;
  n = snprintf(body + pos, sizeof(body) - pos,
               "{\n"
               "  \"id\":\"" A1_FIXTURE_AGENT "\",\n"
               "  \"executor\":\"llm\",\n"
               "  \"model\":{\"ref\":\"responses\"},\n"
               "  \"actions\":[");
  if (n < 0 || (size_t)n >= sizeof(body) - pos) return -1;
  pos += (size_t)n;
  for (size_t i = 0; i < count; ++i) {
    n = snprintf(body + pos, sizeof(body) - pos,
                 "%s\"%s\"", i ? "," : "", ids[i]);
    if (n < 0 || (size_t)n >= sizeof(body) - pos) return -1;
    pos += (size_t)n;
  }
  n = snprintf(body + pos, sizeof(body) - pos,
               "],\n  \"listen\":[\"event\"]\n}\n");
  if (n < 0 || (size_t)n >= sizeof(body) - pos) return -1;
  return test_write_file(
      "floops/" A1_FIXTURE_FLOOP "/agents/" A1_FIXTURE_AGENT "/agent.json",
      body);
}

static int a1_write_full_agent(void) {
  char ids[RT_MAX_ACTIONS][RT_SMALL];
  const char *list[RT_MAX_ACTIONS];
  for (size_t i = 0; i < RT_MAX_ACTIONS; ++i) {
    if (a1_action_id(i, ids[i], sizeof(ids[i])) != 0) return -1;
    list[i] = ids[i];
  }
  return a1_write_agent(list, RT_MAX_ACTIONS);
}

static int a1_scheduler_init(RtScheduler **out, char *err, size_t err_len) {
  RtScheduler *scheduler;
  int init_rc;
  if (out) *out = NULL;
  scheduler = (RtScheduler *)calloc(1, sizeof(*scheduler));
  if (!scheduler) return -1;
  if (err && err_len) err[0] = '\0';
  init_rc = rt_scheduler_init(scheduler, A1_FIXTURE_FLOOP, err, err_len);
  if (out) *out = scheduler;
  else {
    rt_scheduler_destroy(scheduler);
    free(scheduler);
  }
  return init_rc;
}

static void a1_scheduler_close(RtScheduler *scheduler) {
  if (!scheduler) return;
  rt_scheduler_destroy(scheduler);
  free(scheduler);
}

static int a1_run(const char *response_path, char **request_out,
                  char *run_id, size_t run_id_len) {
  char response[8192];
  int run_rc;
  if (request_out) *request_out = NULL;
  if (test_reset_workspace() != 0) return -1;
  (void)setenv("LLM_MOCK_RESPONSE_PATHS", response_path, 1);
  run_rc = harness_run("tests", A1_FIXTURE_FLOOP, "catalog proof",
                       response, sizeof(response), run_id, run_id_len);
  if (request_out && run_id && run_id[0]) {
    char path[512];
    snprintf(path, sizeof(path),
             "workspace/runs/%s/provider_calls/001_request.json", run_id);
    if (test_read_file(path, request_out) != 0) return -1;
  }
  return run_rc;
}

int configured_action_catalog_is_ordered_faithful_growable_and_loud(void) {
  const char *saved_profiles = a1_capture_env("FCLAW_MODEL_PROFILES");
  const char *saved_paths = a1_capture_env("LLM_MOCK_RESPONSE_PATHS");
  const char *ordered[] = { "gcal", "manage_codex", "message" };
  const char *swapped[] = { "message", "manage_codex", "gcal" };
  const char *duplicate[] = { "gcal", "gcal" };
  const char *unknown[] = { "gcal", "missing_action20" };
  const char *only_gcal[] = { "gcal" };
  const char *all_only[] = { "all_actions" };
  const char *gcal_and_all[] = { "gcal", "all_actions" };
  char *request = NULL;
  char *event_log = NULL;
  char run_id[RT_SMALL] = "";
  char err[RT_LARGE] = "";
  int fixtures_installed = 0;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= test_remove_path(A1_FIXTURE_AREA);
  rc |= test_remove_path("floops/" A1_FIXTURE_FLOOP);
  fixtures_installed = 1;
  rc |= expect(a1_set_shipped_areas_loaded(0) == 0,
               "shipped action areas unload for the capacity fixture");
  rc |= a1_write_actions(0);
  rc |= a1_write_floop();
  (void)setenv("FCLAW_MODEL_PROFILES",
               "tests/fixtures/smoke/model_profiles_mock.json", 1);

  /* Config order, descriptions, and the exact source schema reach the
   * provider request without depending on registry scan order. */
  rc |= a1_write_agent(ordered, sizeof(ordered) / sizeof(ordered[0]));
  rc |= expect(a1_run("floops/" A1_FIXTURE_FLOOP "/empty_response.json",
                      &request, run_id, sizeof(run_id)) == 0,
               "ordered catalog fixture run completes");
  if (request) {
    const char *gcal = strstr(request, "\"name\": \"gcal\"");
    const char *codex = strstr(request, "\"name\": \"manage_codex\"");
    const char *message = strstr(request, "\"name\": \"message\"");
    rc |= expect(gcal && codex && message && gcal < codex && codex < message,
                 "provider request preserves configured gcal/codex/message order");
    rc |= expect(test_count_substr(request, "\"name\": \"gcal\"") == 1 &&
                 test_count_substr(request, "\"name\": \"manage_codex\"") == 1 &&
                 test_count_substr(request, "\"name\": \"message\"") == 1,
                 "provider request presents every configured action exactly once");
    rc |= expect_substr(request, "Calendar-specific action.",
                        "provider request preserves manifest description");
    rc |= expect_substr(request, A1_COMPLEX_SCHEMA,
                        "provider request preserves the complete source args schema");
  }
  free(request);
  request = NULL;

  /* all_actions is both the full grant and an in-place presentation entry.
   * The provider receives every authoritative action contract. */
  rc |= a1_write_agent(all_only,
                       sizeof(all_only) / sizeof(all_only[0]));
  {
    RtScheduler *scheduler = NULL;
    char *bootstrap = (char *)malloc(RT_XL);
    char *allowed = (char *)malloc(RT_XL);
    rc |= test_reset_workspace();
    rc |= expect(bootstrap && allowed,
                 "allocate numeric action authorization catalogs");
    rc |= expect(a1_scheduler_init(&scheduler, err, sizeof(err)) == 0,
                 "all_actions sentinel loads without a registry entry");
    if (scheduler && scheduler->agent_meta_count == 1 && bootstrap && allowed) {
      const RtAgentMeta *meta = &scheduler->agent_meta[0];
      const RtActionDef *gcal = rt_action_registry_find(&scheduler->actions,
                                                        "gcal");
      const RtActionDef *last = rt_action_registry_find(
          &scheduler->actions, "action20_63");
      for (size_t i = 0; i < scheduler->actions.count; ++i)
        rc |= expect(scheduler->actions.defs[i].runtime_id == i,
                     "loaded action slot is its process-local numeric id");
      rc |= expect(meta->action_count == 1 &&
                   meta->action_ids[0] == RT_ACTION_PRESENT_ALL &&
                   meta->action_allow_mask == UINT64_MAX,
                   "all_actions retains one presentation entry and fills the grant mask");
      rc |= expect(gcal && last &&
                   rt_agent_allows_action(meta, gcal) &&
                   rt_agent_allows_action(meta, last),
                   "authorization uses the resolved grant mask");
      rc |= expect(rt_action_render_available(&scheduler->actions, meta,
                                              bootstrap, RT_XL) == 0,
                   "all_actions bootstrap catalog renders completely");
      rc |= expect_substr(bootstrap, "\"name\": \"gcal\"",
                          "all_actions presents the first registered action");
      rc |= expect_substr(bootstrap, "\"name\": \"action20_63\"",
                          "all_actions presents the last registered action");
      rc |= expect(test_count_substr(bootstrap, "\"name\": \"gcal\"") == 1,
                   "all_actions presents each registry action once by itself");
      rc |= expect(rt_action_render_allowed_ids(&scheduler->actions, meta,
                                                allowed, RT_XL) == 0,
                   "discovery renders the full allowed mask");
      rc |= expect_substr(allowed, "\"action20_63\"",
                          "discovery includes a wildcard-granted action");
    }
    free(allowed);
    free(bootstrap);
    a1_scheduler_close(scheduler);
  }
  run_id[0] = '\0';
  rc |= expect(a1_run("floops/" A1_FIXTURE_FLOOP "/empty_response.json",
                      &request, run_id, sizeof(run_id)) == 0,
               "all_actions provider-boundary fixture run completes");
  if (request) {
    rc |= expect_substr(request, "\"name\": \"gcal\"",
                        "all_actions reaches the provider with the first contract");
    rc |= expect_substr(request, "\"name\": \"action20_63\"",
                        "all_actions reaches the provider with the last contract");
    rc |= expect_substr(request, "Action20 registry-capacity fixture.",
                        "all_actions reaches the provider with descriptions");
  }
  free(request);
  request = NULL;

  /* Rendering is intentionally dumb and ordered. An explicit entry followed
   * by all_actions appears once explicitly and again inside the expansion;
   * removing that overlap is the agent configuration's responsibility. */
  rc |= a1_write_agent(gcal_and_all,
                       sizeof(gcal_and_all) / sizeof(gcal_and_all[0]));
  {
    RtScheduler *scheduler = NULL;
    char *bootstrap = (char *)malloc(RT_XL);
    rc |= test_reset_workspace();
    rc |= expect(bootstrap != NULL,
                 "allocate overlap-preserving rendered catalog");
    rc |= expect(a1_scheduler_init(&scheduler, err, sizeof(err)) == 0,
                 "explicit action plus all_actions loads");
    if (scheduler && scheduler->agent_meta_count == 1 && bootstrap) {
      const RtAgentMeta *meta = &scheduler->agent_meta[0];
      const RtActionDef *gcal = rt_action_registry_find(&scheduler->actions,
                                                        "gcal");
      rc |= expect(gcal && meta->action_count == 2 &&
                   meta->action_ids[0] == gcal->runtime_id &&
                   meta->action_ids[1] == RT_ACTION_PRESENT_ALL,
                   "presentation order retains explicit then all_actions");
      rc |= expect(rt_action_render_available(&scheduler->actions, meta,
                                              bootstrap, RT_XL) == 0,
                   "overlapping presentation catalog renders");
      rc |= expect(test_count_substr(bootstrap,
                                     "\"name\": \"gcal\"") == 2,
                   "renderer does not deduplicate explicit wildcard overlap");
      rc |= expect(test_count_substr(bootstrap,
                                     "\"name\": \"action20_63\"") == 1,
                   "wildcard expansion still presents every other action once");
    }
    free(bootstrap);
    a1_scheduler_close(scheduler);
  }

  rc |= a1_write_agent(swapped, sizeof(swapped) / sizeof(swapped[0]));
  run_id[0] = '\0';
  rc |= expect(a1_run("floops/" A1_FIXTURE_FLOOP "/empty_response.json",
                      &request, run_id, sizeof(run_id)) == 0,
               "swapped catalog fixture run completes");
  if (request) {
    const char *message = strstr(request, "\"name\": \"message\"");
    const char *codex = strstr(request, "\"name\": \"manage_codex\"");
    const char *gcal = strstr(request, "\"name\": \"gcal\"");
    rc |= expect(message && codex && gcal && message < codex && codex < gcal,
                 "swapping only agent config swaps presentation order");
    rc |= expect_substr(request, A1_COMPLEX_SCHEMA,
                        "config reordering does not rewrite action schema");
  }
  free(request);
  request = NULL;

  /* The registry maximum, not the retired 24-entry ceiling, is supported.
   * Repeated static init/destroy also proves there is no catalog lifetime. */
  rc |= a1_write_full_agent();
  for (int pass = 0; pass < 3; ++pass) {
    RtScheduler *scheduler = NULL;
    char *catalog = (char *)malloc(RT_XL);
    rc |= test_reset_workspace();
    rc |= expect(catalog != NULL, "allocate full rendered catalog");
    rc |= expect(a1_scheduler_init(&scheduler, err, sizeof(err)) == 0,
                 "registry-maximum agent catalog loads");
    if (scheduler && scheduler->agent_meta_count == 1) {
      const RtAgentMeta *meta = &scheduler->agent_meta[0];
      rc |= expect(meta->action_count == RT_MAX_ACTIONS,
                   "agent catalog reaches the registry maximum");
      if (catalog)
        rc |= expect(rt_action_render_available(&scheduler->actions, meta,
                                                catalog, RT_XL) == 0,
                     "registry-maximum catalog renders completely");
    } else {
      rc |= expect(0, "full catalog scheduler exposes one agent metadata entry");
    }
    free(catalog);
    a1_scheduler_close(scheduler);
  }

  /* Duplicate and unknown entries are startup errors with their fixes. */
  rc |= a1_write_agent(duplicate, sizeof(duplicate) / sizeof(duplicate[0]));
  {
    RtScheduler *scheduler = NULL;
    rc |= test_reset_workspace();
    rc |= expect(a1_scheduler_init(&scheduler, err, sizeof(err)) != 0,
                 "duplicate configured action fails scheduler startup");
    rc |= expect_substr(err, "duplicate action gcal",
                        "duplicate startup error names the action");
    rc |= expect_substr(err, "fix: remove the duplicate",
                        "duplicate startup error names its fix");
    a1_scheduler_close(scheduler);
  }
  rc |= a1_write_agent(unknown, sizeof(unknown) / sizeof(unknown[0]));
  {
    RtScheduler *scheduler = NULL;
    rc |= test_reset_workspace();
    rc |= expect(a1_scheduler_init(&scheduler, err, sizeof(err)) != 0,
                 "unknown configured action fails scheduler startup");
    rc |= expect_substr(err, "unknown action missing_action20",
                        "unknown startup error names the action");
    /* There is no registration step to point at: the fix is a directory
     * under actions/, or removing the name from the agent. */
    rc |= expect_substr(err, "fix: put its directory under actions/",
                        "unknown startup error names its fix");
    rc |= expect_substr(err, "not _-prefixed",
                        "unknown startup error names the unload mechanism");
    a1_scheduler_close(scheduler);
  }

  /* Catalog visibility remains hard runtime authorization. */
  rc |= a1_write_agent(only_gcal, sizeof(only_gcal) / sizeof(only_gcal[0]));
  run_id[0] = '\0';
  rc |= expect(a1_run("floops/" A1_FIXTURE_FLOOP "/disallowed_response.json",
                      NULL, run_id, sizeof(run_id)) == 0,
               "unlisted-action fixture run retires cleanly");
  rc |= test_read_run_event_log(run_id, &event_log);
  rc |= expect_substr(event_log, "\"type\":\"action_rejected\"",
                      "unlisted action is rejected by the runner");
  rc |= expect_substr(event_log, "\"reason\":\"not_allowed\"",
                      "unlisted action rejection names allowlist authority");
  rc |= expect_no_substr(event_log, "\"type\":\"action_succeeded\"",
                         "unlisted action never executes");
  free(event_log);
  event_log = NULL;

  /* A complete catalog that cannot fit fails before llm_call can create
   * even a partial provider request artifact. */
  rc |= a1_write_actions(1);
  rc |= a1_write_full_agent();
  run_id[0] = '\0';
  rc |= expect(a1_run("floops/" A1_FIXTURE_FLOOP "/empty_response.json",
                      NULL, run_id, sizeof(run_id)) == 2,
               "oversized complete catalog fails the run loudly");
  rc |= test_read_run_event_log(run_id, &event_log);
  rc |= expect_substr(event_log, "agent catalog_agent action catalog exceeds",
                      "overflow error names the agent and catalog");
  rc |= expect_substr(event_log, "fix: shorten source action descriptions",
                      "overflow error names its manifest/config fix");
  rc |= expect_file_not_exists(
      "workspace/runs/run_001/provider_calls/001_request.json");
  rc |= expect_agent_output_not_exists("run_001", "catalog", ".input.json");

  free(event_log);
  free(request);
  if (fixtures_installed) {
    rc |= test_remove_path(A1_FIXTURE_AREA);
    rc |= test_remove_path("floops/" A1_FIXTURE_FLOOP);
    rc |= expect(a1_set_shipped_areas_loaded(1) == 0,
                 "shipped action areas reload after the capacity fixture");
  }
  (void)test_reset_workspace();
  a1_restore_env("LLM_MOCK_RESPONSE_PATHS", saved_paths);
  a1_restore_env("FCLAW_MODEL_PROFILES", saved_profiles);
  return rc;
}
