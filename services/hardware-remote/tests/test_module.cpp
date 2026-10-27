// hardware-remote through the real module loader.
//   [loopback] OSC send/receive, bundles with time tags, lifecycle: 127.0.0.1 only, always runs.
//   [mdns]     advertise + browse through each discovery backend: needs multicast / the OS DNS-SD
//              service; a backend that cannot see its own advertisement is reported SKIPPED.
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/platform.hpp"
#include "sq/services/hardware_remote/hardware_remote.h"
#include "sq/services/hardware_remote/osc.hpp"
#include "sqtest.hpp"

namespace {

struct Received {
  std::string address;
  std::vector<sq::osc::OwnedArg> args;
  uint64_t timeTag = 0;
  bool timed = false;
  std::string from;
};

class Collector {
 public:
  void push(Received r) {
    std::lock_guard<std::mutex> lk(m_);
    items_.push_back(std::move(r));
    cv_.notify_all();
  }
  bool waitFor(size_t n, int ms) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return items_.size() >= n; });
  }
  std::vector<Received> items() {
    std::lock_guard<std::mutex> lk(m_);
    return items_;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<Received> items_;
};

Received toReceived(const char* address, const sq_osc_arg* args, uint32_t count, const char* from) {
  Received r;
  r.address = address;
  r.from = from ? from : "";
  for (uint32_t k = 0; k < count; ++k) {
    sq::osc::OwnedArg o;
    o.type = args[k].type;
    o.i = args[k].i;
    o.d = args[k].d;
    if (args[k].s) o.s = args[k].s;
    r.args.push_back(o);
  }
  return r;
}

class PlainListener final : public sq::Object<PlainListener, sq::IOscListener> {
 public:
  Collector c;
  void SQ_CALL onOscMessage(const char* address, const sq_osc_arg* args, uint32_t count, const char* fromHost,
                            uint16_t) override {
    c.push(toReceived(address, args, count, fromHost));
  }
};

class TimedListener final : public sq::Object<TimedListener, sq::IOscListener, sq::IOscTimedListener> {
 public:
  Collector c;
  void SQ_CALL onOscMessage(const char* address, const sq_osc_arg* args, uint32_t count, const char* fromHost,
                            uint16_t) override {
    c.push(toReceived(address, args, count, fromHost));  // must not be used when timed is served
  }
  void SQ_CALL onOscTimedMessage(const char* address, const sq_osc_arg* args, uint32_t count, uint64_t timeTag,
                                 const char* fromHost, uint16_t) override {
    Received r = toReceived(address, args, count, fromHost);
    r.timeTag = timeTag;
    r.timed = true;
    c.push(std::move(r));
  }
};

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  bool loaded = false;

  explicit Fixture(const char* discoveryMode = "auto") {
    settings.setString("Services.hardware-remote.OscBindAddress", "127.0.0.1");
    settings.setString("Services.hardware-remote.DiscoveryBackend", discoveryMode);
    std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"),
                                    std::string("hardware-remote") + sq_dylib_suffix());
    loaded = loader.load(path).ok();
  }
};

sq_osc_arg argI(int64_t v) {
  sq_osc_arg a{};
  a.type = 'i';
  a.i = v;
  return a;
}
sq_osc_arg argS(const char* s) {
  sq_osc_arg a{};
  a.type = 's';
  a.s = s;
  return a;
}

}  // namespace

SQ_TEST("hardware-remote [loopback]: OSC message over UDP reaches the listener") {
  Fixture f;
  REQUIRE(f.loaded);
  sq::Ptr<sq::IOscService> osc;
  REQUIRE(f.host.queryService(&sq::IOscService::iid, osc.putVoid()) == SQ_OK);
  REQUIRE(osc);
  auto listener = sq::Ptr<PlainListener>::adopt(new PlainListener());
  uint16_t port = 0;
  uint32_t cookie = 0;
  REQUIRE_OK(osc->listen(0, listener.get(), &port, &cookie));
  REQUIRE(port != 0);
  REQUIRE(cookie != 0);

  sq_osc_arg args[3] = {argI(42), argS("fader"), {}};
  args[2].type = 'f';
  args[2].d = 0.5;
  REQUIRE_OK(osc->send("127.0.0.1", port, "/mixer/track/1/volume", args, 3));
  REQUIRE(listener->c.waitFor(1, 3000));
  auto items = listener->c.items();
  CHECK_EQ(items[0].address, std::string("/mixer/track/1/volume"));
  REQUIRE_EQ(items[0].args.size(), size_t(3));
  CHECK_EQ(items[0].args[0].i, int64_t(42));
  CHECK_EQ(items[0].args[1].s, std::string("fader"));
  CHECK_EQ(items[0].args[2].d, 0.5);
  CHECK_EQ(items[0].from, std::string("127.0.0.1"));

  // Invalid input is rejected before anything is sent.
  CHECK_EQ(osc->send("127.0.0.1", port, "bad address", nullptr, 0), SQ_E_INVALIDARG);
  CHECK_EQ(osc->send(nullptr, port, "/x", nullptr, 0), SQ_E_INVALIDARG);
  CHECK_EQ(osc->send("127.0.0.1", 0, "/x", nullptr, 0), SQ_E_INVALIDARG);
  CHECK_EQ(osc->listen(0, nullptr, nullptr, &cookie), SQ_E_INVALIDARG);

  CHECK_OK(osc->stopListening(cookie));
  CHECK_EQ(osc->stopListening(cookie), SQ_E_NOTFOUND);
  osc.reset();
  CHECK_OK(f.loader.unload("hardware-remote"));
}

