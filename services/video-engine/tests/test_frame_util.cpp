// video-engine: portable frame helpers.
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "frame_util.hpp"
#include "sqtest.hpp"

using namespace sq::video;

SQ_TEST("video util: frame size limits") {
  uint64_t b = 0;
  CHECK(frameBytes(1920, 1080, b));
  CHECK_EQ(b, uint64_t(1920) * 1080 * 4);
  CHECK(!frameBytes(0, 10, b));
  CHECK(!frameBytes(10, 0, b));
  CHECK(!frameBytes(SQ_VIDEO_MAX_DIMENSION + 1, 2, b));
  CHECK(frameBytes(SQ_VIDEO_MAX_DIMENSION, SQ_VIDEO_MAX_DIMENSION, b));  // 1 GiB: still a 32-bit size
  CHECK_EQ(b, uint64_t(1) << 30);
}

SQ_TEST("video util: row copy handles positive and negative pitch, forces alpha") {
  // 2x3 BGRX source, row r has every byte = r*10 + column-ish marker, alpha garbage.
  const uint32_t w = 2, h = 3;
  const size_t row = w * 4, pitch = row + 8;  // padded source rows
  std::vector<uint8_t> src(pitch * h, 0xEE);
  for (uint32_t y = 0; y < h; ++y)
    for (size_t x = 0; x < row; ++x) src[y * pitch + x] = static_cast<uint8_t>((x % 4 == 3) ? 7 : y * 10 + x);
  std::vector<uint8_t> dst(row * h, 0);
  copyRows(src.data(), static_cast<int64_t>(pitch), dst.data(), row, h, row, true);
  for (uint32_t y = 0; y < h; ++y)
    for (size_t x = 0; x < row; ++x) CHECK_EQ(dst[y * row + x], static_cast<uint8_t>((x % 4 == 3) ? 255 : y * 10 + x));
  // Bottom-up: scan line 0 is the last row in memory, pitch negative.
  std::vector<uint8_t> flip(row * h, 0);
  copyRows(src.data() + pitch * (h - 1), -static_cast<int64_t>(pitch), flip.data(), row, h, row, false);
  for (uint32_t y = 0; y < h; ++y) CHECK_EQ(flip[y * row], static_cast<uint8_t>((h - 1 - y) * 10));
  CHECK_EQ(flip[3], uint8_t(7));  // alpha untouched without forceOpaque
}

SQ_TEST("video util: time conversions") {
  CHECK_EQ(hnsToNs(1), int64_t(100));
  CHECK_EQ(hnsToNs(-5), int64_t(-500));
  CHECK_EQ(hnsToNs(std::numeric_limits<int64_t>::max()), std::numeric_limits<int64_t>::max());
  CHECK_EQ(nsToHns(1000), int64_t(10));
  CHECK_EQ(rationalToNs(3, 1, 30), int64_t(100000000));
  CHECK_EQ(rationalToNs(1001, 1, 30000), int64_t(33366667));
  CHECK_EQ(rationalToNs(90000, 1, 90000), int64_t(1000000000));
  CHECK_EQ(rationalToNs(5, 1, 0), int64_t(0));
  CHECK_EQ(rationalToNs(std::numeric_limits<int64_t>::max(), 1, 1), std::numeric_limits<int64_t>::max());
}

SQ_TEST("video util: seek filter keeps the frame on screen at the target") {
  SeekFilter f;
  CHECK(!f.skip(0, 33));
  f.arm(100);
  CHECK(f.skip(0, 33));
  CHECK(f.skip(33, 33));
  CHECK(f.skip(66, 34));    // ends exactly at 100: not on screen at 100
  CHECK(!f.skip(100, 33));  // on screen at 100
  CHECK(!f.armed());
  CHECK(!f.skip(133, 33));
  f.arm(50);
  CHECK(!f.skip(0, 0));  // unknown duration: kept
  char name[4];
  copyName(name, sizeof name, "abcdef");
  CHECK_EQ(std::string(name), std::string("abc"));
  copyName(name, sizeof name, nullptr);
  CHECK_EQ(std::string(name), std::string(""));
}
