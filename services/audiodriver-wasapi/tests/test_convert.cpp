// Sample format conversion (sq/audiodriver/sample_convert.hpp), shared by the WASAPI and ASIO drivers.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "sq/audiodriver/sample_convert.hpp"
#include "sqtest.hpp"

using sq::audiodrv::bytesPerSample;
using sq::audiodrv::fromFloat;
using sq::audiodrv::SampleKind;
using sq::audiodrv::SampleLayout;
using sq::audiodrv::significantBits;
using sq::audiodrv::toFloat;

namespace {

constexpr SampleKind kIntKinds[] = {SampleKind::I16,     SampleKind::I24,     SampleKind::I32,    SampleKind::I32Lo16,
                                    SampleKind::I32Lo18, SampleKind::I32Lo20, SampleKind::I32Lo24};

void storeInt(uint8_t* p, int64_t v, uint32_t nb, bool be) {
  uint64_t u = static_cast<uint64_t>(v);
  for (uint32_t i = 0; i < nb; ++i) {
    uint8_t b = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
    p[be ? nb - 1 - i : i] = b;
  }
}

int64_t loadInt(const uint8_t* p, uint32_t nb, bool be, uint32_t bits) {
  uint64_t u = 0;
  for (uint32_t i = 0; i < nb; ++i) u |= uint64_t(p[be ? nb - 1 - i : i]) << (8 * i);
  const uint64_t m = uint64_t(1) << (bits - 1);
  u &= (bits == 64) ? ~0ull : ((uint64_t(1) << bits) - 1);
  return static_cast<int64_t>((u ^ m) - m);
}

}  // namespace

SQ_TEST("convert: bytes per sample and significant bits") {
  CHECK_EQ(bytesPerSample(SampleKind::I16), 2u);
  CHECK_EQ(bytesPerSample(SampleKind::I24), 3u);
  CHECK_EQ(bytesPerSample(SampleKind::I32Lo20), 4u);
  CHECK_EQ(bytesPerSample(SampleKind::F64), 8u);
  CHECK_EQ(bytesPerSample(SampleKind::Unknown), 0u);
  CHECK_EQ(significantBits(SampleKind::I32Lo18), 18u);
  CHECK_EQ(significantBits(SampleKind::I32Hi24), 24u);
  CHECK_EQ(significantBits(SampleKind::F32), 0u);
}

SQ_TEST("convert: integer round trips are exact (both byte orders)") {
  for (bool be : {false, true}) {
    for (SampleKind k : kIntKinds) {
      const SampleLayout L{k, be};
      const uint32_t nb = bytesPerSample(k);
      const uint32_t bits = significantBits(k);
      const int64_t maxv = (int64_t(1) << (bits - 1)) - 1;
      const int64_t minv = -(int64_t(1) << (bits - 1));
      std::vector<int64_t> codes = {0, 1, -1, maxv, minv, maxv / 3, minv / 7, 12345 % (maxv + 1)};
      std::vector<uint8_t> src(codes.size() * nb), back(codes.size() * nb);
      for (size_t i = 0; i < codes.size(); ++i) storeInt(&src[i * nb], codes[i], nb, be);
      std::vector<float> f(codes.size());
      toFloat(L, src.data(), nb, f.data(), static_cast<uint32_t>(codes.size()));
      // I32 full range does not fit float32's 24-bit mantissa: compare codes within tolerance.
      fromFloat(L, f.data(), back.data(), nb, static_cast<uint32_t>(codes.size()));
      for (size_t i = 0; i < codes.size(); ++i) {
        const int64_t got = loadInt(&back[i * nb], nb, be, bits);
        const int64_t tol = bits > 24 ? (int64_t(1) << (bits - 24)) : 0;
        if (std::llabs(got - codes[i]) > tol) {
          std::printf("    kind %s be=%d code %lld -> %lld\n", sq::audiodrv::kindName(k), be ? 1 : 0,
                      static_cast<long long>(codes[i]), static_cast<long long>(got));
          CHECK(false);
        }
        // float32 cannot represent (2^31-1)/2^31: the largest 32-bit code rounds to exactly 1.0f.
        CHECK(f[i] >= -1.0f && (f[i] < 1.0f || (bits > 24 && f[i] == 1.0f)));
      }
    }
  }
}

SQ_TEST("convert: i16 known values, clipping and NaN") {
  const SampleLayout L{SampleKind::I16, false};
  const float in[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -3.0f, std::numeric_limits<float>::quiet_NaN()};
  int16_t out[8] = {};
  fromFloat(L, in, out, 2, 8);
  CHECK_EQ(out[0], int16_t(0));
  CHECK_EQ(out[1], int16_t(16384));
  CHECK_EQ(out[2], int16_t(-16384));
  CHECK_EQ(out[3], int16_t(32767));   // +1.0 clips to the top code
  CHECK_EQ(out[4], int16_t(-32768));
  CHECK_EQ(out[5], int16_t(32767));
  CHECK_EQ(out[6], int16_t(-32768));
  CHECK_EQ(out[7], int16_t(0));
  float back[8] = {};
  toFloat(L, out, 2, back, 8);
  CHECK_EQ(back[1], 0.5f);
  CHECK_EQ(back[4], -1.0f);
  CHECK_EQ(back[3], 32767.0f / 32768.0f);
}

