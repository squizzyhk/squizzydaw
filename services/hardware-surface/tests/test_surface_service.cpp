// hardware-surface through the real module loader, against mocks (no MIDI hardware needed):
// a mock IMidiDriver (registered as a host service) whose ports record what is sent and let the
// test inject input, a mock IObjectRegistry with IController targets, and mock remote pages.
// Covers: map loading + errors, name-matched and explicit connections, input -> listeners,
// coalesced target writes in pump(), feedback (explicit, target-follow, no echo), toggles,
// relative encoders, displays, remote pages + page buttons, and hot-plug disconnect/reconnect.
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/midi.h"
#include "sq/interfaces/remote.h"
#include "sq/interfaces/scripting.h"
#include "sq/platform.hpp"
#include "sq/services/hardware_surface/hardware_surface.h"
#include "sqtest.hpp"

namespace {

template <size_t N>
void put(char (&dst)[N], const std::string& s) {
  std::strncpy(dst, s.c_str(), N - 1);
  dst[N - 1] = '\0';
}

bool waitUntil(const std::function<bool()>& pred, int ms = 3000) {
  for (int i = 0; i < ms / 2; ++i) {
    if (pred()) return true;
    sq_thread_sleep_ms(2);
  }
  return pred();
}

// ---- mock MIDI driver ----------------------------------------------------------------------

struct Sent {
  std::string port;
  std::vector<uint8_t> bytes;
};

class MockDriver;

class MockIn final : public sq::Object<MockIn, sq::IMidiInputPort> {
 public:
  MockIn(MockDriver* d, sq_midi_port_info info, sq::IMidiInputCallback* cb) : drv_(d), info_(info), cb_(cb) {}
  ~MockIn() override;
  sq_result SQ_CALL getInfo(sq_midi_port_info* out) override {
    *out = info_;
    return SQ_OK;
  }
  sq_result SQ_CALL start() override {
    running = true;
    return SQ_OK;
  }
  sq_result SQ_CALL stop() override {
    running = false;
    return SQ_OK;
  }
  void deliver(const std::vector<uint8_t>& b) {
    if (!running) return;
    sq_midi_timed_event e{};
    e.time_ns = sq_time_monotonic_ns();
    std::memcpy(e.bytes, b.data(), b.size() < 4 ? b.size() : 4);
    e.size = uint32_t(b.size());
    cb_->onMidi(&info_, &e, 1);
  }
  const char* id() const { return info_.id; }
  std::atomic<bool> running{false};

 private:
  MockDriver* drv_;
  sq_midi_port_info info_;
  sq::Ptr<sq::IMidiInputCallback> cb_;
};

class MockOut final : public sq::Object<MockOut, sq::IMidiOutputPort> {
 public:
  MockOut(MockDriver* d, sq_midi_port_info info) : drv_(d), info_(info) {}
  sq_result SQ_CALL getInfo(sq_midi_port_info* out) override {
    *out = info_;
    return SQ_OK;
  }
  sq_result SQ_CALL send(const sq_midi_timed_event* ev, uint32_t n) override;

