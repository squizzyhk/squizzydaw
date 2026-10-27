// Generic (portable C++) kernel set: the reference every SIMD set is tested against.
#include <cmath>
#include <cstring>

#include "ops_internal.hpp"

namespace sq::simd::detail {
namespace {

void gClear(float* d, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] = 0.0f;
}
void gCopy(float* d, const float* s, uint32_t n) {
  if (d != s && n) std::memmove(d, s, sizeof(float) * n);
}
void gAdd(float* d, const float* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] += s[i];
}
void gMul(float* d, const float* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] *= s[i];
}
void gScale(float* d, float g, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] *= g;
}
void gMix(float* d, const float* s, float g, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] += s[i] * g;
}
void gMixRamp(float* d, const float* s, float g0, float g1, uint32_t n) {
  if (!n) return;
  const float step = (g1 - g0) / static_cast<float>(n);
  for (uint32_t i = 0; i < n; ++i) d[i] += s[i] * (g0 + step * static_cast<float>(i));
}
float gPeak(const float* s, uint32_t n) {
  float m = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    float a = std::fabs(s[i]);
    if (a > m) m = a;
  }
  return m;
}
float gSumSquares(const float* s, uint32_t n) {
  double acc = 0.0;
  for (uint32_t i = 0; i < n; ++i) acc += static_cast<double>(s[i]) * s[i];
  return static_cast<float>(acc);
}
float gDot(const float* a, const float* b, uint32_t n) {
  double acc = 0.0;
  for (uint32_t i = 0; i < n; ++i) acc += static_cast<double>(a[i]) * b[i];
  return static_cast<float>(acc);
}
void gClamp(float* d, float lo, float hi, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    float v = d[i];
    v = v < lo ? lo : v;
    d[i] = v > hi ? hi : v;
  }
}
void gInterleave(float* d, const float* const* s, uint32_t ch, uint32_t frames) {
  for (uint32_t f = 0; f < frames; ++f)
    for (uint32_t c = 0; c < ch; ++c) d[f * ch + c] = s[c][f];
}
void gDeinterleave(float* const* d, const float* s, uint32_t ch, uint32_t frames) {
  for (uint32_t f = 0; f < frames; ++f)
    for (uint32_t c = 0; c < ch; ++c) d[c][f] = s[f * ch + c];
}
void gFromI16(float* d, const int16_t* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] = static_cast<float>(s[i]) * (1.0f / kI16Scale);
}
void gFromI24(float* d, const uint8_t* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* p = s + 3 * i;
    int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                                     static_cast<uint32_t>(p[2]) << 24) >> 8;
    d[i] = static_cast<float>(v) * (1.0f / kI24Scale);
  }
}
void gFromI32(float* d, const int32_t* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] = static_cast<float>(s[i]) * (1.0f / kI32Scale);
}
inline int32_t roundClamp(float x, float scale, float maxv) {
  if (x != x) return 0;  // NaN -> silence (lrintf(NaN) is undefined)
  float v = x * scale;
  v = v < -scale ? -scale : v;
  v = v > maxv ? maxv : v;
  return static_cast<int32_t>(std::lrintf(v));
}
void gToI16(int16_t* d, const float* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] = static_cast<int16_t>(roundClamp(s[i], kI16Scale, kI16Max));
}
void gToI24(uint8_t* d, const float* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v = static_cast<uint32_t>(roundClamp(s[i], kI24Scale, kI24Max));
    d[3 * i] = static_cast<uint8_t>(v);
    d[3 * i + 1] = static_cast<uint8_t>(v >> 8);
    d[3 * i + 2] = static_cast<uint8_t>(v >> 16);
  }
}
void gToI32(int32_t* d, const float* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] = roundClamp(s[i], kI32Scale, kI32Max);
}
void gFromF64(float* d, const double* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] = static_cast<float>(s[i]);
}
void gToF64(double* d, const float* s, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) d[i] = s[i];
}

constexpr sq_vector_ops kGeneric = {
    sizeof(sq_vector_ops), SQ_ISA_GENERIC, "generic",
    &gClear, &gCopy, &gAdd, &gMul, &gScale, &gMix, &gMixRamp, &gPeak, &gSumSquares, &gDot, &gClamp,
    &gInterleave, &gDeinterleave,
    &gFromI16, &gFromI24, &gFromI32, &gToI16, &gToI24, &gToI32, &gFromF64, &gToF64,
};

}  // namespace

const sq_vector_ops* opsGeneric() noexcept { return &kGeneric; }

}  // namespace sq::simd::detail
