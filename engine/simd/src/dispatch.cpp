// CPU feature detection + kernel-set selection; defines sq_dsp_vector_ops() (interfaces/dsp.h).
#include <cstdlib>

#include "ops_internal.hpp"
#include "sq/engine/simd/simd.hpp"

#if defined(SQ_SIMD_X86)
#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace sq::simd {

namespace {

#if defined(SQ_SIMD_X86)
void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
#if defined(_MSC_VER)
  int v[4];
  __cpuidex(v, static_cast<int>(leaf), static_cast<int>(sub));
  for (int i = 0; i < 4; ++i) r[i] = static_cast<uint32_t>(v[i]);
#else
  unsigned a = 0, b = 0, c = 0, d = 0;
  __cpuid_count(leaf, sub, a, b, c, d);
  r[0] = a; r[1] = b; r[2] = c; r[3] = d;
#endif
}

uint64_t xcr0() {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  uint32_t lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}
#endif

uint32_t detect() noexcept {
  uint32_t mask = 1u << SQ_ISA_GENERIC;
#if defined(SQ_SIMD_X86)
  uint32_t r[4];
  cpuid(0, 0, r);
  const uint32_t maxLeaf = r[0];
  if (maxLeaf < 1) return mask;
  cpuid(1, 0, r);
  const uint32_t ecx1 = r[2];
  const bool ssse3 = ecx1 & (1u << 9), sse41 = ecx1 & (1u << 19), sse42 = ecx1 & (1u << 20);
  const bool fma = ecx1 & (1u << 12), osxsave = ecx1 & (1u << 27), avx = ecx1 & (1u << 28);
  if (ssse3 && sse41 && sse42) mask |= 1u << SQ_ISA_SSE42;
  if (!(osxsave && avx) || maxLeaf < 7) return mask;
  const uint64_t xc = xcr0();
  const bool osYmm = (xc & 0x6) == 0x6;           // SSE + AVX state
  const bool osZmm = (xc & 0xE6) == 0xE6;         // + opmask, ZMM_Hi256, Hi16_ZMM
  cpuid(7, 0, r);
  const uint32_t ebx7 = r[1];
  const bool avx2 = ebx7 & (1u << 5), avx512f = ebx7 & (1u << 16);
  if (osYmm && avx2 && fma && (mask & (1u << SQ_ISA_SSE42))) mask |= 1u << SQ_ISA_AVX2;
  if (osZmm && avx512f && (mask & (1u << SQ_ISA_AVX2))) mask |= 1u << SQ_ISA_AVX512;
#endif
  return mask;
}

const sq_vector_ops* compiledTable(int32_t isa) noexcept {
  switch (isa) {
    case SQ_ISA_GENERIC: return detail::opsGeneric();
#if defined(SQ_SIMD_X86)
    case SQ_ISA_SSE42: return detail::opsSse42();
    case SQ_ISA_AVX2: return detail::opsAvx2();
    case SQ_ISA_AVX512: return detail::opsAvx512();
#endif
    default: return nullptr;
  }
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool ieq(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (lower(*a) != lower(*b)) return false;
  return *a == *b;
}

// Kernel sets in capability order (x86). NEON would be its own chain on ARM.
constexpr int32_t kOrder[] = {SQ_ISA_AVX512, SQ_ISA_AVX2, SQ_ISA_SSE42, SQ_ISA_GENERIC};
int32_t rank(int32_t isa) {
  for (int32_t i = 0; i < 4; ++i)
    if (kOrder[i] == isa) return 3 - i;
  return -1;
}

}  // namespace

uint32_t supportedIsaMask() noexcept {
  static const uint32_t mask = detect();
  return mask;
}

uint32_t compiledIsaMask() noexcept {
  uint32_t m = 0;
  for (int32_t isa : kOrder)
    if (compiledTable(isa)) m |= 1u << isa;
  return m;
}

const sq_vector_ops* genericOps() noexcept { return detail::opsGeneric(); }

const sq_vector_ops* opsFor(int32_t isa) noexcept {
  if (isa < 0 || isa > 31 || !(supportedIsaMask() & (1u << isa))) return nullptr;
  return compiledTable(isa);
}

int32_t parseIsaName(const char* name) noexcept {
  if (!name) return -1;
  if (ieq(name, "generic")) return SQ_ISA_GENERIC;
  if (ieq(name, "sse42") || ieq(name, "sse4.2")) return SQ_ISA_SSE42;
  if (ieq(name, "avx2")) return SQ_ISA_AVX2;
  if (ieq(name, "avx512") || ieq(name, "avx-512")) return SQ_ISA_AVX512;
  return -1;
}

const sq_vector_ops* selectOps(uint32_t supportedMask, const char* forcedName) noexcept {
  const int32_t forced = parseIsaName(forcedName);
  const int32_t cap = forced >= 0 ? rank(forced) : 3;
  for (int32_t isa : kOrder) {
    if (rank(isa) > cap) continue;
    if (!(supportedMask & (1u << isa))) continue;
    if (const sq_vector_ops* t = compiledTable(isa)) return t;
  }
  return detail::opsGeneric();
}

}  // namespace sq::simd

extern "C" const sq_vector_ops* sq_dsp_vector_ops(void) {
  static const sq_vector_ops* const table = sq::simd::selectOps(sq::simd::supportedIsaMask(), std::getenv("SQ_DSP_ISA"));
  return table;
}
