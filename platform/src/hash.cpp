// CRC32 (reflected, poly 0xEDB88320) and SHA-256 (FIPS 180-4), own implementations.
#include "internal.h"

namespace {

struct Crc32Table {
  uint32_t t[256];
  constexpr Crc32Table() : t() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
  }
};
constexpr Crc32Table kCrc;

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void transform(sq_sha256_ctx* c, const uint8_t* b) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) | (uint32_t(b[i * 4 + 2]) << 8) | uint32_t(b[i * 4 + 3]);
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->state[0], bb = c->state[1], cc = c->state[2], d = c->state[3], e = c->state[4], f = c->state[5], g = c->state[6],
           h = c->state[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + kK[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & bb) ^ (a & cc) ^ (bb & cc);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = bb;
    bb = a;
    a = t1 + t2;
  }
  c->state[0] += a;
  c->state[1] += bb;
  c->state[2] += cc;
  c->state[3] += d;
  c->state[4] += e;
  c->state[5] += f;
  c->state[6] += g;
  c->state[7] += h;
}

}  // namespace

extern "C" {

uint32_t sq_crc32(uint32_t crc, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = kCrc.t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void sq_sha256_init(sq_sha256_ctx* c) {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (int i = 0; i < 8; ++i) c->state[i] = init[i];
  c->total = 0;
  c->buflen = 0;
}

void sq_sha256_update(sq_sha256_ctx* c, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  c->total += len;
  while (len > 0) {
    if (c->buflen == 0 && len >= 64) {
      transform(c, p);
      p += 64;
      len -= 64;
      continue;
    }
    size_t n = 64 - c->buflen;
    if (n > len) n = len;
    std::memcpy(c->buf + c->buflen, p, n);
    c->buflen += static_cast<uint32_t>(n);
    p += n;
    len -= n;
    if (c->buflen == 64) {
      transform(c, c->buf);
      c->buflen = 0;
    }
  }
}

void sq_sha256_final(sq_sha256_ctx* c, uint8_t out[32]) {
  uint64_t bits = c->total * 8;
  uint8_t pad = 0x80;
  // Append 0x80 then zeros until 56 mod 64, then the 64-bit big-endian length.
  sq_sha256_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->buflen != 56) sq_sha256_update(c, &zero, 1);
  uint8_t lenb[8];
  for (int i = 0; i < 8; ++i) lenb[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  sq_sha256_update(c, lenb, 8);
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(c->state[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(c->state[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(c->state[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(c->state[i]);
  }
}

void sq_sha256(const void* data, size_t len, uint8_t out[32]) {
  sq_sha256_ctx c;
  sq_sha256_init(&c);
  sq_sha256_update(&c, data, len);
  sq_sha256_final(&c, out);
}

void sq_hex_encode(const void* data, size_t len, char* out) {
  static const char hx[] = "0123456789abcdef";
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = hx[p[i] >> 4];
    out[i * 2 + 1] = hx[p[i] & 15];
  }
  out[len * 2] = '\0';
}

}  // extern "C"
