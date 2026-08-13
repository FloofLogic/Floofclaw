/* Discord channel adapter — reactor module shim + shared helpers.
 *
 * Boundary: this file owns Discord adapter lifecycle (create/destroy)
 * and the FcReactorModule callbacks (init/collect_fds/on_fd/tick/
 * next_deadline/shutdown). It also hosts shared helpers used by the
 * three protocol layers:
 *   discord_gateway.c  — WSS connect + heartbeat + dispatch
 *   discord_rest.c     — REST POST to /channels/<id>/messages
 *   discord_outbox.c   — deliveries.jsonl tailer -> outq enqueue
 * Shared types and helper declarations live in discord_internal.h. */

#include "discord_internal.h"

#include "../../runtime/secure/auth.h"
#include "../../runtime/secure/secret_store.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/json.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FCLAW_HAVE_OPENSSL
#include <openssl/rand.h>
#endif

/* ===== shared helpers (declared in discord_internal.h) =========== */

void dc_diag(DcAdapter *a, const char *fmt, ...) {
  va_list ap;
  if (!a || !a->debug || !fmt) return;
  fprintf(stderr, "discord: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

void dc_secure_zero(void *ptr, size_t len) {
  secret_store_zero(ptr, len);
}

short dc_tls_want_events(FcTlsResult r) {
  if (r == FC_TLS_WANT_READ) return POLLIN;
  if (r == FC_TLS_WANT_WRITE) return POLLOUT;
  return 0;
}

FcTls *dc_tls_client_new(DcAdapter *a, int fd, const char *hostname) {
  if (!a) return NULL;
  return a->tls_verify
             ? fc_tls_client_new(fd, hostname)
             : fc_tls_client_new_local_insecure(fd, hostname);
}

void dc_narrate_tls_failure(DcAdapter *a, FcTls *tls,
                            const char *hostname) {
  char reason[256];
  if (!a || !tls || !hostname) return;
  if (fc_tls_verify_error(tls, reason, sizeof(reason)) == 1)
    (void)rt_narrate(
        "discord: TLS certificate verification failed for %s: %s",
        hostname, reason);
}

int dc_rand_bytes(unsigned char *out, size_t len) {
#ifdef FCLAW_HAVE_OPENSSL
  return RAND_bytes(out, (int)len) == 1 ? 0 : -1;
#else
  uint64_t x = fc_now_ms();
  for (size_t i = 0; i < len; ++i) {
    x = x * 1103515245ULL + 12345ULL;
    out[i] = (unsigned char)((x >> 16) & 0xffU);
  }
  return 0;
#endif
}

int dc_base64_encode(const unsigned char *src, size_t src_len, char *dst, size_t dst_len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0, o = 0;
  if (!src || !dst) return -1;
  if (dst_len < ((src_len + 2U) / 3U) * 4U + 1U) return -1;
  while (i + 2U < src_len) {
    uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1U] << 8) | src[i + 2U];
    dst[o++] = alphabet[(v >> 18) & 0x3fU];
    dst[o++] = alphabet[(v >> 12) & 0x3fU];
    dst[o++] = alphabet[(v >> 6) & 0x3fU];
    dst[o++] = alphabet[v & 0x3fU];
    i += 3U;
  }
  if (i < src_len) {
    uint32_t v = (uint32_t)src[i] << 16;
    int rem = (int)(src_len - i);
    if (rem == 2) v |= (uint32_t)src[i + 1U] << 8;
    dst[o++] = alphabet[(v >> 18) & 0x3fU];
    dst[o++] = alphabet[(v >> 12) & 0x3fU];
    dst[o++] = rem == 2 ? alphabet[(v >> 6) & 0x3fU] : '=';
    dst[o++] = '=';
  }
  dst[o] = '\0';
  return 0;
}

int dc_resolve_token(DcAdapter *a, char *out, size_t out_len) {
  if (!a || !a->token_key[0] || !out || out_len == 0) return -1;
  return auth_resolve_static_secret(a->token_key, out, out_len);
}

int dc_build_auth_header(DcAdapter *a, char *out, size_t out_len) {
  char token[8192];
  int rc;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  memset(token, 0, sizeof(token));
  if (dc_resolve_token(a, token, sizeof(token)) != 0) {
    dc_secure_zero(token, sizeof(token));
    return -1;
  }
  rc = snprintf(out, out_len, "Authorization: Bot %s", token) < (int)out_len ? 0 : -1;
  dc_secure_zero(token, sizeof(token));
  if (rc != 0) out[0] = '\0';
  return rc;
}

