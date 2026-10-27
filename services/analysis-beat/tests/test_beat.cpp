// analysis-beat: synthesised click tracks -> tempo, beats, downbeats, onsets; service end to end.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "beat_core.hpp"
#include "dsp_test_signals.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/dsp.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {

constexpr double kSr = 44100.0;

std::vector<double> clickTimes(double bpm, double seconds, double offset) {
  std::vector<double> t;
  for (double x = offset; x < seconds; x += 60.0 / bpm) t.push_back(x);
  return t;
}

double nearestDistance(const std::vector<double>& ref, double t) {
  double best = 1e9;
  for (double r : ref) best = std::min(best, std::fabs(r - t));
  return best;
}

void checkClickTrack(double bpm, double offset, float noise) {
  const double secs = 20.0;
  auto audio = sqdsptest::clickTrack(bpm, kSr, secs, offset, 4, 7);
  if (noise > 0.0f) {
    std::mt19937 rng(3);
    std::normal_distribution<float> d(0.0f, noise);
    for (float& v : audio) v += d(rng);
  }
  const auto clicks = clickTimes(bpm, secs, offset);
  auto r = sq::beat::analyze(audio, kSr);
  std::printf("  %.1f BPM (offset %.2f, noise %.3f): detected %.3f BPM, conf %.2f, %zu beats, %zu onsets\n", bpm, offset,
              noise, r.bpm, r.tempoConfidence, r.beats.size(), r.onsets.size());
  CHECK(std::fabs(r.bpm - bpm) < 0.5);
  // beats sit on clicks
  int onClick = 0;
  for (const auto& b : r.beats) onClick += nearestDistance(clicks, b.time) < 0.03;
  CHECK(onClick >= static_cast<int>(r.beats.size()) - 1);
  CHECK(r.beats.size() + 2 >= clicks.size());
  // downbeats on the accented clicks (every 4th from the first)
  int downOk = 0, downs = 0;
  for (const auto& b : r.beats)
    if (b.indexInBar == 1) {
      ++downs;
      const double k = (b.time - offset) / (60.0 / bpm);
      const long idx = std::lround(k);
      downOk += (idx % 4) == 0;
    }
  CHECK(downs > 0);
  CHECK(downOk >= downs - 1);
  // onsets: one per click, accurate to 15 ms
  int matched = 0;
  for (double c : clicks) {
    double best = 1e9;
    for (const auto& o : r.onsets) best = std::min(best, std::fabs(o.time - c));
    matched += best < 0.015;
  }
  CHECK(matched >= static_cast<int>(clicks.size()) - 1);
  CHECK(r.onsets.size() <= clicks.size() + 2);
}

std::string modulePath(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}

}  // namespace

SQ_TEST("beat: 120 BPM click track") { checkClickTrack(120.0, 0.25, 0.0f); }
SQ_TEST("beat: 97 BPM click track") { checkClickTrack(97.0, 0.1, 0.0f); }
SQ_TEST("beat: 140 BPM click track with noise") { checkClickTrack(140.0, 0.5, 0.01f); }
SQ_TEST("beat: 75 BPM click track") { checkClickTrack(75.0, 0.0, 0.0f); }

SQ_TEST("beat: silence gives nothing") {
  std::vector<float> s(static_cast<size_t>(kSr * 3), 0.0f);
  auto r = sq::beat::analyze(s, kSr);
  CHECK(r.beats.empty());
  CHECK(r.onsets.empty());
  CHECK_EQ(r.bpm, 0.0);
}

SQ_TEST("analysis-beat service: beat analyzer end to end") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath("analysis-beat")).ok());
  {
    auto an = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kBeatAnalyzerClassId);
    REQUIRE(an);
    sq_analyzer_info info{};
    info.struct_size = sizeof info;
    REQUIRE(an->getInfo(&info) == SQ_OK);
    CHECK_EQ(std::string(info.kind), std::string("beat"));
    auto opts = sq::makeAttributes();
    opts->setInt("beats_per_bar", 3);
    const double sr = 48000.0;
    REQUIRE(an->prepare(sr, 1, 512, opts.get()) == SQ_OK);
    auto audio = sqdsptest::clickTrack(110.0, sr, 15.0, 0.2, 3, 5);
    for (size_t pos = 0; pos < audio.size(); pos += 512) {
      uint32_t n = static_cast<uint32_t>(std::min<size_t>(512, audio.size() - pos));
      float* ch[1] = {audio.data() + pos};
      sq_audio_block b{ch, 1, n, static_cast<sq_sample_time>(pos)};
      REQUIRE(an->process(&b) == SQ_OK);
    }
    sq::Ptr<sq::IAnalysisResult> res;
    REQUIRE(an->finish(res.put()) == SQ_OK);
    sq_analysis_event e{};
    e.struct_size = sizeof e;
    REQUIRE(res->getEvent(0, &e) == SQ_OK);
    CHECK_EQ(e.type, SQ_ANALYSIS_TEMPO);
    CHECK(std::fabs(e.value - 110.0) < 0.5);
    int beats = 0, onsets = 0, downbeats = 0, downOk = 0;
    double lastTime = -1.0;
    for (uint32_t i = 1; i < res->getEventCount(); ++i) {
      REQUIRE(res->getEvent(i, &e) == SQ_OK);
      CHECK(e.time >= lastTime);
      lastTime = e.time;
      if (e.type == SQ_ANALYSIS_BEAT) {
        ++beats;
        CHECK(e.value >= 1.0 && e.value <= 3.0);
        if (e.value == 1.0) {
          ++downbeats;
          downOk += std::lround((e.time - 0.2) / (60.0 / 110.0)) % 3 == 0;
        }
      } else if (e.type == SQ_ANALYSIS_ONSET) {
        ++onsets;
      }
    }
    CHECK(beats >= 25);
    CHECK(onsets >= 25);
    CHECK(downbeats >= 8);
    CHECK(downOk >= downbeats - 1);
    sq_tuner_value tv{};
    tv.struct_size = sizeof tv;
    CHECK(an->getRealtimeValue(&tv) == SQ_E_UNSUPPORTED);
  }
  CHECK(loader.unload("analysis-beat") == SQ_OK);
}
