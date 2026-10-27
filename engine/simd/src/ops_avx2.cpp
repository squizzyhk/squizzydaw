// AVX2 + FMA kernel set (8 lanes).
#include <immintrin.h>
#include <stdint.h>

#include "ops_internal.hpp"

namespace sq::simd::detail {
namespace {

struct V {
  static constexpr uint32_t W = 8;
  using F = __m256;
  using I = __m256i;
  static F load(const float* p) { return _mm256_loadu_ps(p); }
  static void store(float* p, F v) { _mm256_storeu_ps(p, v); }
  static F set1(float x) { return _mm256_set1_ps(x); }
  static F zero() { return _mm256_setzero_ps(); }
  static F add(F a, F b) { return _mm256_add_ps(a, b); }
  static F sub(F a, F b) { return _mm256_sub_ps(a, b); }
  static F mul(F a, F b) { return _mm256_mul_ps(a, b); }
  static F madd(F a, F b, F c) { return _mm256_fmadd_ps(a, b, c); }
  static F min(F a, F b) { return _mm256_min_ps(a, b); }
  static F max(F a, F b) { return _mm256_max_ps(a, b); }
  static F abs(F a) { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a); }
  static float hsum(F v) {
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
  }
  static float hmax(F v) {
    __m128 s = _mm_max_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    s = _mm_max_ps(s, _mm_movehl_ps(s, s));
    s = _mm_max_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
  }
  static I cvtRound(F v) { return _mm256_cvtps_epi32(v); }
  static F cvtI(I v) { return _mm256_cvtepi32_ps(v); }
  static I loadI32(const int32_t* p) { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }
  static void storeI32(int32_t* p, I v) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v); }
  static I loadI16(const int16_t* p) { return _mm256_cvtepi16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))); }
  static void storeI16(int16_t* p, I v) {
    __m128i lo = _mm256_castsi256_si128(v), hi = _mm256_extracti128_si256(v, 1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_packs_epi32(lo, hi));
  }
  static I loadI24(const uint8_t* p) {
    const __m128i m = _mm_setr_epi8(-1, 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11);
    __m128i lo = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), m);
    __m128i hi = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 12)), m);
    return _mm256_srai_epi32(_mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1), 8);
  }
  static F loadF64(const double* p) {
    __m128 lo = _mm256_cvtpd_ps(_mm256_loadu_pd(p));
    __m128 hi = _mm256_cvtpd_ps(_mm256_loadu_pd(p + 4));
    return _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
  }
  static void storeF64(double* p, F v) {
    _mm256_storeu_pd(p, _mm256_cvtps_pd(_mm256_castps256_ps128(v)));
    _mm256_storeu_pd(p + 4, _mm256_cvtps_pd(_mm256_extractf128_ps(v, 1)));
  }
  static void interleave2(float* d, F a, F b) {
    F lo = _mm256_unpacklo_ps(a, b), hi = _mm256_unpackhi_ps(a, b);
    _mm256_storeu_ps(d, _mm256_permute2f128_ps(lo, hi, 0x20));
    _mm256_storeu_ps(d + 8, _mm256_permute2f128_ps(lo, hi, 0x31));
  }
  static void deinterleave2(const float* s, F& a, F& b) {
    F x = _mm256_loadu_ps(s), y = _mm256_loadu_ps(s + 8);
    F t0 = _mm256_permute2f128_ps(x, y, 0x20), t1 = _mm256_permute2f128_ps(x, y, 0x31);
    a = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(2, 0, 2, 0));
    b = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 1, 3, 1));
  }
  static F zeroNan(F v) { return _mm256_and_ps(v, _mm256_cmp_ps(v, v, _CMP_ORD_Q)); }
  static F iota() { return _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f); }
};

#include "kernels.inl"

constexpr sq_vector_ops kTable = SQ_SIMD_KERNEL_TABLE(SQ_ISA_AVX2, "avx2");

}  // namespace

const sq_vector_ops* opsAvx2() noexcept { return &kTable; }

}  // namespace sq::simd::detail
