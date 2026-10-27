// engine/simd internal: per-ISA kernel tables. Each lives in its own translation unit so that
// non-MSVC compilers can build it with the matching -m flags (see CMakeLists.txt).
#pragma once

#include "sq/interfaces/dsp.h"

namespace sq::simd::detail {

const sq_vector_ops* opsGeneric() noexcept;
#if defined(SQ_SIMD_X86)
const sq_vector_ops* opsSse42() noexcept;
const sq_vector_ops* opsAvx2() noexcept;
const sq_vector_ops* opsAvx512() noexcept;
#endif

// Scaling constants shared by every kernel set (all are exact powers of two).
inline constexpr float kI16Scale = 32768.0f;
inline constexpr float kI24Scale = 8388608.0f;
inline constexpr float kI32Scale = 2147483648.0f;
inline constexpr float kI16Max = 32767.0f;
inline constexpr float kI24Max = 8388607.0f;
inline constexpr float kI32Max = 2147483520.0f;  // largest float below 2^31

}  // namespace sq::simd::detail
