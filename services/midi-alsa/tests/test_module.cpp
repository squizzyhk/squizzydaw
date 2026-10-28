// midi-alsa through the real loader.
//   [enum]      class + host service registration, enumeration (0 ports without a sequencer),
//               bad ids rejected, listeners, unload
//   [loopback]  "Midi Through" (snd-seq-dummy) out -> in: short messages, scheduling, sysex,
//               stop() draining. Skipped (77) when the sequencer or a through port is missing.
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/midi.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
const sq_guid kAlsaMidiDriverClassId = sq::guid("{F1142D1E-3EBA-41C6-8802-6BE263B2A885}");

struct Rx final : sq::Object<Rx, sq::IMidiInputCallback> {
  std::mutex mu;  // test only: not an audio thread
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> got;
  void SQ_CALL onMidi(const sq_midi_port_info*, const sq_midi_timed_event* ev, uint32_t n) override {
    std::lock_guard<std::mutex> lk(mu);
    for (uint32_t i = 0; i < n; ++i) {
      std::vector<uint8_t> b = ev[i].data ? std::vector<uint8_t>(ev[i].data, ev[i].data + ev[i].size)
                                          : std::vector<uint8_t>(ev[i].bytes, ev[i].bytes + ev[i].size);
      got.emplace_back(sq_time_monotonic_ns(), b);
    }
  }
  size_t count() {
    std::lock_guard<std::mutex> lk(mu);
    return got.size();
  }
};

struct Listener final : sq::Object<Listener, sq::IMidiDeviceListener> {
  void SQ_CALL onPortsChanged() override {}
};

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  Fixture() {
    std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("midi-alsa") + sq_dylib_suffix());
    REQUIRE(loader.load(mod).ok());
  }
};
}  // namespace

SQ_TEST("midi-alsa [enum]: class, host service, enumeration, bad ids, listeners, unload") {
  Fixture f;
  {
    auto drv = sq::createInstance<sq::IMidiDriver>(&f.reg, kAlsaMidiDriverClassId);
    REQUIRE(drv);
    sq::Ptr<sq::IMidiDriver> svc;
    REQUIRE_OK(f.host.queryService(&sq::IMidiDriver::iid, svc.putVoid()));  // the platform MIDI service
    REQUIRE(svc);
    REQUIRE_OK(drv->refreshPorts());
    const uint32_t n = drv->getPortCount();
    std::printf("midi-alsa: %u port(s)\n", n);
    for (uint32_t i = 0; i < n; ++i) {
      sq_midi_port_info pi{};
      pi.struct_size = sizeof pi;
      REQUIRE_OK(drv->getPortInfo(i, &pi));
      std::printf("  %s\t%s\n", pi.id, pi.name);
      CHECK(std::strncmp(pi.id, "alsa:", 5) == 0);
      CHECK(pi.flags & (SQ_MIDI_PORT_INPUT | SQ_MIDI_PORT_OUTPUT));
    }
    sq_midi_port_info pi{};
    pi.struct_size = sizeof pi;
    CHECK_EQ(drv->getPortInfo(n, &pi), SQ_E_NOTFOUND);
    auto rx = sq::Ptr<Rx>::adopt(new Rx());
    sq::Ptr<sq::IMidiInputPort> in;
    CHECK_EQ(drv->openInput("alsa:in:none:none#1", rx.get(), in.put()), SQ_E_NOTFOUND);
    sq::Ptr<sq::IMidiOutputPort> out;
    CHECK_EQ(drv->openOutput("alsa:out:none:none#1", out.put()), SQ_E_NOTFOUND);
    CHECK(drv->openInput(nullptr, rx.get(), in.put()) < 0);
    auto l = sq::Ptr<Listener>::adopt(new Listener());
    REQUIRE_OK(drv->addListener(l.get()));
    sq_thread_sleep_ms(150);  // watcher runs
    REQUIRE_OK(drv->removeListener(l.get()));
    CHECK_EQ(drv->removeListener(l.get()), SQ_E_NOTFOUND);
  }
  CHECK(f.loader.unload("midi-alsa") == SQ_OK);
}

SQ_TEST("midi-alsa [loopback]: Midi Through out -> in, scheduling, sysex, stop") {
  Fixture f;
  {
    auto drv = sq::createInstance<sq::IMidiDriver>(&f.reg, kAlsaMidiDriverClassId);
    REQUIRE(drv);
    REQUIRE_OK(drv->refreshPorts());
    std::string inId, outId;
    for (uint32_t i = 0; i < drv->getPortCount(); ++i) {
      sq_midi_port_info pi{};
      pi.struct_size = sizeof pi;
      if (drv->getPortInfo(i, &pi) != SQ_OK || !std::strstr(pi.id, "Midi Through")) continue;
      if ((pi.flags & SQ_MIDI_PORT_INPUT) && inId.empty()) inId = pi.id;
      if ((pi.flags & SQ_MIDI_PORT_OUTPUT) && outId.empty()) outId = pi.id;
    }
    if (drv->getPortCount() == 0) SQ_SKIP("no ALSA sequencer here (/dev/snd/seq missing?)");
    if (inId.empty() || outId.empty()) SQ_SKIP("no 'Midi Through' port (snd-seq-dummy not loaded)");
    auto rx = sq::Ptr<Rx>::adopt(new Rx());
    sq::Ptr<sq::IMidiInputPort> in;
    REQUIRE_OK(drv->openInput(inId.c_str(), rx.get(), in.put()));
    sq::Ptr<sq::IMidiOutputPort> out;
    REQUIRE_OK(drv->openOutput(outId.c_str(), out.put()));
    REQUIRE_OK(in->start());
    const uint64_t now = sq_time_monotonic_ns();
    static uint8_t sysex[700];
    for (size_t i = 0; i < sizeof sysex; ++i) sysex[i] = uint8_t(i & 0x7F);
    sysex[0] = 0xF0;
    sysex[sizeof sysex - 1] = 0xF7;
    sq_midi_timed_event ev[4]{};
    ev[0] = sq_midi_timed_event{0, {0x90, 60, 100, 0}, 3, nullptr};
    ev[1] = sq_midi_timed_event{0, {0xE0, 0x00, 0x40, 0}, 3, nullptr};
    ev[2] = sq_midi_timed_event{now + 30000000ull, {0x80, 60, 0, 0}, 3, nullptr};
    ev[3] = sq_midi_timed_event{now + 30000000ull, {0, 0, 0, 0}, sizeof sysex, sysex};
    REQUIRE_OK(out->send(ev, 4));
    for (int i = 0; i < 400 && rx->count() < 4; ++i) sq_thread_sleep_ms(5);
    REQUIRE_EQ(rx->count(), size_t(4));
    {
      std::lock_guard<std::mutex> lk(rx->mu);
      CHECK(rx->got[0].second == std::vector<uint8_t>({0x90, 60, 100}));
      CHECK(rx->got[1].second == std::vector<uint8_t>({0xE0, 0x00, 0x40}));
      CHECK(rx->got[2].second == std::vector<uint8_t>({0x80, 60, 0}));
      CHECK(rx->got[2].first >= now + 29000000ull);
      CHECK(rx->got[3].second == std::vector<uint8_t>(sysex, sysex + sizeof sysex));
    }
    REQUIRE_OK(in->stop());
    const size_t before = rx->count();
    ev[0].time_ns = 0;
    REQUIRE_OK(out->send(ev, 1));
    sq_thread_sleep_ms(50);
    CHECK_EQ(rx->count(), before);
  }
  CHECK(f.loader.unload("midi-alsa") == SQ_OK);
}
