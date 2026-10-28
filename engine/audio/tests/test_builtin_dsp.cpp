// engine/audio - built-in devices: behaviour of the individual instruments and effects.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sqtest.hpp"
#include "test_builtin_util.hpp"

using namespace sq::engine::audio;
using bt::Channels;

namespace {
constexpr double kSr = 48000.0;

uint32_t paramIndex(const std::string& dev, const std::string& id) {
  const DeviceInfo* d = findBuiltin(dev);
  for (uint32_t i = 0; i < d->params.size(); ++i)
    if (d->params[i].id == id) return i;
  std::printf("no param %s.%s\n", dev.c_str(), id.c_str());
  return 9999;
}

Channels impulse(uint32_t ch, int64_t frames, float amp = 1.0f, int64_t at = 0) {
  Channels in(ch, std::vector<float>(static_cast<size_t>(frames), 0.0f));
  for (auto& c : in) c[static_cast<size_t>(at)] = amp;
  return in;
}

Channels sine(uint32_t ch, int64_t frames, double hz, float amp) {
  Channels in(ch, std::vector<float>(static_cast<size_t>(frames)));
  for (auto& c : in)
    for (size_t i = 0; i < c.size(); ++i) c[i] = amp * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / kSr));
  return in;
}
}  // namespace

SQ_TEST("synth: note on sounds, note off + release goes silent, default is the default instrument") {
  SpecParts sp;
  REQUIRE(parseSpec(kDefaultInstrument, &sp));
  auto p = bt::instance(sp.id, sp.params);
  REQUIRE(p);
  REQUIRE(p->prepare(kSr, 256, 2));
  const int64_t frames = static_cast<int64_t>(kSr * 2.5);
  Channels out = bt::run(*p, 2, frames, 256, {}, {{100, 0x90, 60, 100}, {24000, 0x80, 60, 0}});
  for (int i = 0; i < 100; ++i) CHECK_EQ(out[0][static_cast<size_t>(i)], 0.0f);  // sample accurate start
  const double on = bt::rms(out[0], 2000, 24000);
  CHECK(on > 0.02);
  CHECK(on < 1.0);
  CHECK(bt::rms(out[1], 2000, 24000) > 0.02);
  CHECK(bt::peak(out[0], static_cast<size_t>(kSr * 1.6)) == 0.0f);  // released + voices freed
  CHECK(bt::clean(out));
  // sustain pedal holds the note past its note-off
  p->reset();
  out = bt::run(*p, 2, frames, 256, {}, {{0, 0xB0, 64, 127}, {0, 0x90, 60, 100}, {4800, 0x80, 60, 0}, {96000, 0xB0, 64, 0}});
  CHECK(bt::rms(out[0], 80000, 96000) > 0.01);
  CHECK(bt::peak(out[0], 96000 + 48000) == 0.0f);
  // all notes off
  p->reset();
  out = bt::run(*p, 2, frames, 256, {}, {{0, 0x90, 60, 100}, {0, 0x90, 64, 100}, {4800, 0xB0, 123, 0}});
  CHECK(bt::peak(out[0], 4800 + 72000) == 0.0f);
}

