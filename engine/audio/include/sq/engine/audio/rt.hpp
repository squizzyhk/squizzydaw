// Squizzy Studio engine/audio - realtime primitives (lock-free queues, delay lines, RT markers).
//
// Everything marked "RT" here is wait-free / allocation-free and may run on the audio thread.
// Construction / resizing is not RT safe and happens on the control thread.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace sq::engine::audio {

// ---- RT context marker ----------------------------------------------------------------------
// The engine wraps every realtime entry point (device callback, offline render block) in an
// rt::Scope. Debug tooling and tests (allocation hooks) ask inRealtimeContext() to find
// violations of CONTRACTS section 8.
namespace rt {
bool inRealtimeContext() noexcept;
class Scope {
 public:
  Scope() noexcept;
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool prev_;
};
// Temporarily leaves the RT context (only for test fixtures that must allocate on purpose).
class Unscope {
 public:
  Unscope() noexcept;
  ~Unscope();
  Unscope(const Unscope&) = delete;
  Unscope& operator=(const Unscope&) = delete;

 private:
  bool prev_;
};
}  // namespace rt

inline constexpr size_t kCacheLine = 64;

inline size_t nextPow2(size_t v) {
  size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

// Single-producer / single-consumer bounded queue of trivially copyable items.
template <class T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue items must be trivially copyable");

 public:
  explicit SpscQueue(size_t capacity = 256) : buf_(nextPow2(capacity < 2 ? 2 : capacity)), mask_(buf_.size() - 1) {}
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // RT (producer side). false when full.
  bool push(const T& v) noexcept {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= buf_.size()) return false;
    buf_[h & mask_] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  // RT (consumer side). false when empty.
  bool pop(T& out) noexcept {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
    out = buf_[t & mask_];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  size_t sizeApprox() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  size_t capacity() const noexcept { return buf_.size(); }
  size_t freeApprox() const noexcept { return capacity() - sizeApprox(); }

 private:
  std::vector<T> buf_;
  size_t mask_;
  char pad0_[kCacheLine]{};
  std::atomic<size_t> head_{0};
  char pad1_[kCacheLine]{};
  std::atomic<size_t> tail_{0};
  char pad2_[kCacheLine]{};
};

// SPSC ring of interleaved float frames (record path: RT producer -> disk writer consumer).
class FrameRing {
 public:
  FrameRing(uint32_t channels, size_t frames)
      : channels_(channels ? channels : 1), frames_(nextPow2(frames < 64 ? 64 : frames)), data_(frames_ * channels_) {}
  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;

  uint32_t channels() const noexcept { return channels_; }
  size_t capacityFrames() const noexcept { return frames_; }

  // RT producer: writes `frames` frames from non-interleaved `src` (src[c] may be null = silence).
  // All-or-nothing: false (nothing written) if there is not enough room.
  bool write(const float* const* src, uint32_t srcChannels, uint32_t frames) noexcept {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    if (frames_ - (h - t) < frames) return false;
    for (uint32_t i = 0; i < frames; ++i) {
      float* dst = &data_[((h + i) & (frames_ - 1)) * channels_];
      for (uint32_t c = 0; c < channels_; ++c) dst[c] = (c < srcChannels && src[c]) ? src[c][i] : 0.0f;
    }
    head_.store(h + frames, std::memory_order_release);
    return true;
  }
  // Consumer: reads up to maxFrames interleaved frames. Returns frames read.
  size_t read(float* interleaved, size_t maxFrames) noexcept {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    size_t n = h - t;
    if (n > maxFrames) n = maxFrames;
    for (size_t i = 0; i < n; ++i)
      std::memcpy(interleaved + i * channels_, &data_[((t + i) & (frames_ - 1)) * channels_], sizeof(float) * channels_);
    tail_.store(t + n, std::memory_order_release);
    return n;
  }
  size_t availableFrames() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }

 private:
  uint32_t channels_;
  size_t frames_;
  std::vector<float> data_;
  char pad0_[kCacheLine]{};
  std::atomic<size_t> head_{0};
  char pad1_[kCacheLine]{};
  std::atomic<size_t> tail_{0};
};

// Multi-channel integer delay line. prepare() allocates (control thread); process() is RT.
class DelayLine {
 public:
  void prepare(uint32_t channels, uint32_t delayFrames, uint32_t maxBlock) {
    channels_ = channels;
    delay_ = delayFrames;
    size_ = delayFrames ? nextPow2(static_cast<size_t>(delayFrames) + maxBlock + 1) : 0;
    buf_.assign(static_cast<size_t>(channels) * size_, 0.0f);
    write_ = 0;
  }
  uint32_t delay() const noexcept { return delay_; }
  void reset() noexcept {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    write_ = 0;
  }
  // RT: in-place delay of io[0..channels) by delay() frames.
  void process(float* const* io, uint32_t channels, uint32_t frames) noexcept {
    if (delay_ == 0 || size_ == 0) return;
    const size_t mask = size_ - 1;
    const uint32_t nch = channels < channels_ ? channels : channels_;
    for (uint32_t c = 0; c < nch; ++c) {
      float* line = &buf_[c * size_];
      float* x = io[c];
      size_t w = write_;
      for (uint32_t i = 0; i < frames; ++i) {
        line[w & mask] = x[i];
        x[i] = line[(w - delay_) & mask];
        ++w;
      }
    }
    write_ += frames;
  }

 private:
  uint32_t channels_ = 0;
  uint32_t delay_ = 0;
  size_t size_ = 0;
  size_t write_ = 0;
  std::vector<float> buf_;
};

// Fixed-capacity plain array of pointers for building channel pointer lists on the stack.
inline constexpr uint32_t kMaxChannels = 64;

}  // namespace sq::engine::audio
