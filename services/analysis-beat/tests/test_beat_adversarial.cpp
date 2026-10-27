// analysis-beat: hostile inputs through the IAnalyzer classes (non-finite sample rates and samples).
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "dsp_test_signals.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/interfaces/dsp.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"
#include "tuner_core.hpp"

namespace {

std::string modulePath(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}

double tempoOf(sq::IAnalysisResult* r) {
  for (uint32_t i = 0; i < r->getEventCount(); ++i) {
    sq_analysis_event e{};
    e.struct_size = sizeof e;
    if (r->getEvent(i, &e) == SQ_OK && e.type == SQ_ANALYSIS_TEMPO) return e.value;
  }
  return 0.0;
}

}  // namespace

SQ_TEST("analysis-beat adversarial: non-finite or absurd sample rates are rejected (no hang)") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath("analysis-beat")).ok());
  {
    for (const sq_guid* id : {&sq::dsp::kBeatAnalyzerClassId, &sq::dsp::kTunerClassId}) {
      auto an = sq::createInstance<sq::IAnalyzer>(&reg, *id);
      REQUIRE(an);
      CHECK(an->prepare(std::numeric_limits<double>::infinity(), 1, 256, nullptr) == SQ_E_INVALIDARG);
      CHECK(an->prepare(std::nan(""), 1, 256, nullptr) == SQ_E_INVALIDARG);
      CHECK(an->prepare(1.0e300, 1, 256, nullptr) == SQ_E_INVALIDARG);
      CHECK(an->prepare(44100.0, 0, 256, nullptr) == SQ_E_INVALIDARG);
      CHECK(an->prepare(44100.0, 1, 256, nullptr) == SQ_OK);
      // malformed blocks: no channel array, a null channel, an empty block without channels
      float buf[16] = {};
      float* withNull[2] = {buf, nullptr};
      sq_audio_block noArray{nullptr, 2, 16, 0};
      sq_audio_block nullCh{withNull, 2, 16, 0};
      sq_audio_block empty{nullptr, 2, 0, 0};
      CHECK(an->process(&noArray) == SQ_E_INVALIDARG);
      CHECK(an->process(&nullCh) == SQ_E_INVALIDARG);
      CHECK(an->process(&empty) == SQ_OK);
      CHECK(an->process(nullptr) == SQ_E_INVALIDARG);
    }
    sq::tuner::PitchTracker t;
    CHECK(!t.prepare(std::numeric_limits<double>::infinity(), 256));
  }
  CHECK(loader.unload("analysis-beat") == SQ_OK);
}

SQ_TEST("analysis-beat adversarial: a few NaN/inf samples do not destroy the tempo") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath("analysis-beat")).ok());
  {
    auto an = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kBeatAnalyzerClassId);
    REQUIRE(an);
    REQUIRE(an->prepare(44100.0, 1, 1024, nullptr) == SQ_OK);
    std::vector<float> x = sqdsptest::clickTrack(120.0, 44100.0, 12.0);
    x[44100 * 3 + 17] = std::numeric_limits<float>::quiet_NaN();
    x[44100 * 7 + 5] = std::numeric_limits<float>::infinity();
    for (size_t pos = 0; pos < x.size(); pos += 1024) {
      float* ch[1] = {x.data() + pos};
      sq_audio_block b{ch, 1, static_cast<uint32_t>(std::min<size_t>(1024, x.size() - pos)), 0};
      REQUIRE(an->process(&b) == SQ_OK);
    }
    sq::Ptr<sq::IAnalysisResult> res;
    REQUIRE(an->finish(res.put()) == SQ_OK);
    const double bpm = tempoOf(res.get());
    std::printf("  bpm with NaN/inf samples: %.3f\n", bpm);
    CHECK(std::fabs(bpm - 120.0) < 0.5);
    for (uint32_t i = 0; i < res->getEventCount(); ++i) {
      sq_analysis_event e{};
      e.struct_size = sizeof e;
      REQUIRE(res->getEvent(i, &e) == SQ_OK);
      CHECK(std::isfinite(e.time) && std::isfinite(e.value));
    }
  }
  CHECK(loader.unload("analysis-beat") == SQ_OK);
}

SQ_TEST("analysis-beat adversarial: tuner never reports a non-finite pitch") {
  sq::tuner::PitchTracker t;
  REQUIRE(t.prepare(44100.0, 128));
  std::vector<float> x = sqdsptest::sine(220.0, 44100.0, 22050, 0.3f);
  for (size_t i = 0; i < x.size(); i += 97) x[i] = std::numeric_limits<float>::quiet_NaN();
  for (size_t pos = 0; pos < x.size(); pos += 128) t.push(x.data() + pos, static_cast<uint32_t>(std::min<size_t>(128, x.size() - pos)));
  const sq::tuner::Estimate e = t.latest();
  std::printf("  tuner with NaNs: %g Hz conf %g\n", e.frequency, e.confidence);
  CHECK(std::isfinite(e.frequency));
  CHECK(std::isfinite(e.confidence));
}
