// midi-win: realtime-safe building blocks (no locks, no allocation after construction).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "sq/platform.h"

namespace sq::midiwin {

// Bounded multi-producer / multi-consumer queue (sequence-numbered ring cells).
// push/pop are lock-free and never allocate. Capacity must be a power of two.
template <class T>
class MpmcQueue {
 public:
  explicit MpmcQueue(size_t capacityPow2) : cells_(new Cell[capacityPow2]), mask_(capacityPow2 - 1) {
    for (size_t i = 0; i < capacityPow2; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  MpmcQueue(const MpmcQueue&) = delete;
  MpmcQueue& operator=(const MpmcQueue&) = delete;

  bool push(const T& v) noexcept {
    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const size_t seq = c.seq.load(std::memory_order_acquire);
      const intptr_t dif = intptr_t(seq) - intptr_t(pos);
      if (dif == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.value = v;
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;  // full
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  bool pop(T& v) noexcept {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const size_t seq = c.seq.load(std::memory_order_acquire);
      const intptr_t dif = intptr_t(seq) - intptr_t(pos + 1);
      if (dif == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          v = c.value;
          c.seq.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;  // empty
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

  size_t capacity() const noexcept { return mask_ + 1; }

 private:
  struct Cell {
    std::atomic<size_t> seq{0};
    T value{};
  };
  std::unique_ptr<Cell[]> cells_;
  size_t mask_;
  char pad0_[64] = {};
  std::atomic<size_t> head_{0};
  char pad1_[64] = {};
  std::atomic<size_t> tail_{0};
};

// Wakes a worker thread from any thread, including realtime ones. Windows: an auto-reset kernel
// event (SetEvent never blocks). Elsewhere: an atomic flag the worker polls at 1 ms.
class Waker {
 public:
  Waker() {
#ifdef _WIN32
    h_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif
  }
  ~Waker() {
#ifdef _WIN32
    if (h_) CloseHandle(h_);
#endif
  }
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;

  bool ok() const {
#ifdef _WIN32
    return h_ != nullptr;
#else
    return true;
#endif
  }

  void wake() noexcept {
#ifdef _WIN32
    SetEvent(h_);
#else
    flag_.store(true, std::memory_order_release);
#endif
  }

  // Worker side; timeoutMs < 0 = until woken.
  void wait(int32_t timeoutMs) noexcept {
#ifdef _WIN32
    WaitForSingleObject(h_, timeoutMs < 0 ? INFINITE : DWORD(timeoutMs));
#else
    const uint64_t until = timeoutMs < 0 ? UINT64_MAX : sq_time_monotonic_ns() + uint64_t(timeoutMs) * 1000000ull;
    while (!flag_.exchange(false, std::memory_order_acq_rel)) {
      if (sq_time_monotonic_ns() >= until) return;
      sq_thread_sleep_ms(1);
    }
#endif
  }

 private:
#ifdef _WIN32
  HANDLE h_ = nullptr;
#else
  std::atomic<bool> flag_{false};
#endif
};

// Hands driver buffers back to the driver from a worker thread, so a driver callback never calls
// a driver API itself (WinMM forbids multimedia calls inside MidiInProc: it can deadlock).
// post() is wait-free (lock-free push + SetEvent) and may be called from the driver callback;
// the worker calls readd(user, index) for each posted index. After shutdown() returns, readd is
// not running and will never run again, whatever post() calls race with it.
class BufferRecycler {
 public:
  using Readd = void (*)(void* user, uint32_t index);
  BufferRecycler(Readd readd, void* user) : readd_(readd), user_(user) {}
  ~BufferRecycler() { shutdown(); }
  BufferRecycler(const BufferRecycler&) = delete;
  BufferRecycler& operator=(const BufferRecycler&) = delete;

  bool start() {
    if (thread_.joinable() || closing_.load()) return false;
    if (!waker_.ok()) return false;
    thread_ = std::thread([this] { run(); });
    return true;
  }
  void post(uint32_t index) noexcept {
    if (closing_.load(std::memory_order_acquire)) return;
    if (queue_.push(index)) waker_.wake();
  }
  void shutdown() {
    closing_.store(true, std::memory_order_seq_cst);
    waker_.wake();
    if (thread_.joinable()) thread_.join();
  }
  bool closing() const noexcept { return closing_.load(std::memory_order_acquire); }
  uint64_t readdCount() const noexcept { return readds_.load(std::memory_order_acquire); }

 private:
  void run() {
    for (;;) {
      waker_.wait(50);
      uint32_t i = 0;
      while (!closing_.load(std::memory_order_seq_cst) && queue_.pop(i)) {
        readd_(user_, i);
        readds_.fetch_add(1, std::memory_order_acq_rel);
      }
      if (closing_.load(std::memory_order_seq_cst)) return;
    }
  }

  Readd readd_;
  void* user_;
  MpmcQueue<uint32_t> queue_{64};
  Waker waker_;
  std::atomic<bool> closing_{false};
  std::atomic<uint64_t> readds_{0};
  std::thread thread_;
};

// Raises the system timer resolution to 1 ms for the lifetime of the object (Windows).
class TimerResolution {
 public:
  TimerResolution();
  ~TimerResolution();
  TimerResolution(const TimerResolution&) = delete;
  TimerResolution& operator=(const TimerResolution&) = delete;
};

// Length of a short MIDI message from its status byte (0 for sysex/invalid).
inline uint32_t shortMessageLength(uint8_t status) noexcept {
  if (status < 0x80u) return 0;
  if (status < 0xF0u) {
    const uint8_t hi = status & 0xF0u;
    return (hi == 0xC0u || hi == 0xD0u) ? 2u : 3u;
  }
  switch (status) {
    case 0xF1:
    case 0xF3:
      return 2;
    case 0xF2:
      return 3;
    case 0xF0:
    case 0xF7:
      return 0;
    default:
      return 1;  // F6, F8..FF (F4/F5/F9/FD are undefined but single bytes)
  }
}

}  // namespace sq::midiwin