 private:
  MockDriver* drv_;
  sq_midi_port_info info_;
};

class MockDriver final : public sq::Object<MockDriver, sq::IMidiDriver> {
 public:
  void addPort(const std::string& id, const std::string& name, uint32_t flags) {
    std::lock_guard<std::mutex> lk(mu_);
    sq_midi_port_info p{};
    p.struct_size = sizeof p;
    put(p.id, id);
    put(p.name, name);
    p.flags = flags;
    ports_.push_back(p);
  }
  void removePort(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = ports_.begin(); it != ports_.end(); ++it)
      if (id == it->id) {
        ports_.erase(it);
        return;
      }
  }
  // As the real driver does after a re-enumeration found a change (on its own thread).
  void firePortsChanged() {
    std::vector<sq::Ptr<sq::IMidiDeviceListener>> ls;
    {
      std::lock_guard<std::mutex> lk(mu_);
      ls = listeners_;
    }
    for (auto& l : ls) l->onPortsChanged();
  }
  void inject(const std::string& port, std::vector<uint8_t> bytes) {
    std::vector<sq::Ptr<MockIn>> targets;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (MockIn* in : inputs_)
        if (port == in->id()) targets.push_back(sq::Ptr<MockIn>(in));
    }
    for (auto& t : targets) t->deliver(bytes);
  }
  bool isOpen(const std::string& port) {
    std::lock_guard<std::mutex> lk(mu_);
    for (MockIn* in : inputs_)
      if (port == in->id()) return true;
    return false;
  }
  std::vector<Sent> sent() {
    std::lock_guard<std::mutex> lk(mu_);
    return sent_;
  }
  void clearSent() {
    std::lock_guard<std::mutex> lk(mu_);
    sent_.clear();
  }
  size_t listenerCount() {
    std::lock_guard<std::mutex> lk(mu_);
    return listeners_.size();
  }
  void record(const std::string& port, std::vector<uint8_t> b) {
    std::lock_guard<std::mutex> lk(mu_);
    sent_.push_back(Sent{port, std::move(b)});
  }
  void forget(MockIn* in) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = inputs_.begin(); it != inputs_.end(); ++it)
      if (*it == in) {
        inputs_.erase(it);
        return;
      }
  }

  sq_result SQ_CALL refreshPorts() override { return SQ_OK; }
  uint32_t SQ_CALL getPortCount() override {
    std::lock_guard<std::mutex> lk(mu_);
    return uint32_t(ports_.size());
  }
  sq_result SQ_CALL getPortInfo(uint32_t i, sq_midi_port_info* out) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (i >= ports_.size()) return SQ_E_NOTFOUND;
    *out = ports_[i];
    return SQ_OK;
  }
  sq_result SQ_CALL openInput(const char* id, sq::IMidiInputCallback* cb, sq::IMidiInputPort** out) override {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : ports_)
      if ((p.flags & SQ_MIDI_PORT_INPUT) && std::strcmp(p.id, id) == 0) {
        auto* in = new MockIn(this, p, cb);
        inputs_.push_back(in);
        *out = in;
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL openOutput(const char* id, sq::IMidiOutputPort** out) override {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : ports_)
      if ((p.flags & SQ_MIDI_PORT_OUTPUT) && std::strcmp(p.id, id) == 0) {
        *out = new MockOut(this, p);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL addListener(sq::IMidiDeviceListener* l) override {
    std::lock_guard<std::mutex> lk(mu_);
    listeners_.push_back(sq::Ptr<sq::IMidiDeviceListener>(l));
    return SQ_OK;
  }
  sq_result SQ_CALL removeListener(sq::IMidiDeviceListener* l) override {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
      if (it->get() == l) {
        listeners_.erase(it);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }

 private:
  std::mutex mu_;
  std::vector<sq_midi_port_info> ports_;
  std::vector<MockIn*> inputs_;
  std::vector<sq::Ptr<sq::IMidiDeviceListener>> listeners_;
  std::vector<Sent> sent_;
};

MockIn::~MockIn() { drv_->forget(this); }

sq_result SQ_CALL MockOut::send(const sq_midi_timed_event* ev, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    if (ev[i].data) drv_->record(info_.id, std::vector<uint8_t>(ev[i].data, ev[i].data + ev[i].size));
    else drv_->record(info_.id, std::vector<uint8_t>(ev[i].bytes, ev[i].bytes + ev[i].size));
  }
  return SQ_OK;
}

// ---- mock object graph -------------------------------------------------------------------------

class MockController final : public sq::Object<MockController, sq::IController> {
 public:
  std::map<std::string, double> props;
  std::map<std::string, int> sets;
  sq_result SQ_CALL getProperty(const char* name, sq::IAttributes* out) override {
    auto it = props.find(name);
    if (it == props.end()) return SQ_E_NOTFOUND;
    return out->setDouble("value", it->second);
  }
  sq_result SQ_CALL setProperty(const char* name, sq::IAttributes* in) override {
    props[name] = in->getDouble("value", std::nan(""));
    ++sets[name];
    return SQ_OK;
  }
  sq_result SQ_CALL getChild(const char*, const sq_guid*, void**) override { return SQ_E_NOTIMPL; }
};

class MockObjects final : public sq::Object<MockObjects, sq::IObjectRegistry> {
 public:
  std::map<std::string, sq::Ptr<MockController>> objects;
  MockController* add(const std::string& url) {
    auto c = sq::Ptr<MockController>::adopt(new MockController());
    objects[url] = c;
    return c.get();
  }
  sq_result SQ_CALL registerResolver(const char*, sq::IUrlResolver*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL unregisterResolver(const char*, sq::IUrlResolver*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL registerObject(const char*, sq::IUnknown*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL unregisterObject(const char*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) override {
    auto it = objects.find(url);
    if (it == objects.end()) return SQ_E_NOTFOUND;
    return it->second->queryInterface(riid, out);
  }
  sq_result SQ_CALL openStream(const char*, sq::IByteStream**) override { return SQ_E_NOTIMPL; }
};

// ---- mock remote pages --------------------------------------------------------------------------

class MockPage final : public sq::Object<MockPage, sq::IRemotePage> {
 public:
  std::vector<sq_remote_control> controls;
  void bind(const std::string& id, const std::string& target, const std::string& prop) {
    sq_remote_control c{};
    c.struct_size = sizeof c;
    put(c.id, id);
    put(c.target, target);
    put(c.property, prop);
    c.type = SQ_REMOTE_KNOB;
    controls.push_back(c);
  }
  sq_result SQ_CALL getName(char* buf, uint32_t size, uint32_t* needed) override {
    if (needed) *needed = 2;
    if (!buf || size < 2) return SQ_E_BUFFER_TOO_SMALL;
    std::strcpy(buf, "P");
    return SQ_OK;
  }
  uint32_t SQ_CALL getControlCount() override { return uint32_t(controls.size()); }
  sq_result SQ_CALL getControl(uint32_t i, sq_remote_control* out) override {
    if (i >= controls.size()) return SQ_E_NOTFOUND;
    *out = controls[i];
    return SQ_OK;
  }
};

class MockPages final : public sq::Object<MockPages, sq::IRemotePageSet> {
 public:
  std::vector<sq::Ptr<MockPage>> pages;
  uint32_t SQ_CALL getPageCount() override { return uint32_t(pages.size()); }
  sq_result SQ_CALL getPage(uint32_t i, sq::IRemotePage** out) override {
    if (i >= pages.size()) return SQ_E_NOTFOUND;
    pages[i]->addRef();
    *out = pages[i].get();
    return SQ_OK;
  }
};

class Listener final : public sq::Object<Listener, sq::IControlSurfaceListener> {
 public:
  struct Ev {
    std::string surface, control;
    double value;
  };
  std::mutex mu;
  std::vector<Ev> events;
  std::atomic<int> surfacesChanged{0};
  void SQ_CALL onSurfacesChanged() override { surfacesChanged.fetch_add(1); }
  void SQ_CALL onControlChanged(const char* s, const char* c, double v) override {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(Ev{s, c, v});
  }
  size_t count() {
    std::lock_guard<std::mutex> lk(mu);
    return events.size();
  }
  Ev last() {
    std::lock_guard<std::mutex> lk(mu);
    return events.back();
  }
};

const char* kMap = R"(<?xml version="1.0" encoding="UTF-8"?>
<ControlSurface format="1" name="Test">
  <Controls>
    <Control id="fader" type="fader" feedback="true"><MidiMessage status="0xE0"/></Control>
    <Control id="mute" type="button" toggle="true" feedback="true"><MidiMessage status="0x90" address="16"/></Control>
    <Control id="pan" type="encoder" step="0.01" feedback="true"><MidiMessage status="0xB0" address="16"/></Control>
    <Control id="knob" type="knob"><MidiMessage status="0xB0" address="20"/></Control>
    <Control id="next" type="button" action="page.next"><MidiMessage status="0x90" address="47"/></Control>
    <Control id="pc" type="button"><MidiMessage status="0xC0" address="1"/></Control>
    <Control id="lcd" type="display" length="4"><Sysex header="F0 7D 02"/></Control>
  </Controls>
  <Bindings>
    <Bind control="fader" target="object://doc/tracks/0" property="volumeDb" min="-60" max="6"/>
    <Bind control="mute" target="object://doc/tracks/0" property="mute"/>
    <Bind control="pan" target="object://doc/tracks/0" property="pan"/>
  </Bindings>
</ControlSurface>
)";

const char* kDevice = R"(<Device format="1" id="test.desk" name="Test Desk">
  <Ports input="test desk" output="TEST DESK"/>
  <Surface file="sqtest_hws.surface.xml"/>
</Device>
)";

const char* kBadMap = R"(<ControlSurface name="Bad"><Controls>
  <Control id="a" type="knob"><MidiMessage status="0xB0" address="1"/></Control>
  <Control id="a" type="knob"><MidiMessage status="0xB0" address="2"/></Control>
</Controls></ControlSurface>)";

bool sentContains(MockDriver& d, const std::string& port, std::vector<uint8_t> bytes) {
  for (const auto& s : d.sent())
    if (s.port == port && s.bytes == bytes) return true;
  return false;
}

}  // namespace

