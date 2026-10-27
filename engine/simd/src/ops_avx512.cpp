// AVX-512F kernel set (16 lanes). Only AVX512F instructions are used (no BW/DQ/VL), so the
// selection needs nothing beyond the AVX512F CPUID bit plus OS ZMM state support.
#include <immintrin.h>
#include <stdint.h>

#include "ops_internal.hpp"

namespace sq::simd::detail {
namespace {

struct V {
  static constexpr uint32_t W = 16;
  using F = __m512;
  using I = __m512i;
  static F load(const float* p) { return _mm512_loadu_ps(p); }
  static void store(float* p, F v) { _mm512_storeu_ps(p, v); }
  static F set1(float x) { return _mm512_set1_ps(x); }
  static F zero() { return _mm512_setzero_ps(); }
  static F add(F a, F b) { return _mm512_add_ps(a, b); }
  static F sub(F a, F b) { return _mm512_sub_ps(a, b); }
  static F mul(F a, F b) { return _mm512_mul_ps(a, b); }
  static F madd(F a, F b, F c) { return _mm512_fmadd_ps(a, b, c); }
  static F min(F a, F b) { return _mm512_min_ps(a, b); }
  static F max(F a, F b) { return _mm512_max_ps(a, b); }
  static F abs(F a) {
    return _mm512_castsi512_ps(_mm512_and_si512(_mm512_castps_si512(a), _mm512_set1_epi32(0x7fffffff)));
  }
  static float hsum(F v) {
    __m256 h = _mm256_add_ps(_mm512_castps512_ps256(v),
                             _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(v), 1)));
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(h), _mm256_extractf128_ps(h, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
  }
  static float hmax(F v) {
    __m256 h = _mm256_max_ps(_mm512_castps512_ps256(v),
                             _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(v), 1)));
    __m128 s = _mm_max_ps(_mm256_castps256_ps128(h), _mm256_extractf128_ps(h, 1));
    s = _mm_max_ps(s, _mm_movehl_ps(s, s));
    s = _mm_max_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
  }
  static I cvtRound(F v) { return _mm512_cvtps_epi32(v); }
  static F cvtI(I v) { return _mm512_cvtepi32_ps(v); }
  static I loadI32(const int32_t* p) { return _mm512_loadu_si512(p); }
  static void storeI32(int32_t* p, I v) { _mm512_storeu_si512(p, v); }
  static I loadI16(const int16_t* p) { return _mm512_cvtepi16_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p))); }
  static void storeI16(int16_t* p, I v) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), _mm512_cvtsepi32_epi16(v)); }
  static I loadI24(const uint8_t* p) {
    const __m128i m = _mm_setr_epi8(-1, 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11);
    __m128i q0 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), m);
    __m128i q1 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 12)), m);
    __m128i q2 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 24)), m);
    __m128i q3 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 36)), m);
    __m512i v = _mm512_castsi128_si512(q0);
    v = _mm512_inserti32x4(v, q1, 1);
    v = _mm512_inserti32x4(v, q2, 2);
    v = _mm512_inserti32x4(v, q3, 3);
    return _mm512_srai_epi32(v, 8);
  }
  static F loadF64(const double* p) {
    __m256 lo = _mm512_cvtpd_ps(_mm512_loadu_pd(p));
    __m256 hi = _mm512_cvtpd_ps(_mm512_loadu_pd(p + 8));
    return _mm512_castpd_ps(
        _mm512_insertf64x4(_mm512_castpd256_pd512(_mm256_castps_pd(lo)), _mm256_castps_pd(hi), 1));
  }
  static void storeF64(double* p, F v) {
    _mm512_storeu_pd(p, _mm512_cvtps_pd(_mm512_castps512_ps256(v)));
    _mm512_storeu_pd(p + 8, _mm512_cvtps_pd(_mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(v), 1))));
  }
  static void interleave2(float* d, F a, F b) {
    const __m512i i0 = _mm512_set_epi32(23, 7, 22, 6, 21, 5, 20, 4, 19, 3, 18, 2, 17, 1, 16, 0);
    const __m512i i1 = _mm512_set_epi32(31, 15, 30, 14, 29, 13, 28, 12, 27, 11, 26, 10, 25, 9, 24, 8);
    _mm512_storeu_ps(d, _mm512_permutex2var_ps(a, i0, b));
    _mm512_storeu_ps(d + 16, _mm512_permutex2var_ps(a, i1, b));
  }
  static void deinterleave2(const float* s, F& a, F& b) {
    const __m512i ie = _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
    const __m512i io = _mm512_set_epi32(31, 29, 27, 25, 23, 21, 19, 17, 15, 13, 11, 9, 7, 5, 3, 1);
    F x = _mm512_loadu_ps(s), y = _mm512_loadu_ps(s + 16);
    a = _mm512_permutex2var_ps(x, ie, y);
    b = _mm512_permutex2var_ps(x, io, y);
  }
  static F zeroNan(F v) { return _mm512_maskz_mov_ps(_mm512_cmp_ps_mask(v, v, _CMP_ORD_Q), v); }
  static F iota() {
    return _mm512_set_ps(15.0f, 14.0f, 13.0f, 12.0f, 11.0f, 10.0f, 9.0f, 8.0f, 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f,
                         1.0f, 0.0f);
  }
};

#include "kernels.inl"

constexpr sq_vector_ops kTable = SQ_SIMD_KERNEL_TABLE(SQ_ISA_AVX512, "avx512");

}  // namespace

const sq_vector_ops* opsAvx512() noexcept { return &kTable; }

}  // namespace sq::simd::detail
