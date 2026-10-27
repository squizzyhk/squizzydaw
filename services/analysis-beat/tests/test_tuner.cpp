// Tuner: pitch accuracy on synthesised tones, silence/noise rejection, realtime service API.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "dsp_test_signals.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/dsp.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"
#include "tuner_core.hpp"

namespace {

constexpr double kSr = 44100.0;

double centsOff(double f, double target) { return 1200.0 * std::log2(f / target); }

sq::tuner::Estimate track(const std::vector<float>& x, uint32_t block = 128) {
  sq::tuner::PitchTracker t;
  if (!t.prepare(kSr, block)) return {};
  for (size_t pos = 0; pos < x.size(); pos += block)
    t.push(x.data() + pos, static_cast<uint32_t>(std::min<size_t>(block, x.size() - pos)));
  return t.latest();
}

std::string modulePath(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}

}  // namespace

SQ_TEST("tuner: pure tones within 1 cent") {
  for (double f : {41.2, 82.41, 110.0, 196.0, 440.0, 1000.0, 1567.98}) {
    auto e = track(sqdsptest::sine(f, kSr, 22050, 0.3f));
    std::printf("  %.2f Hz -> %.3f Hz (%+.3f cents, conf %.2f)\n", f, e.frequency, centsOff(e.frequency, f), e.confidence);
    REQUIRE(e.frequency > 0.0);
    CHECK(std::fabs(centsOff(e.frequency, f)) < 1.0);
    CHECK(e.confidence > 0.8f);
  }
}

SQ_TEST("tuner: harmonic-rich tones report the fundamental") {
  for (double f : {55.0, 110.0, 146.83, 329.63}) {
    std::vector<float> x(22050, 0.0f);
    sqdsptest::addTone(x, f, kSr, 0, x.size(), 0.3f, 12);
    auto e = track(x);
    std::printf("  saw %.2f Hz -> %.3f Hz (%+.3f cents)\n", f, e.frequency, centsOff(e.frequency, f));
    CHECK(std::fabs(centsOff(e.frequency, f)) < 2.0);
  }
  // missing fundamental is still heard at the fundamental (harmonics 2..6 of 100 Hz)
  std::vector<float> x(22050, 0.0f);
  for (int h = 2; h <= 6; ++h) sqdsptest::addTone(x, 100.0 * h, kSr, 0, x.size(), 0.1f, 1);
  auto e = track(x);
  CHECK(std::fabs(centsOff(e.frequency, 100.0)) < 3.0);
}

SQ_TEST("tuner: silence and white noise give no pitch") {
  std::vector<float> s(22050, 0.0f);
  CHECK_EQ(track(s).frequency, 0.0);
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> d(-0.3f, 0.3f);
  for (float& v : s) v = d(rng);
  auto e = track(s);
  CHECK(e.frequency == 0.0 || e.confidence < 0.8f);
}

SQ_TEST("tuner service: realtime value, cents and reference pitch") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath("analysis-beat")).ok());
  {
    auto tuner = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kTunerClassId);
    REQUIRE(tuner);
    sq_analyzer_info info{};
    info.struct_size = sizeof info;
    REQUIRE(tuner->getInfo(&info) == SQ_OK);
    CHECK_EQ(std::string(info.kind), std::string("tuner"));
    CHECK(info.flags & SQ_ANALYZER_REALTIME);
    sq_tuner_value v{};
    v.struct_size = sizeof v;
    CHECK(tuner->getRealtimeValue(&v) == SQ_OK);  // nothing yet: frequency 0
    CHECK_EQ(v.frequency_hz, 0.0);

    for (double ref : {440.0, 442.0}) {
      auto opts = sq::makeAttributes();
      opts->setDouble("reference_hz", ref);
      REQUIRE(tuner->prepare(48000.0, 2, 256, opts.get()) == SQ_OK);
      const double f = 440.0 * std::pow(2.0, 30.0 / 1200.0);  // A4 + 30 cents (re 440)
      auto x = sqdsptest::sine(f, 48000.0, 24000, 0.4f);
      for (size_t pos = 0; pos < x.size(); pos += 256) {
        float* ch[2] = {x.data() + pos, x.data() + pos};
        sq_audio_block b{ch, 2, 256, 0};
        REQUIRE(tuner->process(&b) == SQ_OK);
      }
      REQUIRE(tuner->getRealtimeValue(&v) == SQ_OK);
      const double expectCents = 30.0 - 1200.0 * std::log2(ref / 440.0);
      std::printf("  ref %.0f: %.3f Hz, note %d, %+.2f cents (expect %+.2f)\n", ref, v.frequency_hz, v.midi_note, v.cents,
                  expectCents);
      CHECK_EQ(v.midi_note, 69);
      CHECK(std::fabs(v.cents - expectCents) < 1.0);
    }
    sq::Ptr<sq::IAnalysisResult> res;
    CHECK(tuner->finish(res.put()) == SQ_E_UNSUPPORTED);
    tuner->reset();
    REQUIRE(tuner->getRealtimeValue(&v) == SQ_OK);
    CHECK_EQ(v.frequency_hz, 0.0);
  }
  CHECK(loader.unload("analysis-beat") == SQ_OK);
}