void dc_sanitize_text(const char *src, char *out, size_t out_len) {
  size_t used = 0;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!src) return;
  while (*src && used + 1U < out_len) {
    unsigned char ch = (unsigned char)*src++;
    if (ch == '\r') ch = '\n';
    if (ch < 32U && ch != '\n' && ch != '\t') continue;
    out[used++] = (char)ch;
  }
  out[used] = '\0';
}

void dc_diag_message(DcAdapter *a, const char *stage, const DcInboundMessage *msg) {
  const char *scope;
  char text[161];
  size_t n;
  if (!a || !a->debug || !stage || !msg) return;
  scope = msg->is_dm ? "dm" : "guild";
  n = strlen(msg->content);
  if (n >= sizeof(text)) n = sizeof(text) - 1U;
  memcpy(text, msg->content, n);
  text[n] = '\0';
  dc_sanitize_text(text, text, sizeof(text));
  dc_diag(a,
          "MESSAGE_CREATE %s scope=%s guild=%s channel=%s user=%s bot=%d "
          "media_count=%zu media_bytes=%llu content=\"%s%s\"",
          stage,
          scope,
          msg->guild_id[0] ? msg->guild_id : "-",
          msg->channel_id[0] ? msg->channel_id : "-",
          msg->user_id[0] ? msg->user_id : "-",
          msg->author_is_bot,
          msg->attachment_count,
          (unsigned long long)msg->attachment_bytes,
          text,
          strlen(msg->content) > n ? "..." : "");
}

size_t dc_choose_chunk_len(const char *text, size_t max_len) {
  size_t len = text ? strlen(text) : 0;
  size_t chunk = len;
  if (chunk <= max_len) return chunk;
  chunk = max_len;
  for (size_t i = chunk; i > 0U; --i) if (text[i] == '\n') return i;
  for (size_t i = chunk; i > 0U; --i) if (text[i] == ' ') return i;
  return max_len;
}

int dc_queue_raw(DcAdapter *a, const char *channel_id, const char *payload) {
  int idx;
  if (!a || !channel_id || !*channel_id || !payload) return -1;
  if (a->out_count >= DC_OUT_QUEUE_MAX) return -1;
  idx = (a->out_head + a->out_count) % DC_OUT_QUEUE_MAX;
  snprintf(a->outq[idx].channel_id, sizeof(a->outq[idx].channel_id), "%s", channel_id);
  snprintf(a->outq[idx].payload, sizeof(a->outq[idx].payload), "%s", payload);
  a->out_count++;
  return 0;
}

int dc_queue_message_chunks(DcAdapter *a, const char *channel_id, const char *text) {
  char clean[DC_REPLY_MAX];
  const char *cursor;
  int count = 0;
  dc_sanitize_text(text, clean, sizeof(clean));
  cursor = clean;
  while (*cursor) {
    char chunk[DC_CHUNK_MAX + 4];
    char esc[DC_CHUNK_MAX * 2 + 8];
    char payload[DC_CHUNK_MAX * 2 + 64];
    size_t chunk_len = dc_choose_chunk_len(cursor, DC_CHUNK_MAX);
    if (chunk_len == 0U) break;
    memcpy(chunk, cursor, chunk_len);
    chunk[chunk_len] = '\0';
    if (json_escape(chunk, esc, sizeof(esc)) != 0) return -1;
    snprintf(payload, sizeof(payload), "{\"content\":\"%s\"}", esc);
    if (dc_queue_raw(a, channel_id, payload) != 0) return -1;
    count++;
    cursor += chunk_len;
    while (*cursor == ' ' || *cursor == '\n') cursor++;
  }
  if (count == 0) return dc_queue_raw(a, channel_id, "{\"content\":\" \"}");
  return 0;
}

/* ===== config ==================================================== */

