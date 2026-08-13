/* Discord Gateway (WSS) state machine.
 *
 * Owns the WebSocket transport: TCP connect -> TLS handshake ->
 * WebSocket upgrade -> Discord Gateway dispatch. Heartbeats, IDENTIFY,
 * RESUME, READY, and MESSAGE_CREATE handling all live here. The
 * outbound message queue is owned by discord_adapter.c and the REST
 * post layer is in discord_rest.c. */

#include "discord_internal.h"

#include "../../runtime/bus/bus.h"
#include "../../runtime/bus/media_manifest.h"
#include "../../runtime/support/heap_guard.h"
#include "../../runtime/support/json.h"
#include "../../runtime/support/net.h"
#include "../../runtime/version.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dc_build_activity(char *out, size_t out_len) {
  char activity[128];
  snprintf(activity, sizeof(activity), "Floofing Claw | %s", FCLAW_VERSION);
  return json_escape(activity, out, out_len);
}

static int dc_build_identify_json(const char *token, char *out, size_t out_len) {
  char token_esc[8192];
  char activity_esc[256];
  if (!token || !out || json_escape(token, token_esc, sizeof(token_esc)) != 0 ||
      dc_build_activity(activity_esc, sizeof(activity_esc)) != 0) return -1;
  return snprintf(out, out_len,
                  "{\"op\":2,\"d\":{\"token\":\"%s\",\"intents\":%u,"
                  "\"properties\":{\"os\":\"macos\",\"browser\":\"floofclaw\",\"device\":\"floofclaw\"},"
                  "\"presence\":{\"since\":null,\"activities\":[{\"name\":\"%s\",\"type\":0}],\"status\":\"online\",\"afk\":false}}}",
                  token_esc, DC_INTENTS, activity_esc) < (int)out_len ? 0 : -1;
}

static int dc_build_resume_json(const char *token, const char *session_id, long long seq, char *out, size_t out_len) {
  char token_esc[8192];
  char session_esc[512];
  if (!token || !session_id || json_escape(token, token_esc, sizeof(token_esc)) != 0 ||
      json_escape(session_id, session_esc, sizeof(session_esc)) != 0) return -1;
  return snprintf(out, out_len,
                  "{\"op\":6,\"d\":{\"token\":\"%s\",\"session_id\":\"%s\",\"seq\":%lld}}",
                  token_esc, session_esc, seq) < (int)out_len ? 0 : -1;
}

static int dc_build_heartbeat_json(long long seq, char *out, size_t out_len) {
  if (seq <= 0) return snprintf(out, out_len, "{\"op\":1,\"d\":null}") < (int)out_len ? 0 : -1;
  return snprintf(out, out_len, "{\"op\":1,\"d\":%lld}", seq) < (int)out_len ? 0 : -1;
}

static int dc_parse_control(const char *json, DcGatewayControl *out) {
  JsonRef root, data;
  if (!json || !out || json_ref_from_text(json, &root) != 0 || root.type != JSON_REF_OBJECT) return -1;
  memset(out, 0, sizeof(*out));
  out->invalid_session_resumable = -1;
  if (json_ref_object_get_long(&root, "op", &out->op) != 0) return -1;
  (void)json_ref_object_get_long(&root, "s", &out->sequence);
  (void)json_ref_object_get_string(&root, "t", out->event_name, sizeof(out->event_name));
  if (out->op == 9) {
    int resumable = 0;
    if (json_ref_object_get(&root, "d", &data) == 0 && data.type == JSON_REF_BOOL &&
        json_ref_get_bool(&data, &resumable) == 0) out->invalid_session_resumable = resumable;
  } else if (out->op == 10) {
    if (json_ref_object_get_object(&root, "d", &data) == 0)
      (void)json_ref_object_get_long(&data, "heartbeat_interval", &out->heartbeat_interval_ms);
  }
  return 0;
}

