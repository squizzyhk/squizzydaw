// Squizzy Studio - ASIO driver service: sample conversion (see asio_convert.hpp for the rules).
#include "asio_convert.hpp"

#include <bit>
#include <cmath>
#include <cstring>

static_assert(std::endian::native == std::endian::little, "asio_convert assumes a little-endian host");

namespace sq::asio {
namespace {

struct Desc {
  uint32_t bytes = 0;  // container size
  uint32_t bits = 0;   // significant bits
  bool big = false;
  bool flt = false;
};

bool describe(SampleType t, Desc& d) noexcept {
  switch (t) {
    case SampleType::Int16LSB: d = {2, 16, false, false}; return true;
    case SampleType::Int16MSB: d = {2, 16, true, false}; return true;
    case SampleType::Int24LSB: d = {3, 24, false, false}; return true;
    case SampleType::Int24MSB: d = {3, 24, true, false}; return true;
    case SampleType::Int32LSB: d = {4, 32, false, false}; return true;
    case SampleType::Int32MSB: d = {4, 32, true, false}; return true;
    case SampleType::Int32LSB16: d = {4, 16, false, false}; return true;
    case SampleType::Int32MSB16: d = {4, 16, true, false}; return true;
    case SampleType::Int32LSB18: d = {4, 18, false, false}; return true;
    case SampleType::Int32MSB18: d = {4, 18, true, false}; return true;
    case SampleType::Int32LSB20: d = {4, 20, false, false}; return true;
    case SampleType::Int32MSB20: d = {4, 20, true, false}; return true;
    case SampleType::Int32LSB24: d = {4, 24, false, false}; return true;
    case SampleType::Int32MSB24: d = {4, 24, true, false}; return true;
    case SampleType::Float32LSB: d = {4, 32, false, true}; return true;
    case SampleType::Float32MSB: d = {4, 32, true, true}; return true;
    case SampleType::Float64LSB: d = {8, 64, false, true}; return true;
    case SampleType::Float64MSB: d = {8, 64, true, true}; return true;
  }
  return false;
}

inline uint16_t bswap16(uint16_t v) noexcept { return static_cast<uint16_t>((v >> 8) | (v << 8)); }
inline uint32_t bswap32(uint32_t v) noexcept {
  return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}
inline uint64_t bswap64(uint64_t v) noexcept {
  return (static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v))) << 32) | bswap32(static_cast<uint32_t>(v >> 32));
}

// Sign-extend the low `bits` bits of v.
inline int32_t signExtend(uint32_t v, uint32_t bits) noexcept {
  const uint32_t shift = 32u - bits;
  return static_cast<int32_t>(v << shift) >> shift;  // arithmetic shift (C++20)
}

inline double invScale(uint32_t bits) noexcept { return 1.0 / static_cast<double>(1ull << (bits - 1)); }

inline int32_t quantize(float x, uint32_t bits) noexcept {
  if (x != x) return 0;  // NaN
  const double scale = static_cast<double>(1ull << (bits - 1));
  const double v = std::nearbyint(static_cast<double>(x) * scale);  // exact product, ties to even
  if (v >= scale) return static_cast<int32_t>(static_cast<int64_t>(scale) - 1);
  if (v <= -scale) return static_cast<int32_t>(-static_cast<int64_t>(scale));
  return static_cast<int32_t>(v);
}

// ---- integer readers / writers ------------------------------------------------------------

template <bool Big>
void readI16(const uint8_t* s, float* d, uint32_t n) noexcept {
  const double k = invScale(16);
  for (uint32_t i = 0; i < n; ++i, s += 2) {
    uint16_t u;
    std::memcpy(&u, s, 2);
    if constexpr (Big) u = bswap16(u);
    d[i] = static_cast<float>(static_cast<double>(static_cast<int16_t>(u)) * k);
  }
}
template <bool Big>
void writeI16(const float* s, uint8_t* d, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i, d += 2) {
    uint16_t u = static_cast<uint16_t>(quantize(s[i], 16));
    if constexpr (Big) u = bswap16(u);
    std::memcpy(d, &u, 2);
  }
}

