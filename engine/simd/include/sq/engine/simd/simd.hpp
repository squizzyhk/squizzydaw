// Squizzy Studio - engine/simd: runtime-dispatched vector kernels (plan 2.10).
//
// Implements the `sq_vector_ops` table of sq/interfaces/dsp.h with four kernel sets written with
// compiler intrinsics (no IPP): generic C++, SSE4.2, AVX2(+FMA), AVX-512F. The best set the CPU
// (and OS, via XGETBV) supports is chosen once, at the first call of sq_dsp_vector_ops().
// Set env SQ_DSP_ISA=generic|sse42|avx2|avx512 to cap the selection (tests, bisecting).
//
// Link sq::engine_simd (static, no DLL dependency). Everything here is RT-safe once selected.
#pragma once

#include <cstdint>

#include "sq/interfaces/dsp.h"

namespace sq::simd {

// Bit mask of SQ_ISA_* (1u << isa) the running CPU + OS can execute. Always has SQ_ISA_GENERIC.
uint32_t supportedIsaMask() noexcept;

// Bit mask of SQ_ISA_* kernel sets compiled into this build (x86-64: generic..avx512).
uint32_t compiledIsaMask() noexcept;

// Kernel table for one ISA, or nullptr if it is not compiled in or not supported by this CPU.
const sq_vector_ops* opsFor(int32_t isa) noexcept;

// Always available reference implementation.
const sq_vector_ops* genericOps() noexcept;

// "generic" | "sse42" | "avx2" | "avx512" (case-insensitive) -> SQ_ISA_*, else -1.
int32_t parseIsaName(const char* name) noexcept;

// Pure selection policy used by sq_dsp_vector_ops(): the highest ISA in `supportedMask` that is
// compiled in, capped at `forcedName` when that parses (an unsupported cap falls to the next lower
// supported set). Exposed for tests.
const sq_vector_ops* selectOps(uint32_t supportedMask, const char* forcedName) noexcept;

}  // namespace sq::simd