void dc_inbound_message_dispose(DcInboundMessage *msg) {
  if (!msg) return;
  for (size_t i = 0; i < msg->attachment_count; ++i) {
    fc_xfree((void *)msg->attachments[i].id);
    fc_xfree((void *)msg->attachments[i].filename);
    fc_xfree((void *)msg->attachments[i].media_type);
    fc_xfree((void *)msg->attachments[i].source_url);
  }
  fc_xfree(msg->attachments);
  memset(msg, 0, sizeof(*msg));
}

int dc_source_url_is_trusted(const char *url) {
  static const char cdn_host[] = "cdn.discordapp.com";
  static const char media_host[] = "media.discordapp.net";
  static const char attachment_path[] = "/attachments/";
  static const char ephemeral_path[] = "/ephemeral-attachments/";
  const char *host;
  const char *path;
  size_t host_len;
  if (!url || strncmp(url, "https://", 8U) != 0) return 0;
  host = url + 8U;
  path = host;
  while (*path && *path != '/' && *path != '?' && *path != '#') {
    unsigned char ch = (unsigned char)*path;
    if (ch <= 32U || ch == 127U || ch == '\\' ||
        ch == '@' || ch == ':')
      return 0;
    path++;
  }
  host_len = (size_t)(path - host);
  if (!((host_len == sizeof(cdn_host) - 1U &&
         memcmp(host, cdn_host, host_len) == 0) ||
        (host_len == sizeof(media_host) - 1U &&
         memcmp(host, media_host, host_len) == 0)))
    return 0;
  if (strncmp(path, attachment_path, sizeof(attachment_path) - 1U) != 0 &&
      strncmp(path, ephemeral_path, sizeof(ephemeral_path) - 1U) != 0)
    return 0;
  for (const char *p = path; *p; ++p) {
    unsigned char ch = (unsigned char)*p;
    if (ch <= 32U || ch == 127U || ch == '\\' || ch == '#') return 0;
  }
  return 1;
}

static char *dc_attachment_string(const JsonRef *attachment,
                                  const char *key) {
  JsonRef value;
  if (json_ref_object_get(attachment, key, &value) != 0 ||
      value.type != JSON_REF_STRING)
    return NULL;
  return json_ref_string_dup(&value);
}

static int dc_parse_attachment(const JsonRef *attachment,
                               FcMediaDescriptor *out) {
  JsonRef content_type;
  long long size = -1;
  char validation_error[128];
  if (!attachment || !out || attachment->type != JSON_REF_OBJECT) return -1;
  out->id = dc_attachment_string(attachment, "id");
  out->filename = dc_attachment_string(attachment, "filename");
  out->source_url = dc_attachment_string(attachment, "url");
  if (json_ref_object_get(attachment, "content_type", &content_type) != 0 ||
      json_ref_is_null(&content_type)) {
    out->media_type = fc_xstrdup("application/octet-stream");
  } else if (content_type.type == JSON_REF_STRING) {
    out->media_type = json_ref_string_dup(&content_type);
  } else {
    return -1;
  }
  if (!out->id || !out->filename || !out->source_url || !out->media_type ||
      json_ref_object_get_long(attachment, "size", &size) != 0 ||
      size <= 0)
    return -1;
  out->size_bytes = (uint64_t)size;
  if (!dc_source_url_is_trusted(out->source_url) ||
      fc_media_manifest_validate_descriptor(out, validation_error,
                                            sizeof(validation_error)) != 0)
    return -1;
  return 0;
}

