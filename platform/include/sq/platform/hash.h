/* Squizzy Studio platform - hashing: CRC32 (IEEE 802.3), SHA-256, FNV-1a. Realtime safe. */
#ifndef SQ_PLATFORM_HASH_H
#define SQ_PLATFORM_HASH_H

#include "sq/platform/api.h"

/* Incremental: start with crc = 0. sq_crc32(0, "123456789", 9) == 0xCBF43926. */
SQP_API uint32_t sq_crc32(uint32_t crc, const void* data, size_t len);

typedef struct sq_sha256_ctx {
  uint32_t state[8];
  uint64_t total;
  uint8_t buf[64];
  uint32_t buflen;
} sq_sha256_ctx;
SQP_API void sq_sha256_init(sq_sha256_ctx* c);
SQP_API void sq_sha256_update(sq_sha256_ctx* c, const void* data, size_t len);
SQP_API void sq_sha256_final(sq_sha256_ctx* c, uint8_t out[32]);
SQP_API void sq_sha256(const void* data, size_t len, uint8_t out[32]);

/* Lowercase hex; out must hold 2*len+1 chars. */
SQP_API void sq_hex_encode(const void* data, size_t len, char* out);

SQ_INLINE uint64_t sq_fnv1a64(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint64_t h = 14695981039346656037ull;
  size_t i;
  for (i = 0; i < len; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

#endif
