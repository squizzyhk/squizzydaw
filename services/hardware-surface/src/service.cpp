// hardware-surface: IControlSurfaceService (remote.h) for MIDI control surfaces, plus the module.
//
// Data flow
//   MIDI driver thread  -> InputSink::onMidi       copies short messages into a lock-free ring
//   service worker      -> Matcher                  control events; listeners' onControlChanged;
//                                                   page actions; queues (coalesced) target writes
//   main thread pump()  -> object:// targets        IController::setProperty / getProperty
//                       -> IMidiOutputPort::send    feedback when a bound target changed
//                       -> reconnect                after IMidiDeviceListener::onPortsChanged
// Locks: mu_ guards the surface list and everything inside a Surface; lmu_ guards listeners.
// No foreign object is called while mu_ is held except MIDI driver/port objects (open/close/send)
// and immutable remote-page snapshots; listeners and object:// targets are always called unlocked.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "matcher.hpp"
#include "port_diff.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/midi.h"
#include "sq/interfaces/remote.h"
#include "sq/interfaces/scripting.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"
#include "sq/services/hardware_surface/hardware_surface.h"
#include "surface_map.hpp"

namespace sq::hwsurface {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kFeedbackEpsilon = 1e-4;

sq_result copyOut(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed) {
  const uint32_t n = uint32_t(s.size() + 1);
  if (needed) *needed = n;
  if (!buf || bufSize < n) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, s.c_str(), n);
  return SQ_OK;
}