int dc_parse_message_create(const char *json, DcInboundMessage *out) {
  JsonRef root, data, author, attachments;
  char kind[64];
  if (!json || !out || json_ref_from_text(json, &root) != 0 || root.type != JSON_REF_OBJECT) return -1;
  memset(out, 0, sizeof(*out));
  if (json_ref_object_get_string(&root, "t", kind, sizeof(kind)) != 0 || strcmp(kind, "MESSAGE_CREATE") != 0)
    return -1;
  if (json_ref_object_get_object(&root, "d", &data) != 0 ||
      json_ref_object_get_object(&data, "author", &author) != 0 ||
      json_ref_object_get_string(&author, "id", out->user_id, sizeof(out->user_id)) != 0 ||
      json_ref_object_get_string(&data, "channel_id", out->channel_id, sizeof(out->channel_id)) != 0 ||
      json_ref_object_get_string(&data, "content", out->content, sizeof(out->content)) != 0)
    return -1;
  (void)json_ref_object_get_string(&data, "guild_id", out->guild_id, sizeof(out->guild_id));
  out->is_dm = out->guild_id[0] == '\0';
  (void)json_ref_object_get_bool(&author, "bot", &out->author_is_bot);
  if (json_ref_object_get(&data, "attachments", &attachments) == 0) {
    size_t count;
    if (attachments.type != JSON_REF_ARRAY) goto fail;
    count = json_ref_array_size(&attachments);
    if (count > FC_MEDIA_MAX_ITEMS) goto fail;
    if (count > 0U) {
      out->attachments = (FcMediaDescriptor *)fc_xcalloc(
          count, sizeof(*out->attachments));
      if (!out->attachments) goto fail;
      out->attachment_count = count;
      for (size_t i = 0; i < count; ++i) {
        JsonRef attachment;
        if (json_ref_array_get(&attachments, i, &attachment) != 0 ||
            dc_parse_attachment(&attachment, &out->attachments[i]) != 0 ||
            out->attachments[i].size_bytes >
                FC_MEDIA_MAX_TOTAL_BYTES - out->attachment_bytes)
          goto fail;
        out->attachment_bytes += out->attachments[i].size_bytes;
      }
    }
  }
  return 0;

fail:
  dc_inbound_message_dispose(out);
  return -1;
}

static int dc_message_mentions_user(const char *content, const char *user_id) {
  char plain[96];
  char nick[96];
  if (!content || !user_id || !*user_id) return 0;
  snprintf(plain, sizeof(plain), "<@%s>", user_id);
  snprintf(nick, sizeof(nick), "<@!%s>", user_id);
  return strstr(content, plain) != NULL || strstr(content, nick) != NULL;
}

static int dc_transport_allowed(DcAdapter *a, const char *guild_id, const char *channel_id) {
  int is_dm = !guild_id || !*guild_id;
  if (!a || !channel_id || !*channel_id) return 0;
  if (is_dm) return a->allow_dms;
  if (a->guild_id[0] && strcmp(a->guild_id, guild_id) != 0) return 0;
  if (a->allowed_channel_count <= 0) return 1;
  for (int i = 0; i < a->allowed_channel_count; ++i)
    if (strcmp(a->allowed_channel_ids[i], channel_id) == 0) return 1;
  return 0;
}

static void dc_schedule_reconnect(DcAdapter *a, uint64_t now_ms) {
  FcReconnectDelay delay;
  if (!a || !a->enabled) {
    if (a) a->next_reconnect_ms = 0;
    return;
  }
  delay = fc_reconnect_backoff_next(&a->reconnect_backoff, now_ms);
  a->next_reconnect_ms = now_ms + delay.delay_ms;
  (void)rt_narrate("discord: reconnect in %llus (attempt %u)",
                   (unsigned long long)((delay.delay_ms + 999U) / 1000U),
                   delay.attempt);
}

static void dc_schedule_dns_reconnect(DcAdapter *a, uint64_t now_ms,
                                      const char *host) {
  FcReconnectDelay delay;
  if (!a || !a->enabled || !host) return;
  delay = fc_reconnect_backoff_next(&a->reconnect_backoff, now_ms);
  a->next_reconnect_ms = now_ms + delay.delay_ms;
  (void)rt_narrate(
      "discord: dns lookup failed for %s; reconnect in %llus, attempt %u",
      host, (unsigned long long)((delay.delay_ms + 999U) / 1000U),
      delay.attempt);
}

