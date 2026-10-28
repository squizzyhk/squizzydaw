// audiodriver-alsa: float32 <-> FLOAT_LE / S16_LE / S24_3LE / S24_LE / S32_LE byte vectors, exact
// integer round trips, clipping, NaN, and the interleave / de-interleave channel mapping.
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "alsa_convert.hpp"
#include "sqtest.hpp"

using sq::alsa::Fmt;

namespace {
std::vector<uint8_t> enc(Fmt f, float x) {
  std::vector<uint8_t> b(sq::alsa::bytesPerSample(f), 0xAA);
  sq::alsa::encodeSample(f, x, b.data());
  return b;
}
bool same(const std::vector<uint8_t>& a, std::initializer_list<int> b) {
  if (a.size() != b.size()) return false;
  size_t i = 0;
  for (int v : b)
    if (a[i++] != uint8_t(v)) return false;
  return true;
}
}  // namespace

SQ_TEST("alsa convert: S16_LE byte vectors, clipping, NaN") {
  CHECK(same(enc(Fmt::S16, 0.0f), {0x00, 0x00}));
  CHECK(same(enc(Fmt::S16, 0.5f), {0x00, 0x40}));
  CHECK(same(enc(Fmt::S16, -0.5f), {0x00, 0xC0}));
  CHECK(same(enc(Fmt::S16, 1.0f), {0xFF, 0x7F}));   // +1.0 clips to 32767
  CHECK(same(enc(Fmt::S16, -1.0f), {0x00, 0x80}));  // -32768
  CHECK(same(enc(Fmt::S16, 7.0f), {0xFF, 0x7F}));
  CHECK(same(enc(Fmt::S16, -7.0f), {0x00, 0x80}));
  CHECK(same(enc(Fmt::S16, std::numeric_limits<float>::quiet_NaN()), {0x00, 0x00}));
  const uint8_t m1[2] = {0xFF, 0xFF};
  CHECK_EQ(sq::alsa::decodeSample(Fmt::S16, m1), -1.0f / 32768.0f);
  const uint8_t mn[2] = {0x00, 0x80};
  CHECK_EQ(sq::alsa::decodeSample(Fmt::S16, mn), -1.0f);
}