SQ_TEST("hardware-remote [loopback]: bundles deliver their time tag to a timed listener") {
  Fixture f;
  REQUIRE(f.loaded);
  auto osc = sq::createInstance<sq::IOscService>(&f.reg, sq::hardware_remote::kOscServiceClassId);
  REQUIRE(osc);
  auto bundles = osc.as<sq::IOscBundleSender>();
  REQUIRE(bundles);
  auto listener = sq::Ptr<TimedListener>::adopt(new TimedListener());
  uint16_t port = 0;
  uint32_t cookie = 0;
  REQUIRE_OK(osc->listen(0, listener.get(), &port, &cookie));

  sq_osc_arg a1 = argI(1), a2 = argS("two");
  sq_osc_message msgs[2] = {{sizeof(sq_osc_message), "/b/one", &a1, 1}, {sizeof(sq_osc_message), "/b/two", &a2, 1}};
  const uint64_t tag = sq::osc::timeTagFromUnixNs(1700000000250000000ull);
  REQUIRE_OK(bundles->sendBundle("127.0.0.1", port, tag, msgs, 2));
  REQUIRE_OK(osc->send("127.0.0.1", port, "/bare", nullptr, 0));
  REQUIRE(listener->c.waitFor(3, 3000));
  auto items = listener->c.items();
  REQUIRE_EQ(items.size(), size_t(3));
  for (const auto& r : items) CHECK(r.timed);
  CHECK_EQ(items[0].address, std::string("/b/one"));
  CHECK_EQ(items[0].timeTag, tag);
  CHECK_EQ(items[1].address, std::string("/b/two"));
  CHECK_EQ(items[1].args[0].s, std::string("two"));
  CHECK_EQ(items[1].timeTag, tag);
  CHECK_EQ(items[2].address, std::string("/bare"));
  CHECK_EQ(items[2].timeTag, uint64_t(SQ_OSC_TIMETAG_IMMEDIATELY));

  sq_osc_message bad = {sizeof(sq_osc_message), "nope", nullptr, 0};
  CHECK_EQ(bundles->sendBundle("127.0.0.1", port, tag, &bad, 1), SQ_E_INVALIDARG);
  sq_osc_message small = {4, "/x", nullptr, 0};
  CHECK_EQ(bundles->sendBundle("127.0.0.1", port, tag, &small, 1), SQ_E_INVALIDARG);
  // Releasing the service object with a live listener stops its thread cleanly.
  bundles.reset();
  osc.reset();
  CHECK_OK(f.loader.unload("hardware-remote"));
}

