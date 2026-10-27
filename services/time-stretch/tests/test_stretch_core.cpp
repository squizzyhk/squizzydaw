// time-stretch core: lengths, pitch accuracy, identity reconstruction, streaming behaviour.
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp_test_signals.hpp"
#include "sqtest.hpp"
#include "stretch_core.hpp"

namespace {

using sq::stretch::Algorithm;
using sq::stretch::Settings;
using sq::stretch::Stretcher;

constexpr double kSr = 44100.0;

// Offline run: push everything in blocks, flush, pull until drained. Returns per-channel output.
std::vector<std::vector<float>> runOffline(Stretcher& st, const std::vector<std::vector<float>>& in, uint32_t block) {
  const uint32_t C = static_cast<uint32_t>(in.size());
  const size_t n = in[0].size();
  std::vector<std::vector<float>> out(C);
  std::vector<std::vector<float>> tmp(C, std::vector<float>(block));
  std::vector<float*> tp(C);
  for (uint32_t c = 0; c < C; ++c) tp[c] = tmp[c].data();
  auto drain = [&] {
    for (;;) {
      uint32_t got = st.pull(tp.data(), block);
      for (uint32_t c = 0; c < C; ++c) out[c].insert(out[c].end(), tmp[c].begin(), tmp[c].begin() + got);
      if (got < block) break;
    }
  };
  for (size_t pos = 0; pos < n; pos += block) {
    uint32_t len = static_cast<uint32_t>(std::min<size_t>(block, n - pos));
    std::vector<const float*> ip(C);
    for (uint32_t c = 0; c < C; ++c) ip[c] = in[c].data() + pos;
    if (!st.push(ip.data(), len)) {
      drain();
      REQUIRE(st.push(ip.data(), len));
    }
    drain();
  }
  st.flush();
  for (int guard = 0; guard < 100000 && !st.drained(); ++guard) drain();
  return out;
}

Settings settingsFor(Algorithm a, uint32_t channels = 1) {
  Settings s;
  s.sampleRate = kSr;
  s.channels = channels;
  s.maxBlock = 512;
  s.quality = a == Algorithm::PhaseVocoder ? SQ_STRETCH_QUALITY_HIGH : SQ_STRETCH_QUALITY_STANDARD;
  s.algorithm = a;
  s.maxTimeRatio = 4.0;
  return s;
}

const char* nameOf(Algorithm a) { return a == Algorithm::Wsola ? "wsola" : "pv"; }

}  // namespace

SQ_TEST("stretch: ratio 1 reconstructs the input (both algorithms)") {
  for (Algorithm a : {Algorithm::Wsola, Algorithm::PhaseVocoder}) {
    Stretcher st;
    REQUIRE(st.configure(settingsFor(a)));
    std::vector<float> x(20000);
    sqdsptest::addTone(x, 220.0, kSr, 0, x.size(), 0.3f, 6);
    sqdsptest::addTone(x, 331.0, kSr, 0, x.size(), 0.2f, 3);
    auto out = runOffline(st, {x}, 333);
    CHECK_EQ(out[0].size(), x.size());
    double err = 0.0;
    for (size_t i = 0; i < std::min(out[0].size(), x.size()); ++i) if (std::fabs(out[0][i] - x[i]) > err) err = std::fabs(out[0][i] - x[i]);
    std::printf("  %s identity max error %.2e\n", nameOf(a), err);
    CHECK(err < 1e-3);
  }
}

SQ_TEST("stretch: time ratios keep pitch and scale length") {
  const std::vector<float> x = sqdsptest::sine(440.0, kSr, 44100, 0.5f);
  for (Algorithm a : {Algorithm::Wsola, Algorithm::PhaseVocoder})
    for (double ratio : {0.5, 0.8, 1.25, 2.0, 3.0}) {
      Stretcher st;
      REQUIRE(st.configure(settingsFor(a)));
      st.setRatios(ratio, 0.0);
      auto out = runOffline(st, {x}, 512);
      const double expect = x.size() * ratio;
      const double f = sqdsptest::zeroCrossingFreq(out[0], kSr, out[0].size() / 8, out[0].size() * 7 / 8);
      const double r = sqdsptest::rms(out[0], out[0].size() / 8, out[0].size() * 7 / 8);
      std::printf("  %s ratio %.2f: len %zu (expect %.0f), f %.2f Hz, rms %.3f\n", nameOf(a), ratio, out[0].size(), expect, f, r);
      CHECK(std::fabs(out[0].size() - expect) <= 2.0);
      CHECK(std::fabs(f - 440.0) < 2.0);
      CHECK(r > 0.30 && r < 0.42);  // sine rms 0.354
    }
}

SQ_TEST("stretch: pitch shift changes frequency, keeps length") {
  const std::vector<float> x = sqdsptest::sine(440.0, kSr, 44100, 0.5f);
  for (Algorithm a : {Algorithm::Wsola, Algorithm::PhaseVocoder})
    for (double semis : {12.0, -12.0, 7.0, -5.0, 0.5}) {
      Stretcher st;
      REQUIRE(st.configure(settingsFor(a)));
      st.setRatios(1.0, semis);
      auto out = runOffline(st, {x}, 512);
      const double target = 440.0 * std::pow(2.0, semis / 12.0);
      const double f = sqdsptest::zeroCrossingFreq(out[0], kSr, out[0].size() / 8, out[0].size() * 7 / 8);
      const double r = sqdsptest::rms(out[0], out[0].size() / 8, out[0].size() * 7 / 8);
      const double cents = 1200.0 * std::log2(f / target);
      std::printf("  %s pitch %+.1f st: len %zu, f %.2f Hz (target %.2f, %+.2f cents), rms %.3f\n", nameOf(a), semis,
                  out[0].size(), f, target, cents, r);
      CHECK(std::fabs(double(out[0].size()) - double(x.size())) <= 2.0);
      CHECK(std::fabs(cents) < 5.0);
      CHECK(r > 0.28 && r < 0.42);
    }
}

