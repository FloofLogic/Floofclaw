/* One conversation, one context.
 *
 * Neither the Discord nor the IRC adapter published a `context_id`, so
 * intake fell through to `chat:<channel>:<adapter_id>` and every guild
 * channel, thread, DM, and user on a deployment shared ONE context. That is
 * one memory scope and one compacted summary across everybody, one
 * serialize_contexts queue for the whole guild, and DM content recallable
 * from a public channel. These pin the key each adapter derives, that it
 * actually reaches the run, and that memory written in one conversation is
 * invisible from another. */

#include "test_support.h"
#include "harness.h"

#include "../../adapters/discord/discord_internal.h"
#include "../../adapters/irc/irc_adapter.h"
#include "../../runtime/bus/bus.h"
#include "../../runtime/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int context_of_run(const char *run_id, char *out, size_t out_len) {
  char *state = harness_read_runstate(run_id);
  int rc;
  if (!state) return -1;
  rc = rt_json_get_string(state, "context_id", out, out_len);
  free(state);
  return rc;
}

/* Write one memory record as the given context, the way memory.after does. */
static int remember(const char *context_id, const char *text) {
  RtContext *ctx = (RtContext *)calloc(1, sizeof(*ctx));
  char payload[512];
  int rc;
  if (!ctx) return -1;
  snprintf(ctx->context_id, sizeof(ctx->context_id), "%s", context_id);
  snprintf(ctx->run_id, sizeof(ctx->run_id), "run_mem");
  snprintf(payload, sizeof(payload), "{\"text\":\"%s\"}", text);
  rc = rt_memory_apply(ctx, "user", payload, "2026-08-23T00:00:00.000Z",
                       "run_mem");
  free(ctx);
  return rc;
}

static int recalls(const char *context_id, const char *needle) {
  char *projection = (char *)malloc(RT_XL);
  int found;
  if (!projection) return -1;
  if (rt_memory_projection_json(context_id, 20, projection, RT_XL) != 0) {
    free(projection);
    return -1;
  }
  found = strstr(projection, needle) != NULL;
  free(projection);
  return found;
}

int discord_channels_threads_and_dms_are_separate_contexts(void) {
  DcAdapter *a = NULL;
  DcInboundMessage msg;
  char key[256] = "";
  char ctx_a[RT_MED] = "", ctx_b[RT_MED] = "", ctx_dm[RT_MED] = "";
  char *envelope = NULL;
  HarnessGateway *g = NULL;
  int rc = 0;

  rc |= test_reset_workspace();
  a = (DcAdapter *)calloc(1, sizeof(*a));
  rc |= expect(a != NULL, "allocate the discord fixture adapter");
  if (!a) return rc;
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "discord-main");

  /* The key itself: channel id for a guild channel or thread, dm:<user>
   * for a direct message. */
  memset(&msg, 0, sizeof(msg));
  snprintf(msg.channel_id, sizeof(msg.channel_id), "111");
  snprintf(msg.guild_id, sizeof(msg.guild_id), "900");
  snprintf(msg.user_id, sizeof(msg.user_id), "u1");
  dc_context_id(&msg, key, sizeof(key));
  rc |= expect(strcmp(key, "111") == 0, "a guild channel keys on its id");
  snprintf(msg.channel_id, sizeof(msg.channel_id), "222");
  dc_context_id(&msg, key, sizeof(key));
  rc |= expect(strcmp(key, "222") == 0,
               "a thread keys on its own channel id");
  msg.is_dm = 1;
  dc_context_id(&msg, key, sizeof(key));
  rc |= expect(strcmp(key, "dm:u1") == 0, "a DM keys on the user");

  /* Two channels and a DM, published through the real path. */
  memset(&msg, 0, sizeof(msg));
  snprintf(msg.guild_id, sizeof(msg.guild_id), "900");
  snprintf(msg.user_id, sizeof(msg.user_id), "u1");
  snprintf(msg.channel_id, sizeof(msg.channel_id), "111");
  snprintf(msg.content, sizeof(msg.content), "hello from channel one");
  rc |= expect(dc_publish_message(a, &msg) == 0, "publish channel one");
  snprintf(msg.channel_id, sizeof(msg.channel_id), "222");
  snprintf(msg.content, sizeof(msg.content), "hello from channel two");
  rc |= expect(dc_publish_message(a, &msg) == 0, "publish channel two");
  memset(&msg, 0, sizeof(msg));
  snprintf(msg.channel_id, sizeof(msg.channel_id), "333");
  snprintf(msg.user_id, sizeof(msg.user_id), "u1");
  snprintf(msg.content, sizeof(msg.content), "a private confession");
  msg.is_dm = 1;
  rc |= expect(dc_publish_message(a, &msg) == 0, "publish the DM");

  rc |= test_read_file("workspace/bus/inbox/bus_000001.json", &envelope) == 0
            ? 0 : expect(0, "the first discord envelope exists");
  if (envelope) {
    rc |= expect_substr(envelope, "\"context_id\":\"111\"",
                        "the envelope carries the conversation key");
    rc |= expect_substr(envelope, "\"ref\":{\"channel_id\":\"111\"",
                        "the reply route is unchanged");
    free(envelope); envelope = NULL;
  }

  g = harness_gateway_init("fast");
  rc |= expect(g != NULL, "init gateway for the discord context probe");
  if (g) {
    rc |= expect(harness_gateway_drive_until_completed(g, 3, 20000) == 0,
                 "all three discord messages complete");
    harness_gateway_close(g);
  }
  rc |= expect(context_of_run("run_001", ctx_a, sizeof(ctx_a)) == 0 &&
                   context_of_run("run_002", ctx_b, sizeof(ctx_b)) == 0 &&
                   context_of_run("run_003", ctx_dm, sizeof(ctx_dm)) == 0,
               "every discord run records a context");
  rc |= expect(strcmp(ctx_a, "chat:discord:111") == 0,
               ctx_a[0] ? ctx_a : "channel one gets its own context");
  rc |= expect(strcmp(ctx_b, "chat:discord:222") == 0,
               ctx_b[0] ? ctx_b : "channel two gets its own context");
  rc |= expect(strcmp(ctx_dm, "chat:discord:dm:u1") == 0,
               ctx_dm[0] ? ctx_dm : "the DM gets its own context");
  rc |= expect(strcmp(ctx_a, ctx_b) != 0 && strcmp(ctx_a, ctx_dm) != 0 &&
                   strcmp(ctx_b, ctx_dm) != 0,
               "three conversations produce three distinct contexts");
  rc |= expect_no_substr(ctx_a, "discord-main",
                         "the adapter id is no longer the conversation key");

  /* And memory does not cross between them. */
  rc |= expect(remember(ctx_dm, "the private confession") == 0,
               "record memory in the DM context");
  rc |= expect(recalls(ctx_dm, "the private confession") == 1,
               "the DM recalls its own memory");
  rc |= expect(recalls(ctx_a, "the private confession") == 0,
               "a public channel cannot recall DM memory");
  rc |= expect(recalls(ctx_b, "the private confession") == 0,
               "another channel cannot recall DM memory");

  free(a);
  return rc;
}

