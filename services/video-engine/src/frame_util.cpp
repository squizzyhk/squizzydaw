#include "frame_util.hpp"

#include <cstring>
#include <limits>

namespace sq::video {

bool frameBytes(uint32_t width, uint32_t height, uint64_t& bytes) {
  bytes = 0;
  if (width == 0 || height == 0 || width > SQ_VIDEO_MAX_DIMENSION || height > SQ_VIDEO_MAX_DIMENSION) return false;
  bytes = uint64_t(width) * height * 4u;
  return bytes <= 0xFFFFFFFFull;  // readFrame takes a 32-bit buffer size
}

void copyRows(const uint8_t* src, int64_t srcPitch, uint8_t* dst, size_t dstStride, uint32_t rows, size_t rowBytes,
              bool forceOpaque) {
  for (uint32_t y = 0; y < rows; ++y) {
    const uint8_t* s = src + static_cast<std::ptrdiff_t>(srcPitch) * static_cast<std::ptrdiff_t>(y);
    uint8_t* d = dst + dstStride * y;
    std::memcpy(d, s, rowBytes);
    if (forceOpaque)
      for (size_t x = 3; x < rowBytes; x += 4) d[x] = 0xFF;
  }
}

namespace {
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
}  // namespace

int64_t hnsToNs(int64_t hns) {
  if (hns > kMax / 100) return kMax;
  if (hns < kMin / 100) return kMin;
  return hns * 100;
}

int64_t nsToHns(int64_t ns) { return ns / 100; }

int64_t rationalToNs(int64_t value, int64_t num, int64_t den) {
  if (den == 0) return 0;
  // value * num * 1e9 / den with 128-bit-free care: split into whole and remainder.
  long double r = static_cast<long double>(value) * static_cast<long double>(num) * 1e9L / static_cast<long double>(den);
  if (r >= static_cast<long double>(kMax)) return kMax;
  if (r <= static_cast<long double>(kMin)) return kMin;
  return static_cast<int64_t>(r < 0 ? r - 0.5L : r + 0.5L);
}

void copyName(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  size_t n = src ? std::strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n) std::memcpy(dst, src, n);
  dst[n] = 0;
}

}  // namespace sq::video
