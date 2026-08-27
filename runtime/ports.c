/* runtime/ports.c — runtime boundary I/O.
 *
 * Sources: drain envelopes from workspace/bus/inbox/, parse them as
 * user_message events, allocate a run per accepted envelope.
 * Rejected envelopes are logged to workspace/logs/rejected_events.jsonl.
 *
 * Outputs: build the delivery JSON fragment that gets embedded in a
 * action_succeeded event, and append the matching line to
 * workspace/logs/deliveries.jsonl. Same code path serves the
 * "message succeeded" delivery and the "Run failed: ..." delivery.
 *
 * The scheduler doesn't know the shape of an envelope or the layout
 * of deliveries.jsonl — those belong here. */

#include "ports.h"
#include "run_internal.h"
#include "publication_outbox.h"
#include "bus/bus.h"
#include "bus/media_manifest.h"
#include "support/json.h"
#include "support/heap_guard.h"
#include "support/timing.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REJECTED_EVENTS_LOG "workspace/logs/rejected_events.jsonl"
#define DELIVERIES_LOG      "workspace/logs/deliveries.jsonl"

int rt_create_run_owned(RtContext *ctx, const char *workspace, int *next_run_number);

typedef struct {
  char envelope_id[RT_SMALL];
  char origin_event_id[RT_SMALL];
  char type[BUS_TYPE_MAX];
  char channel[BUS_CHANNEL_MAX];
  char user_text[RT_LARGE];
  char adapter_id[RT_SMALL];
  char context_id[RT_MED];
  char ref_json[RT_LARGE];
  char affair_id[RT_SMALL];
  char action[RT_SMALL];
  char action_task_id[RT_SMALL];
  char action_args_json[RT_LARGE];
  char payload_json[RT_EVENT_PAYLOAD_MAX];
  char media_ref_json[FC_MEDIA_REF_JSON_SIZE];
  /* Completion claims (type operation_completed): the narrow transport
   * fields admission validates against the durable operation store. The
   * claim's result body stays inside payload_json and is re-read at
   * populate time. */
  char operation_id[RT_SMALL];
  char completion_token[RT_OPERATION_TOKEN_HEX + 1];
  char claim_cause[RT_SMALL];
  int  has_ref;
  int  has_media;
  int  is_affair_review;
  int  is_action_exec;
  int  is_correlated;
  int  is_completion;
  /* Product-owned typed ingress: an admissible non-reserved kind whose
   * payload the kernel carries verbatim and never reads. */
  int  is_custom;
} ParsedEnvelope;

static void log_rejected_envelope(const char *path, const char *reason) {
  char ts[64];
  char esc_path[BUS_PATH_MAX * 2];
  char esc_reason[RT_LARGE];
  char line[BUS_PATH_MAX * 2 + RT_LARGE + 256];
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  if (json_escape(path   ? path   : "", esc_path,   sizeof(esc_path))   != 0) esc_path[0] = '\0';
  if (json_escape(reason ? reason : "", esc_reason, sizeof(esc_reason)) != 0) esc_reason[0] = '\0';
  snprintf(line, sizeof(line),
           "{\"ts\":\"%s\",\"envelope_path\":\"%s\",\"reason\":\"%s\"}\n",
           ts, esc_path, esc_reason);
  (void)fs_mkdir_p("workspace/logs");
  (void)fs_append_text(REJECTED_EVENTS_LOG, line);
}

#define PARSE_FAIL(reason_lit) do { \
  if (err) snprintf(err, err_len, "%s", reason_lit); \
  fc_xfree(text); return -1; \
} while (0)

static int media_ref_sha256_valid(const char *sha256) {
  size_t i;
  if (!sha256 || strlen(sha256) != FC_MEDIA_SHA256_HEX_SIZE - 1U) return 0;
  for (i = 0; i < FC_MEDIA_SHA256_HEX_SIZE - 1U; ++i)
    if (!((sha256[i] >= '0' && sha256[i] <= '9') ||
          (sha256[i] >= 'a' && sha256[i] <= 'f')))
      return 0;
  return 1;
}

static const char *media_ref_parse(const JsonRef *media,
                                   FcMediaManifestRef *out) {
  char expected[FC_MEDIA_MANIFEST_PATH_SIZE];
  long long version = 0;
  long long count = 0;
  long long total_bytes = 0;
  if (!media || !out || media->type != JSON_REF_OBJECT ||
      json_ref_object_get_long(media, "version", &version) != 0 ||
      version != 1)
    return "invalid_field: payload.media.version";
  memset(out, 0, sizeof(*out));
  if (json_ref_object_get_string(media, "manifest",
                                 out->manifest, sizeof(out->manifest)) != 0 ||
      json_ref_object_get_string(media, "sha256",
                                 out->sha256, sizeof(out->sha256)) != 0 ||
      !media_ref_sha256_valid(out->sha256))
    return "invalid_field: payload.media.manifest";
  if (snprintf(expected, sizeof(expected), "media/inbound/%s.json",
               out->sha256) >=
          (int)sizeof(expected) ||
      strcmp(out->manifest, expected) != 0)
    return "invalid_field: payload.media.manifest";
  if (json_ref_object_get_long(media, "count", &count) != 0 ||
      count < 1 || (uint64_t)count > FC_MEDIA_MAX_ITEMS)
    return "invalid_field: payload.media.count";
  if (json_ref_object_get_long(media, "total_bytes", &total_bytes) != 0 ||
      total_bytes <= 0 ||
      (uint64_t)total_bytes > FC_MEDIA_MAX_TOTAL_BYTES)
    return "invalid_field: payload.media.total_bytes";
  out->count = (size_t)count;
  out->total_bytes = (uint64_t)total_bytes;
  return NULL;
}

/* Read one bounded routing string. Absent is fine (0); present but not a
 * string, or longer than its documented bound, is a named rejection so the
 * producer learns the exact field it got wrong. */
static int routing_string(const JsonRef *routing, const char *key,
                          char *out, size_t out_len, size_t max_bytes) {
  JsonRef value;
  if (json_ref_object_get(routing, key, &value) != 0) return 0;
  if (value.type != JSON_REF_STRING ||
      json_ref_string_copy(&value, out, out_len) != 0 ||
      strlen(out) > max_bytes)
    return -1;
  return 0;
}