static int dc_load_config(DcAdapter *a) {
  char *text = NULL;
  JsonRef root, channels, dc, allowed;
  int enabled = 0;
  memset(a, 0, sizeof(*a));
  a->gw_fd = -1;
  a->rest_fd = -1;
  a->gw_state = DC_DISABLED;
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "%s", DC_MODULE_ID);
  a->allow_dms = 0;
  a->require_mention_in_guild = 1;
  a->tls_verify = 1;
  if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  if (json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_object(&root, "channels", &channels) != 0 ||
      json_ref_object_get_object(&channels, "discord", &dc) != 0) {
    free(text);
    return -1;
  }
  (void)json_ref_object_get_bool(&dc, "enabled", &enabled);
  if (!enabled) { free(text); return 0; }
  a->enabled = 1;
  (void)json_ref_object_get_bool(&dc, "debug", &a->debug);
  (void)json_ref_object_get_bool(&dc, "allow_bots", &a->allow_bots);
  (void)json_ref_object_get_bool(&dc, "allow_dms", &a->allow_dms);
  (void)json_ref_object_get_bool(&dc, "require_mention_in_guild", &a->require_mention_in_guild);
  (void)json_ref_object_get_bool(&dc, "tls_verify", &a->tls_verify);
  (void)json_ref_object_get_string(&dc, "token_key", a->token_key, sizeof(a->token_key));
  (void)json_ref_object_get_string(&dc, "guild_id", a->guild_id, sizeof(a->guild_id));
  (void)json_ref_object_get_string(&dc, "home_channel_id", a->home_channel_id, sizeof(a->home_channel_id));
  if (json_ref_object_get_array(&dc, "allowed_channel_ids", &allowed) == 0) {
    size_t n = json_ref_array_size(&allowed);
    for (size_t i = 0; i < n && a->allowed_channel_count < DC_ALLOWED_MAX; ++i) {
      JsonRef item;
      if (json_ref_array_get(&allowed, i, &item) == 0 &&
          json_ref_string_copy(&item, a->allowed_channel_ids[a->allowed_channel_count], 128) == 0) {
        a->allowed_channel_count++;
      }
    }
  }
  free(text);
  if (!a->token_key[0]) {
    fprintf(stderr,
            "discord: enabled but missing token_key; fix: set "
            "channels.discord.token_key to 'discord_token' in "
            "config/floofclaw_config.json\n");
    return -1;
  }
  if (!a->guild_id[0]) {
    fprintf(stderr,
            "discord: enabled but missing guild_id; fix: set "
            "channels.discord.guild_id in config/floofclaw_config.json "
            "(Developer Mode -> Copy Server ID)\n");
    return -1;
  }
  if (a->allowed_channel_count == 0) {
    fprintf(stderr,
            "discord: enabled but allowed_channel_ids is empty; fix: add a "
            "channel ID to channels.discord.allowed_channel_ids in "
            "config/floofclaw_config.json\n");
    return -1;
  }
  return 1;
}

/* ===== reactor callbacks ========================================= */

static int dc_init(FcReactorModule *m) {
  DcAdapter *a = m ? (DcAdapter *)m->state : NULL;
  if (!a || !a->enabled) return 0;
  fc_reconnect_backoff_init(&a->reconnect_backoff, a->module_id_buf);
  a->gw_state = DC_DISCONNECTED;
  a->next_reconnect_ms = 0;
  return 0;
}

static int dc_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  DcAdapter *a = m ? (DcAdapter *)m->state : NULL;
  if (!a || !a->enabled) return 0;
  if (a->gw_fd >= 0) {
    short ev = 0;
    if (a->gw_state == DC_TCP_CONNECTING) ev = POLLOUT;
    else if (a->gw_state == DC_TLS_HANDSHAKE) ev = a->gw_want ? a->gw_want : (POLLIN | POLLOUT);
    else {
      if (a->gw_tx_len > 0 || a->gw_want == POLLOUT) ev |= POLLOUT;
      if (a->gw_tx_len == 0 || a->gw_want == POLLIN) ev |= POLLIN;
    }
    if (ev) (void)fc_pollset_add(ps, a->gw_fd, ev, m, NULL);
  }
  if (a->rest_fd >= 0) {
    short ev = 0;
    if (a->rest_state == DC_REST_TCP_CONNECTING) ev = POLLOUT;
    else if (a->rest_state == DC_REST_TLS_HANDSHAKE) ev = a->rest_want ? a->rest_want : (POLLIN | POLLOUT);
    else if (a->rest_state == DC_REST_WRITING) ev = POLLOUT;
    else if (a->rest_state == DC_REST_READING) ev = a->rest_want ? a->rest_want : POLLIN;
    if (ev) (void)fc_pollset_add(ps, a->rest_fd, ev, m, NULL);
  }
  return 0;
}

static int dc_on_fd(FcReactorModule *m, int fd, int revents, void *tag) {
  DcAdapter *a = m ? (DcAdapter *)m->state : NULL;
  (void)tag;
  if (!a || !a->enabled) return 0;
  if (fd == a->gw_fd) dc_drive_gateway(a, revents);
  else if (fd == a->rest_fd) dc_drive_rest(a, revents);
  return 0;
}

static int dc_tick(FcReactorModule *m, uint64_t now_ms) {
  DcAdapter *a = m ? (DcAdapter *)m->state : NULL;
  if (!a || !a->enabled) return 0;
  (void)dc_gateway_progress_expired(a, now_ms);
  (void)dc_rest_progress_expired(a, now_ms);
  if (a->gw_state == DC_DISCONNECTED && now_ms >= a->next_reconnect_ms) (void)dc_gateway_start_connect(a, now_ms);
  if (a->gw_state == DC_GATEWAY_OPEN) {
    (void)fc_reconnect_backoff_maybe_reset(&a->reconnect_backoff, now_ms);
    if (a->heartbeat_interval_ms > 0 && now_ms >= a->next_heartbeat_ms)
      dc_drive_gateway(a, POLLOUT);
  }
  dc_drain_deliveries(a);
  if (a->rest_state == DC_REST_IDLE && a->out_count > 0) (void)dc_rest_start(a, now_ms);
  return 0;
}

