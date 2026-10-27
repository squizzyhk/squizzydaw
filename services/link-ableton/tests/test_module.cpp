// link-ableton through the real module loader: singleton registration, ITempoSync semantics of the
// local timeline, argument checks, unload.
#include <cmath>
#include <cstring>
#include <string>

#include "sq/app/module_loader.hpp"
#include "sq/platform.hpp"
#include "sq/services/link_ableton/link_ableton.h"
#include "sqtest.hpp"

namespace {
constexpr uint64_t kSec = 1000000000ull;
bool approx(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  bool loaded = false;
  explicit Fixture(const char* backend) {
    settings.setString("Services.link-ableton.Backend", backend);
    std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"),
                                    std::string("link-ableton") + sq_dylib_suffix());
    loaded = loader.load(path).ok();
  }
};
}  // namespace

SQ_TEST("link-ableton [local]: local timeline through ITempoSync") {
  Fixture f("local");
  REQUIRE(f.loaded);
  sq::Ptr<sq::ITempoSync> ts;
  REQUIRE(f.host.queryService(&sq::ITempoSync::iid, ts.putVoid()) == SQ_OK);
  REQUIRE(ts);
  auto info = ts.as<sq::ITempoSyncInfo>();
  REQUIRE(info);
  char name[32];
  REQUIRE_OK(info->getBackendName(name, sizeof name, nullptr));
  CHECK_EQ(std::string(name), std::string("local-timeline"));

  CHECK_EQ(ts->isEnabled(), SQ_FALSE);
  CHECK_OK(ts->enable(SQ_TRUE));
  CHECK_EQ(ts->isEnabled(), SQ_TRUE);

  const uint64_t t0 = sq_time_monotonic_ns();
  CHECK_OK(ts->setTempo(150.0, t0));
  CHECK_OK(ts->setQuantum(4.0));
  CHECK_OK(ts->setPlaying(SQ_TRUE, t0));
  sq_tempo_sync_state a{}, b{};
  REQUIRE_OK(ts->captureState(t0, &a));
  REQUIRE_OK(ts->captureState(t0 + 2 * kSec, &b));
  CHECK(approx(a.bpm, 150.0));
  CHECK(approx(b.beat - a.beat, 5.0));  // 2 s at 150 bpm
  CHECK_EQ(b.playing, SQ_TRUE);
  CHECK(b.phase >= 0.0 && b.phase < 4.0);
  CHECK(approx(std::fmod(b.beat - b.phase, 4.0), 0.0, 1e-6) || approx(std::fmod(b.beat - b.phase, 4.0), 4.0, 1e-6) ||
        approx(std::fmod(b.beat - b.phase, 4.0), -4.0, 1e-6));
  CHECK_EQ(b.peers, 0u);

  CHECK_EQ(ts->captureState(t0, nullptr), SQ_E_INVALIDARG);
  CHECK_EQ(ts->setTempo(-1.0, t0), SQ_E_INVALIDARG);
  CHECK_EQ(ts->setTempo(std::nan(""), t0), SQ_E_INVALIDARG);
  CHECK_EQ(ts->setQuantum(0.0), SQ_E_INVALIDARG);
  CHECK_OK(ts->enable(SQ_FALSE));

  ts.reset();
  info.reset();
  CHECK_OK(f.loader.unload("link-ableton"));
}

SQ_TEST("link-ableton [local]: 'auto' picks the SDK session when built in, else the local timeline") {
  Fixture f("auto");
  REQUIRE(f.loaded);
  auto ts = sq::createInstance<sq::ITempoSync>(&f.reg, sq::tempo_sync::kTempoSyncClassId);
  REQUIRE(ts);
  auto info = ts.as<sq::ITempoSyncInfo>();
  REQUIRE(info);
  char name[32];
  REQUIRE_OK(info->getBackendName(name, sizeof name, nullptr));
  if (info->hasNetworkBackend())
    CHECK_EQ(std::string(name), std::string("sdk-session"));
  else
    CHECK_EQ(std::string(name), std::string("local-timeline"));
  uint32_t needed = 0;
  CHECK_EQ(info->getBackendName(name, 3, &needed), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(needed, uint32_t(std::strlen(name) + 1));
}

// Separate CTest entry: Skipped unless built with SQ_LINK_SDK_DIR. Never joins the network:
// the session stays disabled (enable(SQ_FALSE)), only the local session state is exercised.
SQ_TEST("link-ableton [sdk]: SDK session tempo, beat and play state through ITempoSync") {
  Fixture f("sdk");
  REQUIRE(f.loaded);
  sq::Ptr<sq::ITempoSync> ts;
  REQUIRE(f.host.queryService(&sq::ITempoSync::iid, ts.putVoid()) == SQ_OK);
  auto info = ts.as<sq::ITempoSyncInfo>();
  REQUIRE(info);
  if (!info->hasNetworkBackend()) SQ_SKIP("built without SQ_LINK_SDK_DIR: no SDK session backend");
  char name[32];
  REQUIRE_OK(info->getBackendName(name, sizeof name, nullptr));
  CHECK_EQ(std::string(name), std::string("sdk-session"));
  CHECK_OK(ts->enable(SQ_FALSE));
  CHECK_OK(ts->setQuantum(4.0));
  const uint64_t t0 = sq_time_monotonic_ns();
  CHECK_OK(ts->setTempo(133.0, t0));
  CHECK_OK(ts->setPlaying(SQ_TRUE, t0));
  // App-side commits reach the audio-side state asynchronously: poll briefly.
  sq_tempo_sync_state a{};
  bool seen = false;
  for (int i = 0; i < 100 && !seen; ++i) {
    REQUIRE_OK(ts->captureState(sq_time_monotonic_ns(), &a));
    seen = approx(a.bpm, 133.0, 1e-6) && a.playing == SQ_TRUE;
    if (!seen) sq_thread_sleep_ms(10);
  }
  REQUIRE(seen);
  sq_tempo_sync_state b{}, c{};
  const uint64_t t1 = sq_time_monotonic_ns();
  REQUIRE_OK(ts->captureState(t1, &b));
  REQUIRE_OK(ts->captureState(t1 + 3 * kSec, &c));
  CHECK(approx(c.beat - b.beat, 133.0 / 20.0, 1e-3));  // 3 s at 133 bpm
  CHECK(c.phase >= 0.0 && c.phase < 4.0);
  CHECK(approx(c.quantum, 4.0));
  CHECK_EQ(c.peers, 0u);
  CHECK_EQ(c.host_time_ns, t1 + 3 * kSec);
  ts.reset();
  info.reset();
  CHECK_OK(f.loader.unload("link-ableton"));
}