static int parse_inbound_envelope(const char *path, ParsedEnvelope *out,
                                  char *err, size_t err_len) {
  char *text = NULL;
  JsonRef root, payload, ref_obj, media;
  FcMediaManifestRef media_ref;
  FcMediaManifest manifest;
  if (err && err_len) err[0] = '\0';
  memset(out, 0, sizeof(*out));
  snprintf(out->channel, sizeof(out->channel), "bus");
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
    if (err) snprintf(err, err_len, "envelope_unreadable");
    return -1;
  }
  if (json_ref_first_object(text, &root) != 0)               PARSE_FAIL("envelope_not_a_json_object");
  if (json_ref_object_get_string(&root, "type", out->type, sizeof(out->type)) != 0)
                                                             PARSE_FAIL("missing_required_field: type");
  if (strcmp(out->type, "user_message") == 0) {
    out->is_affair_review = 0;
  } else if (strcmp(out->type, "affair_review") == 0) {
    out->is_affair_review = 1;
  } else if (strcmp(out->type, "action_exec") == 0) {
    out->is_action_exec = 1;
  } else if (strcmp(out->type, "operation_result") == 0 ||
             strcmp(out->type, "work_step_result") == 0 ||
             strcmp(out->type, "work_outcome") == 0) {
    /* Kernel-owned correlated lifecycle kinds. Their payloads are
     * opaque correlation/result data published from durable source
     * events; the run projects them verbatim. */
    out->is_correlated = 1;
  } else if (strcmp(out->type, "operation_completed") == 0) {
    /* Untrusted managed-operation completion claim. Not a behavioral
     * event kind: admission either rejects it before run allocation or
     * accepts it as the trigger for exactly one result run whose first
     * behavioral event is the runtime-stamped operation_result. */
    out->is_completion = 1;
  } else if (!bus_type_token_valid(out->type)) {
    /* The kernel needs no list of product event names, but it does need a
     * bounded token it can byte-compare in a floop gate. */
    if (err) snprintf(err, err_len, "invalid_field: type (%s)", out->type);
    fc_xfree(text); return -1;
  } else if (bus_type_is_reserved(out->type)) {
    /* Admission of a runtime-owned namespace carries authority the custom
     * path does not have. Refuse the forgery instead of downgrading it. */
    if (err) snprintf(err, err_len, "reserved_envelope_type: %s", out->type);
    fc_xfree(text); return -1;
  } else {
    /* Unknown but valid: admitted. A floop opts in with an exact
     * event_kind: gate; no gate means an ordinary no-op run. */
    out->is_custom = 1;
  }
  if (json_ref_object_get_object(&root, "payload", &payload) != 0)
                                                             PARSE_FAIL("missing_required_field: payload");
  if (json_ref_value_copy(&payload, out->payload_json, sizeof(out->payload_json)) != 0)
                                                             PARSE_FAIL("payload_too_large");
  if (out->is_action_exec) {
    JsonRef args;
    if (json_ref_object_get_string(&payload, "action", out->action,
                                   sizeof(out->action)) != 0 ||
        !out->action[0])
      PARSE_FAIL("missing_required_field: payload.action");
    if (json_ref_object_get_object(&payload, "args", &args) != 0 ||
        json_ref_value_copy(&args, out->action_args_json,
                            sizeof(out->action_args_json)) != 0)
      PARSE_FAIL("missing_or_invalid_field: payload.args");
    (void)json_ref_object_get_string(&payload, "task_id",
                                     out->action_task_id,
                                     sizeof(out->action_task_id));
  } else if (out->is_correlated) {
    if (json_ref_object_get_string(&payload, "text", out->user_text,
                                   sizeof(out->user_text)) != 0)
      out->user_text[0] = '\0';
    (void)json_ref_object_get_string(&payload, "origin_event_id",
                                     out->origin_event_id, sizeof(out->origin_event_id));
  } else if (out->is_completion) {
    JsonRef claim_result, status_ref;
    char claim_status[RT_SMALL] = "";
    {
      /* The claim is a narrow transport shape. A submission carrying any
       * runtime-owned correlation is rejected outright rather than
       * silently stripped — workers must never even appear to author
       * identity, routing, or revision correlation. */
      size_t cursor = 0;
      char key[RT_SMALL];
      JsonRef value;
      while (json_ref_object_iter(&payload, &cursor, key, sizeof(key),
                                  &value) == 1) {
        if (strcmp(key, "operation_id") != 0 &&
            strcmp(key, "completion_token") != 0 &&
            strcmp(key, "cause") != 0 &&
            strcmp(key, "result") != 0)
          PARSE_FAIL("claim_carries_runtime_owned_field");
      }
    }
    if (json_ref_object_get_string(&payload, "operation_id",
                                   out->operation_id,
                                   sizeof(out->operation_id)) != 0 ||
        !out->operation_id[0])
      PARSE_FAIL("missing_required_field: payload.operation_id");
    if (json_ref_object_get_string(&payload, "completion_token",
                                   out->completion_token,
                                   sizeof(out->completion_token)) != 0 ||
        !out->completion_token[0])
      PARSE_FAIL("missing_required_field: payload.completion_token");
    if (json_ref_object_get_string(&payload, "cause", out->claim_cause,
                                   sizeof(out->claim_cause)) != 0 ||
        !out->claim_cause[0])
      snprintf(out->claim_cause, sizeof(out->claim_cause), "worker");
    if (strcmp(out->claim_cause, "worker") != 0 &&
        strcmp(out->claim_cause, "timeout") != 0)
      PARSE_FAIL("invalid_field: payload.cause");
    if (json_ref_object_get_object(&payload, "result", &claim_result) != 0)
      PARSE_FAIL("missing_required_field: payload.result");
    if (strcmp(out->claim_cause, "worker") == 0) {
      if (json_ref_object_get_string(&claim_result, "status", claim_status,
                                     sizeof(claim_status)) != 0 ||
          (strcmp(claim_status, "succeeded") != 0 &&
           strcmp(claim_status, "failed") != 0))
        PARSE_FAIL("invalid_field: payload.result.status");
    }
    /* Result payload overflow fails loudly at the boundary; it must
     * never truncate silently into a smaller canonical result. */
    if (json_ref_object_get(&claim_result, "text", &status_ref) == 0 &&
        status_ref.type == JSON_REF_STRING &&
        (size_t)(status_ref.end - status_ref.start) >
            RT_OPERATION_RESULT_TEXT_MAX)
      PARSE_FAIL("result_payload_too_large");
  } else if (out->is_affair_review) {
    if (json_ref_object_get_string(&payload, "affair_id", out->affair_id, sizeof(out->affair_id)) != 0 ||
        !out->affair_id[0])
                                                             PARSE_FAIL("missing_required_field: payload.affair_id");
    (void)json_ref_object_get_string(&payload, "text", out->user_text, sizeof(out->user_text));
    if (!out->user_text[0]) snprintf(out->user_text, sizeof(out->user_text), "(affair review)");
  } else if (out->is_custom) {
    /* Nothing to extract. The payload is one JSON object of the caller's
     * facts, already bounded and copied verbatim above. There is no
     * required `text`, and user_text stays empty so this event never
     * becomes a conversational memory record. */
  } else {
    if (json_ref_object_get_string(&payload, "text", out->user_text, sizeof(out->user_text)) != 0)
                                                             PARSE_FAIL("missing_required_field: payload.text");
    if (json_ref_object_get(&payload, "media", &media) == 0) {
      const char *reason;
      if (media.type != JSON_REF_OBJECT)
        PARSE_FAIL("invalid_field: payload.media");
      reason = media_ref_parse(&media, &media_ref);
      if (reason) {
        if (err) snprintf(err, err_len, "%s", reason);
        fc_xfree(text);
        return -1;
      }
      memset(&manifest, 0, sizeof(manifest));
      if (fc_media_manifest_read(&media_ref, &manifest, err, err_len) != 0) {
        fc_media_manifest_dispose(&manifest);
        fc_xfree(text);
        return -1;
      }
      /* Intake retains only the compact immutable reference. Full descriptor
       * metadata is re-read at the explicitly selected LLM boundary. */
      fc_media_manifest_dispose(&manifest);
      if (fc_media_manifest_ref_json(
              &media_ref, out->media_ref_json,
              sizeof(out->media_ref_json)) != 0) {
        fc_xfree(text);
        if (err) snprintf(err, err_len,
                          "invalid_field: payload.media");
        return -1;
      }
      out->has_media = 1;
    }
  }
  (void)json_ref_object_get_string(&root,    "channel",    out->channel,    sizeof(out->channel));
  if (out->channel[0] == '\0') snprintf(out->channel, sizeof(out->channel), "bus");
  (void)json_ref_object_get_string(&root,    "event_id",   out->envelope_id,sizeof(out->envelope_id));
  if (out->is_custom) {
    /* Routing is transport metadata and lives in its own envelope block.
     * Reading it from the payload would make an ordinary product key
     * ("context_id", "ref") silently steer delivery. */
    JsonRef routing;
    if (json_ref_object_get(&root, "routing", &routing) == 0) {
      if (routing.type != JSON_REF_OBJECT)          PARSE_FAIL("invalid_field: routing");
      if (routing_string(&routing, "adapter_id", out->adapter_id,
                         sizeof(out->adapter_id),
                         BUS_ROUTING_ADAPTER_MAX) != 0)
                                                    PARSE_FAIL("invalid_field: routing.adapter_id");
      if (routing_string(&routing, "context_id", out->context_id,
                         sizeof(out->context_id),
                         BUS_ROUTING_CONTEXT_MAX) != 0)
                                                    PARSE_FAIL("invalid_field: routing.context_id");
      /* action: ids are runtime-owned correlation, never caller input. */
      if (strncmp(out->context_id, "action:", 7) == 0)
                                                    PARSE_FAIL("invalid_field: routing.context_id");
      if (json_ref_object_get(&routing, "ref", &ref_obj) == 0) {
        if (ref_obj.type != JSON_REF_OBJECT ||
            json_ref_value_copy(&ref_obj, out->ref_json,
                                sizeof(out->ref_json)) != 0 ||
            strlen(out->ref_json) > BUS_ROUTING_REF_MAX)
                                                    PARSE_FAIL("invalid_field: routing.ref");
        out->has_ref = 1;
      }
    }
  } else {
    (void)json_ref_object_get_string(&payload, "adapter_id", out->adapter_id, sizeof(out->adapter_id));
    (void)json_ref_object_get_string(&payload, "context_id",
                                     out->context_id,
                                     sizeof(out->context_id));
    if (json_ref_object_get_object(&payload, "ref", &ref_obj) == 0)
      if (json_ref_value_copy(&ref_obj, out->ref_json, sizeof(out->ref_json)) == 0) out->has_ref = 1;
  }
  fc_xfree(text);
  return 0;
}

