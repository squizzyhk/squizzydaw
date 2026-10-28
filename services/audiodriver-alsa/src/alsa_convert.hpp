// audiodriver-alsa: conversion between the engine's non-interleaved float32 and the interleaved
// little-endian PCM formats an ALSA device accepts. Header-only, realtime safe (no allocation,
// no locks, no syscalls).
//
// Scaling: int = round(x * 2^(N-1)) clamped to [-2^(N-1), 2^(N-1) - 1]; float = int / 2^(N-1).
// Integer -> float -> integer round trips are exact; +1.0 clips to the largest positive code;
// NaN converts to 0.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sq::alsa {

enum class Fmt : uint8_t {
  Unknown = 0,
  F32,     // FLOAT_LE
  S16,     // S16_LE
  S24_3,   // S24_3LE: packed 3 bytes
  S24_32,  // S24_LE: 24 bits in the low 3 bytes of a 4-byte container
  S32,     // S32_LE
};

constexpr uint32_t bytesPerSample(Fmt f) noexcept {
  switch (f) {
    case Fmt::S16: return 2;
    case Fmt::S24_3: return 3;
    case Fmt::F32:
    case Fmt::S24_32:
    case Fmt::S32: return 4;
    default: return 0;
  }
}

constexpr const char* fmtName(Fmt f) noexcept {
  switch (f) {
    case Fmt::F32: return "FLOAT_LE";
    case Fmt::S16: return "S16_LE";
    case Fmt::S24_3: return "S24_3LE";
    case Fmt::S24_32: return "S24_LE";
    case Fmt::S32: return "S32_LE";
    default: return "unknown";
  }
}

namespace detail {

inline int64_t quantize(float x, uint32_t bits) noexcept {
  if (!(x == x)) return 0;  // NaN
  const double scale = static_cast<double>(int64_t(1) << (bits - 1));
  double s = std::nearbyint(static_cast<double>(x) * scale);
  if (s > scale - 1.0) s = scale - 1.0;
  if (s < -scale) s = -scale;
  return static_cast<int64_t>(s);
}

inline void store(uint8_t* p, uint32_t v, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint32_t load(const uint8_t* p, uint32_t n) noexcept {
  uint32_t v = 0;
  for (uint32_t i = n; i-- > 0;) v = (v << 8) | p[i];
  return v;
}

inline int32_t signExtend(uint32_t v, uint32_t bits) noexcept {
  if (bits >= 32) return static_cast<int32_t>(v);
  const uint32_t m = 1u << (bits - 1);
  v &= (1u << bits) - 1u;
  return static_cast<int32_t>(v ^ m) - static_cast<int32_t>(m);
}

}  // namespace detail

// One sample float -> device bytes at p.
inline void encodeSample(Fmt f, float x, uint8_t* p) noexcept {
  switch (f) {
    case Fmt::F32: {
      uint32_t u;
      std::memcpy(&u, &x, 4);
      detail::store(p, u, 4);
      return;
    }
    case Fmt::S16: detail::store(p, static_cast<uint32_t>(detail::quantize(x, 16)), 2); return;
    case Fmt::S24_3: detail::store(p, static_cast<uint32_t>(detail::quantize(x, 24)), 3); return;
    case Fmt::S24_32: detail::store(p, static_cast<uint32_t>(detail::quantize(x, 24)), 4); return;  // sign-extended
    case Fmt::S32: detail::store(p, static_cast<uint32_t>(detail::quantize(x, 32)), 4); return;
    default: return;
  }
}

// One sample device bytes at p -> float.
inline float decodeSample(Fmt f, const uint8_t* p) noexcept {
  switch (f) {
    case Fmt::F32: {
      const uint32_t u = detail::load(p, 4);
      float x;
      std::memcpy(&x, &u, 4);
      return x;
    }
    case Fmt::S16: return static_cast<float>(detail::signExtend(detail::load(p, 2), 16) / 32768.0);
    case Fmt::S24_3:
    case Fmt::S24_32: return static_cast<float>(detail::signExtend(detail::load(p, 3), 24) / 8388608.0);
    case Fmt::S32: return static_cast<float>(static_cast<int32_t>(detail::load(p, 4)) / 2147483648.0);
    default: return 0.0f;
  }
}

// Interleave `count` engine channels into a device buffer of `devChannels` channels.
// map[k] = device channel of engine channel k. Device channels nobody maps to get silence.
inline void interleave(Fmt f, const float* const* src, const uint32_t* map, uint32_t count, uint8_t* dst, uint32_t devChannels,
                       uint32_t frames) noexcept {
  const uint32_t bps = bytesPerSample(f);
  const size_t frameBytes = size_t(bps) * devChannels;
  std::memset(dst, 0, frameBytes * frames);  // zero bytes = silence in every format here
  for (uint32_t k = 0; k < count; ++k) {
    const float* s = src[k];
    uint8_t* d = dst + size_t(map[k]) * bps;
    for (uint32_t i = 0; i < frames; ++i, d += frameBytes) encodeSample(f, s[i], d);
  }
}

// De-interleave: engine channel k receives device channel map[k].
inline void deinterleave(Fmt f, const uint8_t* src, uint32_t devChannels, float* const* dst, const uint32_t* map, uint32_t count,
                         uint32_t frames) noexcept {
  const uint32_t bps = bytesPerSample(f);
  const size_t frameBytes = size_t(bps) * devChannels;
  for (uint32_t k = 0; k < count; ++k) {
    const uint8_t* s = src + size_t(map[k]) * bps;
    float* d = dst[k];
    for (uint32_t i = 0; i < frames; ++i, s += frameBytes) d[i] = decodeSample(f, s);
  }
}

}  // namespace sq::alsa
