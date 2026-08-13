#ifndef FCLAW_SUPPORT_SHA256_H
#define FCLAW_SUPPORT_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define FC_SHA256_DIGEST_SIZE 32U
#define FC_SHA256_HEX_SIZE    65U

typedef struct {
  uint32_t state[8];
  uint64_t total_bytes;
  unsigned char block[64];
  size_t block_len;
} FcSha256;

void fc_sha256_init(FcSha256 *ctx);
void fc_sha256_update(FcSha256 *ctx, const void *data, size_t len);
void fc_sha256_final(FcSha256 *ctx,
                     unsigned char digest[FC_SHA256_DIGEST_SIZE]);
void fc_sha256(const void *data, size_t len,
               unsigned char digest[FC_SHA256_DIGEST_SIZE]);
void fc_sha256_hex(const unsigned char digest[FC_SHA256_DIGEST_SIZE],
                   char hex[FC_SHA256_HEX_SIZE]);

#endif
