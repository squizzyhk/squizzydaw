// Squizzy Studio engine/audio - private meter interface of built-in dynamics devices.
#pragma once

#include <atomic>

namespace sq::engine::audio::builtin {

class MeterSource {
 public:
  virtual ~MeterSource() = default;
  float meterValue() const noexcept { return meter_.load(std::memory_order_relaxed); }

 protected:
  void setMeter(float v) noexcept { meter_.store(v, std::memory_order_relaxed); }

 private:
  std::atomic<float> meter_{0.0f};
};

}  // namespace sq::engine::audio::builtin