SQ_TEST("hardware-surface: maps, input, targets, feedback, pages and hot-plug with a mock MIDI driver") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);

  auto drv = sq::Ptr<MockDriver>::adopt(new MockDriver());
  drv->addPort("mock:in", "Test Desk MIDI", SQ_MIDI_PORT_INPUT);
  drv->addPort("mock:out", "Test Desk MIDI", SQ_MIDI_PORT_OUTPUT);
  drv->addPort("other:in", "Something Else", SQ_MIDI_PORT_INPUT);
  auto objs = sq::Ptr<MockObjects>::adopt(new MockObjects());
  MockController* track0 = objs->add("object://doc/tracks/0");
  MockController* track1 = objs->add("object://doc/tracks/1");
  MockController* track2 = objs->add("object://doc/tracks/2");
  track0->props = {{"volumeDb", 0.0}, {"mute", 0.0}, {"pan", 0.5}};
  track1->props = {{"volume", 0.0}};
  track2->props = {{"volume", 0.5}};
  REQUIRE_OK(host.registerService(&sq::IMidiDriver::iid, drv.get()));
  REQUIRE_OK(host.registerService(&sq::IObjectRegistry::iid, objs.get()));

  const std::string tmp = sq::specialDir(SQ_DIR_TEMP);
  const std::string mapPath = sq::joinPath(tmp, "sqtest_hws.surface.xml");
  const std::string devPath = sq::joinPath(tmp, "sqtest_hws.device");
  const std::string badPath = sq::joinPath(tmp, "sqtest_hws_bad.surface.xml");
  REQUIRE_OK(sq::writeFile(mapPath, kMap));
  REQUIRE_OK(sq::writeFile(devPath, kDevice));
  REQUIRE_OK(sq::writeFile(badPath, kBadMap));

  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("hardware-surface") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    sq::Ptr<sq::IControlSurfaceService> svc;
    REQUIRE_OK(host.queryService(&sq::IControlSurfaceService::iid, svc.putVoid()));
    REQUIRE(svc);
    auto pump = svc.as<sq::ISurfacePump>();
    REQUIRE(pump);
    auto lis = sq::Ptr<Listener>::adopt(new Listener());
    REQUIRE_OK(svc->addListener(lis.get()));
    CHECK_EQ(svc->addListener(lis.get()), SQ_S_FALSE);

    // --- loading: validation errors, buffer contract, name matching -----------------------------
    char id[128];
    uint32_t needed = 0;
    CHECK_EQ(svc->loadSurfaceMap(badPath.c_str(), nullptr, id, sizeof id, &needed), SQ_E_INVALIDARG);
    char err[1024];
    REQUIRE_OK(pump->getLastError(err, sizeof err, &needed));
    CHECK(std::strstr(err, "duplicate id") != nullptr);
    CHECK_EQ(svc->loadSurfaceMap("C:/no/such/file.device", nullptr, id, sizeof id, &needed), SQ_E_IO);
    CHECK_EQ(svc->getSurfaceCount(), 0u);

    CHECK_EQ(svc->loadSurfaceMap(devPath.c_str(), nullptr, nullptr, 0, &needed), SQ_E_BUFFER_TOO_SMALL);
    CHECK_EQ(needed, uint32_t(std::strlen("test.desk") + 1));
    CHECK_EQ(svc->getSurfaceCount(), 0u);  // nothing committed
    REQUIRE_OK(svc->loadSurfaceMap(devPath.c_str(), nullptr, id, sizeof id, &needed));
    CHECK(std::string(id) == "test.desk");
    CHECK(lis->surfacesChanged.load() >= 1);
    CHECK(drv->isOpen("mock:in"));  // matched "test desk" case-insensitively
    CHECK_EQ(drv->listenerCount(), size_t(1));
    sq_surface_info info{};
    info.struct_size = sizeof info;
    REQUIRE_OK(svc->getSurfaceInfo(0, &info));
    CHECK(std::string(info.id) == "test.desk");
    CHECK(std::string(info.name) == "Test Desk");
    CHECK_EQ(info.transport, SQ_SURFACE_MIDI_USB);
    CHECK(info.flags & SQ_SURFACE_CONNECTED);

    // Explicit connection id, input only ("in|" = no output); the id is made unique.
    char id2[128];
    REQUIRE_OK(svc->loadSurfaceMap(mapPath.c_str(), "other:in|", id2, sizeof id2, &needed));
    CHECK(std::string(id2) == "Test");
    CHECK(drv->isOpen("other:in"));
    char id3[128];
    CHECK_EQ(svc->loadSurfaceMap(mapPath.c_str(), "absent:in", id3, sizeof id3, &needed), SQ_S_FALSE);  // loaded, not connected
    CHECK(std::string(id3) == "Test#2");
    CHECK_EQ(svc->getSurfaceCount(), 3u);
    REQUIRE_OK(svc->getSurfaceInfo(2, &info));
    CHECK(!(info.flags & SQ_SURFACE_CONNECTED));
    CHECK_EQ(svc->getSurfaceInfo(3, &info), SQ_E_NOTFOUND);

    // --- first pump: every bound feedback control gets its target's current value ----------------
    CHECK_OK(pump->pump());
    CHECK(sentContains(*drv, "mock:out", {0xE0, 0x2E, 0x74}));  // volumeDb 0 of -60..6 -> 14894
    CHECK(sentContains(*drv, "mock:out", {0x90, 16, 0}));       // mute LED off
    CHECK(sentContains(*drv, "mock:out", {0xB0, 16, 64}));      // pan ring at 0.5
    drv->clearSent();
    CHECK_EQ(pump->pump(), SQ_S_FALSE);  // nothing changed
    CHECK(drv->sent().empty());

    // --- absolute input: listener on the worker, target write on pump, no echo ------------------
    drv->inject("mock:in", {0xE0, 0x00, 0x40});
    REQUIRE(waitUntil([&] { return lis->count() >= 1; }));
    auto e = lis->last();
    CHECK(e.surface == "test.desk");
    CHECK(e.control == "fader");
    CHECK(std::fabs(e.value - 8192.0 / 16383.0) < 1e-9);
    CHECK_EQ(track0->sets["volumeDb"], 0);  // targets only change in pump()
    CHECK_OK(pump->pump());
    CHECK(std::fabs(track0->props["volumeDb"] - (-60.0 + 66.0 * 8192.0 / 16383.0)) < 1e-6);
    CHECK(drv->sent().empty());  // the fader is where the hand put it: no echo

    // A move elsewhere (UI, automation) drives the motor fader.
    track0->props["volumeDb"] = 6.0;
    CHECK_OK(pump->pump());
    REQUIRE_EQ(drv->sent().size(), size_t(1));
    CHECK(sentContains(*drv, "mock:out", {0xE0, 0x7F, 0x7F}));
    drv->clearSent();
    CHECK_EQ(pump->pump(), SQ_S_FALSE);

    // --- toggle button: latches, LED follows the target ----------------------------------------
    size_t before = lis->count();
    drv->inject("mock:in", {0x90, 16, 127});
    drv->inject("mock:in", {0x80, 16, 0});  // release: ignored by a latching button
    REQUIRE(waitUntil([&] { return lis->count() >= before + 1; }));
    sq_thread_sleep_ms(20);
    CHECK_EQ(lis->count(), before + 1);
    CHECK_EQ(lis->last().value, 1.0);
    CHECK_OK(pump->pump());
    CHECK_EQ(track0->props["mute"], 1.0);
    track0->props["mute"] = 0.0;  // unmuted elsewhere
    CHECK_OK(pump->pump());
    CHECK(sentContains(*drv, "mock:out", {0x90, 16, 0}));
    drv->inject("mock:in", {0x90, 16, 127});  // toggle state followed the target: this press mutes again
    REQUIRE(waitUntil([&] { return lis->count() >= before + 2; }));
    CHECK_EQ(lis->last().value, 1.0);
    CHECK_OK(pump->pump());
    CHECK_EQ(track0->props["mute"], 1.0);
    drv->clearSent();

    // --- relative encoder: deltas coalesce, ring follows ----------------------------------------
    before = lis->count();
    drv->inject("mock:in", {0xB0, 16, 3});
    drv->inject("mock:in", {0xB0, 16, 3});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 2; }));
    CHECK(lis->last().value > 0.0);
    CHECK_OK(pump->pump());
    CHECK(std::fabs(track0->props["pan"] - 0.56) < 1e-9);
    CHECK_EQ(track0->sets["pan"], 1);  // one write for both ticks
    CHECK(sentContains(*drv, "mock:out", {0xB0, 16, 71}));
    drv->clearSent();

    // --- explicit feedback ---------------------------------------------------------------------
    CHECK_OK(svc->setControlValue("test.desk", "fader", 0.25, nullptr));
    CHECK(sentContains(*drv, "mock:out", {0xE0, 0x00, 0x20}));  // 4096
    CHECK_OK(svc->setControlValue("test.desk", "lcd", 0.0, "Hi"));
    CHECK(sentContains(*drv, "mock:out", {0xF0, 0x7D, 0x02, 'H', 'i', ' ', ' ', 0xF7}));
    CHECK_OK(svc->setControlValue("test.desk", "lcd", 0.5, nullptr));  // no text: the value
    CHECK(sentContains(*drv, "mock:out", {0xF0, 0x7D, 0x02, '0', '.', '5', '0', 0xF7}));
    CHECK_EQ(svc->setControlValue("test.desk", "pc", 1.0, nullptr), SQ_E_UNSUPPORTED);
    CHECK_EQ(svc->setControlValue("test.desk", "nope", 1.0, nullptr), SQ_E_NOTFOUND);
    CHECK_EQ(svc->setControlValue("nope", "fader", 1.0, nullptr), SQ_E_NOTFOUND);
    CHECK_EQ(svc->setControlValue("Test", "fader", 1.0, nullptr), SQ_E_CLOSED);  // input-only surface
    CHECK_EQ(svc->setControlValue("test.desk", "fader", std::nan(""), nullptr), SQ_E_INVALIDARG);
    drv->clearSent();

    // --- remote pages ---------------------------------------------------------------------------
    auto pages = sq::Ptr<MockPages>::adopt(new MockPages());
    for (int i = 0; i < 2; ++i) {
      auto p = sq::Ptr<MockPage>::adopt(new MockPage());
      p->bind("knob", "object://doc/tracks/" + std::to_string(i + 1), "volume");
      p->bind("not-on-this-surface", "object://doc/tracks/0", "x");  // ignored with a warning
      pages->pages.push_back(p);
    }
    CHECK_EQ(svc->setRemotePages("nope", pages.get()), SQ_E_NOTFOUND);
    REQUIRE_OK(svc->setRemotePages("test.desk", pages.get()));  // selects page 0
    before = lis->count();
    drv->inject("mock:in", {0xB0, 20, 127});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 1; }));
    CHECK_OK(pump->pump());
    CHECK_EQ(track1->props["volume"], 1.0);
    REQUIRE_OK(svc->selectPage("test.desk", 1));
    CHECK_EQ(svc->selectPage("test.desk", 2), SQ_E_INVALIDARG);
    drv->inject("mock:in", {0xB0, 20, 0});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 2; }));
    CHECK_OK(pump->pump());
    CHECK_EQ(track2->props["volume"], 0.0);
    CHECK_EQ(track1->props["volume"], 1.0);
    // The page button wraps 1 -> 0 (handled by the service, still reported to listeners).
    drv->inject("mock:in", {0x90, 47, 127});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 3; }));
    CHECK(lis->last().control == "next");
    drv->inject("mock:in", {0xB0, 20, 64});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 4; }));
    CHECK_OK(pump->pump());
    CHECK(std::fabs(track1->props["volume"] - 64.0 / 127.0) < 1e-9);
    CHECK_EQ(track2->props["volume"], 0.0);
    // Clearing the pages leaves only the map's own bindings (the knob has none).
    REQUIRE_OK(svc->setRemotePages("test.desk", nullptr));
    const int writes = track1->sets["volume"];
    drv->inject("mock:in", {0xB0, 20, 1});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 5; }));
    pump->pump();
    CHECK_EQ(track1->sets["volume"], writes);
    drv->clearSent();

    // --- hot-plug: unplug, plug back in ----------------------------------------------------------
    int changes = lis->surfacesChanged.load();
    drv->removePort("mock:in");
    drv->removePort("mock:out");
    drv->firePortsChanged();
    CHECK_OK(pump->pump());
    CHECK_EQ(lis->surfacesChanged.load(), changes + 1);
    REQUIRE_OK(svc->getSurfaceInfo(0, &info));
    CHECK(!(info.flags & SQ_SURFACE_CONNECTED));
    CHECK(!drv->isOpen("mock:in"));
    REQUIRE_OK(svc->getSurfaceInfo(1, &info));
    CHECK(info.flags & SQ_SURFACE_CONNECTED);  // the other surface is unaffected
    CHECK_EQ(svc->setControlValue("test.desk", "fader", 1.0, nullptr), SQ_E_CLOSED);

    drv->addPort("mock:in2", "Test Desk MIDI", SQ_MIDI_PORT_INPUT);  // comes back under a new id
    drv->addPort("mock:out2", "Test Desk MIDI", SQ_MIDI_PORT_OUTPUT);
    drv->firePortsChanged();
    CHECK_OK(pump->pump());
    CHECK_EQ(lis->surfacesChanged.load(), changes + 2);
    REQUIRE_OK(svc->getSurfaceInfo(0, &info));
    CHECK(info.flags & SQ_SURFACE_CONNECTED);
    CHECK(drv->isOpen("mock:in2"));
    CHECK(sentContains(*drv, "mock:out2", {0xE0, 0x7F, 0x7F}));  // the device gets the current state again
    CHECK(sentContains(*drv, "mock:out2", {0x90, 16, 127}));     // mute is on
    before = lis->count();
    drv->inject("mock:in2", {0xE0, 0, 0});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 1; }));
    CHECK_OK(pump->pump());
    CHECK_EQ(track0->props["volumeDb"], -60.0);

    // A separate instance from the class registry works on its own (and cleans up on release).
    {
      auto other = sq::createInstance<sq::IControlSurfaceService>(&reg, sq::surface::kHardwareSurfaceClassId);
      REQUIRE(other);
      CHECK_EQ(other->getSurfaceCount(), 0u);
      char oid[128];
      REQUIRE_OK(other->loadSurfaceMap(mapPath.c_str(), "other:in|", oid, sizeof oid, &needed));
      CHECK_EQ(drv->listenerCount(), size_t(2));
    }
    CHECK_EQ(drv->listenerCount(), size_t(1));

    REQUIRE_OK(svc->removeListener(lis.get()));
    CHECK_EQ(svc->removeListener(lis.get()), SQ_E_NOTFOUND);
  }
  CHECK(loader.unload("hardware-surface") == SQ_OK);
  CHECK_EQ(drv->listenerCount(), size_t(0));  // the service detached from the driver
  CHECK(!drv->isOpen("mock:in2"));
  CHECK(!drv->isOpen("other:in"));
  sq_fs_remove(mapPath.c_str());
  sq_fs_remove(devPath.c_str());
  sq_fs_remove(badPath.c_str());
}