void dc_close_gateway(DcAdapter *a) {
  if (!a) return;
  if (a->gw_tls) { fc_tls_free(a->gw_tls); a->gw_tls = NULL; }
  if (a->gw_fd >= 0) { net_close(a->gw_fd); a->gw_fd = -1; }
  a->gw_tx_len = 0;
  a->gw_rx_len = 0;
  a->gw_parse_progress_ms = 0;
  a->gw_want = 0;
  a->gw_state = DC_DISCONNECTED;
  dc_schedule_reconnect(a, fc_now_ms());
}

int dc_gateway_start_connect(DcAdapter *a, uint64_t now_ms) {
  int fd = -1;
  int connect_rc;
  if (!a || !a->enabled) return -1;
  connect_rc = net_connect_nb(DC_GATEWAY_HOST, DC_GATEWAY_PORT, &fd);
  if (connect_rc != 0 || fd < 0) {
    if (connect_rc == NET_CONNECT_DNS_FAILED)
      dc_schedule_dns_reconnect(a, now_ms, DC_GATEWAY_HOST);
    else
      dc_schedule_reconnect(a, now_ms);
    return -1;
  }
  a->gw_fd = fd;
  a->gw_state = DC_TCP_CONNECTING;
  dc_diag(a, "gateway tcp connecting %s:%d", DC_GATEWAY_HOST, DC_GATEWAY_PORT);
  return 0;
}

int dc_gw_flush(DcAdapter *a) {
  while (a->gw_tx_len > 0) {
    size_t wrote = 0;
    FcTlsResult tr = fc_tls_write(a->gw_tls, a->gw_tx, a->gw_tx_len, &wrote);
    if (wrote > 0) {
      memmove(a->gw_tx, a->gw_tx + wrote, a->gw_tx_len - wrote);
      a->gw_tx_len -= wrote;
    }
    if (tr == FC_TLS_OK) continue;
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) { a->gw_want = dc_tls_want_events(tr); return 0; }
    return -1;
  }
  a->gw_want = 0;
  return 0;
}

int dc_queue_ws_frame(DcAdapter *a, unsigned char opcode, const char *payload, size_t payload_len) {
  unsigned char hdr[14];
  unsigned char mask[4];
  size_t hdr_len = 0;
  size_t need;
  if (!a || payload_len > 0xffffU || dc_rand_bytes(mask, sizeof(mask)) != 0) return -1;
  hdr[0] = (unsigned char)(0x80U | (opcode & 0x0fU));
  if (payload_len < 126U) {
    hdr[1] = (unsigned char)(0x80U | payload_len);
    hdr_len = 2U;
  } else {
    hdr[1] = 0x80U | 126U;
    hdr[2] = (unsigned char)((payload_len >> 8) & 0xffU);
    hdr[3] = (unsigned char)(payload_len & 0xffU);
    hdr_len = 4U;
  }
  memcpy(hdr + hdr_len, mask, sizeof(mask));
  hdr_len += sizeof(mask);
  need = hdr_len + payload_len;
  if (a->gw_tx_len + need > sizeof(a->gw_tx)) return -1;
  memcpy(a->gw_tx + a->gw_tx_len, hdr, hdr_len);
  a->gw_tx_len += hdr_len;
  for (size_t i = 0; i < payload_len; ++i)
    a->gw_tx[a->gw_tx_len++] = (char)(payload[i] ^ (char)mask[i & 3U]);
  return 0;
}

static int dc_queue_ws_json(DcAdapter *a, const char *json) {
  return dc_queue_ws_frame(a, 0x1U, json ? json : "", json ? strlen(json) : 0U);
}

