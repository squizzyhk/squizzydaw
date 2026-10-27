// engine/simd: every compiled + supported kernel set must match the generic set, and selection
// must only ever pick sets the CPU supports.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "sq/engine/simd/simd.hpp"
#include "sqtest.hpp"

namespace {

using sq::simd::genericOps;
using sq::simd::opsFor;

std::vector<const sq_vector_ops*> simdSets() {
  std::vector<const sq_vector_ops*> v;
  for (int32_t isa : {SQ_ISA_SSE42, SQ_ISA_AVX2, SQ_ISA_AVX512})
    if (const sq_vector_ops* t = opsFor(isa)) v.push_back(t);
  return v;
}

std::vector<float> randomFloats(size_t n, uint32_t seed, float range = 1.5f) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(-range, range);
  std::vector<float> v(n);
  for (auto& x : v) x = d(rng);
  return v;
}

bool closeRel(float a, float b, float rel, float absTol) {
  float diff = std::fabs(a - b);
  return diff <= absTol || diff <= rel * std::max(std::fabs(a), std::fabs(b));
}

constexpr uint32_t kLens[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 63, 64, 65, 100, 255, 1027};
constexpr uint32_t kOffsets[] = {0, 1, 3};
constexpr uint32_t kPad = 64;

int gFailures = 0;
void noteFail(const sq_vector_ops* t, const char* op, uint32_t n, uint32_t off) {
  if (gFailures++ < 20) std::printf("  mismatch: isa=%s op=%s n=%u off=%u\n", t->isa_name, op, n, off);
}

// Runs a float-in/float-out in-place op on both tables and compares.
template <class Fn>
void compareInPlace(const sq_vector_ops* t, const char* op, float rel, float absTol, Fn fn) {
  for (uint32_t n : kLens)
    for (uint32_t off : kOffsets) {
      auto a = randomFloats(n + kPad, 11u + n + off);
      auto b = randomFloats(n + kPad, 97u + n * 3u + off);
      auto ref = a, got = a;
      fn(genericOps(), ref.data() + off, b.data() + off, n);
      fn(t, got.data() + off, b.data() + off, n);
      for (size_t i = 0; i < ref.size(); ++i)
        if (!closeRel(ref[i], got[i], rel, absTol)) {
          noteFail(t, op, n, off);
          break;
        }
    }
}

}  // namespace

SQ_TEST("simd: generic is always supported and compiled") {
  CHECK((sq::simd::supportedIsaMask() & 1u) == 1u);
  CHECK((sq::simd::compiledIsaMask() & 1u) == 1u);
  REQUIRE(genericOps() != nullptr);
  CHECK_EQ(genericOps()->isa, SQ_ISA_GENERIC);
  CHECK_EQ(genericOps()->struct_size, static_cast<uint32_t>(sizeof(sq_vector_ops)));
  std::printf("  supported mask 0x%x, compiled mask 0x%x, selected %s\n", sq::simd::supportedIsaMask(),
              sq::simd::compiledIsaMask(), sq_dsp_vector_ops()->isa_name);
}

SQ_TEST("simd: every table is complete") {
  std::vector<const sq_vector_ops*> all = simdSets();
  all.push_back(genericOps());
  for (const sq_vector_ops* t : all) {
    using Fp = void (*)();
    const Fp* fns = reinterpret_cast<const Fp*>(&t->clear);
    const size_t count = (sizeof(sq_vector_ops) - offsetof(sq_vector_ops, clear)) / sizeof(Fp);
    CHECK_EQ(count, size_t(21));
    for (size_t i = 0; i < count; ++i) CHECK(fns[i] != nullptr);
    CHECK(t->isa_name && *t->isa_name);
  }
}

