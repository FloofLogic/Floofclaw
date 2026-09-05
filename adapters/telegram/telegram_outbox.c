/* Telegram outbound — tail deliveries.jsonl, POST sendMessage.
 *
 * Same shape as the Discord outbox: stat() the log for its length (never a
 * full read), pick up records addressed to this adapter, and push them
 * through one HTTPS connection at a time. */

#include "telegram_internal.h"

#include "../../runtime/runtime.h"
#include "../../runtime/secure/secret_store.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/http1.h"
#include "../../runtime/support/json.h"
#include "../../runtime/support/timing.h"
#include "../../runtime/version.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tg_send_pending(const TgAdapter *a) { return a && a->out_count > 0; }

/* Split at Telegram's own 4096-character message cap. Splitting on a UTF-8
 * boundary matters: half a codepoint is rejected by the API. */
int tg_queue_chunks(TgAdapter *a, const char *chat_id, const char *text) {
  size_t len, pos = 0;
  if (!a || !chat_id || !text) return -1;
  len = strlen(text);
  if (len == 0) return 0;
  while (pos < len) {
    size_t take = len - pos;
    TgOutbound *slot;
    if (take > TG_MESSAGE_CHUNK) {
      take = TG_MESSAGE_CHUNK;
      /* Back off to the last byte that starts a codepoint. */
      while (take > 0 && ((unsigned char)text[pos + take] & 0xC0U) == 0x80U)
        take--;
      if (take == 0) take = TG_MESSAGE_CHUNK; /* pathological: send as-is */
    }
    if (a->out_count >= TG_OUT_QUEUE_MAX) {
      (void)rt_narrate(
          "telegram: outbound queue full (%d); dropping the rest of a reply",
          TG_OUT_QUEUE_MAX);
      return -1;
    }
    slot = &a->outq[(a->out_head + a->out_count) % TG_OUT_QUEUE_MAX];
    snprintf(slot->chat_id, sizeof(slot->chat_id), "%s", chat_id);
    memcpy(slot->text, text + pos, take);
    slot->text[take] = '\0';
    a->out_count++;
    pos += take;
  }
  return 0;
}

static int tg_delivery_for_us(TgAdapter *a, const char *line,
                              char *chat_id, size_t chat_len,
                              char *text, size_t text_len) {
  JsonRef root, delivery, ref;
  char channel[32], adapter[64];
  if (json_ref_first_object(line, &root) != 0) return 0;
  if (json_ref_object_get_object(&root, "delivery", &delivery) != 0) return 0;
  if (json_ref_object_get_string(&delivery, "channel", channel,
                                 sizeof(channel)) != 0 ||
      strcmp(channel, "telegram") != 0)
    return 0;
  if (json_ref_object_get_string(&delivery, "adapter_id", adapter,
                                 sizeof(adapter)) != 0 ||
      strcmp(adapter, a->module_id_buf) != 0)
    return 0;
  if (json_ref_object_get_string(&delivery, "text", text, text_len) != 0)
    return 0;
  if (json_ref_object_get_object(&delivery, "ref", &ref) != 0 ||
      json_ref_object_get_string(&ref, "chat_id", chat_id, chat_len) != 0) {
    /* Proactive delivery with no inbound ref: only a single allowlisted
     * chat makes an unambiguous destination. */
    if (a->allowed_chat_count == 1)
      snprintf(chat_id, chat_len, "%s", a->allowed_chat_ids[0]);
    else
      return 0;
  }
  return 1;
}

void tg_drain_deliveries(TgAdapter *a) {
  long long total = fs_file_size(TG_DELIVERIES_LOG);
  FILE *fp;
  if (!a) return;
  /* stat, not a full read: the tick only needs the end offset. */
  if (!a->delivery_cursor_init) {
    a->delivery_cursor = total > 0 ? total : 0;
    a->delivery_cursor_init = 1;
    return;
  }
  if (total < 0) return;
  if (total < a->delivery_cursor) a->delivery_cursor = 0; /* rotated */
  if (total <= a->delivery_cursor) return;
  fp = fopen(TG_DELIVERIES_LOG, "rb");
  if (!fp) return;
  if (fseek(fp, a->delivery_cursor, SEEK_SET) == 0) {
    char line[RT_XL];
    while (fgets(line, sizeof(line), fp)) {
      char chat_id[64] = "";
      char text[TG_TEXT_MAX] = "";
      size_t len = strlen(line);
      if (len && line[len - 1] == '\n') line[len - 1] = '\0';
      if (tg_delivery_for_us(a, line, chat_id, sizeof(chat_id), text,
                             sizeof(text)))
        (void)tg_queue_chunks(a, chat_id, text);
    }
  }
  fclose(fp);
  a->delivery_cursor = total;
}