int irc_channels_and_private_messages_are_separate_contexts(void) {
  char key[256] = "";
  char ctx_ops[RT_MED], ctx_dev[RT_MED], ctx_pm[RT_MED];
  int rc = 0;

  rc |= test_reset_workspace();

  irc_context_id("#ops", "rick", key, sizeof(key));
  rc |= expect(strcmp(key, "#ops") == 0, "a channel keys on the channel");
  irc_context_id("&local", "rick", key, sizeof(key));
  rc |= expect(strcmp(key, "&local") == 0, "a local channel keys the same way");
  irc_context_id("floofbot", "rick", key, sizeof(key));
  rc |= expect(strcmp(key, "dm:rick") == 0,
               "a private message keys on the sender");
  /* IRC names are case-insensitive: one room, one context. */
  irc_context_id("#OPS", "rick", key, sizeof(key));
  rc |= expect(strcmp(key, "#ops") == 0,
               "channel case does not split a conversation");
  irc_context_id("floofbot", "RICK", key, sizeof(key));
  rc |= expect(strcmp(key, "dm:rick") == 0,
               "nick case does not split a conversation");
  irc_context_id("", "rick", key, sizeof(key));
  rc |= expect(key[0] == '\0', "an empty target yields no key");

  /* Intake namespaces them, and memory stays inside each one. */
  snprintf(ctx_ops, sizeof(ctx_ops), "chat:irc:#ops");
  snprintf(ctx_dev, sizeof(ctx_dev), "chat:irc:#dev");
  snprintf(ctx_pm, sizeof(ctx_pm), "chat:irc:dm:rick");
  rc |= expect(remember(ctx_pm, "a private irc note") == 0,
               "record memory in the private-message context");
  rc |= expect(remember(ctx_ops, "an ops channel note") == 0,
               "record memory in the ops channel context");
  rc |= expect(recalls(ctx_pm, "a private irc note") == 1,
               "the private message recalls its own memory");
  rc |= expect(recalls(ctx_ops, "a private irc note") == 0,
               "a channel cannot recall private-message memory");
  rc |= expect(recalls(ctx_dev, "an ops channel note") == 0,
               "one channel cannot recall another channel's memory");
  rc |= expect(recalls(ctx_ops, "an ops channel note") == 1,
               "the ops channel still recalls its own memory");
  return rc;
}
