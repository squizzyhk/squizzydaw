// SSE4.2-class kernel set (uses SSE2/SSSE3/SSE4.1 instructions; selected when CPUID reports SSE4.2).
#include <immintrin.h>
#include <stdint.h>

#include "ops_internal.hpp"

namespace sq::simd::detail {
namespace {

struct V {
  static constexpr uint32_t W = 4;
  using F = __m128;
  using I = __m128i;
  static F load(const float* p) { return _mm_loadu_ps(p); }
  static void store(float* p, F v) { _mm_storeu_ps(p, v); }
  static F set1(float x) { return _mm_set1_ps(x); }
  static F zero() { return _mm_setzero_ps(); }
  static F add(F a, F b) { return _mm_add_ps(a, b); }
  static F sub(F a, F b) { return _mm_sub_ps(a, b); }
  static F mul(F a, F b) { return _mm_mul_ps(a, b); }
  static F madd(F a, F b, F c) { return _mm_add_ps(_mm_mul_ps(a, b), c); }
  static F min(F a, F b) { return _mm_min_ps(a, b); }
  static F max(F a, F b) { return _mm_max_ps(a, b); }
  static F abs(F a) { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
  static float hsum(F v) {
    F s = _mm_add_ps(v, _mm_movehl_ps(v, v));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
  }
  static float hmax(F v) {
    F s = _mm_max_ps(v, _mm_movehl_ps(v, v));
    s = _mm_max_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
  }
  static I cvtRound(F v) { return _mm_cvtps_epi32(v); }
  static F cvtI(I v) { return _mm_cvtepi32_ps(v); }
  static I loadI32(const int32_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
  static void storeI32(int32_t* p, I v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }
  static I loadI16(const int16_t* p) { return _mm_cvtepi16_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p))); }
  static void storeI16(int16_t* p, I v) { _mm_storel_epi64(reinterpret_cast<__m128i*>(p), _mm_packs_epi32(v, v)); }
  static I loadI24(const uint8_t* p) {
    const __m128i m = _mm_setr_epi8(-1, 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11);
    return _mm_srai_epi32(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), m), 8);
  }
  static F loadF64(const double* p) {
    return _mm_movelh_ps(_mm_cvtpd_ps(_mm_loadu_pd(p)), _mm_cvtpd_ps(_mm_loadu_pd(p + 2)));
  }
  static void storeF64(double* p, F v) {
    _mm_storeu_pd(p, _mm_cvtps_pd(v));
    _mm_storeu_pd(p + 2, _mm_cvtps_pd(_mm_movehl_ps(v, v)));
  }
  static void interleave2(float* d, F a, F b) {
    _mm_storeu_ps(d, _mm_unpacklo_ps(a, b));
    _mm_storeu_ps(d + 4, _mm_unpackhi_ps(a, b));
  }
  static void deinterleave2(const float* s, F& a, F& b) {
    F x = _mm_loadu_ps(s), y = _mm_loadu_ps(s + 4);
    a = _mm_shuffle_ps(x, y, _MM_SHUFFLE(2, 0, 2, 0));
    b = _mm_shuffle_ps(x, y, _MM_SHUFFLE(3, 1, 3, 1));
  }
  static F zeroNan(F v) { return _mm_and_ps(v, _mm_cmpord_ps(v, v)); }
  static F iota() { return _mm_setr_ps(0.0f, 1.0f, 2.0f, 3.0f); }
};

#include "kernels.inl"

constexpr sq_vector_ops kTable = SQ_SIMD_KERNEL_TABLE(SQ_ISA_SSE42, "sse42");

}  // namespace

const sq_vector_ops* opsSse42() noexcept { return &kTable; }

}  // namespace sq::simd::detail