static uint64_t dc_next_deadline(FcReactorModule *m, uint64_t now_ms) {
  DcAdapter *a = m ? (DcAdapter *)m->state : NULL;
  uint64_t d = FC_NO_DEADLINE;
  if (!a || !a->enabled) return FC_NO_DEADLINE;
  if (a->gw_state == DC_DISCONNECTED && a->next_reconnect_ms < d) d = a->next_reconnect_ms;
  if ((a->gw_state == DC_WS_HANDSHAKE ||
       (a->gw_state == DC_GATEWAY_OPEN && a->gw_rx_len > 0)) &&
      a->gw_parse_progress_ms > 0 &&
      a->gw_parse_progress_ms + DC_PARSE_PROGRESS_TIMEOUT_MS < d)
    d = a->gw_parse_progress_ms + DC_PARSE_PROGRESS_TIMEOUT_MS;
  if (a->gw_state == DC_GATEWAY_OPEN && a->heartbeat_interval_ms > 0 && a->next_heartbeat_ms < d) d = a->next_heartbeat_ms;
  if (a->rest_state == DC_REST_IDLE && a->out_count > 0 && a->rest_retry_after_ms < d) d = a->rest_retry_after_ms;
  if (a->rest_state == DC_REST_READING && a->rest_read_started_ms > 0 &&
      a->rest_read_started_ms + DC_PARSE_PROGRESS_TIMEOUT_MS < d)
    d = a->rest_read_started_ms + DC_PARSE_PROGRESS_TIMEOUT_MS;
  (void)now_ms;
  return d;
}

static void dc_shutdown_cb(FcReactorModule *m) {
  DcAdapter *a = m ? (DcAdapter *)m->state : NULL;
  if (!a) return;
  if (a->gw_state == DC_GATEWAY_OPEN) (void)dc_queue_ws_frame(a, 0x8U, "", 0);
  if (a->gw_tx_len > 0) (void)dc_gw_flush(a);
  a->enabled = 0; /* shutdown is terminal, not another reconnect failure */
  dc_close_gateway(a);
  dc_close_rest(a);
}

static FcReactorModule *discord_create(struct RtScheduler *sched,
                                      int *startup_error) {
  (void)sched; /* Discord speaks its own socket; the uniform signature is
                * what lets the generated registry need only a name. */
  FcReactorModule *m;
  DcAdapter *a = (DcAdapter *)calloc(1, sizeof(*a));
  char token[8192];
  int config_rc;
  if (startup_error) *startup_error = 0;
  if (!a) { if (startup_error) *startup_error = 1; return NULL; }
  config_rc = dc_load_config(a);
  if (config_rc < 0) { if (startup_error) *startup_error = 1; free(a); return NULL; }
  if (config_rc == 0 || !a->enabled) { free(a); return NULL; }
  if (!fc_tls_available()) {
    fprintf(stderr, "discord: OpenSSL/TLS support is required; fix: install OpenSSL 3 and run make clean && make\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  if (!a->tls_verify) {
    fprintf(stderr,
            "discord: tls_verify=false is invalid for Discord; fix: set channels.discord.tls_verify to true\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  memset(token, 0, sizeof(token));
  if (dc_resolve_token(a, token, sizeof(token)) != 0) {
    fprintf(stderr,
            "discord: missing token for token_key '%s'; fix: run "
            "./bin/fclaw auth set-stdin %s\n",
            a->token_key, a->token_key);
    if (startup_error) *startup_error = 1;
    dc_secure_zero(token, sizeof(token));
    free(a);
    return NULL;
  }
  dc_secure_zero(token, sizeof(token));
  m = (FcReactorModule *)calloc(1, sizeof(*m));
  if (!m) { free(a); return NULL; }
  m->name = "discord";
  m->module_id = a->module_id_buf;
  m->state = a;
  m->init = dc_init;
  m->collect_fds = dc_collect_fds;
  m->on_fd = dc_on_fd;
  m->tick = dc_tick;
  m->shutdown = dc_shutdown_cb;
  m->next_deadline_ms = dc_next_deadline;
  return m;
}

static void discord_destroy(FcReactorModule *adapter) {
  if (!adapter) return;
  free(adapter->state);
  free(adapter);
}

/* The contract this directory satisfies, stated where you can see it. */
const FcAdapter fc_adapter_discord = {
  .name    = "discord",
  .create  = discord_create,
  .destroy = discord_destroy,
};
