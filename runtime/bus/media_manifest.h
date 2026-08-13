#ifndef FCLAW_BUS_MEDIA_MANIFEST_H
#define FCLAW_BUS_MEDIA_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#define FC_MEDIA_MAX_ITEMS          10U
#define FC_MEDIA_MAX_ITEM_BYTES     (25ULL * 1024ULL * 1024ULL)
#define FC_MEDIA_MAX_TOTAL_BYTES    (50ULL * 1024ULL * 1024ULL)
#define FC_MEDIA_ID_MAX             128U
#define FC_MEDIA_FILENAME_MAX       256U
#define FC_MEDIA_TYPE_MAX           128U
#define FC_MEDIA_SOURCE_URL_MAX     2048U
#define FC_MEDIA_SHA256_HEX_SIZE    65U
#define FC_MEDIA_MANIFEST_PATH_SIZE 128U
#define FC_MEDIA_REF_JSON_SIZE      384U

typedef struct {
  const char *id;
  const char *filename;
  const char *media_type;
  uint64_t size_bytes;
  const char *source_url;
} FcMediaDescriptor;

typedef struct {
  char manifest[FC_MEDIA_MANIFEST_PATH_SIZE];
  char sha256[FC_MEDIA_SHA256_HEX_SIZE];
  size_t count;
  uint64_t total_bytes;
} FcMediaManifestRef;

typedef struct {
  FcMediaDescriptor *items;
  size_t count;
  uint64_t total_bytes;
} FcMediaManifest;

/* Validate transport-neutral descriptor bounds, MIME syntax, size, and the
 * generic HTTPS URL envelope. Channel adapters must additionally enforce
 * their authenticated source-host policy before calling the writer. */
int fc_media_manifest_validate_descriptor(const FcMediaDescriptor *item,
                                          char *err, size_t err_cap);

/* Write the canonical descriptor array as an immutable, content-addressed
 * 0600 file below workspace/media/inbound/. The digest covers the complete
 * one-line JSON record, including its final newline. */
int fc_media_manifest_write(const FcMediaDescriptor *items, size_t count,
                            FcMediaManifestRef *out,
                            char *err, size_t err_cap);

/* Serialize only the compact, non-secret reference suitable for bus and
 * event payloads. */
int fc_media_manifest_ref_json(const FcMediaManifestRef *ref,
                               char *out, size_t out_cap);

/* Load and verify an exact compact reference. The reader rejects path drift,
 * symlinks, non-private/non-regular files, digest or aggregate mismatches,
 * schema additions, and non-canonical JSON. */
int fc_media_manifest_read(const FcMediaManifestRef *ref,
                           FcMediaManifest *out,
                           char *err, size_t err_cap);
/* Child-process convenience for the narrow `--media <workspace-relative
 * manifest>` handoff. The digest is derived from the exact path; canonical
 * count and total are returned in verified_ref when non-NULL. */
int fc_media_manifest_read_path(const char *manifest,
                                FcMediaManifest *out,
                                FcMediaManifestRef *verified_ref,
                                char *err, size_t err_cap);
void fc_media_manifest_dispose(FcMediaManifest *manifest);

#endif
