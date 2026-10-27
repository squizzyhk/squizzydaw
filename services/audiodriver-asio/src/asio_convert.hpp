// Squizzy Studio - ASIO driver service: native sample format <-> float32 conversion.
// SDK-independent, realtime safe (no allocation, no locks, noexcept), unit-tested bit-exact.
//
// Rules (shared by every integer width N = 16, 18, 20, 24, 32):
//   to float   : value / 2^(N-1)            (computed in double, rounded once to float)
//   from float : nearest(x * 2^(N-1)) with ties to even, clamped to [-2^(N-1), 2^(N-1) - 1];
//                NaN -> 0 (same NaN policy as sq_dsp_vector_ops integer conversion)
//   Int32xxNN containers are read by sign-extending the low N bits and written sign-extended.
//   Float32/Float64 pass through unclamped (Float64 -> float rounds once; float -> double exact).
//   MSB variants are the same values byte-swapped.
#pragma once

#include <cstdint>

#include "asio_types.hpp"
#include "sq/media.h"

namespace sq::asio {

bool isSupported(SampleType t) noexcept;
// Bytes per sample in the device buffer; 0 if unsupported.
uint32_t bytesPerSample(SampleType t) noexcept;
// Significant bits (16, 18, 20, 24, 32 for integers; 32 / 64 for floats); 0 if unsupported.
uint32_t significantBits(SampleType t) noexcept;
bool isBigEndian(SampleType t) noexcept;
bool isFloat(SampleType t) noexcept;
// Closest sq_sample_format (for sq_audio_device_config.device_format). Big-endian and narrow
// container types map to the matching width; unsupported -> SQ_SAMPLE_UNKNOWN.
sq_sample_format toSampleFormat(SampleType t) noexcept;
const char* sampleTypeName(SampleType t) noexcept;

// Device buffer (frames samples of type t) -> float. Unsupported types write silence.
void toFloat(SampleType t, const void* src, float* dst, uint32_t frames) noexcept;
// float -> device buffer. Unsupported types leave dst untouched.
void fromFloat(SampleType t, const float* src, void* dst, uint32_t frames) noexcept;
// Writes digital silence in the native format.
void writeSilence(SampleType t, void* dst, uint32_t frames) noexcept;

}  // namespace sq::asio