static int tg_build_send_request(TgAdapter *a, const TgOutbound *msg) {
  char token[8192];
  char path[256];
  char etext[TG_MESSAGE_CHUNK * 2 + 16];
  char body[TG_MESSAGE_CHUNK * 2 + 256];
  int rc = -1;
  memset(token, 0, sizeof(token));
  if (tg_resolve_token(a, token, sizeof(token)) != 0) goto done;
  if (snprintf(path, sizeof(path), "/bot%s/sendMessage", token) >=
      (int)sizeof(path))
    goto done;
  if (json_escape(msg->text, etext, sizeof(etext)) != 0) goto done;
  if (snprintf(body, sizeof(body),
               "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
               msg->chat_id, etext) >= (int)sizeof(body))
    goto done;
  rc = fc_http1_build_request(a->send.tx, sizeof(a->send.tx), "POST", path,
                              a->api_host_header, "floofclaw/" FCLAW_VERSION,
                              NULL, "application/json", body);
  if (rc == 0) a->send.tx_len = strlen(a->send.tx);
  secret_store_zero(path, sizeof(path));
done:
  secret_store_zero(token, sizeof(token));
  return rc;
}

static void tg_send_pop(TgAdapter *a) {
  if (a->out_count <= 0) return;
  a->out_head = (a->out_head + 1) % TG_OUT_QUEUE_MAX;
  a->out_count--;
}

void tg_drive_send(TgAdapter *a, int revents, uint64_t now_ms) {
  FcHttp1Response resp;
  FcHttp1Status status;
  int rc, closed;
  if (!a) return;
  if (a->send.state == TG_CONN_IDLE) {
    if (a->out_count <= 0 || now_ms < a->send.retry_after_ms) return;
    if (tg_conn_start(a, &a->send, now_ms) != 0 ||
        a->send.state != TG_CONN_RESOLVING)
      return;
    if (tg_build_send_request(a, &a->outq[a->out_head]) != 0) {
      (void)rt_narrate(
          "telegram: could not build sendMessage; fix: set the bot token "
          "with `fclaw auth set-stdin %s`", a->token_key);
      tg_conn_close(&a->send);
      a->send.retry_after_ms = now_ms + TG_RETRY_MS;
    }
    return;
  }
  if (tg_conn_drive(a, &a->send, revents, now_ms, "sendMessage") < 0) return;
  if (a->send.state != TG_CONN_READING || !(revents & POLLIN)) return;
  rc = tg_read(&a->send);
  if (rc < 0) {
    tg_conn_close(&a->send);
    a->send.retry_after_ms = now_ms + TG_RETRY_MS;
    return;
  }
  closed = rc > 0;
  status = fc_http1_parse_response(a->send.rx, a->send.rx_len, a->body,
                                   TG_BODY_MAX, &resp);
  if (status == FC_HTTP1_NEED_MORE && !closed) return;
  if (status == FC_HTTP1_BAD ||
      (status == FC_HTTP1_NEED_MORE && closed && resp.status == 0)) {
    (void)rt_narrate("telegram: sendMessage reply was not readable HTTP: %s",
                     resp.error[0] ? resp.error : "closed mid-response");
    tg_conn_close(&a->send);
    a->send.retry_after_ms = now_ms + TG_RETRY_MS;
    return;
  }
  if (resp.status >= 200 && resp.status < 300) {
    tg_send_pop(a);
  } else if (resp.status == 429) {
    /* Honour the API's own backoff rather than hammering it — and keep the
     * message queued, because it was never delivered. */
    char retry[32] = "";
    unsigned long long wait_ms = TG_RETRY_MS;
    JsonRef root, params;
    long long after = 0;
    if (json_ref_top_object(a->body, &root) == 0 &&
        json_ref_object_get_object(&root, "parameters", &params) == 0 &&
        json_ref_object_get_long(&params, "retry_after", &after) == 0 &&
        after > 0)
      wait_ms = (unsigned long long)after * 1000ULL;
    else if (fc_http1_header(a->send.rx, a->send.rx_len, "retry-after", retry,
                             sizeof(retry)) == 0 && retry[0])
      wait_ms = (unsigned long long)atoll(retry) * 1000ULL;
    (void)rt_narrate("telegram: sendMessage rate limited; retrying in %llums",
                     wait_ms);
    a->send.retry_after_ms = now_ms + wait_ms;
  } else {
    (void)rt_narrate("telegram: sendMessage failed status=%d; dropping",
                     resp.status);
    tg_send_pop(a);
  }
  tg_conn_close(&a->send);
}
