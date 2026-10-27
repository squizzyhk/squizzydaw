// midi-win: hot-plug support.
//
//   diffPortIds()   pure snapshot comparison (unit-tested without any device).
//   HotplugWatcher  a thread that re-enumerates the ports when Windows reports a device-interface
//                   arrival/removal (CM_Register_Notification) and, as a fallback for drivers that
//                   never announce themselves, every `pollMs` milliseconds. The re-enumeration is a
//                   caller-supplied function (MidiDriver::refreshPorts), which compares the fresh
//                   list against the previous one and notifies IMidiDeviceListener only on change.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "sq/platform.h"

namespace sq::midiwin {

struct PortDiff {
  std::vector<std::string> added;    // ids present now, absent before (sorted)
  std::vector<std::string> removed;  // ids present before, absent now (sorted)
  bool empty() const noexcept { return added.empty() && removed.empty(); }
};

// Set difference both ways. Duplicate ids are treated as one.
inline PortDiff diffPortIds(std::vector<std::string> before, std::vector<std::string> after) {
  std::sort(before.begin(), before.end());
  before.erase(std::unique(before.begin(), before.end()), before.end());
  std::sort(after.begin(), after.end());
  after.erase(std::unique(after.begin(), after.end()), after.end());
  PortDiff d;
  std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(d.added));
  std::set_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(d.removed));
  return d;
}

class HotplugWatcher {
 public:
  using RefreshFn = void (*)(void* user);
  // A device notification is followed by this settle delay before re-enumerating: drivers publish
  // their WinMM endpoints a little after the PnP interface arrives, and bursts collapse into one.
  static constexpr uint32_t kSettleMs = 150;

  HotplugWatcher(RefreshFn fn, void* user) : fn_(fn), user_(user) {}
  ~HotplugWatcher() { stop(); }
  HotplugWatcher(const HotplugWatcher&) = delete;
  HotplugWatcher& operator=(const HotplugWatcher&) = delete;

  // pollMs: fallback polling interval (0 = notifications only). Main thread.
  sq_result start(uint32_t pollMs);
  // Joins the thread; no refresh runs after this returns. Main thread; idempotent.
  void stop();
  bool running() const noexcept { return thread_ != nullptr; }
  // True when OS device notifications are active (Windows 8+), false when only polling.
  bool osNotifications() const noexcept { return notify_ != nullptr; }
  uint64_t wakeCount() const noexcept { return wakes_.load(std::memory_order_relaxed); }

  // Any thread (the OS notification callback uses it): schedule a re-enumeration.
  void signal() noexcept;

 private:
  static void threadMain(void* self);
  void run();

  RefreshFn fn_;
  void* user_;
  uint32_t pollMs_ = 0;
  sq_event* event_ = nullptr;
  sq_thread* thread_ = nullptr;
  void* notify_ = nullptr;  // HCMNOTIFICATION on Windows
  std::atomic<bool> quit_{false};
  std::atomic<bool> pending_{false};
  std::atomic<uint64_t> wakes_{0};
};

}  // namespace sq::midiwin