static int dc_queue_ws_handshake(DcAdapter *a) {
  unsigned char nonce[16];
  char key[64];
  char req[1024];
  int n;
  if (dc_rand_bytes(nonce, sizeof(nonce)) != 0 || dc_base64_encode(nonce, sizeof(nonce), key, sizeof(key)) != 0) return -1;
  n = snprintf(req, sizeof(req),
               "GET %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Version: 13\r\n"
               "Sec-WebSocket-Key: %s\r\n\r\n",
               DC_GATEWAY_PATH, DC_GATEWAY_HOST, key);
  if (n < 0 || (size_t)n >= sizeof(req) || a->gw_tx_len + (size_t)n > sizeof(a->gw_tx)) return -1;
  memcpy(a->gw_tx + a->gw_tx_len, req, (size_t)n);
  a->gw_tx_len += (size_t)n;
  return 0;
}

static int dc_parse_ws_handshake(DcAdapter *a) {
  char *end;
  if (!a) return -1;
  if (a->gw_rx_len >= sizeof(a->gw_rx)) return -1;
  a->gw_rx[a->gw_rx_len] = '\0';
  end = strstr(a->gw_rx, "\r\n\r\n");
  if (!end) return 0;
  if (!strstr(a->gw_rx, " 101 ")) return -1;
  {
    size_t consumed = (size_t)(end - a->gw_rx) + 4U;
    memmove(a->gw_rx, a->gw_rx + consumed, a->gw_rx_len - consumed);
    a->gw_rx_len -= consumed;
  }
  a->gw_state = DC_GATEWAY_OPEN;
  fc_reconnect_backoff_mark_connected(&a->reconnect_backoff, fc_now_ms());
  dc_diag(a, "gateway websocket open");
  return 1;
}

static int dc_extract_frame(DcAdapter *a, unsigned char *opcode_out, char *payload, size_t payload_len) {
  unsigned char *p = (unsigned char *)a->gw_rx;
  uint64_t len;
  size_t header = 2;
  int masked;
  unsigned char mask[4] = {0};
  if (a->gw_rx_len < 2) return 0;
  *opcode_out = (unsigned char)(p[0] & 0x0fU);
  len = p[1] & 0x7fU;
  masked = (p[1] & 0x80U) != 0;
  if (len == 126U) {
    if (a->gw_rx_len < 4) return 0;
    len = ((uint64_t)p[2] << 8) | p[3];
    header = 4;
  } else if (len == 127U) {
    if (a->gw_rx_len < 10) return 0;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
    header = 10;
  }
  if (masked) {
    if (a->gw_rx_len < header + 4) return 0;
    memcpy(mask, p + header, 4);
    header += 4;
  }
  if (len + 1U > payload_len) return -1;
  if (a->gw_rx_len < header + (size_t)len) return 0;
  for (uint64_t i = 0; i < len; ++i) {
    char ch = (char)p[header + (size_t)i];
    payload[i] = masked ? (char)(ch ^ (char)mask[i & 3U]) : ch;
  }
  payload[len] = '\0';
  {
    size_t consumed = header + (size_t)len;
    memmove(a->gw_rx, a->gw_rx + consumed, a->gw_rx_len - consumed);
    a->gw_rx_len -= consumed;
  }
  return 1;
}