SQ_TEST("simd: selection only picks supported sets") {
  const uint32_t sup = sq::simd::supportedIsaMask();
  const sq_vector_ops* sel = sq_dsp_vector_ops();
  REQUIRE(sel);
  CHECK(sup & (1u << sel->isa));
  CHECK(sel == sq_dsp_vector_ops());  // stable
  // The automatic choice is the best supported+compiled one (unless SQ_DSP_ISA caps it).
  const sq_vector_ops* best = sq::simd::selectOps(sup, nullptr);
  CHECK(sup & (1u << best->isa));
  for (int32_t isa = SQ_ISA_AVX512; isa > best->isa; --isa) CHECK(opsFor(isa) == nullptr);
  // Simulated CPUs.
  CHECK_EQ(sq::simd::selectOps(1u << SQ_ISA_GENERIC, nullptr)->isa, SQ_ISA_GENERIC);
  CHECK_EQ(sq::simd::selectOps(1u << SQ_ISA_GENERIC, "avx512")->isa, SQ_ISA_GENERIC);
  const uint32_t sseOnly = (1u << SQ_ISA_GENERIC) | (1u << SQ_ISA_SSE42);
  const int32_t expectSse = (sq::simd::compiledIsaMask() & (1u << SQ_ISA_SSE42)) ? SQ_ISA_SSE42 : SQ_ISA_GENERIC;
  CHECK_EQ(sq::simd::selectOps(sseOnly, nullptr)->isa, expectSse);
  CHECK_EQ(sq::simd::selectOps(sseOnly, "AVX2")->isa, expectSse);
  CHECK_EQ(sq::simd::selectOps(0xFu, "generic")->isa, SQ_ISA_GENERIC);
  CHECK_EQ(sq::simd::selectOps(sup, "bogus")->isa, best->isa);
  // A cap selects the highest supported set at or below it.
  for (int32_t cap = SQ_ISA_GENERIC; cap <= SQ_ISA_AVX512; ++cap) {
    const char* names[] = {"generic", "sse42", "avx2", "avx512"};
    const sq_vector_ops* t = sq::simd::selectOps(sup, names[cap]);
    CHECK(t->isa <= cap);
    CHECK(sup & (1u << t->isa));
  }
  CHECK_EQ(sq::simd::parseIsaName("SSE4.2"), SQ_ISA_SSE42);
  CHECK_EQ(sq::simd::parseIsaName(nullptr), -1);
}

SQ_TEST("simd: generic reference values") {
  const sq_vector_ops* g = genericOps();
  int16_t i16[4] = {-32768, 32767, 0, 16384};
  float f[4];
  g->from_i16(f, i16, 4);
  CHECK_EQ(f[0], -1.0f);
  CHECK_EQ(f[3], 0.5f);
  float in[6] = {1.0f, -1.0f, 2.0f, -2.0f, 0.5f, 0.0f};
  int16_t o16[6];
  g->to_i16(o16, in, 6);
  CHECK_EQ(o16[0], int16_t(32767));
  CHECK_EQ(o16[1], int16_t(-32768));
  CHECK_EQ(o16[2], int16_t(32767));
  CHECK_EQ(o16[3], int16_t(-32768));
  CHECK_EQ(o16[4], int16_t(16384));
  int32_t o32[6];
  g->to_i32(o32, in, 6);
  CHECK_EQ(o32[0], 2147483520);
  CHECK_EQ(o32[1], int32_t(-2147483647 - 1));
  uint8_t b24[18];
  g->to_i24(b24, in, 6);
  float back[6];
  g->from_i24(back, b24, 6);
  CHECK(std::fabs(back[0] - 1.0f) < 1e-6f);
  CHECK_EQ(back[1], -1.0f);
  CHECK_EQ(back[4], 0.5f);
  CHECK_EQ(g->peak(in, 6), 2.0f);
  CHECK_EQ(g->sum_squares(in, 6), 10.25f);
  float d[4] = {1, 1, 1, 1}, s[4] = {1, 1, 1, 1};
  g->mix_ramp(d, s, 0.0f, 1.0f, 4);
  CHECK_EQ(d[0], 1.0f);
  CHECK_EQ(d[2], 1.5f);
}

