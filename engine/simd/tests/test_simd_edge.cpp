// engine/simd: adversarial inputs (NaN, +-inf, -0, huge values). Every kernel set must agree
// with the generic set bit-for-bit on these, and integer conversion must never turn a NaN into a
// full-scale sample.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "sq/engine/simd/simd.hpp"
#include "sqtest.hpp"

namespace {

std::vector<const sq_vector_ops*> allSets() {
  std::vector<const sq_vector_ops*> v{sq::simd::genericOps()};
  for (int32_t isa : {SQ_ISA_SSE42, SQ_ISA_AVX2, SQ_ISA_AVX512})
    if (const sq_vector_ops* t = sq::simd::opsFor(isa)) v.push_back(t);
  return v;
}

// 37 values (not a multiple of any vector width) so both vector bodies and scalar tails see them.
std::vector<float> nasty() {
  const float qnan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<float> v;
  for (int rep = 0; rep < 3; ++rep) {
    v.insert(v.end(), {qnan, inf, -inf, -0.0f, 0.0f, 1e30f, -1e30f, 0.5f, -0.25f, 1.0f, -1.0f, 1.0000001f});
  }
  v.push_back(qnan);
  return v;
}

bool sameBits(float a, float b) {
  uint32_t x, y;
  std::memcpy(&x, &a, 4);
  std::memcpy(&y, &b, 4);
  return x == y || (std::isnan(a) && std::isnan(b));
}

}  // namespace

SQ_TEST("simd edge: NaN never converts to a non-zero integer sample") {
  const std::vector<float> in = nasty();
  const uint32_t n = static_cast<uint32_t>(in.size());
  for (const sq_vector_ops* t : allSets()) {
    std::vector<int16_t> o16(n, 7);
    std::vector<int32_t> o32(n, 7);
    std::vector<uint8_t> o24(3 * n, 7);
    t->to_i16(o16.data(), in.data(), n);
    t->to_i32(o32.data(), in.data(), n);
    t->to_i24(o24.data(), in.data(), n);
    int bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (!std::isnan(in[i])) continue;
      const int32_t v24 = static_cast<int32_t>(static_cast<uint32_t>(o24[3 * i]) << 8 |
                                               static_cast<uint32_t>(o24[3 * i + 1]) << 16 |
                                               static_cast<uint32_t>(o24[3 * i + 2]) << 24) >> 8;
      if (o16[i] != 0 || o32[i] != 0 || v24 != 0) {
        if (bad++ < 3) std::printf("  isa %s: NaN at %u -> i16 %d i24 %d i32 %d\n", t->isa_name, i, o16[i], v24, o32[i]);
      }
    }
    CHECK_EQ(bad, 0);
    // +-inf and huge values clip to full scale
    CHECK_EQ(o16[1], int16_t(32767));
    CHECK_EQ(o16[2], int16_t(-32768));
    CHECK_EQ(o32[5], 2147483520);
    CHECK_EQ(o32[6], int32_t(-2147483647 - 1));
  }
}

SQ_TEST("simd edge: every set equals generic on NaN/inf/-0 inputs") {
  const std::vector<float> in = nasty();
  const uint32_t n = static_cast<uint32_t>(in.size());
  const sq_vector_ops* g = sq::simd::genericOps();
  std::vector<int16_t> r16(n);
  std::vector<int32_t> r32(n);
  std::vector<uint8_t> r24(3 * n);
  std::vector<float> rc = in;
  g->to_i16(r16.data(), in.data(), n);
  g->to_i32(r32.data(), in.data(), n);
  g->to_i24(r24.data(), in.data(), n);
  g->clamp(rc.data(), -1.0f, 1.0f, n);
  const float rpk = g->peak(in.data() + 3, 8);  // finite + inf part only
  for (const sq_vector_ops* t : allSets()) {
    std::vector<int16_t> o16(n);
    std::vector<int32_t> o32(n);
    std::vector<uint8_t> o24(3 * n);
    std::vector<float> oc = in;
    t->to_i16(o16.data(), in.data(), n);
    t->to_i32(o32.data(), in.data(), n);
    t->to_i24(o24.data(), in.data(), n);
    t->clamp(oc.data(), -1.0f, 1.0f, n);
    int mism = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (o16[i] != r16[i] || o32[i] != r32[i]) ++mism;
      if (!sameBits(oc[i], rc[i])) {
        if (mism < 3) std::printf("  isa %s clamp[%u]: in %g -> %g, generic %g\n", t->isa_name, i, in[i], oc[i], rc[i]);
        ++mism;
      }
    }
    if (o24 != r24) ++mism;
    if (mism) std::printf("  isa %s: %d mismatches vs generic\n", t->isa_name, mism);
    CHECK_EQ(mism, 0);
    CHECK_EQ(t->peak(in.data() + 3, 8), rpk);
    // NaN samples are ignored by the peak meter in every set (generic semantics)
    std::vector<float> withNan(40, 0.25f);
    for (size_t i = 0; i < withNan.size(); i += 5) withNan[i] = std::numeric_limits<float>::quiet_NaN();
    withNan[17] = -0.75f;
    const float pk = t->peak(withNan.data(), static_cast<uint32_t>(withNan.size()));
    if (pk != 0.75f) std::printf("  isa %s: peak with NaNs = %g\n", t->isa_name, pk);
    CHECK_EQ(pk, 0.75f);
  }
}

SQ_TEST("simd edge: zero-length calls touch nothing") {
  for (const sq_vector_ops* t : allSets()) {
    float f = 3.0f;
    int16_t s = 3;
    uint8_t b[3] = {1, 2, 3};
    t->clear(&f, 0);
    t->scale(&f, 0.0f, 0);
    t->mix_ramp(&f, &f, 0.0f, 1.0f, 0);
    t->to_i16(&s, &f, 0);
    t->to_i24(b, &f, 0);
    CHECK_EQ(f, 3.0f);
    CHECK_EQ(s, int16_t(3));
    CHECK_EQ(b[2], uint8_t(3));
    CHECK_EQ(t->peak(&f, 0), 0.0f);
    CHECK_EQ(t->sum_squares(&f, 0), 0.0f);
    CHECK_EQ(t->dot(&f, &f, 0), 0.0f);
  }
}