template <bool Big>
void readI24(const uint8_t* s, float* d, uint32_t n) noexcept {
  const double k = invScale(24);
  for (uint32_t i = 0; i < n; ++i, s += 3) {
    uint32_t u;
    if constexpr (Big) {
      u = (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2];
    } else {
      u = s[0] | (static_cast<uint32_t>(s[1]) << 8) | (static_cast<uint32_t>(s[2]) << 16);
    }
    d[i] = static_cast<float>(static_cast<double>(signExtend(u, 24)) * k);
  }
}
template <bool Big>
void writeI24(const float* s, uint8_t* d, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i, d += 3) {
    const uint32_t u = static_cast<uint32_t>(quantize(s[i], 24));
    const uint8_t b0 = static_cast<uint8_t>(u), b1 = static_cast<uint8_t>(u >> 8), b2 = static_cast<uint8_t>(u >> 16);
    if constexpr (Big) {
      d[0] = b2;
      d[1] = b1;
      d[2] = b0;
    } else {
      d[0] = b0;
      d[1] = b1;
      d[2] = b2;
    }
  }
}

// 32-bit container holding `bits` significant bits (32 = plain Int32).
template <bool Big>
void readI32(const uint8_t* s, float* d, uint32_t n, uint32_t bits) noexcept {
  const double k = invScale(bits);
  for (uint32_t i = 0; i < n; ++i, s += 4) {
    uint32_t u;
    std::memcpy(&u, s, 4);
    if constexpr (Big) u = bswap32(u);
    d[i] = static_cast<float>(static_cast<double>(signExtend(u, bits)) * k);
  }
}
template <bool Big>
void writeI32(const float* s, uint8_t* d, uint32_t n, uint32_t bits) noexcept {
  for (uint32_t i = 0; i < n; ++i, d += 4) {
    uint32_t u = static_cast<uint32_t>(quantize(s[i], bits));  // sign-extended to 32 bits
    if constexpr (Big) u = bswap32(u);
    std::memcpy(d, &u, 4);
  }
}

// ---- float readers / writers --------------------------------------------------------------

template <bool Big>
void readF32(const uint8_t* s, float* d, uint32_t n) noexcept {
  if constexpr (!Big) {
    std::memcpy(d, s, sizeof(float) * n);
  } else {
    for (uint32_t i = 0; i < n; ++i, s += 4) {
      uint32_t u;
      std::memcpy(&u, s, 4);
      u = bswap32(u);
      std::memcpy(&d[i], &u, 4);
    }
  }
}
template <bool Big>
void writeF32(const float* s, uint8_t* d, uint32_t n) noexcept {
  if constexpr (!Big) {
    std::memcpy(d, s, sizeof(float) * n);
  } else {
    for (uint32_t i = 0; i < n; ++i, d += 4) {
      uint32_t u;
      std::memcpy(&u, &s[i], 4);
      u = bswap32(u);
      std::memcpy(d, &u, 4);
    }
  }
}
template <bool Big>
void readF64(const uint8_t* s, float* d, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i, s += 8) {
    uint64_t u;
    std::memcpy(&u, s, 8);
    if constexpr (Big) u = bswap64(u);
    double v;
    std::memcpy(&v, &u, 8);
    d[i] = static_cast<float>(v);
  }
}
template <bool Big>
void writeF64(const float* s, uint8_t* d, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i, d += 8) {
    const double v = static_cast<double>(s[i]);
    uint64_t u;
    std::memcpy(&u, &v, 8);
    if constexpr (Big) u = bswap64(u);
    std::memcpy(d, &u, 8);
  }
}

}  // namespace

bool isSupported(SampleType t) noexcept {
  Desc d;
  return describe(t, d);
}
uint32_t bytesPerSample(SampleType t) noexcept {
  Desc d;
  return describe(t, d) ? d.bytes : 0;
}
uint32_t significantBits(SampleType t) noexcept {
  Desc d;
  return describe(t, d) ? d.bits : 0;
}
bool isBigEndian(SampleType t) noexcept {
  Desc d;
  return describe(t, d) && d.big;
}
bool isFloat(SampleType t) noexcept {
  Desc d;
  return describe(t, d) && d.flt;
}