int dc_publish_message(DcAdapter *a, const DcInboundMessage *msg) {
  char esc_text[DC_TEXT_MAX * 2];
  char esc_adapter[128];
  char esc_channel[256];
  char esc_guild[256];
  char esc_user[128];
  char payload[DC_TEXT_MAX * 2 + 1024];
  char id[BUS_ID_MAX];
  int n;
  const char *scope;
  if (!a || !msg) return -1;
  scope = msg->is_dm ? "dm" : "guild";
  if (json_escape(msg->content, esc_text, sizeof(esc_text)) != 0 ||
      json_escape(a->module_id_buf, esc_adapter, sizeof(esc_adapter)) != 0 ||
      json_escape(msg->channel_id, esc_channel, sizeof(esc_channel)) != 0 ||
      json_escape(msg->guild_id, esc_guild, sizeof(esc_guild)) != 0 ||
      json_escape(msg->user_id, esc_user, sizeof(esc_user)) != 0)
    return -1;
  if (msg->attachment_count > 0U) {
    FcMediaManifestRef media_ref;
    char media_json[FC_MEDIA_REF_JSON_SIZE];
    char error[192];
    if (fc_media_manifest_write(msg->attachments, msg->attachment_count,
                                &media_ref, error, sizeof(error)) != 0 ||
        fc_media_manifest_ref_json(&media_ref, media_json,
                                   sizeof(media_json)) != 0) {
      (void)rt_narrate("discord: media message rejected: %s",
                       error[0] ? error : "manifest reference failed");
      return -1;
    }
    n = snprintf(payload, sizeof(payload),
                 "{\"text\":\"%s\",\"media\":%s,\"adapter_id\":\"%s\","
                 "\"ref\":{\"channel_id\":\"%s\",\"guild_id\":\"%s\","
                 "\"user_id\":\"%s\",\"scope\":\"%s\"}}",
                 esc_text, media_json, esc_adapter, esc_channel, esc_guild,
                 esc_user, scope);
  } else {
    n = snprintf(payload, sizeof(payload),
                 "{\"text\":\"%s\",\"adapter_id\":\"%s\","
                 "\"ref\":{\"channel_id\":\"%s\",\"guild_id\":\"%s\","
                 "\"user_id\":\"%s\",\"scope\":\"%s\"}}",
                 esc_text, esc_adapter, esc_channel, esc_guild, esc_user,
                 scope);
  }
  if (n < 0 || (size_t)n >= sizeof(payload) ||
      bus_publish("discord", "user_message", payload, id, sizeof(id)) != 0) {
    (void)rt_narrate("discord: failed to publish inbound message");
    return -1;
  }
  return 0;
}

static void dc_mark_ready(DcAdapter *a, const JsonRef *data) {
  JsonRef user;
  if (!a || !data) return;
  (void)json_ref_object_get_string(data, "session_id", a->session_id, sizeof(a->session_id));
  if (json_ref_object_get_object(data, "user", &user) == 0)
    (void)json_ref_object_get_string(&user, "id", a->bot_user_id, sizeof(a->bot_user_id));
  a->resume_eligible = 1;
  dc_diag(a, "READY session=%s user=%s", a->session_id, a->bot_user_id);
}

static int dc_handle_dispatch(DcAdapter *a, const char *event_name, const JsonRef *data, const char *raw_json) {
  if (strcmp(event_name, "READY") == 0) { dc_mark_ready(a, data); return 0; }
  if (strcmp(event_name, "RESUMED") == 0) { a->resume_eligible = 1; return 0; }
  if (strcmp(event_name, "MESSAGE_CREATE") == 0) {
    DcInboundMessage msg = {0};
    int attend;
    if (dc_parse_message_create(raw_json, &msg) != 0) {
      dc_diag(a, "MESSAGE_CREATE drop reason=parse_failed");
      return 0;
    }
    dc_diag_message(a, "received", &msg);
    if (msg.user_id[0] && a->bot_user_id[0] && strcmp(msg.user_id, a->bot_user_id) == 0) {
      dc_diag_message(a, "drop reason=self_message", &msg);
      goto cleanup;
    }
    if (msg.author_is_bot && !a->allow_bots) {
      dc_diag_message(a, "drop reason=author_is_bot", &msg);
      goto cleanup;
    }
    if (!msg.content[0] && msg.attachment_count == 0U) {
      dc_diag_message(a, "drop reason=empty_content", &msg);
      goto cleanup;
    }
    if (!dc_transport_allowed(a, msg.guild_id, msg.channel_id)) {
      dc_diag_message(a, "drop reason=transport_not_allowed", &msg);
      goto cleanup;
    }
    attend = msg.is_dm || !a->require_mention_in_guild || dc_message_mentions_user(msg.content, a->bot_user_id);
    if (!attend) {
      dc_diag_message(a, "drop reason=mention_required", &msg);
      goto cleanup;
    }
    dc_diag_message(a, "publish", &msg);
    (void)dc_publish_message(a, &msg);
cleanup:
    dc_inbound_message_dispose(&msg);
  }
  return 0;
}