#undef PARSE_FAIL

/* Transform an admitted completion claim into the canonical result-run
 * context: route and correlation come exclusively from the durable
 * operation record; status and bounded text come from the claim; the
 * canonical operation_result is SELF-SOURCED as this run's first
 * behavioral event. Deterministic in (envelope, immutable record
 * correlation), so live intake, claim-retry recommit, and cold-start
 * recovery all rebuild the exact same first event. */
static int populate_completion_context(RtRun *r, const ParsedEnvelope *env,
                                       char *event_type_out,
                                       size_t event_type_len,
                                       char *payload, size_t payload_len) {
  RtOperationSnapshot op;
  JsonRef claim, claim_result;
  char claim_status[RT_SMALL] = "";
  const char *status;
  char source_event_id[RT_SMALL];
  char eh[RT_MED], ewh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED];
  char esource[RT_MED], estatus[RT_MED], corr[RT_LARGE];
  char *text = NULL, *etext = NULL;
  int n, rc = -1;
  if (!r || !env || !event_type_out || !payload) return -1;
  if (rt_operation_snapshot(env->operation_id, &op) != 0) return -1;
  text = (char *)fc_xmalloc(RT_OPERATION_RESULT_TEXT_MAX + 1U +
                            RT_EVENT_PAYLOAD_MAX);
  if (!text) return -1;
  etext = text + RT_OPERATION_RESULT_TEXT_MAX + 1U;
  text[0] = '\0';
  if (json_ref_top_object(env->payload_json, &claim) == 0 &&
      json_ref_object_get_object(&claim, "result", &claim_result) == 0) {
    (void)json_ref_object_get_string(&claim_result, "status", claim_status,
                                     sizeof(claim_status));
    (void)json_ref_object_get_string(&claim_result, "text", text,
                                     RT_OPERATION_RESULT_TEXT_MAX + 1U);
  }
  if (strcmp(env->claim_cause, "timeout") == 0) {
    status = "timed_out";
    if (!text[0])
      snprintf(text, RT_OPERATION_RESULT_TEXT_MAX + 1U,
               "Managed operation %s reached its completion deadline; "
               "FloofClaw stopped waiting. Its external work may still be "
               "running and was not retried.",
               op.action[0] ? op.action : "(unknown)");
  } else {
    status = strcmp(claim_status, "failed") == 0 ? "failed" : "succeeded";
  }
  /* Route + context restore exclusively from the durable record. */
  snprintf(r->ctx.origin_event_id, sizeof(r->ctx.origin_event_id), "%s",
           op.origin_event_id);
  snprintf(r->ctx.origin_channel, sizeof(r->ctx.origin_channel), "%s",
           op.channel);
  snprintf(r->ctx.origin_adapter_id, sizeof(r->ctx.origin_adapter_id), "%s",
           op.adapter_id);
  if (op.ref_json[0])
    snprintf(r->ctx.origin_ref_json, sizeof(r->ctx.origin_ref_json), "%s",
             op.ref_json);
  snprintf(r->context_id, sizeof(r->context_id), "%s", op.context_id);
  snprintf(r->ctx.context_id, sizeof(r->ctx.context_id), "%s",
           op.context_id);
  /* The canonical event stamps its own exact id (the run's first event),
   * the same self-sourcing rule every wake-bearing source event obeys. */
  if (snprintf(source_event_id, sizeof(source_event_id), "evt_%s_%06d",
               r->ctx.run_id, 1) >= (int)sizeof(source_event_id))
    goto done;
  if (json_escape(op.handle, eh, sizeof(eh)) != 0 ||
      json_escape(op.worker_handle, ewh, sizeof(ewh)) != 0 ||
      json_escape(op.action, ea, sizeof(ea)) != 0 ||
      json_escape(op.request_id, erid, sizeof(erid)) != 0 ||
      json_escape(op.trigger_event_id, etrigger, sizeof(etrigger)) != 0 ||
      json_escape(source_event_id, esource, sizeof(esource)) != 0 ||
      json_escape(status, estatus, sizeof(estatus)) != 0 ||
      json_escape(text, etext, RT_EVENT_PAYLOAD_MAX) != 0)
    goto done;
  {
    char ectx[RT_MED], ech[RT_MED], ead[RT_MED], etask[RT_MED];
    char eorigin[RT_MED];
    if (json_escape(op.context_id, ectx, sizeof(ectx)) != 0 ||
        json_escape(op.channel, ech, sizeof(ech)) != 0 ||
        json_escape(op.adapter_id, ead, sizeof(ead)) != 0 ||
        json_escape(op.task_id, etask, sizeof(etask)) != 0 ||
        json_escape(op.origin_event_id, eorigin, sizeof(eorigin)) != 0 ||
        snprintf(corr, sizeof(corr),
                 "\"task_id\":\"%s\",\"context_id\":\"%s\","
                 "\"channel\":\"%s\",\"adapter_id\":\"%s\","
                 "\"origin_event_id\":\"%s\",\"ref\":%s",
                 etask, ectx, ech, ead, eorigin,
                 op.ref_json[0] ? op.ref_json : "null") >=
            (int)sizeof(corr))
      goto done;
  }
  n = snprintf(payload, payload_len,
               "{\"handle\":\"%s\",\"worker_handle\":\"%s\","
               "\"action\":\"%s\",\"request_id\":\"%s\","
               "\"trigger_event_id\":\"%s\",\"work_rev\":%lld,"
               "\"source_event_id\":\"%s\",%s,\"status\":\"%s\","
               "\"text\":\"%s\"}",
               eh, ewh, ea, erid, etrigger, op.work_rev, esource, corr,
               estatus, etext);
  if (n < 0 || (size_t)n >= payload_len) goto done;
  snprintf(r->ctx.event_kind, sizeof(r->ctx.event_kind), "operation_result");
  snprintf(r->ctx.event_payload_json, sizeof(r->ctx.event_payload_json),
           "%s", payload);
  snprintf(r->created_from_event_id, sizeof(r->created_from_event_id), "%s",
           env->envelope_id);
  snprintf(r->created_from_type, sizeof(r->created_from_type),
           "operation_result");
  r->action_cli_only = 0;
  snprintf(event_type_out, event_type_len, "operation_result");
  rc = 0;