SQ_TEST("synth: 16-voice polyphony with stealing, pitch follows the note, mono legato") {
  auto p = bt::instance("synth", {{paramIndex("synth", "osc2_level"), 0.0}, {paramIndex("synth", "sub_level"), 0.0},
                                  {paramIndex("synth", "unison"), 1.0}, {paramIndex("synth", "osc1_wave"), 0.0},
                                  {paramIndex("synth", "filter_type"), 0.0}, {paramIndex("synth", "cutoff"), 20000.0},
                                  {paramIndex("synth", "drive"), 0.0}});
  REQUIRE(p->prepare(kSr, 128, 1));
  Channels a = bt::run(*p, 1, 24000, 128, {}, {{0, 0x90, 69, 127}});
  int z = 0;
  for (size_t i = 4001; i < 20000; ++i)
    if ((a[0][i - 1] < 0.0f) != (a[0][i] < 0.0f)) ++z;
  const double hz = z * 0.5 / (16000.0 / kSr);
  CHECK(std::fabs(hz - 440.0) < 5.0);
  // 24 simultaneous notes: stealing keeps it finite and bounded
  std::vector<bt::Note> many;
  for (uint8_t k = 40; k < 64; ++k) many.push_back({static_cast<int64_t>(k) * 10, 0x90, k, 100});
  p->reset();
  Channels b = bt::run(*p, 1, 24000, 128, {}, many);
  CHECK(bt::clean(b));
  CHECK(bt::peak(b[0]) < 8.0f);
  // mono mode
  auto m = bt::instance("synth", {{paramIndex("synth", "polyphony"), 1.0}, {paramIndex("synth", "glide"), 80.0}});
  REQUIRE(m->prepare(kSr, 64, 2));
  Channels c = bt::run(*m, 2, 48000, 64, {}, {{0, 0x90, 48, 100}, {12000, 0x90, 55, 100}, {24000, 0x80, 55, 0}, {36000, 0x80, 48, 0}});
  CHECK(bt::rms(c[0], 30000, 36000) > 0.01);  // fell back to the held note
  CHECK(bt::clean(c));
}

SQ_TEST("drums: every kit piece sounds and they differ") {
  const uint8_t notes[] = {36, 37, 38, 39, 42, 46, 41, 45, 48, 49, 51};
  std::vector<std::vector<float>> outs;
  for (uint8_t n : notes) {
    auto p = bt::instance("drums");
    REQUIRE(p->prepare(kSr, 256, 2));
    Channels o = bt::run(*p, 2, 24000, 256, {}, {{0, 0x90, n, 120}});
    const double r = bt::rms(o[0], 0, 6000);
    if (r < 0.005) std::printf("  drum note %d too quiet: %f\n", n, r);
    CHECK(r > 0.005);
    CHECK(bt::peak(o[0]) < 4.0f);
    CHECK(bt::clean(o));
    outs.push_back(o[0]);
  }
  // pairwise: normalized correlation of the first 100 ms must not be ~1
  for (size_t i = 0; i < outs.size(); ++i)
    for (size_t j = i + 1; j < outs.size(); ++j) {
      double xy = 0, xx = 0, yy = 0;
      for (size_t k = 0; k < 4800; ++k) {
        xy += static_cast<double>(outs[i][k]) * outs[j][k];
        xx += static_cast<double>(outs[i][k]) * outs[i][k];
        yy += static_cast<double>(outs[j][k]) * outs[j][k];
      }
      const double corr = xy / std::sqrt(xx * yy + 1e-30);
      if (corr > 0.9) std::printf("  drum notes %d and %d look alike (%f)\n", notes[i], notes[j], corr);
      CHECK(corr < 0.9);
    }
  // unmapped notes are silent; the closed hat chokes the open hat
  auto p = bt::instance("drums");
  REQUIRE(p->prepare(kSr, 256, 2));
  Channels o = bt::run(*p, 2, 4800, 256, {}, {{0, 0x90, 90, 120}});
  CHECK_EQ(bt::peak(o[0]), 0.0f);
  p->reset();
  Channels open = bt::run(*p, 2, 24000, 256, {}, {{0, 0x90, 46, 120}});
  p->reset();
  Channels choked = bt::run(*p, 2, 24000, 256, {}, {{0, 0x90, 46, 120}, {2400, 0x90, 42, 1}});
  CHECK(bt::rms(choked[0], 12000, 24000) < 0.2 * bt::rms(open[0], 12000, 24000));
}

