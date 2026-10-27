// Squizzy Studio engine/audio - AudioRenderThread (priority + MMCSS setup).
#include "sq/engine/audio/render_thread.hpp"

#include <string>

#include "sq/platform.h"
#include "sq/engine/audio/rt.hpp"

#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#endif

namespace sq::engine::audio {

bool AudioRenderThread::start(std::function<void(const std::atomic<bool>& stop)> body, const Options& opt) {
  if (thread_.joinable()) return false;
  stop_.store(false);
  started_.store(false);
  prio_.store(false);
  mmcss_.store(false);
  std::string name = opt.name ? opt.name : "AudioRenderThread";
  const bool wantPrio = opt.realtimePriority;
  const bool wantMmcss = opt.mmcss;
  thread_ = std::thread([this, body = std::move(body), name = std::move(name), wantPrio, wantMmcss]() {
    sq_thread_set_current_name(name.c_str());
    if (wantPrio) prio_.store(sq_thread_set_current_priority(SQ_PRIORITY_REALTIME) == SQ_OK);
#ifdef _WIN32
    HANDLE task = nullptr;
    if (wantMmcss) {
      DWORD taskIndex = 0;
      task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
      if (task) {
        AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
        mmcss_.store(true);
      }
    }
#else
    (void)wantMmcss;
#endif
    started_.store(true);
    {
      rt::Scope scope;
      body(stop_);
    }
#ifdef _WIN32
    if (task) AvRevertMmThreadCharacteristics(task);
#endif
  });
  return true;
}

void AudioRenderThread::stop() {
  if (!thread_.joinable()) return;
  stop_.store(true);
  thread_.join();
}

}  // namespace sq::engine::audio
