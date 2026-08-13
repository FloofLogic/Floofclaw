#include "sha256.h"

#include <string.h>

static const uint32_t k[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
  0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
  0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
  0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
  0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
  0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
  0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
  0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
  0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
  0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
  0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
  0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
  0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
  0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
  0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotr(uint32_t v, unsigned int n) {
  return (v >> n) | (v << (32U - n));
}

static uint32_t load_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static void transform(FcSha256 *ctx, const unsigned char block[64]) {
  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;
  for (size_t i = 0; i < 16U; ++i) w[i] = load_be32(block + i * 4U);
  for (size_t i = 16U; i < 64U; ++i) {
    uint32_t s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^
                  (w[i - 15U] >> 3);
    uint32_t s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^
                  (w[i - 2U] >> 10);
    w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
  }
  a = ctx->state[0]; b = ctx->state[1];
  c = ctx->state[2]; d = ctx->state[3];
  e = ctx->state[4]; f = ctx->state[5];
  g = ctx->state[6]; h = ctx->state[7];
  for (size_t i = 0; i < 64U; ++i) {
    uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
    uint32_t choose = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + s1 + choose + k[i] + w[i];
    uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + majority;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  ctx->state[0] += a; ctx->state[1] += b;
  ctx->state[2] += c; ctx->state[3] += d;
  ctx->state[4] += e; ctx->state[5] += f;
  ctx->state[6] += g; ctx->state[7] += h;
}

void fc_sha256_init(FcSha256 *ctx) {
  if (!ctx) return;
  ctx->state[0] = 0x6a09e667U;
  ctx->state[1] = 0xbb67ae85U;
  ctx->state[2] = 0x3c6ef372U;
  ctx->state[3] = 0xa54ff53aU;
  ctx->state[4] = 0x510e527fU;
  ctx->state[5] = 0x9b05688cU;
  ctx->state[6] = 0x1f83d9abU;
  ctx->state[7] = 0x5be0cd19U;
  ctx->total_bytes = 0;
  ctx->block_len = 0;
}

void fc_sha256_update(FcSha256 *ctx, const void *data, size_t len) {
  const unsigned char *src = (const unsigned char *)data;
  if (!ctx || (!src && len != 0U)) return;
  ctx->total_bytes += (uint64_t)len;
  while (len > 0U) {
    size_t room = sizeof(ctx->block) - ctx->block_len;
    size_t take = len < room ? len : room;
    memcpy(ctx->block + ctx->block_len, src, take);
    ctx->block_len += take;
    src += take;
    len -= take;
    if (ctx->block_len == sizeof(ctx->block)) {
      transform(ctx, ctx->block);
      ctx->block_len = 0;
    }
  }
}

void fc_sha256_final(FcSha256 *ctx,
                     unsigned char digest[FC_SHA256_DIGEST_SIZE]) {
  uint64_t bit_len;
  if (!ctx || !digest) return;
  bit_len = ctx->total_bytes * 8U;
  ctx->block[ctx->block_len++] = 0x80U;
  if (ctx->block_len > 56U) {
    memset(ctx->block + ctx->block_len, 0, sizeof(ctx->block) - ctx->block_len);
    transform(ctx, ctx->block);
    ctx->block_len = 0;
  }
  memset(ctx->block + ctx->block_len, 0, 56U - ctx->block_len);
  for (size_t i = 0; i < 8U; ++i)
    ctx->block[63U - i] = (unsigned char)(bit_len >> (i * 8U));
  transform(ctx, ctx->block);
  for (size_t i = 0; i < 8U; ++i)
    store_be32(digest + i * 4U, ctx->state[i]);
  memset(ctx, 0, sizeof(*ctx));
}

void fc_sha256(const void *data, size_t len,
               unsigned char digest[FC_SHA256_DIGEST_SIZE]) {
  FcSha256 ctx;
  fc_sha256_init(&ctx);
  fc_sha256_update(&ctx, data, len);
  fc_sha256_final(&ctx, digest);
}

void fc_sha256_hex(const unsigned char digest[FC_SHA256_DIGEST_SIZE],
                   char hex[FC_SHA256_HEX_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  if (!digest || !hex) return;
  for (size_t i = 0; i < FC_SHA256_DIGEST_SIZE; ++i) {
    hex[i * 2U] = digits[digest[i] >> 4];
    hex[i * 2U + 1U] = digits[digest[i] & 0x0fU];
  }
  hex[FC_SHA256_HEX_SIZE - 1U] = '\0';
}