done:
  fc_xfree(text);
  return rc;
}

static int populate_inbound_context(RtRun *r, const ParsedEnvelope *env,
                                    char *event_type_out, size_t event_type_len,
                                    char *payload, size_t payload_len) {
  char escaped[RT_LARGE * 2], escaped_origin[RT_SMALL * 2];
  const char *origin_event_id = env->is_correlated && env->origin_event_id[0]
                              ? env->origin_event_id : env->envelope_id;
  const char *event_type = env->is_correlated || env->is_custom ? env->type
                         : env->is_affair_review ? "affair_review"
                         : env->is_action_exec ? "action_exec"
                         : "user_message";
  if (!r || !env || !event_type_out || !payload) return -1;
  if (env->is_completion)
    return populate_completion_context(r, env, event_type_out,
                                       event_type_len, payload, payload_len);
  snprintf(r->ctx.origin_event_id,  sizeof(r->ctx.origin_event_id),  "%s", origin_event_id);
  snprintf(r->ctx.origin_channel,   sizeof(r->ctx.origin_channel),   "%s", env->channel);
  snprintf(r->ctx.origin_adapter_id,sizeof(r->ctx.origin_adapter_id),"%s", env->adapter_id);
  snprintf(r->ctx.inbound_affair_id,sizeof(r->ctx.inbound_affair_id),"%s", env->affair_id);
  if (env->has_ref)
    snprintf(r->ctx.origin_ref_json, sizeof(r->ctx.origin_ref_json), "%s", env->ref_json);
  if (json_escape(env->user_text,   escaped,        sizeof(escaped))        != 0) return -1;
  if (json_escape(origin_event_id, escaped_origin, sizeof(escaped_origin)) != 0) escaped_origin[0] = '\0';
  if (env->is_action_exec) {
    snprintf(r->context_id, sizeof(r->context_id), "action:cli:%s",
             env->envelope_id);
  } else if (env->is_custom) {
    /* Typed ingress gets its own namespace. Folding it into chat: would
     * hand a device or build event a conversation's memory scope and
     * serialization lane by accident. A fully-qualified caller id is
     * preserved verbatim so a product may deliberately run an event
     * inside an existing context. */
    if (strncmp(env->context_id, "event:", 6) == 0 ||
        strncmp(env->context_id, "chat:", 5) == 0)
      snprintf(r->context_id, sizeof(r->context_id), "%s", env->context_id);
    else if (env->context_id[0])
      snprintf(r->context_id, sizeof(r->context_id), "event:%s:%s",
               env->channel, env->context_id);
    else if (env->adapter_id[0])
      snprintf(r->context_id, sizeof(r->context_id), "event:%s:%s",
               env->channel, env->adapter_id);
    else
      snprintf(r->context_id, sizeof(r->context_id), "event:%s", env->channel);
  } else if (strncmp(env->context_id, "chat:", 5) == 0) {
    /* Runtime-produced correlated envelopes already carry the complete
     * durable context id. Preserve it verbatim across continuation runs. */
    snprintf(r->context_id, sizeof(r->context_id), "%s", env->context_id);
  } else if (env->context_id[0]) {
    snprintf(r->context_id, sizeof(r->context_id), "chat:%s:%s",
             env->channel, env->context_id);
  } else if (env->adapter_id[0]) {
    snprintf(r->context_id, sizeof(r->context_id), "chat:%s:%s", env->channel, env->adapter_id);
  } else {
    snprintf(r->context_id, sizeof(r->context_id), "chat:%s", env->channel);
  }
  snprintf(r->ctx.context_id, sizeof(r->ctx.context_id), "%s", r->context_id);
  if (env->has_media && !r->ctx.media_ref_json) {
    r->ctx.media_ref_json = fc_xstrdup(env->media_ref_json);
    if (!r->ctx.media_ref_json) return -1;
  } else if (!env->has_media && !env->is_correlated &&
             !env->is_affair_review) {
    rt_context_clear_media_ref(&r->ctx);
  }
  if (env->is_action_exec) {
    char eaction[RT_MED], eorigin[RT_MED], etask[RT_MED] = "";
    if (json_escape(env->action, eaction, sizeof(eaction)) != 0 ||
        json_escape(env->envelope_id, eorigin, sizeof(eorigin)) != 0 ||
        (env->action_task_id[0] &&
         json_escape(env->action_task_id, etask, sizeof(etask)) != 0))
      return -1;
    snprintf(payload, payload_len,
             "{\"action\":\"%s\",\"args\":%s,"
             "\"origin_event_id\":\"%s\"%s%s%s}",
             eaction,
             env->action_args_json[0] ? env->action_args_json : "{}",
             eorigin,
             etask[0] ? ",\"task_id\":\"" : "",
             etask[0] ? etask : "",
             etask[0] ? "\"" : "");
  } else if (env->is_correlated) {
    /* Correlated lifecycle envelopes carry opaque driver correlation +
     * result/outcome payloads. Preserve them verbatim; the kernel does not
     * reshape or interpret them. */
    snprintf(payload, payload_len, "%s", env->payload_json);
  } else if (env->is_custom) {
    /* The producer's object, byte-for-byte: no renaming, no flattening
     * into text, no routing metadata mixed in. Overflow is loud rather
     * than a quietly truncated behavioral event. */
    if (snprintf(payload, payload_len, "%s", env->payload_json) >=
        (int)payload_len)
      return -1;
  } else if (env->is_affair_review) {
    char escaped_affair[RT_SMALL * 2];
    if (json_escape(env->affair_id, escaped_affair, sizeof(escaped_affair)) != 0)
      escaped_affair[0] = '\0';
    snprintf(payload, payload_len,
             "{\"affair_id\":\"%s\",\"text\":\"%s\","
             "\"origin_event_id\":\"%s\",\"channel\":\"%s\",\"adapter_id\":\"%s\"%s%s}",
             escaped_affair, escaped, escaped_origin, env->channel, env->adapter_id,
             env->has_ref ? ",\"ref\":" : "",
             env->has_ref ? env->ref_json : "");
  } else if (env->has_media) {
    int n = snprintf(
        payload, payload_len,
        "{\"text\":\"%s\",\"origin_event_id\":\"%s\",\"channel\":\"%s\","
        "\"adapter_id\":\"%s\"%s%s,\"media\":%s}",
        escaped, escaped_origin, env->channel, env->adapter_id,
        env->has_ref ? ",\"ref\":" : "",
        env->has_ref ? env->ref_json : "",
        r->ctx.media_ref_json);
    if (n < 0 || (size_t)n >= payload_len) return -1;
  } else {
    snprintf(payload, payload_len,
             "{\"text\":\"%s\",\"origin_event_id\":\"%s\",\"channel\":\"%s\",\"adapter_id\":\"%s\"%s%s}",
             escaped, escaped_origin, env->channel, env->adapter_id,
             env->has_ref ? ",\"ref\":" : "",
             env->has_ref ? env->ref_json : "");
  }
  /* Triggering-event projection: kind + opaque payload, available to
   * event-kind gates and the "event" listen block. */
  snprintf(r->ctx.event_kind, sizeof(r->ctx.event_kind), "%s", env->type);
  if (env->has_media) {
    if (snprintf(r->ctx.event_payload_json,
                 sizeof(r->ctx.event_payload_json), "%s", payload) >=
        (int)sizeof(r->ctx.event_payload_json))
      return -1;
  } else {
    snprintf(r->ctx.event_payload_json,
             sizeof(r->ctx.event_payload_json), "%s", payload);
  }
  snprintf(r->created_from_event_id, sizeof(r->created_from_event_id), "%s", env->envelope_id);
  snprintf(r->created_from_type, sizeof(r->created_from_type), "%s", env->type);
  r->action_cli_only = env->is_action_exec;
  snprintf(event_type_out, event_type_len, "%s", event_type);
  return 0;
}