// Verifier regressions: a button never lights its own LED, so a press must not be treated as
// "the hardware already shows this value" (only continuous controls are protected from echo),
// and a latching button must follow its target even when it has no LED / no output port.
SQ_TEST("hardware-surface: button LEDs follow presses; toggles follow the target without feedback") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);

  auto drv = sq::Ptr<MockDriver>::adopt(new MockDriver());
  drv->addPort("b:in", "Btn In", SQ_MIDI_PORT_INPUT);
  drv->addPort("b:out", "Btn Out", SQ_MIDI_PORT_OUTPUT);
  drv->addPort("c:in", "Plain In", SQ_MIDI_PORT_INPUT);
  auto objs = sq::Ptr<MockObjects>::adopt(new MockObjects());
  MockController* t = objs->add("object://doc/tracks/0");
  t->props = {{"mute", 0.0}, {"solo", 0.0}, {"armed", 0.0}, {"volume", 0.0}};
  REQUIRE_OK(host.registerService(&sq::IMidiDriver::iid, drv.get()));
  REQUIRE_OK(host.registerService(&sq::IObjectRegistry::iid, objs.get()));

  const char* map = R"(<ControlSurface name="Btn"><Controls>
    <Control id="mute" type="button" toggle="true" feedback="true"><MidiMessage status="0x90" address="16"/></Control>
    <Control id="solo" type="button" feedback="true"><MidiMessage status="0x90" address="17"/></Control>
    <Control id="arm" type="button" toggle="true"><MidiMessage status="0x90" address="18"/></Control>
    <Control id="fader" type="fader" feedback="true"><MidiMessage status="0xB0" address="7"/></Control>
  </Controls><Bindings>
    <Bind control="mute" target="object://doc/tracks/0" property="mute"/>
    <Bind control="solo" target="object://doc/tracks/0" property="solo"/>
    <Bind control="arm" target="object://doc/tracks/0" property="armed"/>
    <Bind control="fader" target="object://doc/tracks/0" property="volume"/>
  </Bindings></ControlSurface>)";
  const std::string path = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest_hws_btn.surface.xml");
  REQUIRE_OK(sq::writeFile(path, map));

  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("hardware-surface") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    sq::Ptr<sq::IControlSurfaceService> svc;
    REQUIRE_OK(host.queryService(&sq::IControlSurfaceService::iid, svc.putVoid()));
    auto pump = svc.as<sq::ISurfacePump>();
    REQUIRE(pump);
    auto lis = sq::Ptr<Listener>::adopt(new Listener());
    REQUIRE_OK(svc->addListener(lis.get()));
    char id[128], id2[128];
    uint32_t needed = 0;
    REQUIRE_OK(svc->loadSurfaceMap(path.c_str(), "b:in|b:out", id, sizeof id, &needed));
    REQUIRE_OK(svc->loadSurfaceMap(path.c_str(), "c:in|", id2, sizeof id2, &needed));  // no output at all
    CHECK_OK(pump->pump());
    drv->clearSent();

    // Latching mute: press -> target on AND the LED lights; press again -> both off.
    size_t before = lis->count();
    drv->inject("b:in", {0x90, 16, 127});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 1; }));
    CHECK_OK(pump->pump());
    CHECK_EQ(t->props["mute"], 1.0);
    CHECK(sentContains(*drv, "b:out", {0x90, 16, 127}));
    drv->clearSent();
    drv->inject("b:in", {0x90, 16, 127});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 2; }));
    CHECK_OK(pump->pump());
    CHECK_EQ(t->props["mute"], 0.0);
    CHECK(sentContains(*drv, "b:out", {0x90, 16, 0}));
    drv->clearSent();

    // Momentary solo with an LED: held -> LED on, released -> LED off.
    drv->inject("b:in", {0x90, 17, 127});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 3; }));
    CHECK_OK(pump->pump());
    CHECK_EQ(t->props["solo"], 1.0);
    CHECK(sentContains(*drv, "b:out", {0x90, 17, 127}));
    drv->inject("b:in", {0x80, 17, 0});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 4; }));
    CHECK_OK(pump->pump());
    CHECK_EQ(t->props["solo"], 0.0);
    CHECK(sentContains(*drv, "b:out", {0x90, 17, 0}));
    drv->clearSent();

    // A continuous control is still never echoed back while the hand moves it.
    drv->inject("b:in", {0xB0, 7, 100});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 5; }));
    CHECK_OK(pump->pump());
    CHECK(std::fabs(t->props["volume"] - 100.0 / 127.0) < 1e-9);
    CHECK(!sentContains(*drv, "b:out", {0xB0, 7, 100}));

    // Latching arm without LED on an input-only surface: armed elsewhere, then one press must
    // disarm (flip the target's state), not write "armed" a second time.
    t->props["armed"] = 1.0;
    pump->pump();
    before = lis->count();
    drv->inject("c:in", {0x90, 18, 127});
    REQUIRE(waitUntil([&] { return lis->count() >= before + 1; }));
    CHECK_EQ(lis->last().value, 0.0);
    CHECK_OK(pump->pump());
    CHECK_EQ(t->props["armed"], 0.0);

    REQUIRE_OK(svc->removeListener(lis.get()));
  }
  CHECK(loader.unload("hardware-surface") == SQ_OK);
  sq_fs_remove(path.c_str());
}
