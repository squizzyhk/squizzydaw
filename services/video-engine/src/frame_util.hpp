// video-engine: portable helpers shared by the backends (unit-tested on their own).
#pragma once

#include <cstddef>
#include <cstdint>

#include "sq/interfaces/video.h"

namespace sq::video {

// True when width x height BGRA8 fits the limits; bytes receives width * height * 4.
bool frameBytes(uint32_t width, uint32_t height, uint64_t& bytes);

// Copies `rows` rows of `rowBytes` from a source whose first row is at `src` and whose pitch may
// be negative (bottom-up images) into a top-down destination with pitch dstStride. When
// forceOpaque, every 4th byte (alpha of BGRA/BGRX) is set to 255.
void copyRows(const uint8_t* src, int64_t srcPitch, uint8_t* dst, size_t dstStride, uint32_t rows, size_t rowBytes,
              bool forceOpaque);

// 100-ns units (Media Foundation) <-> ns, saturating.
int64_t hnsToNs(int64_t hns);
int64_t nsToHns(int64_t ns);
// value * num / den in ns for a rational time base (ffmpeg), saturating; den 0 -> 0.
int64_t rationalToNs(int64_t value, int64_t num, int64_t den);

// After a seek to targetNs, drops decoded frames that end at or before the target, so the first
// frame kept is the one on screen at the target. Frames without a known duration are kept.
class SeekFilter {
 public:
  void arm(int64_t targetNs) {
    target_ = targetNs;
    armed_ = true;
  }
  void disarm() { armed_ = false; }
  bool armed() const { return armed_; }
  // True when the frame (ts, duration) must be skipped; the first kept frame disarms the filter.
  bool skip(int64_t ts, int64_t duration) {
    if (!armed_) return false;
    if (duration > 0 && ts + duration <= target_) return true;
    armed_ = false;
    return false;
  }

 private:
  int64_t target_ = 0;
  bool armed_ = false;
};

void copyName(char* dst, size_t cap, const char* src);

}  // namespace sq::video
