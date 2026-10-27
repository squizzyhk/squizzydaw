// Squizzy Studio audio drivers - sample format conversion between the engine's float32 and the
// native formats of audio devices (WASAPI interleaved PCM/float, ASIO per-channel buffers).
//
// Header-only, shared by services/audiodriver-wasapi and services/audiodriver-asio (same owner).
// Every function here is realtime safe: no allocation, no locks, no syscalls.
//
// Conventions
// - Integer <-> float scaling uses 2^(N-1): int = round(x * 2^(N-1)), clamped to the N-bit range;
//   float = int / 2^(N-1). Integer round trips are exact; +1.0f clips to the largest positive code.
// - NaN converts to 0.
// - "stride" is the distance in bytes between two consecutive frames of the same channel
//   (interleaved: channels * bytesPerSample; planar: bytesPerSample).
// - Byte order is explicit per layout, independent of the host's endianness.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sq::audiodrv {

enum class SampleKind : uint8_t {
  Unknown = 0,
  F32,       // IEEE float32
  F64,       // IEEE float64
  I16,       // 16-bit signed
  I24,       // 24-bit signed, packed in 3 bytes
  I32,       // 32-bit signed, full range
  I32Hi24,   // 24 valid bits left-aligned in a 32-bit container (low byte written as 0)
  I32Lo16,   // value in the low 16 bits of a 32-bit container, sign-extended
  I32Lo18,   // ... low 18 bits
  I32Lo20,   // ... low 20 bits
  I32Lo24,   // ... low 24 bits
};

struct SampleLayout {
  SampleKind kind = SampleKind::Unknown;
  bool bigEndian = false;

  constexpr bool operator==(const SampleLayout& o) const noexcept { return kind == o.kind && bigEndian == o.bigEndian; }
};

constexpr uint32_t bytesPerSample(SampleKind k) noexcept {
  switch (k) {
    case SampleKind::I16: return 2;
    case SampleKind::I24: return 3;
    case SampleKind::F32:
    case SampleKind::I32:
    case SampleKind::I32Hi24:
    case SampleKind::I32Lo16:
    case SampleKind::I32Lo18:
    case SampleKind::I32Lo20:
    case SampleKind::I32Lo24: return 4;
    case SampleKind::F64: return 8;
    default: return 0;
  }
}

// Number of significant bits of an integer kind (0 for floats / unknown).
constexpr uint32_t significantBits(SampleKind k) noexcept {
  switch (k) {
    case SampleKind::I16:
    case SampleKind::I32Lo16: return 16;
    case SampleKind::I32Lo18: return 18;
    case SampleKind::I32Lo20: return 20;
    case SampleKind::I24:
    case SampleKind::I32Hi24:
    case SampleKind::I32Lo24: return 24;
    case SampleKind::I32: return 32;
    default: return 0;
  }
}

namespace detail {

inline uint64_t loadBytes(const uint8_t* p, uint32_t n, bool be) noexcept {
  uint64_t v = 0;
  if (be) {
    for (uint32_t i = 0; i < n; ++i) v = (v << 8) | p[i];
  } else {
    for (uint32_t i = n; i-- > 0;) v = (v << 8) | p[i];
  }
  return v;
}

inline void storeBytes(uint8_t* p, uint64_t v, uint32_t n, bool be) noexcept {
  if (be) {
    for (uint32_t i = n; i-- > 0;) {
      p[i] = static_cast<uint8_t>(v & 0xFFu);
      v >>= 8;
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      p[i] = static_cast<uint8_t>(v & 0xFFu);
      v >>= 8;
    }
  }
}

// Sign-extend the low `bits` bits of v.
inline int64_t signExtend(uint64_t v, uint32_t bits) noexcept {
  const uint64_t m = uint64_t(1) << (bits - 1);
  v &= (bits == 64) ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1);
  return static_cast<int64_t>((v ^ m) - m);
}

// float -> signed integer with `bits` significant bits.
inline int64_t quantize(double x, uint32_t bits) noexcept {
  if (!(x == x)) return 0;  // NaN
  const double scale = static_cast<double>(int64_t(1) << (bits - 1));
  const double maxv = scale - 1.0;
  double s = std::nearbyint(x * scale);
  if (s > maxv) s = maxv;
  if (s < -scale) s = -scale;
  return static_cast<int64_t>(s);
}

inline bool hostIsLittleEndian() noexcept {
  const uint16_t one = 1;
  uint8_t b;
  std::memcpy(&b, &one, 1);
  return b == 1;
}

}  // namespace detail