static int commit_inbound_event(RtRun *r, const ParsedEnvelope *env) {
  char event_type[RT_SMALL];
  char payload[RT_EVENT_PAYLOAD_MAX];
  if (populate_inbound_context(r, env, event_type, sizeof(event_type),
                               payload, sizeof(payload)) != 0) return -1;
  /* A completion claim's first event carries the record's route as its
   * source (deterministic across recovery); every other kind keeps the
   * envelope channel. */
  if (rt_append_event(&r->ctx, event_type,
                      env->is_completion ? r->ctx.origin_channel
                                         : env->channel,
                      payload) != 0) return -1;
  if (env->is_completion) {
    char status[RT_SMALL] = "", excerpt[RT_MED] = "";
    (void)rt_json_get_string(payload, "status", status, sizeof(status));
    (void)rt_json_get_string(payload, "text", excerpt, sizeof(excerpt));
    rt_operation_on_claim_committed(r, env->operation_id, status, excerpt);
  }
  if (env->is_action_exec) {
    char eaction[RT_MED], etask[RT_MED] = "";
    char output[RT_XL], committed[RT_XL];
    if (json_escape(env->action, eaction, sizeof(eaction)) != 0 ||
        (env->action_task_id[0] &&
         json_escape(env->action_task_id, etask, sizeof(etask)) != 0) ||
        snprintf(output, sizeof(output),
                 "{\"events\":[{\"type\":\"action_request\","
                 "\"payload\":{\"action\":\"%s\",\"args\":%s%s%s%s}}]}",
                 eaction,
                 env->action_args_json[0] ? env->action_args_json : "{}",
                 etask[0] ? ",\"task_id\":\"" : "",
                 etask[0] ? etask : "",
                 etask[0] ? "\"" : "") >=
            (int)sizeof(output) ||
        rt_append_events_from_output(&r->ctx, "action_cli", 0, output,
                                     committed, sizeof(committed)) != 0)
      return -1;
    rt_run_apply_output_events(r, "action_cli", committed);
    /* This run exists only to execute the requested action. The implicit
     * pending-effect drain runs before profile steps, then the run finishes
     * without dispatching product agents. */
  }
  /* User-facing inbound kinds create an input task so the speaker's
   * message action has something to bind to. For affair_review the
   * input.text is the diagnostic label; the speaker's projection
   * branches on ctx->inbound_affair_id to render the full review
   * payload from the affair store. Operation lifecycle events are not
   * user input and bind no input task, and neither is a custom typed
   * event: publishing a fact must not mutate a reducer-owned projection
   * on its own. */
  if (!env->is_correlated && !env->is_action_exec && !env->is_completion &&
      !env->is_custom &&
      rt_task_create_input_for_user_message(&r->ctx, env->user_text) != 0) return -1;
  r->phase_index =
      env->is_action_exec && r->profile
          ? (size_t)r->profile->step_count
          : 0;
  rt_run_set_state(r, RT_RUN_READY);
  return 0;
}

int rt_ports_restore_origin(RtRun *r, const char *envelope_path) {
  ParsedEnvelope env;
  char event_type[RT_SMALL];
  char payload[RT_EVENT_PAYLOAD_MAX];
  char err[RT_LARGE] = "";
  if (!r || parse_inbound_envelope(envelope_path, &env, err, sizeof(err)) != 0)
    return -1;
  return populate_inbound_context(r, &env, event_type, sizeof(event_type),
                                  payload, sizeof(payload));
}

int rt_ports_recommit_inbound(RtRun *r, const char *envelope_path) {
  ParsedEnvelope env;
  char err[RT_LARGE] = "";
  if (!r || parse_inbound_envelope(envelope_path, &env, err, sizeof(err)) != 0)
    return -1;
  return commit_inbound_event(r, &env);
}

enum {
  INBOUND_COMMIT_IO = -2,
  INBOUND_COMMIT_CONFLICT = -1,
  INBOUND_COMMIT_ABSENT = 0,
  INBOUND_COMMIT_EXACT = 1
};

/* The processed envelope is claimed before its behavioral append. A retry
 * may observe a complete first line even when the original append reported a
 * close/fsync-style error, so prove that exact line before appending again. */
int rt_ports_inbound_commit_state(const RtRun *r) {
  char path[PATH_MAX], expected_event_id[RT_SMALL];
  char event_id[RT_SMALL] = "", run_id[RT_SMALL] = "";
  char type[RT_SMALL] = "", source[BUS_CHANNEL_MAX] = "";
  char *text = NULL, *newline, *actual_payload = NULL;
  char *expected_payload = NULL;
  JsonRef event, payload;
  int result = INBOUND_COMMIT_CONFLICT;
  size_t expected_len;
  if (!r ||
      snprintf(expected_event_id, sizeof(expected_event_id),
               "evt_%s_%06d", r->ctx.run_id, 1) >=
          (int)sizeof(expected_event_id))
    return INBOUND_COMMIT_CONFLICT;
  if (rt_event_log_path(&r->ctx, path, sizeof(path)) != 0)
    return INBOUND_COMMIT_CONFLICT;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? INBOUND_COMMIT_ABSENT
                          : INBOUND_COMMIT_IO;
  newline = strchr(text, '\n');
  if (!newline) {
    fc_xfree(text);
    return INBOUND_COMMIT_ABSENT;
  }
  *newline = '\0';
  expected_len = strlen(r->ctx.event_payload_json) + 1U;
  expected_payload = (char *)fc_xmalloc(expected_len);
  if (!expected_payload) {
    fc_xfree(text);
    return INBOUND_COMMIT_IO;
  }
  if (json_ref_top_object(text, &event) == 0 &&
      json_ref_object_get_string(&event, "event_id", event_id,
                                 sizeof(event_id)) == 0 &&
      json_ref_object_get_string(&event, "run_id", run_id,
                                 sizeof(run_id)) == 0 &&
      json_ref_object_get_string(&event, "type", type,
                                 sizeof(type)) == 0 &&
      json_ref_object_get_string(&event, "source", source,
                                 sizeof(source)) == 0 &&
      json_ref_object_get(&event, "payload", &payload) == 0 &&
      (actual_payload = json_ref_value_compact_dup(&payload)) != NULL &&
      json_compact(r->ctx.event_payload_json, expected_payload,
                   expected_len) == 0 &&
      strcmp(event_id, expected_event_id) == 0 &&
      strcmp(run_id, r->ctx.run_id) == 0 &&
      strcmp(type, r->created_from_type) == 0 &&
      strcmp(source, r->ctx.origin_channel) == 0 &&
      strcmp(actual_payload, expected_payload) == 0)
    result = INBOUND_COMMIT_EXACT;
  fc_xfree(actual_payload);
  fc_xfree(expected_payload);
  fc_xfree(text);
  return result;
}

