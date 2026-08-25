#include "action_runtime_internal.h"
#include "support/duration.h"
#include "support/timing.h"

#include <stdio.h>
#include <string.h>

/* note_add / counter_bump / affair_open / affair_close / defer mutate
 * affair state through ordinary runtime actions. Each checks its own
 * preconditions (for example, an active affair_review) and reports
 * failures through the action boundary.
 *
 * All five are outside_world:false, so a crash between action_started
 * and the terminal leaves the request replay-eligible and recovery
 * re-dispatches it under the same stable request id. Each therefore
 * stamps that id into the event it appends and asks
 * rt_action_committed_effect first: without it a replay adds a second
 * note, a second counter delta, or a whole second affair. */

int fc_note_add(RtRun *r, const char *rid, const RtActionDef *def,
                       const char *args, char *result, size_t result_len,
                       char *error, size_t error_len) {
  char text[RT_LARGE] = "";
  char affair_id[RT_SMALL] = "";
  char etext[RT_LARGE * 2], eaff[RT_MED], erid[RT_MED];
  char payload[RT_LARGE * 2];
  char committed[RT_LARGE * 2];
  (void)def;
  /* Target: an explicit args.affair_id (a real affair in this context,
   * e.g. the user adding a source to a watched concern mid-chat), or
   * the affair under review. */
  (void)rt_json_get_string(args && *args ? args : "{}", "affair_id",
                           affair_id, sizeof(affair_id));
  if (affair_id[0] &&
      !rt_affair_reference_in_context(r->ctx.context_id, affair_id)) {
    snprintf(error, error_len,
             "{\"message\":\"args.affair_id does not reference an affair in this context\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (!affair_id[0])
    snprintf(affair_id, sizeof(affair_id), "%s", r->ctx.inbound_affair_id);
  if (!affair_id[0]) {
    snprintf(error, error_len,
             "{\"message\":\"note_add requires an active affair_review or args.affair_id\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  (void)rt_json_get_string(args && *args ? args : "{}", "text", text, sizeof(text));
  if (!text[0]) {
    snprintf(error, error_len,
             "{\"message\":\"note_add.text must be a non-empty string\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  switch (rt_action_committed_effect(r, "affair_note_added", rid,
                                     committed, sizeof(committed))) {
    case 1:
      snprintf(result, result_len, "{\"ok\":true,\"recovered\":true}");
      snprintf(error, error_len, "null");
      return 0;
    case 0: break;
    default:
      snprintf(error, error_len,
               "{\"message\":\"note_add could not read its own committed effect\"}");
      snprintf(result, result_len, "{}");
      return -1;
  }
  if (json_escape(text, etext, sizeof(etext)) != 0 ||
      json_escape(affair_id, eaff, sizeof(eaff)) != 0 ||
      json_escape(rid ? rid : "", erid, sizeof(erid)) != 0) {
    snprintf(error, error_len, "{\"message\":\"note_add json escape failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_append_event_format(&r->ctx, "affair_note_added", "action",
                             payload, sizeof(payload), 0,
                             "{\"affair_id\":\"%s\",\"text\":\"%s\","
                             "\"request_id\":\"%s\"}",
                             eaff, etext, erid) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"failed to append affair_note_added event\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(result, result_len, "{\"ok\":true}");
  snprintf(error, error_len, "null");
  (void)rt_narrate("note on %s: %.120s", affair_id, text);
  return 0;
}

int fc_counter_bump(RtRun *r, const char *rid, const RtActionDef *def,
                           const char *args, char *result, size_t result_len,
                           char *error, size_t error_len) {
  JsonRef root, delta_ref, set_ref;
  char affair_id[RT_SMALL] = "", name[32] = "";
  char eaff[RT_MED], ename[32 * 6 + 1], erid[RT_MED], payload[RT_LARGE];
  char committed[RT_LARGE];
  long long value = 0;
  int has_delta, has_set;
  (void)def;
  if (json_ref_first_object(args && *args ? args : "{}", &root) != 0 ||
      json_ref_object_get_string(&root, "affair_id", affair_id,
                                 sizeof(affair_id)) != 0 ||
      !rt_affair_reference_in_context(r->ctx.context_id, affair_id)) {
    snprintf(error, error_len,
             "{\"message\":\"counter_bump.affair_id must reference an affair in this context\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (json_ref_object_get_string(&root, "name", name, sizeof(name)) != 0 ||
      !name[0]) {
    snprintf(error, error_len,
             "{\"message\":\"counter_bump.name must be 1 to 31 bytes\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  has_delta = json_ref_object_get(&root, "delta", &delta_ref) == 0;
  has_set = json_ref_object_get(&root, "set", &set_ref) == 0;
  if (has_delta == has_set ||
      json_ref_object_get_long(&root, has_delta ? "delta" : "set", &value) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"counter_bump requires exactly one integer: delta or set\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  /* A replayed delta double-counts; this is the one mutation whose
   * duplicate is silently plausible rather than visibly wrong. */
  switch (rt_action_committed_effect(r, "affair_counter_bumped", rid,
                                     committed, sizeof(committed))) {
    case 1:
      snprintf(result, result_len, "{\"ok\":true,\"recovered\":true}");
      snprintf(error, error_len, "null");
      return 0;
    case 0: break;
    default:
      snprintf(error, error_len,
               "{\"message\":\"counter_bump could not read its own committed effect\"}");
      snprintf(result, result_len, "{}");
      return -1;
  }
  if (json_escape(affair_id, eaff, sizeof(eaff)) != 0 ||
      json_escape(name, ename, sizeof(ename)) != 0 ||
      json_escape(rid ? rid : "", erid, sizeof(erid)) != 0) {
    snprintf(error, error_len, "{\"message\":\"counter_bump json escape failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_append_event_format(
          &r->ctx, "affair_counter_bumped", "action", payload,
          sizeof(payload), 0,
          "{\"affair_id\":\"%s\",\"name\":\"%s\",\"%s\":%lld,"
          "\"request_id\":\"%s\"}",
          eaff, ename, has_delta ? "delta" : "set", value, erid) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"failed to append affair_counter_bumped event\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(result, result_len, "{\"ok\":true}");
  snprintf(error, error_len, "null");
  (void)rt_narrate("counter %s on %s: %s %lld", name, affair_id,
                   has_delta ? "delta" : "set", value);
  return 0;
}

int fc_affair_close(RtRun *r, const char *rid, const RtActionDef *def,
                           const char *args, char *result, size_t result_len,
                           char *error, size_t error_len) {
  char eaff[RT_MED], ectx[RT_MED], erid[RT_MED];
  char payload[RT_LARGE];
  char committed[RT_LARGE];
  (void)def; (void)args;
  if (!r->ctx.inbound_affair_id[0]) {
    snprintf(error, error_len,
             "{\"message\":\"affair_close requires an active affair_review\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  switch (rt_action_committed_effect(r, "affair_patch", rid,
                                     committed, sizeof(committed))) {
    case 1:
      snprintf(result, result_len, "{\"ok\":true,\"recovered\":true}");
      snprintf(error, error_len, "null");
      return 0;
    case 0: break;
    default:
      snprintf(error, error_len,
               "{\"message\":\"affair_close could not read its own committed effect\"}");
      snprintf(result, result_len, "{}");
      return -1;
  }
  if (json_escape(r->ctx.inbound_affair_id, eaff, sizeof(eaff)) != 0 ||
      json_escape(r->ctx.context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(rid ? rid : "", erid, sizeof(erid)) != 0) {
    snprintf(error, error_len, "{\"message\":\"affair_close json escape failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_append_event_format(
          &r->ctx, "affair_patch", "action", payload, sizeof(payload), 0,
          "{\"action\":\"update\",\"affair_id\":\"%s\","
          "\"context_id\":\"%s\",\"status\":\"closed\","
          "\"request_id\":\"%s\"}",
          eaff, ectx, erid) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"failed to append affair_patch event\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(result, result_len, "{\"ok\":true}");
  snprintf(error, error_len, "null");
  (void)rt_narrate("concern closed: %s", r->ctx.inbound_affair_id);
  return 0;
}

int fc_defer(RtRun *r, const char *rid, const RtActionDef *def,
                    const char *args, char *result, size_t result_len,
                    char *error, size_t error_len) {
  char value[RT_SMALL] = "";
  char eaff[RT_MED], ectx[RT_MED], erid[RT_MED];
  char payload[RT_LARGE];
  char committed[RT_LARGE];
  long long defer_ms = 0;
  long long next_review_at_ms;
  (void)def;
  if (!r->ctx.inbound_affair_id[0]) {
    snprintf(error, error_len,
             "{\"message\":\"defer requires an active affair_review\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  (void)rt_json_get_string(args && *args ? args : "{}", "value", value, sizeof(value));
  if (!value[0] || fc_parse_duration_ms(value, &defer_ms) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"defer.value must be a duration like \\\"1d\\\" or \\\"60s\\\"\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  /* A replay recomputes the deadline from the current clock, so the
   * committed one is the answer -- otherwise a restart hours later
   * silently pushes the review that much further out. */
  switch (rt_action_committed_effect(r, "affair_patch", rid,
                                     committed, sizeof(committed))) {
    case 1: {
      JsonRef prior;
      long long prior_ms = 0;
      if (json_ref_first_object(committed, &prior) != 0 ||
          json_ref_object_get_long(&prior, "next_review_at_ms",
                                   &prior_ms) != 0) {
        snprintf(error, error_len,
                 "{\"message\":\"defer committed effect is unreadable\"}");
        snprintf(result, result_len, "{}");
        return -1;
      }
      snprintf(result, result_len,
               "{\"ok\":true,\"next_review_at_ms\":%lld,\"recovered\":true}",
               prior_ms);
      snprintf(error, error_len, "null");
      return 0;
    }
    case 0: break;
    default:
      snprintf(error, error_len,
               "{\"message\":\"defer could not read its own committed effect\"}");
      snprintf(result, result_len, "{}");
      return -1;
  }
  next_review_at_ms = (long long)timing_stable_wall_ms() + defer_ms;
  if (json_escape(r->ctx.inbound_affair_id, eaff, sizeof(eaff)) != 0 ||
      json_escape(r->ctx.context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(rid ? rid : "", erid, sizeof(erid)) != 0) {
    snprintf(error, error_len, "{\"message\":\"defer json escape failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_append_event_format(
          &r->ctx, "affair_patch", "action", payload, sizeof(payload), 0,
          "{\"action\":\"update\",\"affair_id\":\"%s\","
          "\"context_id\":\"%s\",\"status\":\"active\","
          "\"next_review_at_ms\":%lld,\"request_id\":\"%s\"}",
          eaff, ectx, next_review_at_ms, erid) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"failed to append affair_patch event\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(result, result_len,
           "{\"ok\":true,\"next_review_at_ms\":%lld}", next_review_at_ms);
  snprintf(error, error_len, "null");
  (void)rt_narrate("review deferred %s: %s", value, r->ctx.inbound_affair_id);
  return 0;
}

int fc_affair_open(RtRun *r, const char *rid, const RtActionDef *def,
                          const char *args, char *result, size_t result_len,
                          char *error, size_t error_len) {
  char manage[RT_MED] = "", review_in[RT_SMALL] = "";
  char affair_id[RT_SMALL], eaff[RT_MED], emanage[RT_MED * 2], ectx[RT_MED];
  char erid[RT_MED];
  char payload[RT_LARGE];
  char committed[RT_LARGE];
  long long next_review_at_ms = 0;
  (void)def;
  if (!r) return -1;
  /* The affair id is derived from the run's event counter, so a replay
   * would mint a second, differently-named concern for one request. */
  switch (rt_action_committed_effect(r, "affair_patch", rid,
                                     committed, sizeof(committed))) {
    case 1: {
      JsonRef prior;
      char prior_id[RT_SMALL] = "";
      long long prior_ms = 0;
      if (json_ref_first_object(committed, &prior) != 0 ||
          json_ref_object_get_string(&prior, "affair_id", prior_id,
                                     sizeof(prior_id)) != 0) {
        snprintf(error, error_len,
                 "{\"message\":\"affair_open committed effect is unreadable\"}");
        snprintf(result, result_len, "{}");
        return -1;
      }
      (void)json_ref_object_get_long(&prior, "next_review_at_ms", &prior_ms);
      snprintf(result, result_len,
               "{\"affair_id\":\"%s\",\"next_review_at_ms\":%lld,"
               "\"recovered\":true}",
               prior_id, prior_ms);
      snprintf(error, error_len, "null");
      return 0;
    }
    case 0: break;
    default:
      snprintf(error, error_len,
               "{\"message\":\"affair_open could not read its own committed effect\"}");
      snprintf(result, result_len, "{}");
      return -1;
  }
  (void)rt_json_get_string(args && *args ? args : "{}", "manage", manage, sizeof(manage));
  if (!manage[0]) {
    snprintf(error, error_len,
             "{\"message\":\"args.manage must describe the concern to watch\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  (void)rt_json_get_string(args && *args ? args : "{}", "review_in",
                           review_in, sizeof(review_in));
  if (review_in[0]) {
    long long in_ms = 0;
    if (fc_parse_duration_ms(review_in, &in_ms) != 0 || in_ms <= 0) {
      snprintf(error, error_len,
               "{\"message\":\"args.review_in must be a duration like \\\"1d\\\" or \\\"6h\\\"\"}");
      snprintf(result, result_len, "{}");
      return -1;
    }
    next_review_at_ms = (long long)timing_stable_wall_ms() + in_ms;
  }
  snprintf(affair_id, sizeof(affair_id), "aff_%s_%06d",
           r->ctx.run_id[0] ? r->ctx.run_id : "run", r->ctx.next_event);
  if (json_escape(affair_id, eaff, sizeof(eaff)) != 0 ||
      json_escape(manage, emanage, sizeof(emanage)) != 0 ||
      json_escape(r->ctx.context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(rid ? rid : "", erid, sizeof(erid)) != 0) {
    snprintf(error, error_len, "{\"message\":\"affair_open json escape failed\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_append_event_format(
          &r->ctx, "affair_patch", "action", payload, sizeof(payload), 0,
          "{\"action\":\"create\",\"affair_id\":\"%s\",\"context_id\":\"%s\","
          "\"manage\":\"%s\",\"status\":\"active\","
          "\"next_review_at_ms\":%lld,\"request_id\":\"%s\"}",
          eaff, ectx, emanage, next_review_at_ms, erid) != 0) {
    snprintf(error, error_len,
             "{\"message\":\"failed to append affair_patch event\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  snprintf(result, result_len,
           "{\"affair_id\":\"%s\",\"next_review_at_ms\":%lld}",
           eaff, next_review_at_ms);
  snprintf(error, error_len, "null");
  (void)rt_narrate("concern opened: %s — %.100s%s%s", affair_id, manage,
                   review_in[0] ? ", first review in " : "",
                   review_in[0] ? review_in : "");
  return 0;
}


