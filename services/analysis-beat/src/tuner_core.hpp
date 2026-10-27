// analysis-beat: realtime pitch tracker for the tuner (YIN-style difference function computed with
// an FFT cross-correlation, cumulative-mean normalisation, absolute threshold, parabolic
// refinement). Own implementation.
//
// prepare() allocates; push() is RT-safe (no allocation, no locks); latest() is wait-free from
// any thread (frequency + confidence are packed into one 64-bit atomic).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "sq/engine/simd/fft.hpp"

namespace sq::tuner {

struct Estimate {
  double frequency = 0.0;  // 0 = no pitch
  float confidence = 0.0f;
};

class PitchTracker {
 public:
  bool prepare(double sampleRate, uint32_t maxBlock, double minHz = 40.0);
  bool prepared() const noexcept { return fft_ != nullptr; }
  // Mono samples; analyses every hop. RT-safe.
  void push(const float* x, uint32_t n) noexcept;
  // Mixes channels to mono first (uses the prepared scratch buffer). RT-safe.
  void pushChannels(const float* const* ch, uint32_t channels, uint32_t n) noexcept;
  Estimate latest() const noexcept;
  void reset() noexcept;
  // Runs the estimator on an arbitrary buffer of window() samples (offline helper / tests).
  Estimate estimate(const float* frame) noexcept;
  uint32_t window() const noexcept { return w_; }

 private:
  void analyse() noexcept;
  double sr_ = 44100.0;
  uint32_t w_ = 0, hop_ = 0, pos_ = 0, sinceLast_ = 0, filled_ = 0;
  std::vector<float> ring_, frame_, mono_;
  std::unique_ptr<sq::simd::RealFft> fft_;
  std::vector<float> a_, b_, are_, aim_, bre_, bim_, corr_;
  std::vector<double> d_, cmnd_;
  std::atomic<uint64_t> packed_{0};
};

}  // namespace sq::tuner
