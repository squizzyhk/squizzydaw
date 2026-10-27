// Squizzy Studio engine/audio - AudioRenderThread: the dedicated realtime render thread.
//
// Explicit priority setup on the new thread, in this order:
//   1. sq_thread_set_current_priority(SQ_PRIORITY_REALTIME) (platform layer),
//   2. Windows: MMCSS registration with the "Pro Audio" task + AVRT_PRIORITY_HIGH (avrt.dll),
//   3. the body runs inside an rt::Scope (allocation hooks / RT assertions see it as RT).
// The registration is undone before the thread exits. Used by the in-process NullDevice (a
// device clock for tests and headless runs); real drivers own their RT threads and do their own
// MMCSS setup (CONTRACTS section 8.5).
#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace sq::engine::audio {

class AudioRenderThread {
 public:
  struct Options {
    const char* name = "AudioRenderThread";
    bool realtimePriority = true;
    bool mmcss = true;
  };
  AudioRenderThread() = default;
  ~AudioRenderThread() { stop(); }
  AudioRenderThread(const AudioRenderThread&) = delete;
  AudioRenderThread& operator=(const AudioRenderThread&) = delete;

  // body(stopFlag) loops until stopFlag becomes true. Returns false if already running.
  bool start(std::function<void(const std::atomic<bool>& stop)> body, const Options& opt);
  bool start(std::function<void(const std::atomic<bool>& stop)> body) { return start(std::move(body), Options{}); }
  void stop();
  bool running() const noexcept { return thread_.joinable(); }

  // Results of the priority setup (valid once the body started running).
  bool priorityApplied() const noexcept { return prio_.load(); }
  bool mmcssApplied() const noexcept { return mmcss_.load(); }
  bool started() const noexcept { return started_.load(); }

 private:
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> prio_{false};
  std::atomic<bool> mmcss_{false};
  std::atomic<bool> started_{false};
};

}  // namespace sq::engine::audio