SQ_TEST("simd: arithmetic kernels match generic") {
  for (const sq_vector_ops* t : simdSets()) {
    compareInPlace(t, "clear", 0, 0, [](const sq_vector_ops* o, float* d, const float*, uint32_t n) { o->clear(d, n); });
    compareInPlace(t, "copy", 0, 0, [](const sq_vector_ops* o, float* d, const float* s, uint32_t n) { o->copy(d, s, n); });
    compareInPlace(t, "add", 0, 0, [](const sq_vector_ops* o, float* d, const float* s, uint32_t n) { o->add(d, s, n); });
    compareInPlace(t, "mul", 0, 0, [](const sq_vector_ops* o, float* d, const float* s, uint32_t n) { o->mul(d, s, n); });
    compareInPlace(t, "scale", 0, 0, [](const sq_vector_ops* o, float* d, const float*, uint32_t n) { o->scale(d, 0.7071f, n); });
    compareInPlace(t, "mix", 1e-6f, 1e-6f,
                   [](const sq_vector_ops* o, float* d, const float* s, uint32_t n) { o->mix(d, s, -0.33f, n); });
    compareInPlace(t, "mix_ramp", 1e-5f, 1e-5f,
                   [](const sq_vector_ops* o, float* d, const float* s, uint32_t n) { o->mix_ramp(d, s, 0.1f, 1.3f, n); });
    compareInPlace(t, "clamp", 0, 0,
                   [](const sq_vector_ops* o, float* d, const float*, uint32_t n) { o->clamp(d, -0.8f, 0.9f, n); });
    // in-place copy is a no-op
    compareInPlace(t, "copy-inplace", 0, 0, [](const sq_vector_ops* o, float* d, const float*, uint32_t n) { o->copy(d, d, n); });
  }
  CHECK_EQ(gFailures, 0);
}

SQ_TEST("simd: reductions match generic") {
  int fails = 0;
  for (const sq_vector_ops* t : simdSets())
    for (uint32_t n : kLens)
      for (uint32_t off : kOffsets) {
        auto a = randomFloats(n + kPad, 5u + n + off);
        auto b = randomFloats(n + kPad, 6u + n + off);
        const float* pa = a.data() + off;
        const float* pb = b.data() + off;
        if (t->peak(pa, n) != genericOps()->peak(pa, n)) ++fails, noteFail(t, "peak", n, off);
        if (!closeRel(t->sum_squares(pa, n), genericOps()->sum_squares(pa, n), 1e-5f, 1e-5f))
          ++fails, noteFail(t, "sum_squares", n, off);
        if (!closeRel(t->dot(pa, pb, n), genericOps()->dot(pa, pb, n), 1e-4f, 1e-4f)) ++fails, noteFail(t, "dot", n, off);
      }
  CHECK_EQ(fails, 0);
}

SQ_TEST("simd: (de)interleave matches generic for 1..6 channels") {
  int fails = 0;
  for (const sq_vector_ops* t : simdSets())
    for (uint32_t ch = 1; ch <= 6; ++ch)
      for (uint32_t frames : kLens) {
        std::vector<std::vector<float>> planes(ch);
        std::vector<const float*> src(ch);
        for (uint32_t c = 0; c < ch; ++c) {
          planes[c] = randomFloats(frames + 1, 100u + c * 7u + frames);
          src[c] = planes[c].data();
        }
        std::vector<float> ref(ch * frames + 1, -9.0f), got(ch * frames + 1, -9.0f);
        genericOps()->interleave(ref.data(), src.data(), ch, frames);
        t->interleave(got.data(), src.data(), ch, frames);
        if (ref != got) ++fails, noteFail(t, "interleave", frames, ch);
        std::vector<std::vector<float>> outR(ch, std::vector<float>(frames + 1, -7.0f)), outG = outR;
        std::vector<float*> pr(ch), pg(ch);
        for (uint32_t c = 0; c < ch; ++c) pr[c] = outR[c].data(), pg[c] = outG[c].data();
        genericOps()->deinterleave(pr.data(), ref.data(), ch, frames);
        t->deinterleave(pg.data(), ref.data(), ch, frames);
        if (outR != outG) ++fails, noteFail(t, "deinterleave", frames, ch);
        for (uint32_t c = 0; c < ch; ++c)
          if (!std::equal(outR[c].begin(), outR[c].begin() + frames, planes[c].begin()))
            ++fails, noteFail(genericOps(), "roundtrip", frames, ch);
      }
  CHECK_EQ(fails, 0);
}

