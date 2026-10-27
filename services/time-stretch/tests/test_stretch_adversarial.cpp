// time-stretch: adversarial cases - phase-cancelling stereo through the phase vocoder, ratio /
// pitch automation while streaming, hostile values through the ITimeStretcher-level core API.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp_test_signals.hpp"
#include "sqtest.hpp"
#include "stretch_core.hpp"

namespace {

using sq::stretch::Algorithm;
using sq::stretch::Settings;
using sq::stretch::Stretcher;

constexpr double kSr = 44100.0;

std::vector<std::vector<float>> runOffline(Stretcher& st, const std::vector<std::vector<float>>& in, uint32_t block) {
  const uint32_t C = static_cast<uint32_t>(in.size());
  const size_t n = in[0].size();
  std::vector<std::vector<float>> out(C), tmp(C, std::vector<float>(block));
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

// Short-time RMS spread: max/min of 20 ms RMS over [from, to). A clean stretched sine is flat.
double rmsSpread(const std::vector<float>& v, size_t from, size_t to) {
  const size_t w = static_cast<size_t>(0.02 * kSr);
  double lo = 1e9, hi = 0.0;
  for (size_t p = from; p + w <= to; p += w) {
    const double r = sqdsptest::rms(v, p, p + w);
    lo = std::min(lo, r), hi = std::max(hi, r);
  }
  return lo > 0.0 ? hi / lo : 1e9;
}

}  // namespace

SQ_TEST("stretch adversarial: phase vocoder keeps polarity-inverted stereo intact") {
  // R = -L: the channel sum is zero in every bin. The stretched output must still be a clean,
  // steady 440 Hz tone on both channels, with R == -L.
  const std::vector<float> l = sqdsptest::sine(440.0, kSr, 44100, 0.5f);
  std::vector<float> r(l.size());
  for (size_t i = 0; i < l.size(); ++i) r[i] = -l[i];
  for (double ratio : {1.5, 0.8}) {
    Settings s;
    s.sampleRate = kSr;
    s.channels = 2;
    s.maxBlock = 512;
    s.quality = SQ_STRETCH_QUALITY_HIGH;
    s.algorithm = Algorithm::PhaseVocoder;
    Stretcher st;
    REQUIRE(st.configure(s));
    st.setRatios(ratio, 0.0);
    auto out = runOffline(st, {l, r}, 512);
    REQUIRE(out[0].size() == out[1].size());
    const size_t a = out[0].size() / 8, b = out[0].size() * 7 / 8;
    const double f = sqdsptest::zeroCrossingFreq(out[0], kSr, a, b);
    const double rmsL = sqdsptest::rms(out[0], a, b);
    const double spread = rmsSpread(out[0], a, b);
    double sumErr = 0.0;
    for (size_t i = a; i < b; ++i) sumErr = std::max(sumErr, double(std::fabs(out[0][i] + out[1][i])));
    std::printf("  ratio %.2f: f %.2f Hz, rms %.3f, rms spread %.3f, max |L+R| %.2e\n", ratio, f, rmsL, spread, sumErr);
    CHECK(std::fabs(f - 440.0) < 2.0);
    CHECK(rmsL > 0.30 && rmsL < 0.42);
    CHECK(spread < 1.15);
    CHECK(sumErr < 1e-5);
  }
}

SQ_TEST("stretch adversarial: hard-panned different tones keep their pitch per channel (PV)") {
  // L and R carry unrelated tones; every bin's phase must follow the channel that carries it.
  const std::vector<float> l = sqdsptest::sine(300.0, kSr, 44100, 0.5f);
  const std::vector<float> r = sqdsptest::sine(1250.0, kSr, 44100, 0.5f, 1.0);
  Settings s;
  s.sampleRate = kSr;
  s.channels = 2;
  s.maxBlock = 256;
  s.quality = SQ_STRETCH_QUALITY_HIGH;
  s.algorithm = Algorithm::PhaseVocoder;
  Stretcher st;
  REQUIRE(st.configure(s));
  st.setRatios(1.7, 0.0);
  auto out = runOffline(st, {l, r}, 256);
  const size_t a = out[0].size() / 8, b = out[0].size() * 7 / 8;
  const double fl = sqdsptest::zeroCrossingFreq(out[0], kSr, a, b), fr = sqdsptest::zeroCrossingFreq(out[1], kSr, a, b);
  const double sl = rmsSpread(out[0], a, b), sr = rmsSpread(out[1], a, b);
  std::printf("  L %.2f Hz (spread %.3f), R %.2f Hz (spread %.3f)\n", fl, sl, fr, sr);
  CHECK(std::fabs(fl - 300.0) < 1.5);
  CHECK(std::fabs(fr - 1250.0) < 3.0);
  CHECK(sl < 1.15);
  CHECK(sr < 1.15);
}

SQ_TEST("stretch adversarial: ratio and pitch automation while streaming stays finite and gap-free") {
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> ratioD(0.25, 4.0), pitchD(-24.0, 24.0);
  std::uniform_int_distribution<uint32_t> blockD(1, 512);
  for (Algorithm a : {Algorithm::Wsola, Algorithm::PhaseVocoder}) {
    Settings s;
    s.sampleRate = 48000.0;
    s.channels = 2;
    s.maxBlock = 512;
    s.quality = a == Algorithm::PhaseVocoder ? SQ_STRETCH_QUALITY_HIGH : SQ_STRETCH_QUALITY_STANDARD;
    s.algorithm = a;
    s.maxTimeRatio = 4.0;
    Stretcher st;
    REQUIRE(st.configure(s));
    std::vector<float> l(512), r(512), ol(512), orr(512);
    float* op[2] = {ol.data(), orr.data()};
    double phase = 0.0;
    int pushFail = 0, shortPulls = 0, nonFinite = 0;
    float peak = 0.0f;
    for (int iter = 0; iter < 3000; ++iter) {
      if (iter % 37 == 0) st.setRatios(ratioD(rng), (iter % 74 == 0) ? 0.0 : pitchD(rng));
      const uint32_t want = blockD(rng);
      uint32_t need = st.requiredInput(want);
      while (need > 0) {
        const uint32_t k = std::min<uint32_t>(need, 512);
        for (uint32_t i = 0; i < k; ++i) {
          l[i] = 0.4f * static_cast<float>(std::sin(phase));
          r[i] = 0.4f * static_cast<float>(std::cos(phase * 1.5));
          phase += 2.0 * sqdsptest::kPi * 220.0 / 48000.0;
        }
        const float* ip[2] = {l.data(), r.data()};
        if (!st.push(ip, k)) {
          ++pushFail;
          break;
        }
        need = st.requiredInput(want);
      }
      const uint32_t got = st.pull(op, want);
      if (got != want) ++shortPulls;
      for (uint32_t i = 0; i < got; ++i) {
        if (!std::isfinite(ol[i]) || !std::isfinite(orr[i])) ++nonFinite;
        peak = std::max({peak, std::fabs(ol[i]), std::fabs(orr[i])});
      }
    }
    std::printf("  %s: push failures %d, short pulls %d, non-finite %d, peak %.3f\n", st.algorithmName(), pushFail,
                shortPulls, nonFinite, peak);
    CHECK_EQ(pushFail, 0);
    CHECK_EQ(shortPulls, 0);
    CHECK_EQ(nonFinite, 0);
    CHECK(peak < 2.0f);
  }
}

SQ_TEST("stretch adversarial: hostile ratios are sanitised") {
  Settings s;
  s.sampleRate = 44100.0;
  s.channels = 1;
  Stretcher st;
  REQUIRE(st.configure(s));
  st.setRatios(std::nan(""), std::nan(""));
  CHECK_EQ(st.timeRatio(), 1.0);
  CHECK_EQ(st.pitchFactor(), 1.0);
  st.setRatios(INFINITY, INFINITY);
  CHECK(std::isfinite(st.timeRatio()));
  CHECK_EQ(st.pitchFactor(), 1.0);
  st.setRatios(-3.0, -INFINITY);
  CHECK_EQ(st.timeRatio(), 1.0);
  // non-finite configuration values are rejected rather than sized from
  Settings bad = s;
  bad.sampleRate = std::nan("");
  CHECK(!st.configure(bad));
  bad.sampleRate = INFINITY;
  CHECK(!st.configure(bad));
}