static int dc_handle_gateway_json(DcAdapter *a, const char *json) {
  JsonRef root, data;
  DcGatewayControl c;
  if (dc_parse_control(json, &c) != 0 || json_ref_from_text(json, &root) != 0) return -1;
  if (c.sequence != 0) a->sequence = c.sequence;
  switch (c.op) {
  case 0:
    if (json_ref_object_get_object(&root, "d", &data) == 0) return dc_handle_dispatch(a, c.event_name, &data, json);
    return 0;
  case 7:
    return -1;
  case 9:
    a->resume_eligible = c.invalid_session_resumable > 0;
    if (!a->resume_eligible) { a->session_id[0] = '\0'; a->sequence = 0; }
    a->invalid_session = 1;
    return -1;
  case 10: {
    char token[8192];
    char payload[12000];
    memset(token, 0, sizeof(token));
    a->heartbeat_interval_ms = c.heartbeat_interval_ms;
    a->next_heartbeat_ms = fc_now_ms() + (uint64_t)(a->heartbeat_interval_ms > 0 ? a->heartbeat_interval_ms : 45000);
    if (dc_resolve_token(a, token, sizeof(token)) != 0) return -1;
    if (a->invalid_session || !a->resume_eligible || !a->session_id[0]) {
      if (dc_build_identify_json(token, payload, sizeof(payload)) != 0) { dc_secure_zero(token, sizeof(token)); return -1; }
      a->invalid_session = 0;
    } else if (dc_build_resume_json(token, a->session_id, a->sequence, payload, sizeof(payload)) != 0) {
      dc_secure_zero(token, sizeof(token));
      return -1;
    }
    dc_secure_zero(token, sizeof(token));
    if (dc_queue_ws_json(a, payload) != 0) { dc_secure_zero(payload, sizeof(payload)); return -1; }
    dc_secure_zero(payload, sizeof(payload));
    return 0;
  }
  case 11:
    return 0;
  default:
    return 0;
  }
}

static int dc_gw_read_available(DcAdapter *a, uint64_t now_ms) {
  for (;;) {
    size_t n = 0;
    FcTlsResult tr;
    if (a->gw_rx_len >= sizeof(a->gw_rx) - 1U) return -1;
    tr = fc_tls_read(a->gw_tls, a->gw_rx + a->gw_rx_len, sizeof(a->gw_rx) - 1U - a->gw_rx_len, &n);
    if (n > 0) {
      if (a->gw_rx_len == 0) a->gw_parse_progress_ms = now_ms;
      a->gw_rx_len += n;
    }
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) { a->gw_want = dc_tls_want_events(tr); break; }
    if (tr == FC_TLS_CLOSED || tr == FC_TLS_ERROR || tr == FC_TLS_NOT_BUILT) return -1;
    if (n == 0) break;
  }
  return 0;
}

static int dc_gw_process_rx(DcAdapter *a, uint64_t now_ms) {
  if (a->gw_state == DC_WS_HANDSHAKE) {
    int rc = dc_parse_ws_handshake(a);
    if (rc < 0) return -1;
    if (rc == 0) return 0;
    a->gw_parse_progress_ms = a->gw_rx_len > 0 ? now_ms : 0;
  }
  while (a->gw_state == DC_GATEWAY_OPEN) {
    char payload[DC_RX_MAX];
    unsigned char opcode = 0;
    int rc = dc_extract_frame(a, &opcode, payload, sizeof(payload));
    if (rc < 0) return -1;
    if (rc == 0) break;
    a->gw_parse_progress_ms = a->gw_rx_len > 0 ? now_ms : 0;
    if (opcode == 0x8U) return -1;
    if (opcode == 0x9U) { (void)dc_queue_ws_frame(a, 0xAU, payload, strlen(payload)); continue; }
    if (opcode == 0x1U && dc_handle_gateway_json(a, payload) != 0) return -1;
  }
  return 0;
}