SQ_TEST("keys + bass: play and release") {
  for (const char* id : {"keys", "bass"}) {
    auto p = bt::instance(id);
    REQUIRE(p->prepare(kSr, 128, 2));
    Channels o = bt::run(*p, 2, 96000, 128, {}, {{0, 0x90, 45, 110}, {12000, 0x80, 45, 0}});
    CHECK(bt::rms(o[0], 0, 12000) > 0.02);
    CHECK(bt::peak(o[0], 60000) < 1e-3f);
    CHECK(bt::clean(o));
  }
  // velocity -> brightness on keys: a hard hit has more high-frequency energy
  auto hf = [](int vel) {
    auto p = bt::instance("keys", {{paramIndex("keys", "velocity"), 0.0}});
    p->prepare(kSr, 128, 1);
    Channels o = bt::run(*p, 1, 9600, 128, {}, {{0, 0x90, 60, static_cast<uint8_t>(vel)}});
    double d = 0;
    for (size_t i = 1; i < 9600; ++i) d += std::fabs(o[0][i] - o[0][i - 1]);
    return d / (bt::rms(o[0]) + 1e-9);
  };
  CHECK(hf(127) > 1.2 * hf(30));
}

SQ_TEST("eq: flat settings are transparent, a boost boosts, cuts cut") {
  auto p = bt::instance("eq");
  REQUIRE(p->prepare(kSr, 512, 2));
  Channels in = bt::testSignal(2, 48000, kSr);
  Channels out = bt::run(*p, 2, 48000, 512, in);
  float maxDiff = 0.0f;
  for (int c = 0; c < 2; ++c)
    for (size_t i = 0; i < in[c].size(); ++i) maxDiff = std::max(maxDiff, std::fabs(out[c][i] - in[c][i]));
  CHECK(maxDiff < 1e-4f);
  // +12 dB at 1 kHz on band 2
  auto b = bt::instance("eq", {{paramIndex("eq", "peak2_gain"), 12.0}});
  REQUIRE(b->prepare(kSr, 512, 1));
  Channels s = sine(1, 48000, 1000.0, 0.1f);
  Channels o = bt::run(*b, 1, 48000, 512, s);
  const double gain = bt::rms(o[0], 24000) / bt::rms(s[0], 24000);
  CHECK(std::fabs(gain - 3.98) < 0.1);
  // low cut at 500 Hz / 48 dB removes 50 Hz
  auto l = bt::instance("eq", {{paramIndex("eq", "lowcut_on"), 1.0}, {paramIndex("eq", "lowcut_freq"), 500.0}, {paramIndex("eq", "lowcut_slope"), 2.0}});
  REQUIRE(l->prepare(kSr, 512, 1));
  Channels lo = sine(1, 48000, 50.0, 0.5f);
  Channels lout = bt::run(*l, 1, 48000, 512, lo);
  CHECK(bt::rms(lout[0], 24000) < 1e-3 * bt::rms(lo[0], 24000) * 10);
}

SQ_TEST("limiter: never exceeds the ceiling after its latency and delays by its latency") {
  auto p = bt::instance("limiter", {{paramIndex("limiter", "ceiling"), -1.0}, {paramIndex("limiter", "gain"), 6.0}});
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    REQUIRE(p->prepare(sr, 333, 2));
    const uint32_t lat = p->latency();
    CHECK(lat > 0);
    CHECK(lat < static_cast<uint32_t>(sr * 0.01));
    Channels in = bt::testSignal(2, static_cast<int64_t>(sr), sr, 2.0f);
    for (size_t i = 5000; i < 5010; ++i) in[0][i] = 8.0f;  // sharp transient
    in[1][7000] = -10.0f;
    Channels out = bt::run(*p, 2, static_cast<int64_t>(sr), 333, in);
    const float ceil = std::pow(10.0f, -1.0f / 20.0f);
    CHECK(bt::peak(out[0]) <= ceil * 1.00001f);
    CHECK(bt::peak(out[1]) <= ceil * 1.00001f);
    CHECK(bt::clean(out));
    CHECK(builtinMeterValue(p.get()) > 0.0f);
    // quiet impulse passes unchanged, delayed by latency()
    auto q = bt::instance("limiter");
    REQUIRE(q->prepare(sr, 64, 1));
    Channels imp = impulse(1, 4096, 0.25f, 100);
    Channels qo = bt::run(*q, 1, 4096, 64, imp);
    CHECK(std::fabs(qo[0][100 + q->latency()] - 0.25f) < 1e-6f);
    CHECK(bt::peak(qo[0], 0, 100 + q->latency()) == 0.0f);
  }
}