sq_sample_format toSampleFormat(SampleType t) noexcept {
  Desc d;
  if (!describe(t, d)) return SQ_SAMPLE_UNKNOWN;
  if (d.flt) return d.bits == 64 ? SQ_SAMPLE_F64 : SQ_SAMPLE_F32;
  if (d.bytes == 2) return SQ_SAMPLE_I16;
  if (d.bytes == 3) return SQ_SAMPLE_I24;
  if (d.bits == 32) return SQ_SAMPLE_I32;
  if (d.bits == 24) return SQ_SAMPLE_I24_IN32;
  return SQ_SAMPLE_I32;  // 16/18/20 bits in a 32-bit container
}

const char* sampleTypeName(SampleType t) noexcept {
  switch (t) {
    case SampleType::Int16MSB: return "Int16MSB";
    case SampleType::Int24MSB: return "Int24MSB";
    case SampleType::Int32MSB: return "Int32MSB";
    case SampleType::Float32MSB: return "Float32MSB";
    case SampleType::Float64MSB: return "Float64MSB";
    case SampleType::Int32MSB16: return "Int32MSB16";
    case SampleType::Int32MSB18: return "Int32MSB18";
    case SampleType::Int32MSB20: return "Int32MSB20";
    case SampleType::Int32MSB24: return "Int32MSB24";
    case SampleType::Int16LSB: return "Int16LSB";
    case SampleType::Int24LSB: return "Int24LSB";
    case SampleType::Int32LSB: return "Int32LSB";
    case SampleType::Float32LSB: return "Float32LSB";
    case SampleType::Float64LSB: return "Float64LSB";
    case SampleType::Int32LSB16: return "Int32LSB16";
    case SampleType::Int32LSB18: return "Int32LSB18";
    case SampleType::Int32LSB20: return "Int32LSB20";
    case SampleType::Int32LSB24: return "Int32LSB24";
  }
  return "unsupported";
}

void toFloat(SampleType t, const void* src, float* dst, uint32_t frames) noexcept {
  if (!dst || frames == 0) return;
  Desc d;
  if (!src || !describe(t, d)) {
    std::memset(dst, 0, sizeof(float) * frames);
    return;
  }
  const auto* s = static_cast<const uint8_t*>(src);
  if (d.flt) {
    if (d.bits == 32) {
      d.big ? readF32<true>(s, dst, frames) : readF32<false>(s, dst, frames);
    } else {
      d.big ? readF64<true>(s, dst, frames) : readF64<false>(s, dst, frames);
    }
  } else if (d.bytes == 2) {
    d.big ? readI16<true>(s, dst, frames) : readI16<false>(s, dst, frames);
  } else if (d.bytes == 3) {
    d.big ? readI24<true>(s, dst, frames) : readI24<false>(s, dst, frames);
  } else {
    d.big ? readI32<true>(s, dst, frames, d.bits) : readI32<false>(s, dst, frames, d.bits);
  }
}

void fromFloat(SampleType t, const float* src, void* dst, uint32_t frames) noexcept {
  if (!src || !dst || frames == 0) return;
  Desc d;
  if (!describe(t, d)) return;
  auto* o = static_cast<uint8_t*>(dst);
  if (d.flt) {
    if (d.bits == 32) {
      d.big ? writeF32<true>(src, o, frames) : writeF32<false>(src, o, frames);
    } else {
      d.big ? writeF64<true>(src, o, frames) : writeF64<false>(src, o, frames);
    }
  } else if (d.bytes == 2) {
    d.big ? writeI16<true>(src, o, frames) : writeI16<false>(src, o, frames);
  } else if (d.bytes == 3) {
    d.big ? writeI24<true>(src, o, frames) : writeI24<false>(src, o, frames);
  } else {
    d.big ? writeI32<true>(src, o, frames, d.bits) : writeI32<false>(src, o, frames, d.bits);
  }
}

void writeSilence(SampleType t, void* dst, uint32_t frames) noexcept {
  const uint32_t b = bytesPerSample(t);
  if (dst && b) std::memset(dst, 0, static_cast<size_t>(b) * frames);  // zero is silence in every format
}

}  // namespace sq::asio
