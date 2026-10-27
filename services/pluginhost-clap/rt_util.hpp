// pluginhost-clap: small realtime helpers (no locks, no allocation after construction).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace sqclap {

// Fixed-capacity single-producer / single-consumer ring (main thread -> audio thread).
template <class T>
class SpscRing {
 public:
  explicit SpscRing(uint32_t capacityPow2) : mask_(capacityPow2 - 1), buf_(new T[capacityPow2]) {}
  bool push(const T& v) noexcept {  // producer
    uint32_t w = w_.load(std::memory_order_relaxed);
    if (w - r_.load(std::memory_order_acquire) > mask_) return false;
    buf_[w & mask_] = v;
    w_.store(w + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& out) noexcept {  // consumer
    uint32_t r = r_.load(std::memory_order_relaxed);
    if (r == w_.load(std::memory_order_acquire)) return false;
    out = buf_[r & mask_];
    r_.store(r + 1, std::memory_order_release);
    return true;
  }
  bool empty() const noexcept { return r_.load(std::memory_order_acquire) == w_.load(std::memory_order_acquire); }

 private:
  const uint32_t mask_;
  std::unique_ptr<T[]> buf_;
  std::atomic<uint32_t> w_{0};
  std::atomic<uint32_t> r_{0};
};

}  // namespace sqclap
