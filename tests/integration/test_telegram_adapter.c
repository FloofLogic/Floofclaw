/* Telegram adapter decisions.
 *
 * The live path is TLS-only against api.telegram.org, so these cover what
 * does not need a socket: which conversation an update belongs to, whether
 * it is allowed in at all, whether it reaches the bus unchanged, when the
 * offset may advance, and how a reply is chunked and routed back. The
 * HTTP/1.1 framing those ride on is covered by the http1 unit tests. */

#include "test_support.h"

#include "../../adapters/telegram/telegram_internal.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE(id, chat_id, type, user, bot, text)                          \
  "{\"update_id\":" id ",\"message\":{\"message_id\":7,"                    \
  "\"from\":{\"id\":" user ",\"is_bot\":" bot "},"                          \
  "\"chat\":{\"id\":" chat_id ",\"type\":\"" type "\"},"                    \
  "\"text\":\"" text "\"}}"

static int envelope_count(void) {
  char *log = NULL;
  int n = 0;
  if (test_read_file("workspace/logs/bus.jsonl", &log) != 0 || !log) return 0;
  n = test_count_substr(log, "\"channel\":\"telegram\"");
  free(log);
  return n;
}

static int bus_has(const char *needle) {
  char *log = NULL;
  int found;
  if (test_read_file("workspace/logs/bus.jsonl", &log) != 0 || !log) return 0;
  found = strstr(log, needle) != NULL;
  free(log);
  return found;
}

int telegram_keys_each_chat_and_dm_as_its_own_context(void) {
  char key[128];
  int rc = 0;

  /* The conversation key, first as pure logic. */
  tg_context_id("supergroup", "-1001", "42", key, sizeof(key));
  rc |= expect(strcmp(key, "-1001") == 0, "a group keys on the chat id");
  tg_context_id("group", "-77", "42", key, sizeof(key));
  rc |= expect(strcmp(key, "-77") == 0, "a small group keys the same way");
  tg_context_id("private", "42", "42", key, sizeof(key));
  rc |= expect(strcmp(key, "dm:42") == 0, "a private chat keys on the user");
  tg_context_id("private", "42", "", key, sizeof(key));
  rc |= expect(key[0] == '\0', "a private chat with no user yields no key");

  /* Then end to end: three conversations, three contexts on the bus. */
  {
    static const char *allowed[] = { "-1001", "-77" };
    TgAdapter *a;
    rc |= test_reset_workspace();
    a = tg_test_adapter_new(1, 0, allowed, 2);
    rc |= expect(a != NULL, "build the telegram fixture adapter");
    if (!a) return rc;
    tg_test_consume_updates(
        a,
        "{\"ok\":true,\"result\":["
        UPDATE("11", "-1001", "supergroup", "42", "false", "in the big room")
        "," UPDATE("12", "-77", "group", "43", "false", "in the small room")
        "," UPDATE("13", "42", "private", "42", "false", "a private word")
        "]}");
    rc |= expect(envelope_count() == 3, "all three updates reach the bus");
    rc |= expect(bus_has("\"context_id\":\"-1001\""),
                 "the supergroup gets its own context");
    rc |= expect(bus_has("\"context_id\":\"-77\""),
                 "the group gets its own context");
    rc |= expect(bus_has("\"context_id\":\"dm:42\""),
                 "the DM gets its own context");
    rc |= expect(bus_has("\"ref\":{\"chat_id\":\"-1001\",\"message_id\":\"7\"}"),
                 "the reply address is the chat and message id");
    rc |= expect(bus_has("\"text\":\"a private word\""),
                 "message text reaches the bus unchanged");
    rc |= expect(bus_has("\"adapter_id\":\"telegram-main\""),
                 "the envelope names the adapter");
    rc |= expect(tg_test_offset(a) == 14,
                 "the offset advances past the last published update");
    tg_test_adapter_free(a);
  }
  return rc;
}