SQ_TEST("stretch: combined stretch + pitch") {
  const std::vector<float> x = sqdsptest::sine(330.0, kSr, 30000, 0.5f);
  Stretcher st;
  REQUIRE(st.configure(settingsFor(Algorithm::PhaseVocoder)));
  st.setRatios(1.5, 3.0);
  auto out = runOffline(st, {x}, 256);
  CHECK(std::fabs(double(out[0].size()) - 45000.0) <= 2.0);
  const double f = sqdsptest::zeroCrossingFreq(out[0], kSr, 5000, 40000);
  CHECK(std::fabs(1200.0 * std::log2(f / (330.0 * std::pow(2.0, 3.0 / 12.0)))) < 5.0);
}

SQ_TEST("stretch: identical stereo channels stay identical (PV phase coherence)") {
  std::vector<float> x(30000);
  sqdsptest::addTone(x, 196.0, kSr, 0, x.size(), 0.3f, 8);
  for (Algorithm a : {Algorithm::Wsola, Algorithm::PhaseVocoder}) {
    Stretcher st;
    REQUIRE(st.configure(settingsFor(a, 2)));
    st.setRatios(1.37, 2.0);
    auto out = runOffline(st, {x, x}, 400);
    REQUIRE(out[0].size() == out[1].size());
    double diff = 0.0;
    for (size_t i = 0; i < out[0].size(); ++i) diff = std::max(diff, double(std::fabs(out[0][i] - out[1][i])));
    CHECK(diff < 1e-6);
  }
}

SQ_TEST("stretch: realtime pattern (getRequiredInput + fixed pulls) is gap-free") {
  const std::vector<float> x = sqdsptest::sine(440.0, kSr, 88200, 0.5f);
  for (Algorithm a : {Algorithm::Wsola, Algorithm::PhaseVocoder}) {
    Stretcher st;
    REQUIRE(st.configure(settingsFor(a)));
    st.setRatios(1.2, -2.0);
    const uint32_t block = 128;
    std::vector<float> out, tmp(block);
    float* tp = tmp.data();
    size_t pos = 0;
    int shortPulls = 0;
    for (int i = 0; i < 400; ++i) {
      uint32_t need = st.requiredInput(block);
      while (need > 0 && pos < x.size()) {
        uint32_t n = static_cast<uint32_t>(std::min<size_t>({need, 512u, x.size() - pos}));
        const float* ip = x.data() + pos;
        REQUIRE(st.push(&ip, n));
        pos += n;
        need = st.requiredInput(block);
      }
      uint32_t got = st.pull(&tp, block);
      if (got != block) ++shortPulls;
      out.insert(out.end(), tmp.begin(), tmp.begin() + got);
    }
    CHECK_EQ(shortPulls, 0);
    CHECK(st.requiredInput(block) <= st.latencyFrames() + 2 * block);
    const double f = sqdsptest::zeroCrossingFreq(out, kSr, 4000, out.size());
    CHECK(std::fabs(1200.0 * std::log2(f / (440.0 * std::pow(2.0, -2.0 / 12.0)))) < 5.0);
  }
}

SQ_TEST("stretch: preview quality and transient flag run") {
  std::vector<float> x = sqdsptest::clickTrack(120.0, kSr, 3.0);
  Settings s = settingsFor(Algorithm::Auto);
  s.quality = SQ_STRETCH_QUALITY_PREVIEW;
  Stretcher a;
  REQUIRE(a.configure(s));
  CHECK_EQ(std::string(a.algorithmName()), std::string("wsola"));
  a.setRatios(1.5, 0.0);
  CHECK(std::fabs(double(runOffline(a, {x}, 512)[0].size()) - x.size() * 1.5) <= 2.0);
  s.quality = SQ_STRETCH_QUALITY_HIGH;
  s.flags = SQ_STRETCH_PRESERVE_TRANSIENTS;
  Stretcher b;
  REQUIRE(b.configure(s));
  CHECK_EQ(std::string(b.algorithmName()), std::string("phase-vocoder"));
  b.setRatios(0.75, 0.0);
  CHECK(std::fabs(double(runOffline(b, {x}, 512)[0].size()) - x.size() * 0.75) <= 2.0);
}

SQ_TEST("stretch: rejects bad configuration, clamps ratios") {
  Stretcher st;
  Settings s;
  s.channels = 0;
  CHECK(!st.configure(s));
  s.channels = 1;
  s.sampleRate = 0;
  CHECK(!st.configure(s));
  s.sampleRate = 48000;
  REQUIRE(st.configure(s));
  st.setRatios(100.0, 99.0);
  CHECK_EQ(st.timeRatio(), 4.0);
  CHECK(std::fabs(st.pitchFactor() - 4.0) < 1e-9);
  st.setRatios(0.01, -99.0);
  CHECK_EQ(st.timeRatio(), sq::stretch::kMinTimeRatio);
}
