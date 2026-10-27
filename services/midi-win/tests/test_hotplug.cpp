// midi-win hot-plug: the pure port diff, the watcher thread on its own, and the real driver
// noticing ports that appear/disappear (the loopback cable count is a setting, so changing it is
// a hardware-free stand-in for plugging a device in or out).
#include <atomic>
#include <cstring>

#include "hotplug.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/interfaces/midi.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

using sq::midiwin::diffPortIds;

SQ_TEST("midi-win hotplug: diff of port id snapshots") {
  auto d = diffPortIds({"a", "b", "c"}, {"b", "c", "d", "e"});
  REQUIRE_EQ(d.added.size(), size_t(2));
  CHECK(d.added[0] == "d");
  CHECK(d.added[1] == "e");
  REQUIRE_EQ(d.removed.size(), size_t(1));
  CHECK(d.removed[0] == "a");

  CHECK(diffPortIds({"x", "y"}, {"y", "x"}).empty());  // order does not matter
  CHECK(diffPortIds({}, {}).empty());
  auto all = diffPortIds({}, {"p", "p"});  // duplicates count once
  CHECK_EQ(all.added.size(), size_t(1));
  auto gone = diffPortIds({"p", "q"}, {});
  CHECK_EQ(gone.removed.size(), size_t(2));
  CHECK(gone.added.empty());
}

namespace {
std::atomic<int> g_refreshes{0};
void countRefresh(void*) { g_refreshes.fetch_add(1); }

bool waitFor(const std::atomic<int>& v, int atLeast, int ms) {
  for (int i = 0; i < ms / 5 && v.load() < atLeast; ++i) sq_thread_sleep_ms(5);
  return v.load() >= atLeast;
}
}  // namespace

SQ_TEST("midi-win hotplug: watcher polls, reacts to signals, stops cleanly") {
  {
    sq::midiwin::HotplugWatcher w(&countRefresh, nullptr);
    g_refreshes = 0;
    REQUIRE_OK(w.start(20));
    CHECK(w.running());
    CHECK(waitFor(g_refreshes, 3, 2000));  // fallback polling
    w.stop();
    CHECK(!w.running());
    int after = g_refreshes.load();
    sq_thread_sleep_ms(80);
    CHECK_EQ(g_refreshes.load(), after);  // nothing runs after stop()
  }
  {
    sq::midiwin::HotplugWatcher w(&countRefresh, nullptr);
    g_refreshes = 0;
    REQUIRE_OK(w.start(0));  // notifications only
    sq_thread_sleep_ms(60);
    CHECK_EQ(g_refreshes.load(), 0);
    for (int i = 0; i < 5; ++i) w.signal();  // a burst collapses into one refresh after settling
    CHECK(waitFor(g_refreshes, 1, 2000));
    sq_thread_sleep_ms(sq::midiwin::HotplugWatcher::kSettleMs + 100);
    CHECK_EQ(g_refreshes.load(), 1);
    std::printf("midi-win hotplug: OS device notifications %s\n", w.osNotifications() ? "active" : "unavailable (polling only)");
  }
}

namespace {
struct Listener final : sq::Object<Listener, sq::IMidiDeviceListener> {
  std::atomic<int> changes{0};
  void SQ_CALL onPortsChanged() override { changes.fetch_add(1); }
};

bool hasPort(sq::IMidiDriver* d, const char* id) {
  for (uint32_t i = 0; i < d->getPortCount(); ++i) {
    sq_midi_port_info pi{};
    pi.struct_size = sizeof pi;
    if (d->getPortInfo(i, &pi) == SQ_OK && std::strcmp(pi.id, id) == 0) return true;
  }
  return false;
}
}  // namespace

SQ_TEST("midi-win hotplug: driver notifies listeners when ports appear and disappear") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  settings.setInt("Services.midi-win.HotplugPollMs", 20);
  settings.setInt("Services.midi-win.LoopbackPorts", 4);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("midi-win") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    auto drv = sq::createInstance<sq::IMidiDriver>(&reg, sq::midi::kWinMidiDriverClassId);
    REQUIRE(drv);
    auto l = sq::Ptr<Listener>::adopt(new Listener());
    REQUIRE_OK(drv->addListener(l.get()));
    const uint32_t base = drv->getPortCount();
    CHECK(hasPort(drv.get(), "loopback:in:3"));
    CHECK(!hasPort(drv.get(), "loopback:in:5"));

    sq_thread_sleep_ms(150);  // several poll cycles without a change: no notification
    CHECK_EQ(l->changes.load(), 0);

    settings.setInt("Services.midi-win.LoopbackPorts", 6);  // "plug in" two in/out pairs
    CHECK(waitFor(l->changes, 1, 3000));
    CHECK_EQ(drv->getPortCount(), base + 4);
    CHECK(hasPort(drv.get(), "loopback:in:5"));
    {
      struct Rx final : sq::Object<Rx, sq::IMidiInputCallback> {
        void SQ_CALL onMidi(const sq_midi_port_info*, const sq_midi_timed_event*, uint32_t) override {}
      };
      auto rx = sq::Ptr<Rx>::adopt(new Rx());
      sq::Ptr<sq::IMidiInputPort> in;
      CHECK_OK(drv->openInput("loopback:in:5", rx.get(), in.put()));  // the new port is usable
    }

    const int beforeUnplug = l->changes.load();
    settings.setInt("Services.midi-win.LoopbackPorts", 2);  // "unplug" four pairs
    CHECK(waitFor(l->changes, beforeUnplug + 1, 3000));
    CHECK_EQ(drv->getPortCount(), base - 4);
    CHECK(!hasPort(drv.get(), "loopback:in:3"));
    sq::Ptr<sq::IMidiOutputPort> out;
    CHECK_EQ(drv->openOutput("loopback:out:3", out.put()), SQ_E_NOTFOUND);

    // Without listeners the watcher stops: later changes are only seen by refreshPorts().
    REQUIRE_OK(drv->removeListener(l.get()));
    CHECK_EQ(drv->removeListener(l.get()), SQ_E_NOTFOUND);
    const int stopped = l->changes.load();
    settings.setInt("Services.midi-win.LoopbackPorts", 4);
    sq_thread_sleep_ms(150);
    CHECK_EQ(l->changes.load(), stopped);
    REQUIRE_OK(drv->refreshPorts());
    CHECK_EQ(drv->getPortCount(), base);
  }
  CHECK(loader.unload("midi-win") == SQ_OK);
}