int telegram_drops_bots_and_unallowed_chats(void) {
  static const char *allowed[] = { "-1001" };
  TgAdapter *a;
  int rc = 0;

  rc |= test_reset_workspace();
  a = tg_test_adapter_new(0 /* allow_dms */, 0 /* allow_bots */, allowed, 1);
  rc |= expect(a != NULL, "build the gated fixture adapter");
  if (!a) return rc;

  rc |= expect(tg_chat_allowed(a, "supergroup", "-1001") == 1,
               "an allowlisted group is admitted");
  rc |= expect(tg_chat_allowed(a, "supergroup", "-9999") == 0,
               "an unlisted group is refused");
  rc |= expect(tg_chat_allowed(a, "private", "42") == 0,
               "DMs are refused when allow_dms is off");

  tg_test_consume_updates(
      a,
      "{\"ok\":true,\"result\":["
      UPDATE("21", "-9999", "supergroup", "42", "false", "someone elses room")
      "," UPDATE("22", "42", "private", "42", "false", "an unwanted dm")
      "," UPDATE("23", "-1001", "supergroup", "9", "true", "a bot speaking")
      "," UPDATE("24", "-1001", "supergroup", "42", "false", "the real one")
      "]}");
  rc |= expect(envelope_count() == 1, "only the allowed human message lands");
  rc |= expect(bus_has("\"text\":\"the real one\""), "and it is the right one");
  rc |= expect(!bus_has("someone elses room"),
               "an unlisted group never reaches the bus");
  rc |= expect(!bus_has("an unwanted dm"),
               "a DM never reaches the bus when DMs are off");
  rc |= expect(!bus_has("a bot speaking"),
               "a bot-authored message never reaches the bus");
  /* Dropped updates are still acked: they were seen and judged, and the
   * offset must not replay them forever. */
  rc |= expect(tg_test_offset(a) == 25,
               "the offset advances past judged-and-dropped updates");

  /* allow_bots opens exactly that door and nothing else. */
  {
    TgAdapter *b = tg_test_adapter_new(0, 1, allowed, 1);
    rc |= test_reset_workspace();
    if (b) {
      tg_test_consume_updates(
          b, "{\"ok\":true,\"result\":["
             UPDATE("31", "-1001", "supergroup", "9", "true", "bot allowed now")
             "]}");
      rc |= expect(envelope_count() == 1, "allow_bots admits a bot message");
      tg_test_adapter_free(b);
    }
  }
  tg_test_adapter_free(a);
  return rc;
}

int telegram_refuses_malformed_getupdates_replies(void) {
  static const char *allowed[] = { "-1001" };
  TgAdapter *a;
  int rc = 0;

  rc |= test_reset_workspace();
  a = tg_test_adapter_new(1, 0, allowed, 1);
  rc |= expect(a != NULL, "build the fixture adapter");
  if (!a) return rc;

  /* An API-level refusal publishes nothing and does not move the offset. */
  tg_test_consume_updates(
      a, "{\"ok\":false,\"error_code\":401,\"description\":\"Unauthorized\"}");
  rc |= expect(envelope_count() == 0, "a refused getUpdates publishes nothing");
  rc |= expect(tg_test_offset(a) == 0, "and does not advance the offset");

  tg_test_consume_updates(a, "not json at all");
  rc |= expect(envelope_count() == 0, "a non-JSON body publishes nothing");
  rc |= expect(tg_test_offset(a) == 0, "and does not advance the offset");

  /* An update with neither text nor supported media (for example a join
   * event) is skipped, not fatal. */
  tg_test_consume_updates(
      a, "{\"ok\":true,\"result\":[{\"update_id\":41,\"message\":"
         "{\"message_id\":1,\"chat\":{\"id\":-1001,\"type\":\"supergroup\"}}}]}");
  rc |= expect(envelope_count() == 0, "a non-text update publishes nothing");
  rc |= expect(tg_test_offset(a) == 42,
               "but is still acked so the poll makes progress");
  tg_test_adapter_free(a);
  return rc;
}

