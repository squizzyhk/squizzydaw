// midi-win: IMidiDriver (category "MidiDriver") over WinMM devices plus built-in loopback cables,
// and the module definition.
//
// Port ids (stable, persisted in Devices.Midi.*):
//   loopback:in:<k> / loopback:out:<k>    k = 0..3: what is sent to out:k arrives at every in:k
//   winmm:in:<name>#<n> / winmm:out:<name>#<n>   n = occurrence of that product name (1-based),
//                                                so ids survive reordering of unrelated devices
// Hot-plug: WinMM itself has no notification. While at least one IMidiDeviceListener is
// registered, a HotplugWatcher thread re-enumerates on Windows device-interface arrival/removal
// notifications (CM_Register_Notification) and every Services.midi-win.HotplugPollMs ms (default
// 1000, 0 = notifications only) as a fallback. refreshPorts() compares the fresh list with the
// previous one and calls onPortsChanged() (on the watcher or the caller's thread) only on change.
//
// Settings (read at enumeration / when the first listener is added):
//   Services.midi-win.LoopbackPorts   number of loopback cables shown (0..16, default 4)
//   Services.midi-win.HotplugPollMs   fallback poll interval in ms (default 1000, 0 = off, min 20)
#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

#include "hotplug.hpp"
#include "ports.hpp"

namespace sq::midiwin {
namespace {

constexpr uint32_t kDefaultLoopbackShown = 4;
constexpr uint32_t kDefaultPollMs = 1000;

int64_t settingInt(const char* key, int64_t def) {
  IHostContext* h = host();
  return h ? h->getSettings()->getInt(key, def) : def;
}

uint32_t loopbackShown() {
  int64_t n = settingInt("Services.midi-win.LoopbackPorts", kDefaultLoopbackShown);
  return uint32_t(std::clamp<int64_t>(n, 0, kMaxLoopbackPorts));
}

struct PortEntry {
  sq_midi_port_info info{};
  bool winmm = false;
  uint32_t index = 0;  // winmm device index / loopback cable
};

std::vector<PortEntry> enumeratePorts() {
  std::vector<PortEntry> out;
  auto add = [&](const std::string& id, const std::string& name, uint32_t flags, bool winmm, uint32_t index) {
    PortEntry e;
    e.info.struct_size = sizeof e.info;
    copyCString(e.info.id, sizeof e.info.id, id);
    copyCString(e.info.name, sizeof e.info.name, name);
    e.info.flags = flags;
    e.winmm = winmm;
    e.index = index;
    out.push_back(e);
  };
  std::vector<WinmmDevice> ins, outs;
  winmmEnumerate(ins, outs);
  auto addDevices = [&](const std::vector<WinmmDevice>& devs, bool input) {
    std::vector<std::string> seen;
    for (const auto& d : devs) {
      seen.push_back(d.name);
      auto n = std::count(seen.begin(), seen.end(), d.name);
      add(std::string(input ? "winmm:in:" : "winmm:out:") + d.name + "#" + std::to_string(n), d.name,
          input ? SQ_MIDI_PORT_INPUT : SQ_MIDI_PORT_OUTPUT, true, d.index);
    }
  };
  addDevices(ins, true);
  addDevices(outs, false);
  const uint32_t shown = loopbackShown();
  for (uint32_t k = 0; k < shown; ++k) {
    add("loopback:in:" + std::to_string(k), "Loopback " + std::to_string(k + 1) + " In", SQ_MIDI_PORT_INPUT | SQ_MIDI_PORT_VIRTUAL, false, k);
    add("loopback:out:" + std::to_string(k), "Loopback " + std::to_string(k + 1) + " Out", SQ_MIDI_PORT_OUTPUT | SQ_MIDI_PORT_VIRTUAL, false, k);
  }
  return out;
}

class MidiDriver final : public Object<MidiDriver, IMidiDriver> {
 public:
  // Enumeration is lazy: WinMM device enumeration can take seconds on some systems.
  MidiDriver() = default;
  ~MidiDriver() override { watcher_.stop(); }

