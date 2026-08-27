#include "test_support.h"

#include "../../runtime/bus/media_manifest.h"
#include "../../adapters/discord/discord_internal.h"
#include "../../adapters/telegram/telegram_internal.h"
#include "../../runtime/support/heap_guard.h"
#include "../../runtime/support/sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int appendf(char *out, size_t out_cap, size_t *used,
                   const char *fmt, ...) {
  va_list ap;
  int n;
  if (!out || !used || *used >= out_cap) return -1;
  va_start(ap, fmt);
  n = vsnprintf(out + *used, out_cap - *used, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= out_cap - *used) return -1;
  *used += (size_t)n;
  return 0;
}

static int build_discord_message(char *out, size_t out_cap,
                                 size_t attachment_count,
                                 unsigned long long size_bytes) {
  size_t used = 0;
  if (appendf(out, out_cap, &used,
              "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{"
              "\"author\":{\"id\":\"42\"},\"channel_id\":\"99\","
              "\"content\":\"\",\"attachments\":[") != 0)
    return -1;
  for (size_t i = 0; i < attachment_count; ++i) {
    if (appendf(out, out_cap, &used,
                "%s{\"id\":\"%zu\",\"filename\":\"item-%zu.bin\","
                "\"content_type\":\"application/octet-stream\","
                "\"size\":%llu,"
                "\"url\":\"https://cdn.discordapp.com/attachments/"
                "11/%zu/item.bin?ex=signed\"}",
                i ? "," : "", i + 1U, i + 1U, size_bytes, i + 1U) != 0)
      return -1;
  }
  return appendf(out, out_cap, &used, "]}}");
}

int sha256_known_vectors_and_streaming_match(void) {
  static const char empty_sha[] =
      "e3b0c44298fc1c149afbf4c8996fb924"
      "27ae41e4649b934ca495991b7852b855";
  static const char abc_sha[] =
      "ba7816bf8f01cfea414140de5dae2223"
      "b00361a396177a9cb410ff61f20015ad";
  static const char million_a_sha[] =
      "cdc76e5c9914fb9281a1c7e284d73e67"
      "f1809a48a497200e046d39ccc7112cd0";
  unsigned char digest[FC_SHA256_DIGEST_SIZE];
  char hex[FC_SHA256_HEX_SIZE];
  char a_block[1000];
  FcSha256 streaming;
  int rc = 0;
  fc_sha256("", 0U, digest);
  fc_sha256_hex(digest, hex);
  rc |= expect(strcmp(hex, empty_sha) == 0, "SHA-256 empty known vector");
  fc_sha256("abc", 3U, digest);
  fc_sha256_hex(digest, hex);
  rc |= expect(strcmp(hex, abc_sha) == 0, "SHA-256 abc known vector");
  fc_sha256_init(&streaming);
  fc_sha256_update(&streaming, "a", 1U);
  fc_sha256_update(&streaming, "b", 1U);
  fc_sha256_update(&streaming, "c", 1U);
  fc_sha256_final(&streaming, digest);
  fc_sha256_hex(digest, hex);
  rc |= expect(strcmp(hex, abc_sha) == 0,
               "streamed SHA-256 matches one-shot vector");
  memset(a_block, 'a', sizeof(a_block));
  fc_sha256_init(&streaming);
  for (size_t i = 0; i < 1000U; ++i)
    fc_sha256_update(&streaming, a_block, sizeof(a_block));
  fc_sha256_final(&streaming, digest);
  fc_sha256_hex(digest, hex);
  rc |= expect(strcmp(hex, million_a_sha) == 0,
               "SHA-256 matches the million-byte multi-block vector");
  return rc;
}

