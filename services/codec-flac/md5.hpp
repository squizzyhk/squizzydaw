// codec-flac: small MD5 (RFC 1321) for the FLAC STREAMINFO audio signature. Own implementation.
#pragma once

#include <cstdint>
#include <cstring>

namespace sq::flac {

class Md5 {
 public:
  Md5() { reset(); }
  void reset() {
    a_ = 0x67452301u; b_ = 0xefcdab89u; c_ = 0x98badcfeu; d_ = 0x10325476u;
    len_ = 0;
    fill_ = 0;
  }
  void update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += n;
    if (fill_) {
      size_t take = 64 - fill_ < n ? 64 - fill_ : n;
      std::memcpy(buf_ + fill_, p, take);
      fill_ += take; p += take; n -= take;
      if (fill_ == 64) { block(buf_); fill_ = 0; }
    }
    while (n >= 64) { block(p); p += 64; n -= 64; }
    if (n) { std::memcpy(buf_, p, n); fill_ = n; }
  }
  void final(uint8_t out[16]) {
    const uint64_t bits = len_ * 8;
    static const uint8_t pad[64] = {0x80};
    size_t padLen = fill_ < 56 ? 56 - fill_ : 120 - fill_;
    update(pad, padLen);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = static_cast<uint8_t>(bits >> (8 * i));
    update(lb, 8);
    const uint32_t v[4] = {a_, b_, c_, d_};
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(v[i] >> (8 * j));
  }

 private:
  static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
      m[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
             (static_cast<uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
    uint32_t a = a_, b = b_, c = c_, d = d_;
    for (int i = 0; i < 64; ++i) {
      uint32_t f;
      int g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~d); g = (7 * i) & 15; }
      uint32_t t = d;
      d = c;
      c = b;
      b = b + rol(a + f + K[i] + m[g], S[i]);
      a = t;
    }
    a_ += a; b_ += b; c_ += c; d_ += d;
  }
  uint32_t a_, b_, c_, d_;
  uint64_t len_;
  uint8_t buf_[64];
  size_t fill_;
};

}  // namespace sq::flac