int dc_gateway_progress_expired(DcAdapter *a, uint64_t now_ms) {
  int waiting_for_parse;
  if (!a) return 0;
  waiting_for_parse = a->gw_state == DC_WS_HANDSHAKE ||
      (a->gw_state == DC_GATEWAY_OPEN && a->gw_rx_len > 0);
  if (!waiting_for_parse ||
      !net_progress_timed_out(a->gw_parse_progress_ms, now_ms,
                              DC_PARSE_PROGRESS_TIMEOUT_MS))
    return 0;
  (void)rt_narrate(
      "discord: gateway input made no parse progress for %llus; reconnecting",
      (unsigned long long)(DC_PARSE_PROGRESS_TIMEOUT_MS / 1000U));
  dc_close_gateway(a);
  return 1;
}

void dc_drive_gateway(DcAdapter *a, int revents) {
  uint64_t now = fc_now_ms();
  if (!a || !a->enabled) return;
  if (a->gw_state == DC_TCP_CONNECTING && (revents & (POLLOUT | POLLERR | POLLHUP))) {
    if (net_check_connected(a->gw_fd) != 1) { dc_close_gateway(a); return; }
    a->gw_tls = dc_tls_client_new(a, a->gw_fd, DC_GATEWAY_HOST);
    if (!a->gw_tls) {
      (void)rt_narrate("discord: TLS client setup failed for %s",
                       DC_GATEWAY_HOST);
      dc_close_gateway(a);
      return;
    }
    a->gw_state = DC_TLS_HANDSHAKE;
    a->gw_want = POLLOUT;
  }
  if (a->gw_state == DC_TLS_HANDSHAKE && (revents & (POLLIN | POLLOUT))) {
    FcTlsResult tr = fc_tls_handshake(a->gw_tls);
    if (tr == FC_TLS_OK) {
      if (dc_queue_ws_handshake(a) != 0) { dc_close_gateway(a); return; }
      a->gw_state = DC_WS_HANDSHAKE;
      a->gw_want = POLLOUT;
      a->gw_parse_progress_ms = now;
    } else if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      a->gw_want = dc_tls_want_events(tr);
    } else {
      dc_narrate_tls_failure(a, a->gw_tls, DC_GATEWAY_HOST);
      dc_close_gateway(a);
      return;
    }
  }
  if ((a->gw_state == DC_WS_HANDSHAKE || a->gw_state == DC_GATEWAY_OPEN) && a->gw_tx_len > 0 && (revents & POLLOUT)) {
    if (dc_gw_flush(a) != 0) { dc_close_gateway(a); return; }
  }
  if ((a->gw_state == DC_WS_HANDSHAKE || a->gw_state == DC_GATEWAY_OPEN) && (revents & POLLIN)) {
    if (dc_gw_read_available(a, now) != 0 ||
        dc_gw_process_rx(a, now) != 0) {
      dc_close_gateway(a);
      return;
    }
  }
  if (a->gw_state == DC_GATEWAY_OPEN && a->heartbeat_interval_ms > 0 && now >= a->next_heartbeat_ms) {
    char hb[128];
    if (dc_build_heartbeat_json(a->sequence, hb, sizeof(hb)) != 0 || dc_queue_ws_json(a, hb) != 0) { dc_close_gateway(a); return; }
    a->next_heartbeat_ms = now + (uint64_t)a->heartbeat_interval_ms;
  }
}