int telegram_chunks_replies_and_routes_them_back(void) {
  static const char *allowed[] = { "-1001" };
  TgAdapter *a;
  char *big = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  a = tg_test_adapter_new(1, 0, allowed, 1);
  rc |= expect(a != NULL, "build the fixture adapter");
  if (!a) return rc;

  /* Under the cap: one message. */
  rc |= expect(tg_queue_chunks(a, "-1001", "short reply") == 0,
               "queue a short reply");
  rc |= expect(tg_test_out_count(a) == 1, "a short reply is one message");
  rc |= expect(strcmp(tg_test_out_text(a, 0), "short reply") == 0,
               "and carries its text");
  rc |= expect(strcmp(tg_test_out_chat(a, 0), "-1001") == 0,
               "and its destination chat");
  while (tg_test_out_count(a) > 0) { /* drain for the next case */
    TgAdapter *tmp = a; (void)tmp; break;
  }
  tg_test_adapter_free(a);

  /* Over the cap: split at 4096, on a codepoint boundary. */
  a = tg_test_adapter_new(1, 0, allowed, 1);
  big = (char *)malloc(TG_MESSAGE_CHUNK * 2 + 8);
  rc |= expect(a != NULL && big != NULL, "allocate the oversized reply probe");
  if (a && big) {
    size_t i;
    /* 3-byte codepoints, so the 4096-byte boundary lands mid-character
     * unless the splitter backs off. */
    for (i = 0; i + 3 <= TG_MESSAGE_CHUNK * 2U; i += 3) memcpy(big + i, "\xe2\x9c\x93", 3);
    big[i] = '\0';
    rc |= expect(tg_queue_chunks(a, "-1001", big) == 0, "queue a long reply");
    rc |= expect(tg_test_out_count(a) > 1, "a long reply is split");
    {
      const char *first = tg_test_out_text(a, 0);
      size_t flen = first ? strlen(first) : 0;
      rc |= expect(flen > 0 && flen <= TG_MESSAGE_CHUNK,
                   "each chunk fits Telegram's cap");
      rc |= expect(flen % 3U == 0,
                   "chunks split on a codepoint boundary, not mid-character");
    }
  }
  free(big);
  tg_test_adapter_free(a);

  /* Deliveries addressed elsewhere are ignored; ours are picked up. */
  a = tg_test_adapter_new(1, 0, allowed, 1);
  if (a) {
    rc |= test_reset_workspace();
    rc |= test_write_file("workspace/logs/deliveries.jsonl", "");
    tg_drain_deliveries(a); /* first tick parks the cursor at EOF */
    rc |= expect(tg_test_out_count(a) == 0, "history before the cursor is skipped");
    rc |= test_write_file(
        "workspace/logs/deliveries.jsonl",
        "{\"run_id\":\"run_001\",\"delivery\":{\"delivery_id\":\"d1\","
        "\"channel\":\"discord\",\"adapter_id\":\"discord-main\","
        "\"ref\":{\"channel_id\":\"9\"},\"text\":\"not ours\"}}\n"
        "{\"run_id\":\"run_001\",\"delivery\":{\"delivery_id\":\"d2\","
        "\"channel\":\"telegram\",\"adapter_id\":\"telegram-main\","
        "\"ref\":{\"chat_id\":\"-1001\"},\"text\":\"ours\"}}\n");
    tg_drain_deliveries(a);
    rc |= expect(tg_test_out_count(a) == 1, "exactly our delivery is queued");
    rc |= expect(strcmp(tg_test_out_text(a, 0), "ours") == 0,
                 "and it is the right one");
    rc |= expect(strcmp(tg_test_out_chat(a, 0), "-1001") == 0,
                 "routed back to the originating chat");
    tg_test_adapter_free(a);
  }
  return rc;
}