int media_manifest_roundtrip_is_private_and_tamper_evident(void) {
  const FcMediaDescriptor descriptors[] = {
      {.id = "1001",
       .filename = "salad.png",
       .media_type = "image/png",
       .size_bytes = 8411U,
       .source_url =
           "https://cdn.discordapp.com/attachments/11/22/salad.png?ex=one"},
      {.id = "1002",
       .filename = "nutrition.pdf",
       .media_type = "application/pdf",
       .size_bytes = 1200U,
       .source_url =
           "https://media.discordapp.net/attachments/11/23/nutrition.pdf"}};
  FcMediaDescriptor zero = descriptors[0];
  FcMediaDescriptor loopback = descriptors[0];
  FcMediaManifestRef ref;
  FcMediaManifestRef bad_ref;
  FcMediaManifestRef verified;
  FcMediaManifest loaded = {0};
  struct stat st;
  char ref_json[FC_MEDIA_REF_JSON_SIZE];
  char path[256];
  char err[256] = "";
  char *text = NULL;
  char *saved_loopback = NULL;
  const char *prior_loopback;
  int rc = 0;

  rc |= test_reset_workspace();
  rc |= expect(fc_media_manifest_write(descriptors, 2U, &ref,
                                       err, sizeof(err)) == 0,
               err[0] ? err : "write media manifest");
  rc |= expect(snprintf(path, sizeof(path), "workspace/%s", ref.manifest) <
                   (int)sizeof(path),
               "build media manifest path");
  rc |= expect(stat(path, &st) == 0 && S_ISREG(st.st_mode),
               "manifest is a regular file");
  rc |= expect((st.st_mode & 0777) == 0600, "manifest mode is exactly 0600");
  rc |= expect(ref.count == 2U && ref.total_bytes == 9611U,
               "manifest reference preserves aggregates");
  rc |= expect(fc_media_manifest_ref_json(&ref, ref_json,
                                          sizeof(ref_json)) == 0,
               "serialize compact manifest reference");
  rc |= expect_substr(ref_json, "\"version\":1",
                      "compact reference has schema version");
  rc |= expect_no_substr(ref_json, "salad.png",
                         "compact reference omits filenames");
  rc |= expect_no_substr(ref_json, "discordapp.com",
                         "compact reference omits source URLs");
  rc |= expect(fc_media_manifest_read(&ref, &loaded,
                                      err, sizeof(err)) == 0,
               err[0] ? err : "read verified media manifest");
  rc |= expect(loaded.count == 2U && loaded.total_bytes == 9611U,
               "reader returns verified aggregates");
  rc |= expect(strcmp(loaded.items[0].filename, "salad.png") == 0 &&
                   strcmp(loaded.items[1].media_type, "application/pdf") == 0,
               "reader preserves descriptor order");
  fc_media_manifest_dispose(&loaded);
  rc |= expect(fc_media_manifest_read_path(ref.manifest, &loaded, &verified,
                                           err, sizeof(err)) == 0,
               err[0] ? err : "path-only reader verifies manifest");
  rc |= expect(strcmp(verified.sha256, ref.sha256) == 0 &&
                   verified.count == ref.count &&
                   verified.total_bytes == ref.total_bytes,
               "path-only reader reconstructs complete reference");
  fc_media_manifest_dispose(&loaded);

  bad_ref = ref;
  snprintf(bad_ref.manifest, sizeof(bad_ref.manifest),
           "media/inbound/../%s.json", bad_ref.sha256);
  rc |= expect(fc_media_manifest_read(&bad_ref, &loaded,
                                      err, sizeof(err)) != 0,
               "reader rejects manifest path traversal");

  zero.size_bytes = 0U;
  rc |= expect(fc_media_manifest_write(&zero, 1U, &verified,
                                       err, sizeof(err)) != 0,
               "zero-byte descriptor is rejected");

  loopback.source_url = "http://127.0.0.1:18080/salad.png";
  prior_loopback = getenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK");
  if (prior_loopback) saved_loopback = strdup(prior_loopback);
  (void)unsetenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK");
  rc |= expect(fc_media_manifest_write(&loopback, 1U, &verified,
                                       err, sizeof(err)) != 0,
               "production manifest validation rejects HTTP loopback");
  rc |= expect(setenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK", "1", 1) == 0,
               "enable hermetic media loopback exception");
  rc |= expect(fc_media_manifest_write(&loopback, 1U, &verified,
                                       err, sizeof(err)) == 0,
               err[0] ? err : "test-only loopback manifest is accepted");
  rc |= expect(fc_media_manifest_read_path(verified.manifest, &loaded, NULL,
                                           err, sizeof(err)) == 0,
               err[0] ? err : "reader shares test-only loopback exception");
  fc_media_manifest_dispose(&loaded);
  if (saved_loopback) {
    (void)setenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK", saved_loopback, 1);
  } else {
    (void)unsetenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK");
  }
  free(saved_loopback);

  rc |= expect(chmod(path, 0644) == 0, "make manifest non-private");
  rc |= expect(fc_media_manifest_read(&ref, &loaded,
                                      err, sizeof(err)) != 0,
               "reader rejects non-private manifest");
  rc |= expect(chmod(path, 0600) == 0, "restore private manifest mode");
  rc |= expect(test_read_file(path, &text) == 0 && text != NULL,
               "read manifest for tamper test");
  if (text) {
    char *needle = strstr(text, "salad.png");
    rc |= expect(needle != NULL, "locate manifest byte to tamper");
    if (needle) needle[0] = 'S';
    rc |= expect(test_write_file(path, text) == 0, "tamper manifest bytes");
  }
  free(text);
  rc |= expect(fc_media_manifest_read(&ref, &loaded,
                                      err, sizeof(err)) != 0,
               "reader rejects digest-tampered manifest");
  fc_media_manifest_dispose(&loaded);
  return rc;
}