SQ_TEST("convert: i24 packed byte order") {
  const float v = static_cast<float>(0x123456) / 8388608.0f;
  uint8_t le[3] = {}, be[3] = {};
  fromFloat(SampleLayout{SampleKind::I24, false}, &v, le, 3, 1);
  fromFloat(SampleLayout{SampleKind::I24, true}, &v, be, 3, 1);
  CHECK_EQ(int(le[0]), 0x56);
  CHECK_EQ(int(le[1]), 0x34);
  CHECK_EQ(int(le[2]), 0x12);
  CHECK_EQ(int(be[0]), 0x12);
  CHECK_EQ(int(be[1]), 0x34);
  CHECK_EQ(int(be[2]), 0x56);
  const uint8_t neg[3] = {0x00, 0x00, 0x80};  // most negative, little endian
  float f = 0;
  toFloat(SampleLayout{SampleKind::I24, false}, neg, 3, &f, 1);
  CHECK_EQ(f, -1.0f);
}

SQ_TEST("convert: right-aligned 32-bit containers are sign extended") {
  const float v = -1.0f;
  int32_t out = 0;
  fromFloat(SampleLayout{SampleKind::I32Lo20, false}, &v, &out, 4, 1);
  CHECK_EQ(out, -(1 << 19));
  const float p = 0.5f;
  fromFloat(SampleLayout{SampleKind::I32Lo16, false}, &p, &out, 4, 1);
  CHECK_EQ(out, 16384);
  // Garbage in the unused high bits is ignored on read.
  const uint32_t raw = 0xABC00001u;  // low 18 bits = 1
  float f = 0;
  toFloat(SampleLayout{SampleKind::I32Lo18, false}, &raw, 4, &f, 1);
  CHECK_EQ(f, 1.0f / 131072.0f);
}

SQ_TEST("convert: 24-in-32 left aligned keeps the low byte zero") {
  const float in[] = {0.5f, -1.0f, 0.123456f};
  uint32_t out[3] = {};
  fromFloat(SampleLayout{SampleKind::I32Hi24, false}, in, out, 4, 3);
  CHECK_EQ(out[0], 0x40000000u);
  CHECK_EQ(out[1], 0x80000000u);
  CHECK_EQ(out[2] & 0xFFu, 0u);
  float back[3] = {};
  toFloat(SampleLayout{SampleKind::I32Hi24, false}, out, 4, back, 3);
  CHECK_EQ(back[0], 0.5f);
  CHECK_EQ(back[1], -1.0f);
  CHECK(std::fabs(back[2] - 0.123456f) < 1.0f / 8388608.0f);
}

SQ_TEST("convert: float32/float64 in both byte orders") {
  const float in[] = {0.25f, -0.75f, 1.5f};  // floats are not clipped
  uint8_t be[12] = {}, le[12] = {};
  fromFloat(SampleLayout{SampleKind::F32, true}, in, be, 4, 3);
  fromFloat(SampleLayout{SampleKind::F32, false}, in, le, 4, 3);
  for (int i = 0; i < 3; ++i)
    for (int b = 0; b < 4; ++b) CHECK_EQ(int(be[i * 4 + b]), int(le[i * 4 + 3 - b]));
  float back[3] = {};
  toFloat(SampleLayout{SampleKind::F32, true}, be, 4, back, 3);
  CHECK_EQ(back[0], 0.25f);
  CHECK_EQ(back[2], 1.5f);
  uint8_t d[24] = {};
  fromFloat(SampleLayout{SampleKind::F64, true}, in, d, 8, 3);
  CHECK_EQ(int(d[0]), 0x3F);  // 0.25 = 0x3FD0000000000000
  CHECK_EQ(int(d[1]), 0xD0);
  toFloat(SampleLayout{SampleKind::F64, true}, d, 8, back, 3);
  CHECK_EQ(back[1], -0.75f);
}

SQ_TEST("convert: interleaved stride touches only its channel") {
  // 3 interleaved channels of i16; write channel 1 only.
  int16_t frame[3 * 4];
  for (auto& s : frame) s = 0x1111;
  const float ch[4] = {0.5f, -0.5f, 0.25f, 0.0f};
  fromFloat(SampleLayout{SampleKind::I16, false}, ch, &frame[1], 3 * 2, 4);
  for (int i = 0; i < 4; ++i) {
    CHECK_EQ(frame[i * 3 + 0], int16_t(0x1111));
    CHECK_EQ(frame[i * 3 + 2], int16_t(0x1111));
  }
  CHECK_EQ(frame[1], int16_t(16384));
  CHECK_EQ(frame[4], int16_t(-16384));
  float back[4] = {};
  toFloat(SampleLayout{SampleKind::I16, false}, &frame[1], 3 * 2, back, 4);
  CHECK_EQ(back[2], 0.25f);
  sq::audiodrv::writeSilence(SampleLayout{SampleKind::I16, false}, &frame[1], 3 * 2, 4);
  CHECK_EQ(frame[1], int16_t(0));
  CHECK_EQ(frame[10], int16_t(0));
  CHECK_EQ(frame[9], int16_t(0x1111));
}