SQ_TEST("alsa convert: S24_3LE and S24_LE byte vectors") {
  CHECK(same(enc(Fmt::S24_3, 0.5f), {0x00, 0x00, 0x40}));
  CHECK(same(enc(Fmt::S24_3, -1.0f), {0x00, 0x00, 0x80}));
  CHECK(same(enc(Fmt::S24_3, 1.0f), {0xFF, 0xFF, 0x7F}));
  CHECK(same(enc(Fmt::S24_3, -1.0f / 8388608.0f), {0xFF, 0xFF, 0xFF}));
  // S24_LE: low three bytes carry the value, the container is sign-extended.
  CHECK(same(enc(Fmt::S24_32, -1.0f / 8388608.0f), {0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(same(enc(Fmt::S24_32, 0.25f), {0x00, 0x00, 0x20, 0x00}));
  const uint8_t junkTop[4] = {0x00, 0x00, 0x40, 0x7E};  // decoder ignores the top byte
  CHECK_EQ(sq::alsa::decodeSample(Fmt::S24_32, junkTop), 0.5f);
  const uint8_t neg[3] = {0x00, 0x00, 0xC0};
  CHECK_EQ(sq::alsa::decodeSample(Fmt::S24_3, neg), -0.5f);
}

SQ_TEST("alsa convert: S32_LE and FLOAT_LE byte vectors") {
  CHECK(same(enc(Fmt::S32, 1.0f), {0xFF, 0xFF, 0xFF, 0x7F}));
  CHECK(same(enc(Fmt::S32, -1.0f), {0x00, 0x00, 0x00, 0x80}));
  CHECK(same(enc(Fmt::S32, -0.5f), {0x00, 0x00, 0x00, 0xC0}));
  CHECK(same(enc(Fmt::F32, 1.0f), {0x00, 0x00, 0x80, 0x3F}));
  CHECK(same(enc(Fmt::F32, -2.0f), {0x00, 0x00, 0x00, 0xC0}));
  const uint8_t f[4] = {0x00, 0x00, 0x40, 0x3F};
  CHECK_EQ(sq::alsa::decodeSample(Fmt::F32, f), 0.75f);
  const uint8_t s[4] = {0x00, 0x00, 0x00, 0x40};
  CHECK_EQ(sq::alsa::decodeSample(Fmt::S32, s), 0.5f);
}

SQ_TEST("alsa convert: integer -> float -> integer round trips are exact") {
  for (int v = -32768; v <= 32767; ++v) {
    uint8_t b[2] = {uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF)};
    uint8_t o[2];
    sq::alsa::encodeSample(Fmt::S16, sq::alsa::decodeSample(Fmt::S16, b), o);
    REQUIRE(std::memcmp(b, o, 2) == 0);
  }
  for (int v = -8388608; v <= 8388607; v += 97) {
    uint8_t b[3] = {uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF), uint8_t((v >> 16) & 0xFF)};
    uint8_t o[3];
    sq::alsa::encodeSample(Fmt::S24_3, sq::alsa::decodeSample(Fmt::S24_3, b), o);
    REQUIRE(std::memcmp(b, o, 3) == 0);
  }
  // S32: float32 carries 24 bits of mantissa, so 24-bit-aligned codes survive exactly.
  for (int64_t v = INT32_MIN; v <= INT32_MAX; v += int64_t(256) * 7919) {
    const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(v) & ~0xFF);
    uint8_t b[4] = {uint8_t(u), uint8_t(u >> 8), uint8_t(u >> 16), uint8_t(u >> 24)};
    uint8_t o[4];
    sq::alsa::encodeSample(Fmt::S32, sq::alsa::decodeSample(Fmt::S32, b), o);
    REQUIRE(std::memcmp(b, o, 4) == 0);
  }
}

SQ_TEST("alsa convert: interleave/deinterleave honour the channel map, unmapped channels are silent") {
  const uint32_t frames = 5, dev = 4;
  float l[frames], r[frames];
  for (uint32_t i = 0; i < frames; ++i) {
    l[i] = 0.25f * float(i) / 4.0f;
    r[i] = -0.5f;
  }
  const float* src[2] = {l, r};
  const uint32_t map[2] = {3, 1};  // engine L -> device ch 3, engine R -> device ch 1
  for (Fmt f : {Fmt::F32, Fmt::S16, Fmt::S24_3, Fmt::S24_32, Fmt::S32}) {
    const uint32_t bps = sq::alsa::bytesPerSample(f);
    std::vector<uint8_t> buf(size_t(frames) * dev * bps, 0x55);
    sq::alsa::interleave(f, src, map, 2, buf.data(), dev, frames);
    for (uint32_t i = 0; i < frames; ++i) {
      for (uint32_t c : {0u, 2u})
        for (uint32_t k = 0; k < bps; ++k) CHECK_EQ(buf[(size_t(i) * dev + c) * bps + k], 0);
      CHECK_EQ(sq::alsa::decodeSample(f, &buf[(size_t(i) * dev + 3) * bps]), l[i]);
      CHECK_EQ(sq::alsa::decodeSample(f, &buf[(size_t(i) * dev + 1) * bps]), -0.5f);
    }
    float a[frames], b[frames];
    float* dst[2] = {a, b};
    const uint32_t back[2] = {1, 3};
    sq::alsa::deinterleave(f, buf.data(), dev, dst, back, 2, frames);
    for (uint32_t i = 0; i < frames; ++i) {
      CHECK_EQ(a[i], -0.5f);
      CHECK_EQ(b[i], l[i]);
    }
  }
}