int discord_media_parser_is_ordered_dynamic_and_attachment_only(void) {
  static const char image_only[] =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{"
      "\"author\":{\"id\":\"42\"},\"channel_id\":\"99\",\"content\":\"\","
      "\"attachments\":["
      "{\"id\":\"1\",\"filename\":\"salad.png\","
      "\"content_type\":\"image/png\",\"size\":8411,"
      "\"url\":\"https://cdn.discordapp.com/attachments/11/1/salad.png?ex=x\"},"
      "{\"id\":\"2\",\"filename\":\"menu.pdf\","
      "\"content_type\":\"application/pdf\",\"size\":1200,"
      "\"url\":\"https://media.discordapp.net/ephemeral-attachments/11/2/menu.pdf\"},"
      "{\"id\":\"3\",\"filename\":\"note.mp3\","
      "\"content_type\":\"audio/mpeg\",\"size\":900,"
      "\"url\":\"https://cdn.discordapp.com/attachments/11/3/note.mp3\"},"
      "{\"id\":\"4\",\"filename\":\"clip.mp4\",\"size\":700,"
      "\"url\":\"https://cdn.discordapp.com/attachments/11/4/clip.mp4\"}"
      "]}}";
  static const char text_only[] =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{"
      "\"author\":{\"id\":\"42\"},\"channel_id\":\"99\","
      "\"content\":\"hello\",\"attachments\":[]}}";
  DcInboundMessage msg = {0};
  FcHeapStats before;
  FcHeapStats after;
  int rc = 0;

  fc_heap_snapshot(&before);
  rc |= expect(dc_parse_message_create(text_only, &msg) == 0,
               "parse text-only Discord message");
  fc_heap_snapshot(&after);
  rc |= expect(msg.attachments == NULL && msg.attachment_count == 0U,
               "text-only message owns no media allocation");
  rc |= expect(before.mallocs == after.mallocs &&
                   before.callocs == after.callocs &&
                   before.strdups == after.strdups,
               "text-only parse leaves heap hot path unchanged");
  dc_inbound_message_dispose(&msg);

  rc |= expect(dc_parse_message_create(image_only, &msg) == 0,
               "parse attachment-only Discord message");
  rc |= expect(msg.content[0] == '\0' && msg.is_dm,
               "attachment-only DM remains admissible");
  rc |= expect(msg.attachments != NULL && msg.attachment_count == 4U &&
                   msg.attachment_bytes == 11211U,
               "media descriptors are allocated to exact observed count");
  rc |= expect(strcmp(msg.attachments[0].media_type, "image/png") == 0 &&
                   strcmp(msg.attachments[1].media_type, "application/pdf") == 0 &&
                   strcmp(msg.attachments[2].media_type, "audio/mpeg") == 0 &&
                   strcmp(msg.attachments[3].media_type,
                          "application/octet-stream") == 0,
               "image, PDF, audio, and fallback media keep Discord order");
  dc_inbound_message_dispose(&msg);
  rc |= expect(msg.attachments == NULL && msg.attachment_count == 0U &&
                   msg.attachment_bytes == 0U,
               "message disposal clears all media ownership");
  return rc;
}

int discord_media_parser_rejects_untrusted_sources_and_caps(void) {
  char json[32768];
  DcInboundMessage msg = {0};
  int rc = 0;
  rc |= expect(dc_source_url_is_trusted(
                   "https://cdn.discordapp.com/attachments/1/2/a.png?ex=x") == 1,
               "Discord CDN signed attachment URL is trusted");
  rc |= expect(dc_source_url_is_trusted(
                   "https://media.discordapp.net/ephemeral-attachments/1/2/a.png") == 1,
               "Discord media ephemeral URL is trusted");
  rc |= expect(dc_source_url_is_trusted(
                   "http://cdn.discordapp.com/attachments/1/2/a.png") == 0,
               "non-HTTPS Discord URL is rejected");
  rc |= expect(dc_source_url_is_trusted(
                   "https://cdn.discordapp.com.evil.test/attachments/1/2/a.png") == 0,
               "host suffix attack is rejected");
  rc |= expect(dc_source_url_is_trusted(
                   "https://user@cdn.discordapp.com/attachments/1/2/a.png") == 0,
               "URL credentials are rejected");
  rc |= expect(dc_source_url_is_trusted(
                   "https://cdn.discordapp.com:443/attachments/1/2/a.png") == 0,
               "explicit port is rejected");
  rc |= expect(dc_source_url_is_trusted(
                   "https://cdn.discordapp.com/attachments/1/2/a.png#leak") == 0,
               "URL fragment is rejected");
  rc |= expect(dc_source_url_is_trusted(
                   "https://cdn.discordapp.com/external/1/2/a.png") == 0,
               "non-attachment CDN path is rejected");

  rc |= expect(build_discord_message(json, sizeof(json),
                                     FC_MEDIA_MAX_ITEMS + 1U, 1U) == 0,
               "build over-count Discord event");
  rc |= expect(dc_parse_message_create(json, &msg) != 0 &&
                   msg.attachments == NULL,
               "over-count Discord event is rejected and cleaned");
  rc |= expect(build_discord_message(json, sizeof(json), 1U,
                                     FC_MEDIA_MAX_ITEM_BYTES + 1U) == 0,
               "build over-item-size Discord event");
  rc |= expect(dc_parse_message_create(json, &msg) != 0 &&
                   msg.attachments == NULL,
               "over-item-size Discord event is rejected and cleaned");
  rc |= expect(build_discord_message(json, sizeof(json), 3U,
                                     20ULL * 1024ULL * 1024ULL) == 0,
               "build over-total-size Discord event");
  rc |= expect(dc_parse_message_create(json, &msg) != 0 &&
                   msg.attachments == NULL,
               "over-total-size Discord event is rejected and cleaned");
  return rc;
}

int telegram_media_parser_keeps_caption_order_and_largest_photo(void) {
  static const char payload[] =
      "{\"message_id\":17,\"caption\":\"look at these\","
      "\"text\":\"caption wins\","
      "\"chat\":{\"id\":424242,\"type\":\"private\"},"
      "\"from\":{\"id\":424242,\"is_bot\":false},"
      "\"photo\":["
      "{\"file_id\":\"small-file\",\"file_unique_id\":\"small\","
      "\"file_size\":4,\"width\":16,\"height\":16},"
      "{\"file_id\":\"large-file\",\"file_unique_id\":\"large\","
      "\"file_size\":8192,\"width\":1024,\"height\":768}],"
      "\"document\":{\"file_id\":\"doc-file\","
      "\"file_unique_id\":\"doc-unique\",\"file_size\":1200,"
      "\"file_name\":\"field-notes.pdf\","
      "\"mime_type\":\"application/pdf\"}}";
  static const char text_only[] =
      "{\"message_id\":18,\"text\":\"plain telegram\","
      "\"chat\":{\"id\":424242,\"type\":\"private\"},"
      "\"from\":{\"id\":424242,\"is_bot\":false}}";
  JsonRef root;
  TgInboundMessage msg;
  int rc = 0;

  rc |= expect(json_ref_top_object(payload, &root) == 0,
               "parse Telegram media fixture JSON");
  rc |= expect(tg_parse_inbound_message(&root, &msg) == 0,
               "parse Telegram photo and document message");
  rc |= expect(strcmp(msg.text, "look at these") == 0,
               "Telegram caption becomes the model text");
  rc |= expect(msg.media_count == 2U && msg.media_bytes == 9392U,
               "Telegram media aggregates are bounded and complete");
  rc |= expect(strcmp(msg.media[0].file_id, "large-file") == 0 &&
                   strcmp(msg.media[0].id, "large") == 0 &&
                   msg.media[0].size_bytes == 8192U &&
                   strcmp(msg.media[0].media_type, "image/jpeg") == 0,
               "the last and largest photo size is selected first");
  rc |= expect(strcmp(msg.media[1].file_id, "doc-file") == 0 &&
                   strcmp(msg.media[1].filename, "field-notes.pdf") == 0 &&
                   strcmp(msg.media[1].media_type, "application/pdf") == 0 &&
                   msg.media[1].size_bytes == 1200U,
               "the document follows the photo with its metadata intact");

  rc |= expect(json_ref_top_object(text_only, &root) == 0 &&
                   tg_parse_inbound_message(&root, &msg) == 0,
               "text-only Telegram messages remain accepted");
  rc |= expect(msg.media_count == 0U &&
                   strcmp(msg.text, "plain telegram") == 0,
               "text-only Telegram payload shape remains unchanged");
  return rc;
}

int telegram_media_parser_rejects_untrusted_sources_and_caps(void) {
  static const char over_cap[] =
      "{\"message_id\":19,\"caption\":\"too large\","
      "\"chat\":{\"id\":424242,\"type\":\"private\"},"
      "\"from\":{\"id\":424242,\"is_bot\":false},"
      "\"photo\":[{\"file_id\":\"huge-file\","
      "\"file_unique_id\":\"huge\",\"file_size\":20971521}]}";
  JsonRef root;
  TgInboundMessage msg;
  int rc = 0;

  rc |= expect(tg_source_url_is_trusted(
                   "https://api.telegram.org/file/bot123:ABC/photos/a.jpg",
                   "123:ABC") == 1,
               "the exact Telegram file host, token, and safe path are trusted");
  rc |= expect(tg_source_url_is_trusted(
                   "http://api.telegram.org/file/bot123:ABC/photos/a.jpg",
                   "123:ABC") == 0,
               "non-HTTPS Telegram file URLs are rejected");
  rc |= expect(tg_source_url_is_trusted(
                   "https://api.telegram.org.evil.test/file/"
                   "bot123:ABC/photos/a.jpg",
                   "123:ABC") == 0,
               "Telegram file host suffix attacks are rejected");
  rc |= expect(tg_source_url_is_trusted(
                   "https://api.telegram.org/file/bot999:XYZ/photos/a.jpg",
                   "123:ABC") == 0,
               "a Telegram URL for another bot token is rejected");
  rc |= expect(tg_source_url_is_trusted(
                   "https://api.telegram.org/file/bot123:ABC/../secrets",
                   "123:ABC") == 0,
               "Telegram file path traversal is rejected");
  rc |= expect(tg_source_url_is_trusted(
                   "https://api.telegram.org/file/bot123:ABC/photos/a.jpg?q=x",
                   "123:ABC") == 0,
               "Telegram file URL query material is rejected");
  rc |= expect(tg_source_url_is_trusted(
                   "https://api.telegram.org/file/bot123:ABC/photos/a.jpg#x",
                   "123:ABC") == 0,
               "Telegram file URL fragments are rejected");

  rc |= expect(json_ref_top_object(over_cap, &root) == 0,
               "parse oversized Telegram media fixture JSON");
  rc |= expect(tg_parse_inbound_message(&root, &msg) != 0 &&
                   msg.media_count == 0U,
               "Telegram's 20 MiB getFile ceiling is enforced at ingress");
  return rc;
}

int discord_media_publish_keeps_source_urls_private_and_text_shape_stable(void) {
  static const char image_only[] =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{"
      "\"author\":{\"id\":\"42\"},\"channel_id\":\"99\",\"content\":\"\","
      "\"attachments\":[{\"id\":\"1\",\"filename\":\"salad.png\","
      "\"content_type\":\"image/png\",\"size\":8411,"
      "\"url\":\"https://cdn.discordapp.com/attachments/11/1/salad.png?"
      "ex=super-secret-signature\"}]}}";
  DcAdapter adapter;
  DcInboundMessage msg = {0};
  char *bus = NULL;
  char *manifest = NULL;
  char manifest_path[256];
  FcMediaManifestRef ref;
  FcMediaManifest loaded = {0};
  char err[256] = "";
  int rc = 0;

  memset(&adapter, 0, sizeof(adapter));
  snprintf(adapter.module_id_buf, sizeof(adapter.module_id_buf),
           "discord-main");
  rc |= test_reset_workspace();
  rc |= expect(dc_parse_message_create(image_only, &msg) == 0,
               "parse image-only message for publication");
  rc |= expect(dc_publish_message(&adapter, &msg) == 0,
               "publish image-only message");
  rc |= expect(test_read_file("workspace/bus/inbox/bus_000001.json",
                              &bus) == 0,
               "read published image-only bus envelope");
  if (bus) {
    rc |= expect_substr(bus, "\"text\":\"\",\"media\":{\"version\":1",
                        "image-only payload carries compact media reference");
    rc |= expect_no_substr(bus, "super-secret-signature",
                           "bus envelope never contains signed source URL");
    rc |= expect_no_substr(bus, "salad.png",
                           "bus envelope never contains attachment filename");
  }
  rc |= expect(fc_media_manifest_write(msg.attachments, msg.attachment_count,
                                       &ref, err, sizeof(err)) == 0,
               err[0] ? err : "recover content-addressed manifest reference");
  rc |= expect(snprintf(manifest_path, sizeof(manifest_path), "workspace/%s",
                        ref.manifest) < (int)sizeof(manifest_path),
               "build private manifest path");
  rc |= expect(test_read_file(manifest_path, &manifest) == 0,
               "read private media manifest");
  if (manifest)
    rc |= expect_substr(manifest, "super-secret-signature",
                        "signed source URL exists only in private manifest");
  rc |= expect(fc_media_manifest_read(&ref, &loaded,
                                      err, sizeof(err)) == 0,
               err[0] ? err : "published manifest remains readable");
  fc_media_manifest_dispose(&loaded);
  dc_inbound_message_dispose(&msg);
  free(bus);
  free(manifest);

  memset(&msg, 0, sizeof(msg));
  snprintf(msg.user_id, sizeof(msg.user_id), "42");
  snprintf(msg.channel_id, sizeof(msg.channel_id), "99");
  snprintf(msg.content, sizeof(msg.content), "hello");
  msg.is_dm = 1;
  bus = NULL;
  rc |= test_reset_workspace();
  rc |= expect(dc_publish_message(&adapter, &msg) == 0,
               "publish text-only message");
  rc |= expect(test_read_file("workspace/bus/inbox/bus_000001.json",
                              &bus) == 0,
               "read published text-only bus envelope");
  if (bus) {
    /* The shape gained context_id in 0.27.2 so each conversation gets its
     * own memory scope; a DM keys on the user. Everything else, including
     * the ref that routes the reply, is unchanged. */
    rc |= expect_substr(
        bus,
        "\"payload\":{\"text\":\"hello\",\"adapter_id\":\"discord-main\","
        "\"context_id\":\"dm:42\","
        "\"ref\":{\"channel_id\":\"99\",\"guild_id\":\"\","
        "\"user_id\":\"42\",\"scope\":\"dm\"}}",
        "text-only payload shape is exact");
    rc |= expect_no_substr(bus, "\"media\":",
                           "text-only payload has no media field");
  }
  free(bus);
  return rc;
}