SQ_TEST("simd: integer and f64 conversions match generic") {
  int fails = 0;
  std::mt19937 rng(1234);
  for (const sq_vector_ops* t : simdSets())
    for (uint32_t n : kLens)
      for (uint32_t off : kOffsets) {
        const uint32_t total = n + kPad;
        std::vector<int16_t> i16(total);
        std::vector<int32_t> i32(total);
        std::vector<uint8_t> i24(3 * total);
        std::vector<double> f64(total);
        for (auto& v : i16) v = static_cast<int16_t>(rng());
        for (auto& v : i32) v = static_cast<int32_t>(rng());
        for (auto& v : i24) v = static_cast<uint8_t>(rng());
        for (auto& v : f64) v = static_cast<double>(static_cast<int32_t>(rng())) / 1.0e9;
        auto fl = randomFloats(total, 77u + n + off, 1.2f);  // includes out-of-range values
        fl[off] = 1.0f;
        if (n > 1) fl[off + 1] = -1.0f;
        std::vector<float> r(total, 0.f), g(total, 0.f);
        genericOps()->from_i16(r.data(), i16.data() + off, n);
        t->from_i16(g.data(), i16.data() + off, n);
        if (r != g) ++fails, noteFail(t, "from_i16", n, off);
        genericOps()->from_i24(r.data(), i24.data() + 3 * off, n);
        t->from_i24(g.data(), i24.data() + 3 * off, n);
        if (r != g) ++fails, noteFail(t, "from_i24", n, off);
        genericOps()->from_i32(r.data(), i32.data() + off, n);
        t->from_i32(g.data(), i32.data() + off, n);
        if (r != g) ++fails, noteFail(t, "from_i32", n, off);
        genericOps()->from_f64(r.data(), f64.data() + off, n);
        t->from_f64(g.data(), f64.data() + off, n);
        if (r != g) ++fails, noteFail(t, "from_f64", n, off);

        std::vector<int16_t> o16r(total, 5), o16g(total, 5);
        genericOps()->to_i16(o16r.data(), fl.data() + off, n);
        t->to_i16(o16g.data(), fl.data() + off, n);
        if (o16r != o16g) ++fails, noteFail(t, "to_i16", n, off);
        std::vector<uint8_t> o24r(3 * total, 5), o24g(3 * total, 5);
        genericOps()->to_i24(o24r.data(), fl.data() + off, n);
        t->to_i24(o24g.data(), fl.data() + off, n);
        if (o24r != o24g) ++fails, noteFail(t, "to_i24", n, off);
        std::vector<int32_t> o32r(total, 5), o32g(total, 5);
        genericOps()->to_i32(o32r.data(), fl.data() + off, n);
        t->to_i32(o32g.data(), fl.data() + off, n);
        if (o32r != o32g) ++fails, noteFail(t, "to_i32", n, off);
        std::vector<double> d64r(total, 5.0), d64g(total, 5.0);
        genericOps()->to_f64(d64r.data(), fl.data() + off, n);
        t->to_f64(d64g.data(), fl.data() + off, n);
        if (d64r != d64g) ++fails, noteFail(t, "to_f64", n, off);
      }
  CHECK_EQ(fails, 0);
}
