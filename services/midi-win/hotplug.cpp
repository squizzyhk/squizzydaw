// midi-win: hot-plug watcher thread (see hotplug.hpp).
#include "hotplug.hpp"

#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#endif

namespace sq::midiwin {

#ifdef _WIN32
namespace {
// Called on a system thread-pool thread for every device-interface arrival/removal. Only wakes
// the watcher (the enumeration itself never runs on the notification thread).
DWORD CALLBACK onDeviceNotification(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
  if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
    static_cast<HotplugWatcher*>(context)->signal();
  return ERROR_SUCCESS;
}
}  // namespace
#endif

sq_result HotplugWatcher::start(uint32_t pollMs) {
  if (thread_) return SQ_S_FALSE;
  pollMs_ = pollMs;
  quit_.store(false, std::memory_order_seq_cst);
  pending_.store(false, std::memory_order_seq_cst);
  if (!event_) {
    sq_result r = sq_event_create(SQ_FALSE, SQ_FALSE, &event_);
    if (SQ_FAILED(r)) return r;
  }
  sq_result r = sq_thread_create(&HotplugWatcher::threadMain, this, "sq-midi-hotplug", &thread_);
  if (SQ_FAILED(r)) {
    thread_ = nullptr;
    return r;
  }
#ifdef _WIN32
  CM_NOTIFY_FILTER filter{};
  filter.cbSize = sizeof filter;
  filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
  filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  HCMNOTIFICATION h = nullptr;
  if (CM_Register_Notification(&filter, this, &onDeviceNotification, &h) == CR_SUCCESS) notify_ = h;
#endif
  return SQ_OK;
}

void HotplugWatcher::stop() {
#ifdef _WIN32
  // Unregistering waits for callbacks in progress, so none can touch `this` afterwards.
  if (notify_) CM_Unregister_Notification(static_cast<HCMNOTIFICATION>(notify_));
#endif
  notify_ = nullptr;
  if (thread_) {
    quit_.store(true, std::memory_order_seq_cst);
    sq_event_set(event_);
    sq_thread_join(thread_);
    thread_ = nullptr;
  }
  if (event_) {
    sq_event_destroy(event_);
    event_ = nullptr;
  }
}

void HotplugWatcher::signal() noexcept {
  pending_.store(true, std::memory_order_release);
  if (event_) sq_event_set(event_);
}

void HotplugWatcher::threadMain(void* self) { static_cast<HotplugWatcher*>(self)->run(); }

void HotplugWatcher::run() {
  for (;;) {
    sq_event_wait(event_, pollMs_ ? int32_t(pollMs_) : -1);
    if (quit_.load(std::memory_order_acquire)) return;
    if (pending_.load(std::memory_order_acquire)) {
      // Let the burst settle; further signals during the delay collapse into this refresh.
      const uint64_t until = sq_time_monotonic_ns() + uint64_t(kSettleMs) * 1000000ull;
      for (uint64_t now = sq_time_monotonic_ns(); now < until; now = sq_time_monotonic_ns()) {
        sq_event_wait(event_, int32_t((until - now) / 1000000ull) + 1);
        if (quit_.load(std::memory_order_acquire)) return;
      }
      pending_.store(false, std::memory_order_release);
    }
    wakes_.fetch_add(1, std::memory_order_relaxed);
    fn_(user_);
  }
}

}  // namespace sq::midiwin