// Native -> float32 for one channel.
inline void toFloat(SampleLayout L, const void* src, uint32_t strideBytes, float* dst, uint32_t frames) noexcept {
  const uint8_t* p = static_cast<const uint8_t*>(src);
  const bool be = L.bigEndian;
  switch (L.kind) {
    case SampleKind::F32: {
      if (be != !detail::hostIsLittleEndian() || strideBytes != 4) {
        for (uint32_t i = 0; i < frames; ++i, p += strideBytes) {
          const uint32_t u = static_cast<uint32_t>(detail::loadBytes(p, 4, be));
          float f;
          std::memcpy(&f, &u, 4);
          dst[i] = f;
        }
      } else {
        std::memcpy(dst, p, size_t(frames) * 4);
      }
      return;
    }
    case SampleKind::F64: {
      for (uint32_t i = 0; i < frames; ++i, p += strideBytes) {
        const uint64_t u = detail::loadBytes(p, 8, be);
        double d;
        std::memcpy(&d, &u, 8);
        dst[i] = static_cast<float>(d);
      }
      return;
    }
    case SampleKind::I16:
    case SampleKind::I24:
    case SampleKind::I32:
    case SampleKind::I32Hi24:
    case SampleKind::I32Lo16:
    case SampleKind::I32Lo18:
    case SampleKind::I32Lo20:
    case SampleKind::I32Lo24: {
      const uint32_t nb = bytesPerSample(L.kind);
      // Left-aligned containers read as full-width values; right-aligned ones as their low bits.
      uint32_t bits = significantBits(L.kind);
      if (L.kind == SampleKind::I32Hi24) bits = 32;
      const double inv = 1.0 / static_cast<double>(int64_t(1) << (bits - 1));
      for (uint32_t i = 0; i < frames; ++i, p += strideBytes) {
        const int64_t v = detail::signExtend(detail::loadBytes(p, nb, be), bits);
        dst[i] = static_cast<float>(static_cast<double>(v) * inv);
      }
      return;
    }
    default:
      std::memset(dst, 0, size_t(frames) * sizeof(float));
      return;
  }
}

// float32 -> native for one channel (clips).
inline void fromFloat(SampleLayout L, const float* src, void* dstv, uint32_t strideBytes, uint32_t frames) noexcept {
  uint8_t* p = static_cast<uint8_t*>(dstv);
  const bool be = L.bigEndian;
  switch (L.kind) {
    case SampleKind::F32: {
      if (be != !detail::hostIsLittleEndian() || strideBytes != 4) {
        for (uint32_t i = 0; i < frames; ++i, p += strideBytes) {
          uint32_t u;
          std::memcpy(&u, &src[i], 4);
          detail::storeBytes(p, u, 4, be);
        }
      } else {
        std::memcpy(p, src, size_t(frames) * 4);
      }
      return;
    }
    case SampleKind::F64: {
      for (uint32_t i = 0; i < frames; ++i, p += strideBytes) {
        const double d = src[i];
        uint64_t u;
        std::memcpy(&u, &d, 8);
        detail::storeBytes(p, u, 8, be);
      }
      return;
    }
    case SampleKind::I32Hi24: {
      for (uint32_t i = 0; i < frames; ++i, p += strideBytes) {
        const int64_t v = detail::quantize(src[i], 24) * 256;  // left-align, low byte zero
        detail::storeBytes(p, static_cast<uint64_t>(v), 4, be);
      }
      return;
    }
    case SampleKind::I16:
    case SampleKind::I24:
    case SampleKind::I32:
    case SampleKind::I32Lo16:
    case SampleKind::I32Lo18:
    case SampleKind::I32Lo20:
    case SampleKind::I32Lo24: {
      const uint32_t nb = bytesPerSample(L.kind);
      const uint32_t bits = significantBits(L.kind);
      for (uint32_t i = 0; i < frames; ++i, p += strideBytes) {
        // Right-aligned containers keep the sign extension in the upper bits (two's complement).
        detail::storeBytes(p, static_cast<uint64_t>(detail::quantize(src[i], bits)), nb, be);
      }
      return;
    }
    default:
      return;  // unknown layout: nothing sensible to write
  }
}

// Writes digital silence (all-zero bytes are silence in every supported format).
inline void writeSilence(SampleLayout L, void* dstv, uint32_t strideBytes, uint32_t frames) noexcept {
  const uint32_t nb = bytesPerSample(L.kind);
  uint8_t* p = static_cast<uint8_t*>(dstv);
  if (nb == 0) return;
  if (strideBytes == nb) {
    std::memset(p, 0, size_t(frames) * nb);
    return;
  }
  for (uint32_t i = 0; i < frames; ++i, p += strideBytes) std::memset(p, 0, nb);
}

// Short label for logs and tests.
constexpr const char* kindName(SampleKind k) noexcept {
  switch (k) {
    case SampleKind::F32: return "f32";
    case SampleKind::F64: return "f64";
    case SampleKind::I16: return "i16";
    case SampleKind::I24: return "i24";
    case SampleKind::I32: return "i32";
    case SampleKind::I32Hi24: return "i24in32hi";
    case SampleKind::I32Lo16: return "i16in32lo";
    case SampleKind::I32Lo18: return "i18in32lo";
    case SampleKind::I32Lo20: return "i20in32lo";
    case SampleKind::I32Lo24: return "i24in32lo";
    default: return "unknown";
  }
}

}  // namespace sq::audiodrv