void copyField(char* dst, size_t cap, const std::string& s) {
  size_t n = std::min(s.size(), cap - 1);
  while (n > 0 && n < s.size() && (uint8_t(s[n]) & 0xC0u) == 0x80u) --n;  // keep UTF-8 whole
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

// ---- lock-free plumbing between the MIDI driver thread and the worker -----------------------

// Bounded MPMC ring (sequence-numbered cells). push/pop never block or allocate.
template <class T>
class Ring {
 public:
  explicit Ring(size_t pow2) : cells_(new Cell[pow2]), mask_(pow2 - 1) {
    for (size_t i = 0; i < pow2; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  bool push(const T& v) noexcept {
    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const intptr_t dif = intptr_t(c.seq.load(std::memory_order_acquire)) - intptr_t(pos);
      if (dif == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.value = v;
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }
  bool pop(T& v) noexcept {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const intptr_t dif = intptr_t(c.seq.load(std::memory_order_acquire)) - intptr_t(pos + 1);
      if (dif == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          v = c.value;
          c.seq.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

 private:
  struct Cell {
    std::atomic<size_t> seq{0};
    T value{};
  };
  std::unique_ptr<Cell[]> cells_;
  size_t mask_;
  char pad0_[64] = {};  // keep producer and consumer indices on separate cache lines
  std::atomic<size_t> head_{0};
  char pad1_[64] = {};
  std::atomic<size_t> tail_{0};
};

// Auto-reset event shared by every sink and the worker (sq_event_set never blocks).
class Wake {
 public:
  Wake() { sq_event_create(SQ_FALSE, SQ_FALSE, &e_); }
  ~Wake() {
    if (e_) sq_event_destroy(e_);
  }
  Wake(const Wake&) = delete;
  Wake& operator=(const Wake&) = delete;
  bool ok() const { return e_ != nullptr; }
  void set() noexcept {
    if (e_) sq_event_set(e_);
  }
  void wait(int32_t ms) noexcept {
    if (e_) sq_event_wait(e_, ms);
  }

 private:
  sq_event* e_ = nullptr;
};

struct RawMsg {
  uint8_t bytes[3] = {};
  uint8_t size = 0;
};

// The IMidiInputCallback of one surface. Driver thread: copy + wake, nothing else.
class InputSink final : public Object<InputSink, IMidiInputCallback> {
 public:
  explicit InputSink(std::shared_ptr<Wake> wake) : wake_(std::move(wake)) {}
  void SQ_CALL onMidi(const sq_midi_port_info*, const sq_midi_timed_event* ev, uint32_t n) override {
    bool any = false;
    for (uint32_t i = 0; i < n; ++i) {
      if (ev[i].data || ev[i].size < 1 || ev[i].size > 3) continue;  // surfaces map short messages only
      RawMsg m;
      std::memcpy(m.bytes, ev[i].bytes, ev[i].size);
      m.size = uint8_t(ev[i].size);
      if (ring_.push(m)) any = true;
      else dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    if (any) wake_->set();
  }
  bool pop(RawMsg& m) noexcept { return ring_.pop(m); }
  uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

 private:
  std::shared_ptr<Wake> wake_;
  Ring<RawMsg> ring_{1024};
  std::atomic<uint64_t> dropped_{0};
};

// ---- surfaces ---------------------------------------------------------------------------------

struct Binding {
  bool active = false;
  std::string target;
  std::string property;
  double min = 0.0;
  double max = 1.0;
};

struct Pending {
  bool any = false;
  bool absolute = false;
  double value = 0.0;  // absolute: normalized value; relative: summed delta
};

struct Surface {
  std::string id;
  std::string path;
  DeviceInfo dev;
  SurfaceMap map;
  Matcher matcher;
  std::string wantIn, wantOut;  // explicit port ids from the connection id ("" = match by name)
  std::string curIn, curOut;    // ids of the ports currently open
  Ptr<InputSink> sink;
  Ptr<IMidiInputPort> in;
  Ptr<IMidiOutputPort> out;
  std::vector<Binding> staticBindings;  // from <Bindings>
  std::vector<Binding> pageBindings;    // from the selected remote page (win over static ones)
  Ptr<IRemotePageSet> pages;
  uint32_t page = 0;
  std::vector<Pending> pending;
  std::vector<double> lastFeedback;  // normalized value last sent (or received), NaN = unknown
  std::vector<uint8_t> touched;      // absolute input since the last pump: do not echo back

  const Binding* binding(size_t i) const {
    if (pageBindings[i].active) return &pageBindings[i];
    if (staticBindings[i].active) return &staticBindings[i];
    return nullptr;
  }
  bool connected() const { return !curIn.empty() || !curOut.empty(); }
  void forgetFeedback() { std::fill(lastFeedback.begin(), lastFeedback.end(), kNaN); }
};
using SurfacePtr = std::shared_ptr<Surface>;

class Service;

// Forwards driver port-list changes to the service; detach() makes it inert (and waits out a
// callback in flight), so the driver may keep it a little longer without touching a dead service.
class DeviceListener final : public Object<DeviceListener, IMidiDeviceListener> {
 public:
  explicit DeviceListener(Service* s) : owner_(s) {}
  void SQ_CALL onPortsChanged() override;
  void detach() noexcept {
    owner_.store(nullptr, std::memory_order_seq_cst);
    while (inflight_.load(std::memory_order_seq_cst) != 0) sq_thread_yield();
  }

 private:
  std::atomic<Service*> owner_;
  std::atomic<int32_t> inflight_{0};
};

class Service final : public Object<Service, IControlSurfaceService, ISurfacePump> {
 public:
  Service() : wake_(std::make_shared<Wake>()) {}
  ~Service() override { shutdown(); }

  // ---- IControlSurfaceService ----
  uint32_t SQ_CALL getSurfaceCount() override {
    std::lock_guard<std::mutex> lk(mu_);
    return uint32_t(surfaces_.size());
  }

  sq_result SQ_CALL getSurfaceInfo(uint32_t index, sq_surface_info* out) override {
    if (!out || out->struct_size < sizeof(sq_surface_info)) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lk(mu_);
    if (index >= surfaces_.size()) return SQ_E_NOTFOUND;
    const Surface& s = *surfaces_[index];
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    copyField(out->id, sizeof out->id, s.id);
    copyField(out->name, sizeof out->name, s.dev.name);
    out->transport = SQ_SURFACE_MIDI_USB;
    out->flags = s.connected() ? SQ_SURFACE_CONNECTED : 0u;
    return SQ_OK;
  }

  sq_result SQ_CALL loadSurfaceMap(const char* path, const char* connectionId, char* idBuf, uint32_t bufSize,
                                   uint32_t* needed) override {
    if (!path || !*path) return SQ_E_INVALIDARG;
    try {
      auto s = std::make_shared<Surface>();
      Diagnostics diag;
      sq_result r = loadSurfaceFiles(path, s->dev, s->map, diag);
      for (const auto& w : diag.warnings) sq::logf(SQ_LOG_WARN, "%s: %s", path, w.c_str());
      if (SQ_FAILED(r)) {
        for (const auto& e : diag.errors) sq::logf(SQ_LOG_ERROR, "%s: %s", path, e.c_str());
        std::lock_guard<std::mutex> lk(mu_);
        lastError_ = diag.text();
        return r;
      }
      s->path = path;
      if (connectionId && *connectionId) {
        std::string c = connectionId;
        const size_t bar = c.find('|');
        s->wantIn = c.substr(0, bar);
        if (bar != std::string::npos) s->wantOut = c.substr(bar + 1);
        if (bar != std::string::npos && s->wantOut.empty()) s->wantOut = "-";  // "in|" = no output
        if (s->wantIn.empty() && bar != std::string::npos) s->wantIn = "-";    // "|out" = no input
      }
      const size_t n = s->map.controls.size();
      s->matcher.build(s->map);
      s->staticBindings.assign(n, Binding{});
      s->pageBindings.assign(n, Binding{});
      s->pending.assign(n, Pending{});
      s->lastFeedback.assign(n, kNaN);
      s->touched.assign(n, 0);
      for (const StaticBinding& b : s->map.bindings) {
        const int32_t i = s->map.find(b.control);
        if (i >= 0) s->staticBindings[size_t(i)] = Binding{true, b.target, b.property, b.min, b.max};
      }

      bool connected = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        s->id = uniqueIdLocked(s->dev.id);
        sq_result cr = copyOut(s->id, idBuf, bufSize, needed);
        if (SQ_FAILED(cr)) return cr;  // nothing is committed: the caller retries with a bigger buffer
        if (!ensureWorkerLocked()) return SQ_E_FAIL;
        if (ensureDriverLocked()) {
          std::vector<PortRef> ports = enumeratePortsLocked();
          relinkLocked(*s, ports);
        }
        surfaces_.push_back(s);
        lastError_.clear();
        connected = s->connected();
        sq::logf(SQ_LOG_INFO, "surface '%s' (%zu controls) from %s: %s%s%s", s->id.c_str(), n, path,
                 connected ? "connected" : "not connected", s->curIn.empty() ? "" : " in=", s->curIn.c_str());
      }
      notifySurfacesChanged();
      return connected ? SQ_OK : SQ_S_FALSE;
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL setRemotePages(const char* surfaceId, IRemotePageSet* pages) override {
    std::lock_guard<std::mutex> lk(mu_);
    Surface* s = findLocked(surfaceId);
    if (!s) return SQ_E_NOTFOUND;
    s->pages = Ptr<IRemotePageSet>(pages);
    if (!pages || pages->getPageCount() == 0) {
      for (auto& b : s->pageBindings) b = Binding{};
      s->page = 0;
      clearPendingLocked(*s);
      s->forgetFeedback();
      return SQ_OK;
    }
    return selectPageLocked(*s, 0);
  }

  sq_result SQ_CALL selectPage(const char* surfaceId, uint32_t pageIndex) override {
    std::lock_guard<std::mutex> lk(mu_);
    Surface* s = findLocked(surfaceId);
    if (!s) return SQ_E_NOTFOUND;
    return selectPageLocked(*s, pageIndex);
  }

  sq_result SQ_CALL setControlValue(const char* surfaceId, const char* controlId, double value, const char* text) override {
    if (!controlId) return SQ_E_INVALIDARG;
    Ptr<IMidiOutputPort> out;
    std::vector<uint8_t> sysex;
    sq_midi_timed_event ev[2]{};
    uint32_t count = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      Surface* s = findLocked(surfaceId);
      if (!s) return SQ_E_NOTFOUND;
      const int32_t idx = s->map.find(controlId);
      if (idx < 0) return SQ_E_NOTFOUND;
      const Control& c = s->map.controls[size_t(idx)];
      if (c.type == SQ_REMOTE_DISPLAY) {
        char fmt[32];
        if (!text) std::snprintf(fmt, sizeof fmt, "%.2f", std::isfinite(value) ? value : 0.0);
        sysex = encodeDisplay(c, text ? text : fmt);
      } else {
        if (!std::isfinite(value)) return SQ_E_INVALIDARG;
        value = std::clamp(value, 0.0, 1.0);
        count = encodeFeedback(c, value, ev);
        if (!count) return SQ_E_UNSUPPORTED;
        if (c.flags & SQ_REMOTE_TOGGLE) s->matcher.setToggleState(uint32_t(idx), value >= 0.5);
        s->lastFeedback[size_t(idx)] = value;
      }
      out = s->out;
    }
    if (!out) return SQ_E_CLOSED;
    if (!sysex.empty()) {
      sq_midi_timed_event e{};
      e.data = sysex.data();
      e.size = uint32_t(sysex.size());
      return out->send(&e, 1);
    }
    return out->send(ev, count);
  }

  sq_result SQ_CALL addListener(IControlSurfaceListener* l) override {
    if (!l) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lk(lmu_);
    for (auto& x : listeners_)
      if (x.get() == l) return SQ_S_FALSE;
    listeners_.push_back(Ptr<IControlSurfaceListener>(l));
    return SQ_OK;
  }
  sq_result SQ_CALL removeListener(IControlSurfaceListener* l) override {
    std::lock_guard<std::mutex> lk(lmu_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
      if (it->get() == l) {
        listeners_.erase(it);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }

  // ---- ISurfacePump ----
  sq_result SQ_CALL pump() override {
    try {
      bool did = false;
      bool changed = false;
      if (portsDirty_.exchange(false, std::memory_order_acq_rel)) changed |= reconcile();
      if (changed) {
        did = true;
        notifySurfacesChanged();
      }
      did |= applyTargets();
      return did ? SQ_OK : SQ_S_FALSE;
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL getLastError(char* buf, uint32_t bufSize, uint32_t* needed) override {
    std::lock_guard<std::mutex> lk(mu_);
    return copyOut(lastError_, buf, bufSize, needed);
  }

  // ---- internal ----
  void portsChanged() noexcept {
    portsDirty_.store(true, std::memory_order_release);
    wake_->set();
  }

  // Idempotent: stops the worker, detaches from the driver, closes every port.
  void shutdown() {
    Ptr<IMidiDriver> driver;
    Ptr<DeviceListener> dl;
    {
      std::lock_guard<std::mutex> lk(mu_);
      driver = std::move(driver_);
      dl = std::move(devListener_);
    }
    if (dl) dl->detach();
    if (driver && dl) driver->removeListener(dl.get());
    if (worker_) {
      quit_.store(true, std::memory_order_seq_cst);
      wake_->set();
      sq_thread_join(worker_);
      worker_ = nullptr;
    }
    std::vector<SurfacePtr> gone;
    {
      std::lock_guard<std::mutex> lk(mu_);
      gone.swap(surfaces_);
    }
    for (auto& s : gone) closePorts(*s);
    gone.clear();
    std::lock_guard<std::mutex> lk(lmu_);
    listeners_.clear();
  }

 private:
  struct Note {
    std::string surface;
    std::string control;
    double value;
  };

  // ---- worker ----
  static void workerMain(void* self) { static_cast<Service*>(self)->run(); }
  void run() {
    while (!quit_.load(std::memory_order_acquire)) {
      wake_->wait(100);
      if (quit_.load(std::memory_order_acquire)) break;
      try {
        processInput();
      } catch (...) {
        // out of memory while building notifications: drop this batch, keep running
      }
    }
  }

  void processInput() {
    std::vector<Note> notes;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& sp : surfaces_) {
        Surface& s = *sp;
        if (!s.sink) continue;
        RawMsg m;
        while (s.sink->pop(m)) {
          ControlEvent ev;
          if (!s.matcher.match(m.bytes, m.size, ev)) continue;
          handleEventLocked(s, ev);
          notes.push_back(Note{s.id, s.map.controls[ev.control].id, ev.value});
        }
      }
    }
    if (notes.empty()) return;
    std::vector<Ptr<IControlSurfaceListener>> ls;
    {
      std::lock_guard<std::mutex> lk(lmu_);
      ls = listeners_;
    }
    for (const Note& n : notes)
      for (auto& l : ls) l->onControlChanged(n.surface.c_str(), n.control.c_str(), n.value);
  }

  void handleEventLocked(Surface& s, const ControlEvent& ev) {
    const Control& c = s.map.controls[ev.control];
    if (c.action != Action::None && !ev.relative && ev.value >= 0.5 && s.pages) {
      const uint32_t count = s.pages->getPageCount();
      if (count) {
        uint32_t next = s.page;
        if (c.action == Action::PageNext) next = (s.page + 1) % count;
        else if (c.action == Action::PagePrev) next = (s.page + count - 1) % count;
        else if (c.actionPage < count) next = c.actionPage;
        if (next != s.page) selectPageLocked(s, next);
      }
    }
    if (!s.binding(ev.control)) return;
    Pending& p = s.pending[ev.control];
    if (ev.relative) {
      if (p.any && p.absolute) p.value = std::clamp(p.value + ev.value, 0.0, 1.0);
      else p.value = (p.any ? p.value : 0.0) + ev.value;
    } else {
      p.absolute = true;
      p.value = ev.value;
      // Only a continuous control (fader, knob) already shows the value the hand set: no echo.
      // Buttons and pads do not light their own LEDs, so their feedback follows the target.
      if (c.type != SQ_REMOTE_BUTTON && c.type != SQ_REMOTE_PAD) {
        s.touched[ev.control] = 1;
        s.lastFeedback[ev.control] = ev.value;
      }
    }
    p.any = true;
  }

  // ---- pages ----
  sq_result selectPageLocked(Surface& s, uint32_t index) {
    if (!s.pages) return SQ_E_NOTFOUND;
    if (index >= s.pages->getPageCount()) return SQ_E_INVALIDARG;
    Ptr<IRemotePage> page;
    sq_result r = s.pages->getPage(index, page.put());
    if (SQ_FAILED(r) || !page) return SQ_FAILED(r) ? r : SQ_E_FAIL;
    for (auto& b : s.pageBindings) b = Binding{};
    const uint32_t n = page->getControlCount();
    uint32_t unknown = 0;
    for (uint32_t i = 0; i < n; ++i) {
      sq_remote_control rc{};
      rc.struct_size = sizeof rc;
      if (page->getControl(i, &rc) != SQ_OK) continue;
      rc.id[sizeof rc.id - 1] = '\0';
      rc.target[sizeof rc.target - 1] = '\0';
      rc.property[sizeof rc.property - 1] = '\0';
      const int32_t idx = s.map.find(rc.id);
      if (idx < 0) {
        ++unknown;
        continue;
      }
      if (!rc.target[0]) continue;  // an unbound slot on this page
      s.pageBindings[size_t(idx)] = Binding{true, rc.target, rc.property[0] ? rc.property : "value", 0.0, 1.0};
    }
    if (unknown) sq::logf(SQ_LOG_WARN, "surface '%s' page %u: %u control id(s) not in the surface map", s.id.c_str(), index, unknown);
    s.page = index;
    clearPendingLocked(s);  // queued input belonged to the previous page's targets
    s.forgetFeedback();     // the next pump shows the new page's values
    return SQ_OK;
  }
  static void clearPendingLocked(Surface& s) {
    for (auto& p : s.pending) p = Pending{};
  }

  // ---- connections ----
  bool ensureWorkerLocked() {
    if (worker_) return true;
    if (!wake_->ok()) return false;
    quit_.store(false, std::memory_order_seq_cst);
    return SQ_SUCCEEDED(sq_thread_create(&Service::workerMain, this, "sq-surface", &worker_));
  }

  bool ensureDriverLocked() {
    if (driver_) return true;
    IHostContext* h = sq::host();
    if (!h) return false;
    Ptr<IMidiDriver> d;
    h->queryService(&IMidiDriver::iid, d.putVoid());
    IClassRegistry* reg = h->getClassRegistry();
    const std::string want = sq::getString(h->getSettings(), surface::kSettingMidiDriver);
    sq_guid g{};
    if (!d && !want.empty() && sq_guid_parse(want.c_str(), &g)) reg->createInstance(&g, &IMidiDriver::iid, d.putVoid());
    if (!d) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      for (uint32_t i = 0; i < reg->getClassCount() && !d; ++i)
        if (reg->getClassInfo(i, &ci) == SQ_OK && ci.category && std::strcmp(ci.category, midi::kDriverCategory) == 0)
          reg->createInstance(&ci.class_id, &IMidiDriver::iid, d.putVoid());
    }
    if (!d) {
      sq::logf(SQ_LOG_WARN, "no MIDI driver available: surfaces stay disconnected");
      return false;
    }
    driver_ = d;
    devListener_ = Ptr<DeviceListener>::adopt(new DeviceListener(this));
    driver_->addListener(devListener_.get());
    return true;
  }

  std::vector<PortRef> enumeratePortsLocked() {
    std::vector<PortRef> ports;
    if (!driver_) return ports;
    const uint32_t n = driver_->getPortCount();
    for (uint32_t i = 0; i < n; ++i) {
      sq_midi_port_info pi{};
      pi.struct_size = sizeof pi;
      if (driver_->getPortInfo(i, &pi) != SQ_OK) continue;
      pi.id[sizeof pi.id - 1] = '\0';
      pi.name[sizeof pi.name - 1] = '\0';
      ports.push_back(PortRef{pi.id, pi.name, pi.flags});
    }
    return ports;
  }

  static void closePorts(Surface& s) {
    if (s.in) s.in->stop();
    s.in.reset();  // released = closed; the driver drops its reference to the sink
    s.out.reset();
    s.sink.reset();
    s.curIn.clear();
    s.curOut.clear();
  }

  // Resolves what the surface should be connected to and (re)opens ports. True if it changed.
  bool relinkLocked(Surface& s, const std::vector<PortRef>& ports) {
    const std::string in = s.wantIn == "-" ? std::string() : resolvePort(ports, SQ_MIDI_PORT_INPUT, s.wantIn, s.dev.inputMatch);
    const std::string out = s.wantOut == "-" ? std::string() : resolvePort(ports, SQ_MIDI_PORT_OUTPUT, s.wantOut, s.dev.outputMatch);
    const LinkChange change = planLink(s.curIn, s.curOut, in, out);
    if (change == LinkChange::None) return false;
    closePorts(s);
    if (change == LinkChange::Disconnect) {
      sq::logf(SQ_LOG_INFO, "surface '%s' disconnected", s.id.c_str());
      return true;
    }
    if (!in.empty()) {
      auto sink = Ptr<InputSink>::adopt(new InputSink(wake_));
      Ptr<IMidiInputPort> port;
      sq_result r = driver_->openInput(in.c_str(), sink.get(), port.put());
      if (SQ_SUCCEEDED(r) && port && SQ_SUCCEEDED(port->start())) {
        s.sink = sink;
        s.in = port;
        s.curIn = in;
      } else {
        sq::logf(SQ_LOG_WARN, "surface '%s': cannot open MIDI input %s (%d)", s.id.c_str(), in.c_str(), r);
      }
    }
    if (!out.empty()) {
      Ptr<IMidiOutputPort> port;
      sq_result r = driver_->openOutput(out.c_str(), port.put());
      if (SQ_SUCCEEDED(r) && port) {
        s.out = port;
        s.curOut = out;
      } else {
        sq::logf(SQ_LOG_WARN, "surface '%s': cannot open MIDI output %s (%d)", s.id.c_str(), out.c_str(), r);
      }
    }
    s.forgetFeedback();  // a (re)connected device gets every bound value again
    return true;
  }

  bool reconcile() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!driver_) return false;
    std::vector<PortRef> ports = enumeratePortsLocked();
    bool changed = false;
    for (auto& s : surfaces_) changed |= relinkLocked(*s, ports);
    return changed;
  }

  void notifySurfacesChanged() {
    std::vector<Ptr<IControlSurfaceListener>> ls;
    {
      std::lock_guard<std::mutex> lk(lmu_);
      ls = listeners_;
    }
    for (auto& l : ls) l->onSurfacesChanged();
  }

  // ---- targets (main thread) ----
  struct Write {
    Binding b;
    bool relative;
    bool boolean;
    double value;
  };
  struct Follow {
    SurfacePtr s;
    uint32_t control;
    Binding b;
  };

  static double toNormalized(const Binding& b, double v) {
    const double span = b.max - b.min;
    return std::clamp(span != 0.0 ? (v - b.min) / span : 0.0, 0.0, 1.0);
  }

  bool applyTargets() {
    std::vector<Write> writes;
    std::vector<Follow> follows;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& sp : surfaces_) {
        Surface& s = *sp;
        for (size_t i = 0; i < s.pending.size(); ++i) {
          const Binding* b = s.binding(i);
          Pending& p = s.pending[i];
          if (p.any && b) {
            const int32_t t = s.map.controls[i].type;
            writes.push_back(Write{*b, !p.absolute, t == SQ_REMOTE_BUTTON || t == SQ_REMOTE_PAD, p.value});
          }
          p = Pending{};
          const bool touched = s.touched[i] != 0;
          s.touched[i] = 0;
          const Control& c = s.map.controls[i];
          const bool shows = s.out && (c.flags & SQ_REMOTE_FEEDBACK) && c.type != SQ_REMOTE_DISPLAY;
          // A latching button follows its target even without LED/output: its next press must
          // flip the target's current state, whoever changed it last.
          const bool latch = (c.flags & SQ_REMOTE_TOGGLE) != 0;
          if (b && !touched && (shows || latch)) follows.push_back(Follow{sp, uint32_t(i), *b});
        }
      }
    }
    if (writes.empty() && follows.empty()) return false;
    Ptr<IObjectRegistry> objects;
    if (IHostContext* h = sq::host()) h->queryService(&IObjectRegistry::iid, objects.putVoid());
    if (!objects) {
      if (!warnedNoRegistry_) sq::logf(SQ_LOG_WARN, "no object registry: surface bindings are not applied");
      warnedNoRegistry_ = true;
      return !writes.empty();
    }
    bool did = false;
    auto attrs = makeAttributes();
    for (const Write& w : writes) {
      Ptr<IController> ctl;
      if (SQ_FAILED(objects->resolve(w.b.target.c_str(), &IController::iid, ctl.putVoid())) || !ctl) continue;
      double norm = w.value;
      if (w.relative) {
        attrs->clear();
        if (SQ_FAILED(ctl->getProperty(w.b.property.c_str(), attrs.get()))) continue;
        const double cur = attrs->getDouble("value", w.b.min);
        norm = std::clamp(toNormalized(w.b, cur) + w.value, 0.0, 1.0);
      }
      attrs->clear();
      if (w.boolean) attrs->setBool("value", norm >= 0.5 ? SQ_TRUE : SQ_FALSE);
      else attrs->setDouble("value", w.b.min + norm * (w.b.max - w.b.min));
      if (SQ_SUCCEEDED(ctl->setProperty(w.b.property.c_str(), attrs.get()))) did = true;
    }
    for (const Follow& f : follows) {
      Ptr<IController> ctl;
      if (SQ_FAILED(objects->resolve(f.b.target.c_str(), &IController::iid, ctl.putVoid())) || !ctl) continue;
      attrs->clear();
      if (SQ_FAILED(ctl->getProperty(f.b.property.c_str(), attrs.get()))) continue;
      const double raw = attrs->getDouble("value", kNaN);
      if (!std::isfinite(raw)) continue;
      const double norm = toNormalized(f.b, raw);
      sq_midi_timed_event ev[2]{};
      uint32_t count = 0;
      Ptr<IMidiOutputPort> out;
      {
        std::lock_guard<std::mutex> lk(mu_);
        Surface& s = *f.s;
        if (f.control >= s.lastFeedback.size() || s.touched[f.control] || s.pending[f.control].any) continue;  // input meanwhile
        const Control& c = s.map.controls[f.control];
        if (c.flags & SQ_REMOTE_TOGGLE) s.matcher.setToggleState(f.control, norm >= 0.5);
        if (!s.out || !(c.flags & SQ_REMOTE_FEEDBACK) || c.type == SQ_REMOTE_DISPLAY) continue;
        const double last = s.lastFeedback[f.control];
        if (std::isfinite(last) && std::fabs(last - norm) < kFeedbackEpsilon) continue;
        count = encodeFeedback(c, norm, ev);
        s.lastFeedback[f.control] = norm;
        out = s.out;
      }
      if (count && out && SQ_SUCCEEDED(out->send(ev, count))) did = true;
    }
    return did;
  }

  // ---- lookup ----
  Surface* findLocked(const char* id) {
    if (!id) return nullptr;
    for (auto& s : surfaces_)
      if (s->id == id) return s.get();
    return nullptr;
  }
  std::string uniqueIdLocked(const std::string& base) {
    std::string id = base.empty() ? std::string("surface") : base;
    for (int n = 2; findLocked(id.c_str()); ++n) id = base + "#" + std::to_string(n);
    return id;
  }

  std::mutex mu_;
  std::vector<SurfacePtr> surfaces_;
  std::string lastError_;
  bool warnedNoRegistry_ = false;
  Ptr<IMidiDriver> driver_;
  Ptr<DeviceListener> devListener_;
  std::shared_ptr<Wake> wake_;
  sq_thread* worker_ = nullptr;
  std::atomic<bool> quit_{false};
  std::atomic<bool> portsDirty_{false};

  std::mutex lmu_;
  std::vector<Ptr<IControlSurfaceListener>> listeners_;
};

void SQ_CALL DeviceListener::onPortsChanged() {
  inflight_.fetch_add(1, std::memory_order_seq_cst);
  if (Service* s = owner_.load(std::memory_order_seq_cst)) s->portsChanged();
  inflight_.fetch_sub(1, std::memory_order_seq_cst);
}

// ---- module -----------------------------------------------------------------------------------

Ptr<Service>& singleton() {
  static Ptr<Service> s;
  return s;
}
bool& registeredAsService() {
  static bool r = false;
  return r;
}

sq_result onInit(IHostContext* host) {
  registeredAsService() = false;
  singleton() = Ptr<Service>::adopt(new Service());
  Ptr<IControlSurfaceService> existing;
  if (SQ_SUCCEEDED(host->queryService(&IControlSurfaceService::iid, existing.putVoid())) && existing) {
    sq::logf(SQ_LOG_INFO, "another IControlSurfaceService is registered; MIDI surfaces stay available as class %s",
             toString(surface::kHardwareSurfaceClassId).c_str());
    return SQ_OK;
  }
  sq_result r = host->registerService(&IControlSurfaceService::iid, static_cast<IControlSurfaceService*>(singleton().get()));
  if (SQ_FAILED(r)) {
    singleton().reset();
    return r;
  }
  registeredAsService() = true;
  return SQ_OK;
}

void onShutdown() {
  if (singleton()) {
    if (registeredAsService())
      if (IHostContext* h = sq::host()) h->unregisterService(&IControlSurfaceService::iid, static_cast<IControlSurfaceService*>(singleton().get()));
    singleton()->shutdown();
  }
  registeredAsService() = false;
  singleton().reset();
}

constexpr ClassEntry kClasses[] = {
    classEntry<Service>(surface::kHardwareSurfaceClassId, surface::kCategory, "MIDI Control Surface Service"),
};

constexpr ModuleDesc kModule{
    .fourcc = SQ_FOURCC('H', 'W', 'S', 'F'),
    .name = "hardware-surface",
    .description = "MIDI control surfaces: .device/.surface.xml maps, feedback, remote pages",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace
}  // namespace sq::hwsurface

SQ_MODULE_EXPORTS(sq::hwsurface::kModule)
