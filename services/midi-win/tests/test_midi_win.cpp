// midi-win through the real loader: port enumeration, loopback out -> in with scheduled delivery,
// sysex, stop() draining, and WinMM device listing (whatever this machine has).
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/midi.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
struct Rx final : sq::Object<Rx, sq::IMidiInputCallback> {
  std::mutex mu;  // test only: the callback is not on an audio thread here
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
}  // namespace

SQ_TEST("midi-win: loopback out -> in, scheduling, sysex, enumeration") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("midi-win") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    auto drv = sq::createInstance<sq::IMidiDriver>(&reg, sq::midi::kWinMidiDriverClassId);
    REQUIRE(drv);
    REQUIRE_OK(drv->refreshPorts());
    uint32_t n = drv->getPortCount(), winmm = 0;
    for (uint32_t i = 0; i < n; ++i) {
      sq_midi_port_info pi{};
      pi.struct_size = sizeof pi;
      REQUIRE_OK(drv->getPortInfo(i, &pi));
      if (std::strncmp(pi.id, "winmm:", 6) == 0) ++winmm;
    }
    std::printf("midi-win: %u port(s), %u WinMM\n", n, winmm);
    CHECK(n >= 8);

    auto rx = sq::Ptr<Rx>::adopt(new Rx());
    sq::Ptr<sq::IMidiInputPort> in;
    REQUIRE_OK(drv->openInput("loopback:in:0", rx.get(), in.put()));
    sq::Ptr<sq::IMidiOutputPort> out;
    REQUIRE_OK(drv->openOutput("loopback:out:0", out.put()));
    REQUIRE_OK(in->start());

    uint64_t now = sq_time_monotonic_ns();
    static const uint8_t sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
    sq_midi_timed_event ev[4]{};
    ev[0] = sq_midi_timed_event{0, {0x90, 60, 100, 0}, 3, nullptr};
    ev[1] = sq_midi_timed_event{0, {0xB0, 7, 99, 0}, 3, nullptr};
    ev[2] = sq_midi_timed_event{now + 30000000ull, {0x80, 60, 0, 0}, 3, nullptr};  // due in 30 ms
    ev[3] = sq_midi_timed_event{now + 30000000ull, {0, 0, 0, 0}, sizeof sysex, sysex};
    REQUIRE_OK(out->send(ev, 4));
    for (int i = 0; i < 200 && rx->count() < 4; ++i) sq_thread_sleep_ms(5);
    REQUIRE_EQ(rx->count(), size_t(4));
    {
      std::lock_guard<std::mutex> lk(rx->mu);
      CHECK_EQ(rx->got[0].second[0], 0x90);
      CHECK_EQ(rx->got[1].second[1], 7);
      CHECK_EQ(rx->got[2].second[0], 0x80);
      CHECK(rx->got[2].first >= now + 29000000ull);  // not delivered before it was due
      CHECK_EQ(rx->got[3].second.size(), sizeof sysex);
    }
    REQUIRE_OK(in->stop());
    size_t before = rx->count();
    ev[0].time_ns = 0;
    REQUIRE_OK(out->send(ev, 1));
    sq_thread_sleep_ms(50);
    CHECK_EQ(rx->count(), before);  // stopped ports deliver nothing
    CHECK_EQ(drv->openOutput("no:such:port", out.put()), SQ_E_NOTFOUND);
  }
  CHECK(loader.unload("midi-win") == SQ_OK);
}