SQ_TEST("compressor + gate: reduce gain as configured") {
  auto c = bt::instance("compressor");
  REQUIRE(c->prepare(kSr, 256, 2));
  Channels s = sine(2, 48000, 200.0, 1.0f);
  Channels o = bt::run(*c, 2, 48000, 256, s);
  const double grDb = 20.0 * std::log10(bt::rms(s[0], 24000) / bt::rms(o[0], 24000));
  CHECK(grDb > 6.0);
  CHECK(grDb < 16.0);
  CHECK(builtinMeterValue(c.get()) > 6.0f);
  CHECK_EQ(builtinMeterValue(nullptr), 0.0f);
  // quiet signal below threshold is untouched
  c->reset();
  Channels q = sine(2, 48000, 200.0, 0.01f);
  Channels qo = bt::run(*c, 2, 48000, 256, q);
  CHECK(std::fabs(bt::rms(qo[0], 24000) / bt::rms(q[0], 24000) - 1.0) < 0.01);
  // gate: -60 dBFS is gated, -6 dBFS passes
  auto g = bt::instance("gate");
  REQUIRE(g->prepare(kSr, 256, 1));
  Channels low = sine(1, 48000, 200.0, 0.001f);
  Channels lo = bt::run(*g, 1, 48000, 256, low);
  CHECK(bt::rms(lo[0], 24000) < 0.01 * bt::rms(low[0], 24000));
  g->reset();
  Channels hi = sine(1, 48000, 200.0, 0.5f);
  Channels ho = bt::run(*g, 1, 48000, 256, hi);
  CHECK(std::fabs(bt::rms(ho[0], 24000) / bt::rms(hi[0], 24000) - 1.0) < 0.01);
}

SQ_TEST("reverb + delay: tails ring and then decay within tail()") {
  for (const char* id : {"reverb", "delay"}) {
    auto p = bt::instance(id, {{paramIndex(id, "mix"), 100.0}});
    REQUIRE(p->prepare(kSr, 512, 2));
    const uint32_t tail = p->tail();
    CHECK(tail > static_cast<uint32_t>(kSr * 0.3));
    CHECK(tail < static_cast<uint32_t>(kSr * 60));
    const int64_t frames = static_cast<int64_t>(tail) + static_cast<int64_t>(kSr);
    Channels in = impulse(2, frames, 1.0f, 10);
    Channels out = bt::run(*p, 2, frames, 512, in);
    CHECK(bt::clean(out));
    const double early = std::max(bt::rms(out[0], 0, static_cast<size_t>(kSr * 0.6)), bt::rms(out[1], 0, static_cast<size_t>(kSr * 0.6)));
    CHECK(early > 1e-3);
    const size_t end = static_cast<size_t>(frames);
    const double late = bt::rms(out[0], static_cast<size_t>(tail), end) + bt::rms(out[1], static_cast<size_t>(tail), end);
    if (late > early * 2e-3) std::printf("  %s: early %g late %g\n", id, early, late);
    CHECK(late < early * 2e-3);
  }
  // tempo sync: 1/4 at 120 bpm = 500 ms
  auto d = bt::instance("delay", {{paramIndex("delay", "sync"), 9.0}, {paramIndex("delay", "mode"), 0.0},
                                  {paramIndex("delay", "mix"), 100.0}, {paramIndex("delay", "lowcut"), 20.0},
                                  {paramIndex("delay", "highcut"), 20000.0}});
  REQUIRE(d->prepare(kSr, 256, 1));
  Channels o = bt::run(*d, 1, 48000, 256, impulse(1, 48000, 1.0f, 0));
  size_t at = 0;
  for (size_t i = 0; i < o[0].size(); ++i)
    if (std::fabs(o[0][i]) > std::fabs(o[0][at])) at = i;
  CHECK(at >= 23999 && at <= 24001);
}