SQ_TEST("hardware-remote [loopback]: discovery backend selection, 'off' mode and argument checks") {
  {
    Fixture f("auto");
    REQUIRE(f.loaded);
    sq::Ptr<sq::IDiscoveryService> disc;
    REQUIRE(f.host.queryService(&sq::IDiscoveryService::iid, disc.putVoid()) == SQ_OK);
    auto info = disc.as<sq::IDiscoveryBackendInfo>();
    REQUIRE(info);
    char name[64] = {};
    uint32_t needed = 0;
    REQUIRE_OK(info->getBackendName(name, sizeof name, &needed));
    CHECK(std::strcmp(name, "system-dnssd") == 0 || std::strcmp(name, "builtin-mdns") == 0);
    CHECK_EQ(info->getBackendName(name, 2, &needed), SQ_E_BUFFER_TOO_SMALL);
    CHECK_EQ(needed, uint32_t(std::strlen(name) + 1));
#if defined(_WIN32)
    CHECK(std::strcmp(name, "system-dnssd") == 0);  // Windows 10 1809+ always has the API
#endif
    sq_discovery_record r{};
    r.struct_size = sizeof r;
    uint32_t cookie = 0;
    CHECK_EQ(disc->advertise(&r, &cookie), SQ_E_INVALIDARG);  // no name/type/port
    CHECK_EQ(disc->browse("", nullptr, &cookie), SQ_E_INVALIDARG);
    CHECK_EQ(disc->cancel(12345), SQ_E_NOTFOUND);
    disc.reset();
    info.reset();
    CHECK_OK(f.loader.unload("hardware-remote"));
  }
  {
    Fixture f("off");
    REQUIRE(f.loaded);
    auto disc = sq::createInstance<sq::IDiscoveryService>(&f.reg, sq::hardware_remote::kDiscoveryServiceClassId);
    REQUIRE(disc);
    sq_discovery_record r{};
    r.struct_size = sizeof r;
    std::snprintf(r.name, sizeof r.name, "x");
    std::snprintf(r.type, sizeof r.type, "_osc._udp");
    r.port = 1;
    uint32_t cookie = 0;
    CHECK_EQ(disc->advertise(&r, &cookie), SQ_E_UNSUPPORTED);
  }
}

// ---- [mdns] ------------------------------------------------------------------------------
namespace {

class DiscoveryCollector final : public sq::Object<DiscoveryCollector, sq::IDiscoveryListener> {
 public:
  void SQ_CALL onServiceFound(const sq_discovery_record* r) override {
    std::lock_guard<std::mutex> lk(m_);
    found_.push_back(*r);
    cv_.notify_all();
  }
  void SQ_CALL onServiceLost(const sq_discovery_record* r) override {
    std::lock_guard<std::mutex> lk(m_);
    lost_.push_back(*r);
    cv_.notify_all();
  }
  bool waitFound(const std::string& name, int ms, sq_discovery_record* out) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] {
      for (auto& r : found_)
        if (name == r.name) {
          *out = r;
          return true;
        }
      return false;
    });
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<sq_discovery_record> found_, lost_;
};

void advertiseAndBrowse(const char* mode) {
  Fixture f(mode);
  REQUIRE(f.loaded);
  auto disc = sq::createInstance<sq::IDiscoveryService>(&f.reg, sq::hardware_remote::kDiscoveryServiceClassId);
  REQUIRE(disc);
  sq_discovery_record rec{};
  rec.struct_size = sizeof rec;
  std::snprintf(rec.name, sizeof rec.name, "sqtest %s %u", mode, static_cast<unsigned>(sq_process_id()));
  std::snprintf(rec.type, sizeof rec.type, "_sqtest._udp");
  rec.port = 45000 + static_cast<uint16_t>(sq_process_id() % 1000);
  std::snprintf(rec.txt, sizeof rec.txt, "ver=1;mode=%s", mode);
  uint32_t adCookie = 0, brCookie = 0;
  sq_result r = disc->advertise(&rec, &adCookie);
  if (r == SQ_E_IO || r == SQ_E_UNSUPPORTED)
    SQ_SKIP(std::string(mode) + " discovery backend unavailable here (advertise failed)");
  REQUIRE_OK(r);
  auto col = sq::Ptr<DiscoveryCollector>::adopt(new DiscoveryCollector());
  REQUIRE_OK(disc->browse("_sqtest._udp", col.get(), &brCookie));
  sq_discovery_record got{};
  bool seen = col->waitFound(rec.name, 15000, &got);
  CHECK_OK(disc->cancel(brCookie));
  CHECK_OK(disc->cancel(adCookie));
  if (!seen) SQ_SKIP(std::string(mode) + " backend: own advertisement not seen within 15 s (no multicast path?)");
  CHECK_EQ(got.port, rec.port);
  CHECK_EQ(std::string(got.type), std::string("_sqtest._udp"));
  CHECK(std::strstr(got.txt, "ver=1") != nullptr);
  CHECK(got.host[0] != 0);
  disc.reset();
  CHECK_OK(f.loader.unload("hardware-remote"));
}

}  // namespace

SQ_TEST("hardware-remote [mdns]: system DNS-SD backend sees its own advertisement") { advertiseAndBrowse("system"); }

SQ_TEST("hardware-remote [mdns]: builtin mDNS backend sees its own advertisement") { advertiseAndBrowse("builtin"); }
