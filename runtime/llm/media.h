#ifndef FCLAW_LLM_MEDIA_H
#define FCLAW_LLM_MEDIA_H

#include "llm.h"

/* Load one canonical ingress manifest and materialize its ordered items under
 * one aggregate download deadline as verified run-local files. The returned
 * descriptor array owns no file bytes and is allocated only when media
 * exists. */
int llm_media_load(const char *run_dir, const char *manifest_path,
                   LlmMedia **media_out, size_t *media_count_out,
                   uint64_t *media_bytes_out,
                   char *err, size_t err_len);

void llm_media_dispose(LlmMedia *media);

#endif