SQ_TEST("utility: mono, invert, width and balance") {
  auto p = bt::instance("utility", {{paramIndex("utility", "mono"), 1.0}});
  REQUIRE(p->prepare(kSr, 64, 2));
  Channels in = bt::testSignal(2, 4800, kSr);
  Channels o = bt::run(*p, 2, 4800, 64, in);
  for (size_t i = 0; i < 4800; ++i) CHECK(std::fabs(o[0][i] - o[1][i]) < 1e-6f);
  auto inv = bt::instance("utility", {{paramIndex("utility", "invert_l"), 1.0}, {paramIndex("utility", "pan"), 100.0}});
  REQUIRE(inv->prepare(kSr, 64, 2));
  Channels io = bt::run(*inv, 2, 4800, 64, in);
  for (size_t i = 0; i < 4800; ++i) {
    CHECK(std::fabs(io[0][i]) < 1e-6f);  // full right balance silences left
    CHECK(std::fabs(io[1][i] - in[1][i]) < 1e-6f);
  }
}

SQ_TEST("modulation + distortion + filter: process audibly") {
  for (const char* id : {"chorus", "flanger", "phaser", "tremolo", "distortion", "filter"}) {
    auto p = bt::instance(id);
    REQUIRE(p->prepare(kSr, 256, 2));
    Channels in = bt::testSignal(2, 48000, kSr);
    Channels o = bt::run(*p, 2, 48000, 256, in);
    double diff = 0;
    for (size_t i = 0; i < 48000; ++i) diff += std::fabs(o[0][i] - in[0][i]);
    CHECK(diff / 48000.0 > 1e-3);
    CHECK(bt::clean(o));
  }
  // bitcrusher quantizes
  auto b = bt::instance("distortion", {{paramIndex("distortion", "type"), 4.0}, {paramIndex("distortion", "bits"), 3.0},
                                       {paramIndex("distortion", "drive"), 0.0}, {paramIndex("distortion", "tone"), 20000.0},
                                       {paramIndex("distortion", "output"), 0.0}});
  REQUIRE(b->prepare(kSr, 256, 1));
  Channels o = bt::run(*b, 1, 4800, 256, sine(1, 4800, 100.0, 0.9f));
  CHECK(bt::clean(o));
}

SQ_TEST("feedback effects and instruments decay to silence, never into denormals") {
  struct Case {
    const char* id;
    std::vector<std::pair<std::string, double>> params;
  };
  const Case cases[] = {
      {"reverb", {{"decay", 0.2}, {"mix", 100.0}}},
      {"delay", {{"time_l", 1.0}, {"time_r", 1.0}, {"feedback", 60.0}, {"mix", 100.0}}},
      {"flanger", {{"feedback", 95.0}}},
      {"chorus", {{"feedback", 90.0}}},
      {"phaser", {{"feedback", 100.0}}},
      {"eq", {{"lowcut_on", 1.0}, {"peak1_gain", 12.0}, {"highcut_on", 1.0}}},
      {"filter", {{"resonance", 1.0}}},
      {"compressor", {}},
      {"distortion", {}},
  };
  for (const Case& c : cases) {
    std::vector<std::pair<uint32_t, double>> params;
    for (const auto& [id, v] : c.params) params.emplace_back(paramIndex(c.id, id), v);
    auto p = bt::instance(c.id, params);
    REQUIRE(p->prepare(kSr, 512, 2));
    const int64_t frames = static_cast<int64_t>(kSr * 12);
    Channels out = bt::run(*p, 2, frames, 512, impulse(2, frames, 1.0f, 0));
    if (!bt::clean(out)) std::printf("  %s produced denormals/NaN\n", c.id);
    CHECK(bt::clean(out));
  }
  for (const char* id : {"synth", "drums", "keys", "bass"}) {
    auto p = bt::instance(id);
    REQUIRE(p->prepare(kSr, 512, 2));
    Channels out = bt::run(*p, 2, static_cast<int64_t>(kSr * 12), 512, {}, bt::chordNotes(0, 4800));
    if (!bt::clean(out)) std::printf("  %s produced denormals/NaN\n", id);
    CHECK(bt::clean(out));
  }
}