static void retry_waiting_inbound_claims(RtScheduler *s) {
  if (!s) return;
  for (size_t i = 0; i < s->run_count; ++i) {
    RtRun *r = s->runs[i];
    char envelope_path[PATH_MAX], event_path[PATH_MAX];
    int state, append_rc;
    if (!r || r->state != RT_RUN_WAITING_EVENT) continue;
    state = rt_ports_inbound_commit_state(r);
    if (state == INBOUND_COMMIT_EXACT) {
      rt_run_set_state(r, RT_RUN_READY);
      continue;
    }
    if (state == INBOUND_COMMIT_IO) continue;
    if (state == INBOUND_COMMIT_CONFLICT) {
      rt_run_set_error(
          r, "inbound_claim_conflict",
          "claimed processed envelope does not match the exact first run event",
          NULL);
      rt_run_fail(r, "inbound_claim_conflict");
      continue;
    }
    if (rt_event_log_path(&r->ctx, event_path, sizeof(event_path)) != 0)
      continue;
    if (fs_truncate_incomplete_line(event_path) != 0 ||
        snprintf(envelope_path, sizeof(envelope_path),
                 "workspace/bus/processed/%s.json",
                 r->created_from_event_id) >=
            (int)sizeof(envelope_path))
      continue;
    r->ctx.next_event = 1;
    append_rc = rt_ports_recommit_inbound(r, envelope_path);
    state = rt_ports_inbound_commit_state(r);
    if (state == INBOUND_COMMIT_EXACT) {
      /* A full line owns the claim even if the original call observed an
       * error after writing it. Correlated inbound events need no input-task
       * side effect, so advancing this exact run is safe. */
      rt_run_set_state(r, RT_RUN_READY);
    } else if (state == INBOUND_COMMIT_CONFLICT) {
      rt_run_set_error(
          r, "inbound_claim_conflict",
          "retried processed envelope did not become the exact first run event",
          NULL);
      rt_run_fail(r, "inbound_claim_conflict");
    } else if (append_rc != 0) {
      (void)rt_narrate("inbound claim retry deferred for %s",
                       r->ctx.run_id);
    }
  }
}

/* Idempotent admission decision for one completion claim. NULL means
 * the claim would open the one result run; a reason string rejects it
 * before any run artifact exists. Deterministic in durable state, so
 * the orphaned-claim reconciler can safely re-evaluate it. */
static const char *completion_claim_disposition(const ParsedEnvelope *env) {
  char status[RT_SMALL] = "";
  char stored_token[RT_OPERATION_TOKEN_HEX + 1] = "";
  int lookup = rt_operation_status(env->operation_id, status,
                                   sizeof(status));
  if (lookup < 0) return "operation_store_unreadable";
  if (lookup == 1) return "unknown_operation";
  if (strcmp(status, "running") != 0) return "operation_already_terminal";
  if (strcmp(env->claim_cause, "timeout") == 0) {
    /* Runtime-authored deadline claims: the token must match whenever
     * the record holds one; a legacy pre-token record may still be
     * timed out so upgrades converge instead of waiting forever. */
    if (rt_operation_token_of(env->operation_id, stored_token,
                              sizeof(stored_token)) == 0 &&
        stored_token[0] &&
        !rt_operation_token_matches(env->operation_id,
                                    env->completion_token))
      return "completion_token_mismatch";
    return NULL;
  }
  return rt_operation_token_matches(env->operation_id,
                                    env->completion_token)
             ? NULL : "completion_token_mismatch";
}

static int create_run_from_envelope(RtScheduler *s, RtRun *r, const char *envelope_path) {
  ParsedEnvelope env;
  char err[RT_LARGE] = "";
  char expected_name[BUS_ID_MAX + 6];
  const char *basename;
  int validated_outbox = 0;
  int deferred_claim;
  if (parse_inbound_envelope(envelope_path, &env, err, sizeof(err)) != 0) {
    log_rejected_envelope(envelope_path, err);
    return -1;
  }
  basename = strrchr(envelope_path, '/');
  basename = basename ? basename + 1 : envelope_path;
  if (snprintf(expected_name, sizeof(expected_name), "%s.json",
               env.envelope_id) >= (int)sizeof(expected_name) ||
      strcmp(basename, expected_name) != 0) {
    log_rejected_envelope(envelope_path,
                          "envelope_path_identity_mismatch");
    return -1;
  }
  if (env.is_completion) {
    /* Validate the claim against durable operation state BEFORE any run
     * allocation. Duplicate, stale, mismatched, and forged claims are
     * rejected here, inspectably, without becoming behavioral truth. */
    const char *reason = completion_claim_disposition(&env);
    if (reason) {
      log_rejected_envelope(envelope_path, reason);
      return -1;
    }
  }
  {
    int require_outbox =
        strcmp(env.type, "work_step_result") == 0 ||
        strcmp(env.type, "work_outcome") == 0;
    if (strcmp(env.type, "operation_result") == 0) {
      JsonRef payload, value;
      int has_source = 0, has_request = 0, has_rev = 0;
      if (json_ref_top_object(env.payload_json, &payload) == 0) {
        has_source =
            json_ref_object_get(&payload, "source_event_id", &value) == 0;
        has_request =
            json_ref_object_get(&payload, "request_id", &value) == 0;
        has_rev = json_ref_object_get(&payload, "work_rev", &value) == 0;
      }
      /* A4 correlations are all-or-nothing. Legacy fixed-worker operation
       * results carry none of these fields and remain accepted. */
      if ((has_source || has_request || has_rev) &&
          !(has_source && has_request && has_rev)) {
        log_rejected_envelope(envelope_path,
                              "incomplete_internal_wake_correlation");
        return -1;
      }
      require_outbox = has_source && has_request && has_rev;
    }
    if (require_outbox) {
      int valid = rt_publication_outbox_validate(
          env.envelope_id, env.channel, env.type, env.payload_json);
      if (valid != 1) {
        log_rejected_envelope(
            envelope_path,
            valid == 0 ? "internal_wake_missing_outbox"
                       : "internal_wake_outbox_mismatch");
        return -1;
      }
      validated_outbox = 1;
    }
  }
  if (!s->profile_loaded) {
    log_rejected_envelope(envelope_path, "scheduler_profile_not_loaded");
    return -1;
  }
  {
    char next_run_id[RT_SMALL];
    if (snprintf(next_run_id, sizeof(next_run_id), "run_%03d", s->next_run_number) >=
        (int)sizeof(next_run_id) ||
        rt_task_ids_exist_for_run(next_run_id)) {
      char reason[RT_LARGE];
      snprintf(reason, sizeof(reason),
               "workspace_task_id_collision: next run_id %s conflicts with existing task ids. "
               "Run `./bin/fclaw clear --yes` to start fresh, or restore workspace/runs to match "
               "workspace/memory/state. See docs/getting-started/project-layout.md#starting-fresh.",
               next_run_id[0] ? next_run_id : "(unknown)");
      log_rejected_envelope(envelope_path, reason);
      return -1;
    }
  }
  r->profile   = &s->profile;
  r->scheduler = s;
  if (rt_create_run_owned(&r->ctx, "workspace", &s->next_run_number) != 0) {
    log_rejected_envelope(envelope_path, "rt_create_run_failed");
    return -1;
  }
  {
    char event_type[RT_SMALL];
    char payload[RT_EVENT_PAYLOAD_MAX];
    if (populate_inbound_context(r, &env, event_type, sizeof(event_type),
                                 payload, sizeof(payload)) != 0) {
      log_rejected_envelope(envelope_path, "populate_inbound_failed");
      return -1;
    }
    r->phase_index = 0;
    deferred_claim = validated_outbox || env.is_completion;
    r->state = deferred_claim ? RT_RUN_WAITING_EVENT : RT_RUN_READY;
    /* The control claim precedes the behavioral append. If SIGKILL tears
     * the first event, startup can still map this run back to its durable
     * processed envelope and recommit it under the same run id. */
    if (rt_run_persist_state(r) != 0) {
      log_rejected_envelope(envelope_path, "persist_intake_claim_failed");
      return -1;
    }
  }
  if (commit_inbound_event(r, &env) != 0) {
    if (deferred_claim) {
      int state = rt_ports_inbound_commit_state(r);
      if (state == INBOUND_COMMIT_EXACT) {
        rt_run_set_state(r, RT_RUN_READY);
        return 0;
      }
      if (state == INBOUND_COMMIT_ABSENT ||
          state == INBOUND_COMMIT_IO) {
        (void)rt_narrate(
            "internal wake %s claimed by %s; inbound append will retry",
            env.envelope_id, r->ctx.run_id);
        return 1;
      }
    }
    log_rejected_envelope(envelope_path, "apply_inbound_failed");
    return -1;
  }
  return 0;
}