  sq_result SQ_CALL refreshPorts() override {
    bool changed;
    std::vector<Ptr<IMidiDeviceListener>> ls;
    {
      // Serialize enumerate+commit: otherwise a slow, older enumeration (watcher vs. a manual
      // refresh) could commit after a newer one and leave a stale port list until the next poll.
      // Listeners are called outside this lock, so they may call refreshPorts() themselves.
      std::lock_guard<std::mutex> rk(refreshMu_);
      auto fresh = enumeratePorts();
      std::lock_guard<std::mutex> lk(mu_);
      changed = fresh.size() != ports_.size() ||
                !std::equal(fresh.begin(), fresh.end(), ports_.begin(), [](const PortEntry& a, const PortEntry& b) {
                  return std::strcmp(a.info.id, b.info.id) == 0 && a.index == b.index;
                });
      ports_ = std::move(fresh);
      enumerated_ = true;
      ls = listeners_;
    }
    if (changed)
      for (auto& l : ls) l->onPortsChanged();
    return SQ_OK;
  }
  uint32_t SQ_CALL getPortCount() override {
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<uint32_t>(ports_.size());
  }
  sq_result SQ_CALL getPortInfo(uint32_t index, sq_midi_port_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    if (index >= ports_.size()) return SQ_E_NOTFOUND;
    *out = ports_[index].info;
    return SQ_OK;
  }
  sq_result SQ_CALL openInput(const char* portId, IMidiInputCallback* cb, IMidiInputPort** out) override {
    if (!portId || !cb || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    PortEntry e;
    if (!find(portId, SQ_MIDI_PORT_INPUT, e)) return SQ_E_NOTFOUND;
    if (!e.winmm) return openLoopInput(e.info, cb, e.index, out);
    return openWinmmInput(e.info, e.index, cb, out);
  }
  sq_result SQ_CALL openOutput(const char* portId, IMidiOutputPort** out) override {
    if (!portId || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    PortEntry e;
    if (!find(portId, SQ_MIDI_PORT_OUTPUT, e)) return SQ_E_NOTFOUND;
    std::unique_ptr<OutputSink> sink;
    if (e.winmm) {
      sq_result r = openWinmmOutputSink(e.index, sink);
      if (SQ_FAILED(r)) return r;
    } else {
      sink = makeLoopSink(e.index);
    }
    auto port = Ptr<OutputPort>::adopt(new OutputPort(e.info, std::move(sink)));
    sq_result r = port->start();
    if (SQ_FAILED(r)) return r;
    *out = port.detach();
    return SQ_OK;
  }
  sq_result SQ_CALL addListener(IMidiDeviceListener* l) override {
    if (!l) return SQ_E_INVALIDARG;
    // Take the baseline before watching, so the first watcher pass only reports real changes.
    ensure();
    bool first;
    {
      std::lock_guard<std::mutex> lk(mu_);
      first = listeners_.empty();
      listeners_.push_back(Ptr<IMidiDeviceListener>(l));
    }
    if (first && !watcher_.running() && !onWatcherThread()) {
      int64_t poll = settingInt("Services.midi-win.HotplugPollMs", kDefaultPollMs);
      if (poll < 0) poll = 0;
      if (poll > 0 && poll < 20) poll = 20;
      sq_result r = watcher_.start(uint32_t(std::min<int64_t>(poll, 3600000)));
      if (SQ_FAILED(r)) sq::logf(SQ_LOG_WARN, "hot-plug watcher not started (%d); call refreshPorts() manually", r);
    }
    return SQ_OK;
  }
  sq_result SQ_CALL removeListener(IMidiDeviceListener* l) override {
    bool last = false, found = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->get() == l) {
          listeners_.erase(it);
          found = true;
          last = listeners_.empty();
          break;
        }
    }
    if (!found) return SQ_E_NOTFOUND;
    // The watcher cannot join itself (a listener removing itself from inside onPortsChanged):
    // it then keeps running idle until the next add/remove or the driver's destruction.
    if (last && !onWatcherThread()) {
      watcher_.stop();
      // The joined thread's id may be reused by an unrelated thread, which would then be
      // mistaken for the watcher (and never start it again in addListener).
      watcherThread_.store(std::thread::id{}, std::memory_order_release);
    }
    return SQ_OK;
  }

 private:
  static void watcherRefresh(void* self) {
    auto* d = static_cast<MidiDriver*>(self);
    d->watcherThread_.store(std::this_thread::get_id(), std::memory_order_release);
    d->refreshPorts();
  }
  bool onWatcherThread() const { return watcherThread_.load(std::memory_order_acquire) == std::this_thread::get_id(); }

  void ensure() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (enumerated_) return;
    }
    refreshPorts();
  }
  bool find(const char* id, uint32_t dir, PortEntry& out) {
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : ports_)
      if ((p.info.flags & dir) && std::strcmp(p.info.id, id) == 0) {
        out = p;
        return true;
      }
    return false;
  }
  std::mutex refreshMu_;  // taken before mu_, never while calling listeners
  std::mutex mu_;
  std::vector<PortEntry> ports_;
  bool enumerated_ = false;
  std::vector<Ptr<IMidiDeviceListener>> listeners_;
  std::atomic<std::thread::id> watcherThread_{};
  HotplugWatcher watcher_{&MidiDriver::watcherRefresh, this};  // last: stopped first in ~MidiDriver
};

constexpr ClassEntry kClasses[] = {
    classEntry<MidiDriver>(sq::midi::kWinMidiDriverClassId, sq::midi::kDriverCategory, "Windows MIDI Driver"),
};

constexpr ModuleDesc kModule{
    .fourcc = SQ_FOURCC('M', 'I', 'D', 'W'),
    .name = "midi-win",
    .description = "Windows MIDI (WinMM) ports + loopback cables",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace
}  // namespace sq::midiwin

SQ_MODULE_EXPORTS(sq::midiwin::kModule)