static int intake_envelope(const char *envelope_path, void *user_data) {
  RtScheduler *s = (RtScheduler *)user_data;
  RtRun *r;
  if (s->run_count >= RT_MAX_ACTIVE_RUNS) return 1; /* stop batch */
  r = rt_scheduler_alloc_run(s);
  if (!r) return 1; /* pool full; same backpressure as run_count cap */
  if (create_run_from_envelope(s, r, envelope_path) < 0) {
    rt_scheduler_free_run(s, r);
    return 0; /* keep draining; rejection already logged */
  }
  s->runs[s->run_count++] = r;
  return 0; /* keep draining */
}

int rt_ports_drain_one(RtScheduler *s) {
  if (!s) return -1;
  if (s->run_count >= RT_MAX_ACTIVE_RUNS) return 1;
  return bus_drain_inbox(1, intake_envelope, s) > 0 ? 0 : 1;
}

void rt_ports_drain_sources(RtScheduler *s, int max) {
  int budget;
  if (!s) return;
  retry_waiting_inbound_claims(s);
  if (rt_publication_outbox_reconcile() != 0) return;
  if (max <= 0) max = RT_INTAKE_PER_TICK;
  budget = (int)(RT_MAX_ACTIVE_RUNS - s->run_count);
  if (budget <= 0) return;
  if (budget > max) budget = max;
  (void)bus_drain_inbox(budget, intake_envelope, s);
}

void rt_ports_emit_message(RtRun *r,
                           const char *request_id,
                           const char *message_text,
                           char *out_fragment,
                           size_t out_fragment_len) {
  char escaped_text[RT_LARGE * 2];
  char note[RT_XL];
  if (out_fragment && out_fragment_len) out_fragment[0] = '\0';
  if (!r || !r->ctx.origin_channel[0] || !r->ctx.origin_event_id[0]) return;
  if (json_escape(message_text ? message_text : "",
                  escaped_text, sizeof(escaped_text)) != 0) escaped_text[0] = '\0';
  if (out_fragment) {
    snprintf(out_fragment, out_fragment_len,
             ",\"delivery\":{\"delivery_id\":\"deliv_%s_%06d\",\"origin_event_id\":\"%s\","
             "\"channel\":\"%s\",\"adapter_id\":\"%s\"%s%s,\"text\":\"%s\"}",
             r->ctx.run_id, r->ctx.next_delivery, r->ctx.origin_event_id,
             r->ctx.origin_channel, r->ctx.origin_adapter_id,
             r->ctx.origin_ref_json[0] ? ",\"ref\":" : "",
             r->ctx.origin_ref_json[0] ? r->ctx.origin_ref_json : "",
             escaped_text);
  }
  (void)fs_mkdir_p("workspace/logs");
  snprintf(note, sizeof(note),
           "{\"run_id\":\"%s\",\"request_id\":\"%s\","
           "\"delivery\":{\"delivery_id\":\"deliv_%s_%06d\",\"origin_event_id\":\"%s\","
           "\"channel\":\"%s\",\"adapter_id\":\"%s\"%s%s,\"text\":\"%s\"}}\n",
           r->ctx.run_id, request_id ? request_id : "",
           r->ctx.run_id, r->ctx.next_delivery, r->ctx.origin_event_id,
           r->ctx.origin_channel, r->ctx.origin_adapter_id,
           r->ctx.origin_ref_json[0] ? ",\"ref\":" : "",
           r->ctx.origin_ref_json[0] ? r->ctx.origin_ref_json : "",
           escaped_text);
  (void)fs_append_text(DELIVERIES_LOG, note);
  r->ctx.next_delivery++;
}

int rt_ports_delivery_exists(const char *run_id, const char *request_id) {
  char *text = NULL;
  char *line;
  if (!run_id || !*run_id || !request_id || !*request_id) return 0;
  if (fs_read_text(DELIVERIES_LOG, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? 0 : -1;
  line = text;
  while (*line) {
    char *next = strchr(line, '\n');
    JsonRef root;
    char logged_run[RT_SMALL] = "";
    char logged_request[RT_SMALL] = "";
    if (next) *next = '\0';
    if (json_ref_top_object(line, &root) == 0 &&
        json_ref_object_get_string(&root, "run_id", logged_run,
                                   sizeof(logged_run)) == 0 &&
        json_ref_object_get_string(&root, "request_id", logged_request,
                                   sizeof(logged_request)) == 0 &&
        strcmp(logged_run, run_id) == 0 &&
        strcmp(logged_request, request_id) == 0) {
      fc_xfree(text);
      return 1;
    }
    if (!next) break;
    line = next + 1;
  }
  fc_xfree(text);
  return 0;
}

/* ----- managed-operation deadline pump + orphan reconciler ----- */

#define OP_PUMP_MIN_SCAN_INTERVAL_MS 250
#define OP_RECONCILE_MIN_INTERVAL_MS 2000
#define OP_CLAIM_CHANNEL "operations"

/* Publish the runtime's timeout claim for one due operation through the
 * same admission path a worker completion uses. The claim's bus
 * identity is reserved once and persisted on the record BEFORE
 * publication, so a crash between fire and commit refires the identical
 * envelope and bus_publish_stable absorbs the duplicate. */
static void publish_timeout_claim(const char *op_id) {
  RtOperationSnapshot op;
  char envelope_id[RT_SMALL] = "";
  char token[RT_OPERATION_TOKEN_HEX + 1] = "";
  char eop[RT_MED], etoken[RT_OPERATION_TOKEN_HEX * 2 + 2], etext[RT_LARGE];
  char text[RT_MED];
  char payload[BUS_PAYLOAD_MAX];
  int n;
  if (rt_operation_snapshot(op_id, &op) != 0 ||
      strcmp(op.status, "running") != 0)
    return;
  if (rt_operation_claim_envelope_of(op_id, envelope_id,
                                     sizeof(envelope_id)) != 0)
    return;
  if (!envelope_id[0]) {
    char reserved[BUS_ID_MAX];
    if (bus_reserve_envelope_id(reserved, sizeof(reserved)) != 0) return;
    if (rt_operation_bind_claim_envelope(op_id, reserved) != 0) return;
    snprintf(envelope_id, sizeof(envelope_id), "%s", reserved);
  }
  (void)rt_operation_token_of(op_id, token, sizeof(token));
  snprintf(text, sizeof(text),
           "Managed operation %s reached its completion deadline; "
           "FloofClaw stopped waiting. Its external work may still be "
           "running and was not retried.",
           op.action[0] ? op.action : "(unknown)");
  if (json_escape(op_id, eop, sizeof(eop)) != 0 ||
      json_escape(token, etoken, sizeof(etoken)) != 0 ||
      json_escape(text, etext, sizeof(etext)) != 0)
    return;
  n = snprintf(payload, sizeof(payload),
               "{\"operation_id\":\"%s\",\"completion_token\":\"%s\","
               "\"cause\":\"timeout\",\"result\":{\"status\":\"timed_out\","
               "\"text\":\"%s\"}}",
               eop, etoken[0] ? etoken : "expired", etext);
  if (n < 0 || n >= (int)sizeof(payload)) return;
  if (bus_publish_stable(envelope_id, OP_CLAIM_CHANNEL,
                         "operation_completed", payload) == 0)
    (void)rt_narrate("operation deadline: %s -> timeout claim %s",
                     op_id, envelope_id);
}

/* True when some durable run already claims this envelope id as its
 * triggering event. Scans runstate control files only; used solely for
 * rare orphaned-claim reconciliation. */
static int envelope_claimed_by_some_run(const char *envelope_id) {
  DIR *dir = opendir("workspace/runs");
  struct dirent *entry;
  int claimed = 0;
  if (!dir) return 0;
  while (!claimed && (entry = fs_readdir_checked(dir)) != NULL) {
    char path[PATH_MAX];
    char *text = NULL;
    JsonRef state, created;
    char event_id[RT_SMALL] = "";
    if (strncmp(entry->d_name, "run_", 4) != 0) continue;
    if (snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json",
                 entry->d_name) >= (int)sizeof(path))
      continue;
    if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
      continue;
    if (json_ref_top_object(text, &state) == 0 &&
        json_ref_object_get_object(&state, "created_from", &created) == 0 &&
        json_ref_object_get_string(&created, "event_id", event_id,
                                   sizeof(event_id)) == 0 &&
        strcmp(event_id, envelope_id) == 0)
      claimed = 1;
    fc_xfree(text);
  }
  (void)closedir(dir);
  return claimed;
}

/* Crash-window recovery for completion claims: a claim consumed out of
 * the inbox but never bound to a run (the gateway died between the
 * destructive rename and the run-claim persist, or the intake attempt
 * failed transiently) would otherwise be silently lost and the deadline
 * would report timed_out on work that finished. Requeue any processed
 * claim that would still be admitted and that no run claims. Rejections
 * re-evaluate deterministically, so a refused claim never loops. */
void rt_ports_reconcile_orphaned_claims(void) {
  DIR *dir;
  struct dirent *entry;
  /* Heap-owned scratch: this runs on the reactor stack, and a
   * ParsedEnvelope is far too large to keep in a reactor-path frame
   * (the small-stack robustness gate is the enforcement). */
  ParsedEnvelope *env;
  if (!rt_operation_any_running()) return;
  dir = opendir("workspace/bus/processed");
  if (!dir) return;
  env = (ParsedEnvelope *)fc_xmalloc(sizeof(*env));
  if (!env) {
    (void)closedir(dir);
    return;
  }
  while ((entry = fs_readdir_checked(dir)) != NULL) {
    char path[PATH_MAX];
    char err[RT_LARGE] = "";
    if (strncmp(entry->d_name, "bus_", 4) != 0) continue;
    if (snprintf(path, sizeof(path), "workspace/bus/processed/%s",
                 entry->d_name) >= (int)sizeof(path))
      continue;
    if (parse_inbound_envelope(path, env, err, sizeof(err)) != 0 ||
        !env->is_completion)
      continue;
    if (completion_claim_disposition(env) != NULL) continue;
    if (envelope_claimed_by_some_run(env->envelope_id)) continue;
    if (bus_requeue_processed(env->envelope_id) == 0)
      (void)rt_narrate("operation completion reconciled: requeued orphaned "
                       "claim %s for %s",
                       env->envelope_id, env->operation_id);
  }
  fc_xfree(env);
  (void)closedir(dir);
}

/* Janitor retention guard: a processed completion claim whose operation
 * is still running is pending evidence — either an active claim run
 * needs it for recommit, or the reconciler will requeue it. */
int rt_ports_completion_claim_pinned(const char *envelope_id) {
  char path[PATH_MAX];
  /* Heap-owned: called from the janitor's reactor-path tick. */
  ParsedEnvelope *env;
  char err[RT_LARGE] = "";
  char status[RT_SMALL] = "";
  int pinned = 0;
  if (!envelope_id || !*envelope_id) return 0;
  if (snprintf(path, sizeof(path), "workspace/bus/processed/%s.json",
               envelope_id) >= (int)sizeof(path))
    return 0;
  env = (ParsedEnvelope *)fc_xmalloc(sizeof(*env));
  if (!env) return 1; /* cannot inspect: conservatively keep */
  if (parse_inbound_envelope(path, env, err, sizeof(err)) == 0 &&
      env->is_completion) {
    if (rt_operation_status(env->operation_id, status, sizeof(status)) != 0)
      pinned = 1; /* unreadable store: conservatively keep */
    else
      pinned = strcmp(status, "running") == 0;
  }
  fc_xfree(env);
  return pinned;
}

void rt_operation_pump(RtScheduler *s, uint64_t now_ms) {
  static uint64_t next_scan_ms = 0;
  static uint64_t next_reconcile_ms = 0;
  char ids[8][RT_SMALL];
  size_t n = 0;
  if (!s) return;
  if (now_ms < next_scan_ms) return;
  next_scan_ms = now_ms + OP_PUMP_MIN_SCAN_INTERVAL_MS;
  if (rt_operation_list_deadline_due(
          (long long)timing_stable_wall_from_monotonic(now_ms), ids,
          sizeof(ids) / sizeof(ids[0]), &n) == 0) {
    for (size_t i = 0; i < n; ++i) publish_timeout_claim(ids[i]);
  }
  if (now_ms >= next_reconcile_ms) {
    next_reconcile_ms = now_ms + OP_RECONCILE_MIN_INTERVAL_MS;
    rt_ports_reconcile_orphaned_claims();
  }
}

void rt_ports_emit_run_failure(RtRun *r) {
  char text[RT_LARGE];
  const char *code;
  const char *message;
  if (!r || !r->ctx.origin_channel[0] || !r->ctx.origin_event_id[0]) return;
  code = r->last_error_code[0] ? r->last_error_code : "runtime_error";
  message = r->last_error_message[0] ? r->last_error_message : code;
  snprintf(text, sizeof(text), "Run failed: %s: %s", code, message);
  /* The failure delivery has a stable request_id sentinel "run_failed"
   * that the CLI uses to distinguish from real success deliveries. See
   * runtime/channel/channel.c:CHANNEL_FAILURE_REQUEST_ID. */
  rt_ports_emit_message(r, "run_failed", text, NULL, 0);
}
