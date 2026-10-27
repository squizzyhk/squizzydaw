// analysis-chords: synthesised chord progressions -> chord labels + key; service via the loader.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "chords_core.hpp"
#include "dsp_test_signals.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/dsp.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {

constexpr double kSr = 44100.0;

// Renders chords given by name (via the theory parser), `secs` each, notes voiced in octave 3-4.
std::vector<float> renderProgression(const std::vector<std::string>& names, double secs, int harmonics = 3) {
  std::vector<float> v(static_cast<size_t>(names.size() * secs * kSr), 0.0f);
  for (size_t i = 0; i < names.size(); ++i) {
    int root = 0, bass = -1;
    sq::theory::ChordQuality q{};
    if (!sq::theory::parseChordName(names[i], root, q, bass)) continue;
    const uint16_t iv = sq::theory::chordQualityInfo(q).intervals;
    const size_t start = static_cast<size_t>(i * secs * kSr), len = static_cast<size_t>(secs * kSr);
    for (int k = 0; k < 12; ++k)
      if ((iv >> k) & 1) {
        const int midi = 48 + root + k;  // C3-based voicing
        sqdsptest::addTone(v, sq::theory::midiToFrequency(midi), kSr, start, len, 0.15f, harmonics);
      }
    // doubled root an octave below for a fuller sound
    sqdsptest::addTone(v, sq::theory::midiToFrequency(36 + root), kSr, start, len, 0.1f, harmonics);
  }
  return v;
}

std::vector<std::string> labelsAt(const sq::chords::Result& r, const std::vector<double>& times) {
  std::vector<std::string> out;
  for (double t : times) {
    std::string label = "N";
    for (const auto& c : r.chords)
      if (t >= c.start && t < c.start + c.duration) label = sq::theory::chordName(c.root, c.quality);
    out.push_back(label);
  }
  return out;
}

std::string modulePath(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}

}  // namespace

SQ_TEST("chords: I-vi-IV-V7 in C") {
  const std::vector<std::string> prog = {"C", "Am", "F", "G7", "C"};
  auto audio = renderProgression(prog, 2.0);
  auto r = sq::chords::analyze(audio, kSr);
  auto got = labelsAt(r, {1.0, 3.0, 5.0, 7.0, 9.0});
  for (size_t i = 0; i < prog.size(); ++i) {
    std::printf("  t=%zu: expected %s got %s\n", 2 * i + 1, prog[i].c_str(), got[i].c_str());
    CHECK_EQ(got[i], prog[i]);
  }
  CHECK_EQ(sq::theory::keyName(r.key.tonic, r.key.minor), std::string("C major"));
  // boundaries within ~0.25 s of the true changes
  for (const auto& c : r.chords)
    if (c.start > 0.1) {
      double nearest = std::round(c.start / 2.0) * 2.0;
      CHECK(std::fabs(c.start - nearest) < 0.25);
    }
}

SQ_TEST("chords: qualities and a minor key") {
  const std::vector<std::string> prog = {"Dm", "Bdim", "E7", "Am", "Fmaj7", "Dm7", "E", "Am"};
  auto audio = renderProgression(prog, 1.5);
  auto r = sq::chords::analyze(audio, kSr);
  std::vector<double> times;
  for (size_t i = 0; i < prog.size(); ++i) times.push_back(0.75 + 1.5 * i);
  auto got = labelsAt(r, times);
  int correct = 0;
  for (size_t i = 0; i < prog.size(); ++i) {
    std::printf("  expected %-6s got %s\n", prog[i].c_str(), got[i].c_str());
    correct += got[i] == prog[i];
  }
  CHECK(correct >= 7);
  CHECK_EQ(sq::theory::keyName(r.key.tonic, r.key.minor), std::string("A minor"));
}

SQ_TEST("chords: silence yields no chords; key estimate from a scale") {
  std::vector<float> silence(static_cast<size_t>(kSr * 2), 0.0f);
  CHECK(sq::chords::analyze(silence, kSr).chords.empty());
  // G major scale profile -> G major
  float chroma[12] = {};
  for (int pc : {7, 9, 11, 0, 2, 4, 6}) chroma[pc] = 1.0f;
  chroma[7] = 2.0f, chroma[2] = 1.5f;
  auto k = sq::chords::estimateKey(chroma);
  CHECK_EQ(sq::theory::keyName(k.tonic, k.minor), std::string("G major"));
}

SQ_TEST("analysis-chords service: IAnalyzer end to end") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath("analysis-chords")).ok());
  {
    auto an = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kChordAnalyzerClassId);
    REQUIRE(an);
    sq_analyzer_info info{};
    info.struct_size = sizeof info;
    REQUIRE(an->getInfo(&info) == SQ_OK);
    CHECK_EQ(std::string(info.kind), std::string("chord"));
    CHECK(info.flags & SQ_ANALYZER_OFFLINE);
    auto opts = sq::makeAttributes();
    opts->setDouble("reference_hz", 440.0);
    REQUIRE(an->prepare(kSr, 2, 1024, opts.get()) == SQ_OK);
    auto audio = renderProgression({"Bb", "Gm", "Eb", "F"}, 2.0);
    for (size_t pos = 0; pos < audio.size(); pos += 1024) {
      uint32_t n = static_cast<uint32_t>(std::min<size_t>(1024, audio.size() - pos));
      float* chans[2] = {audio.data() + pos, audio.data() + pos};
      sq_audio_block b{chans, 2, n, static_cast<sq_sample_time>(pos)};
      REQUIRE(an->process(&b) == SQ_OK);
    }
    sq::Ptr<sq::IAnalysisResult> res;
    REQUIRE(an->finish(res.put()) == SQ_OK);
    REQUIRE(res);
    REQUIRE(res->getEventCount() >= 5);
    sq_analysis_event e{};
    e.struct_size = sizeof e;
    REQUIRE(res->getEvent(0, &e) == SQ_OK);
    CHECK_EQ(e.type, SQ_ANALYSIS_KEY);
    CHECK_EQ(std::string(e.label), std::string("Bb major"));
    std::vector<std::string> labels;
    for (uint32_t i = 1; i < res->getEventCount(); ++i) {
      REQUIRE(res->getEvent(i, &e) == SQ_OK);
      CHECK_EQ(e.type, SQ_ANALYSIS_CHORD);
      labels.push_back(e.label);
    }
    REQUIRE(labels.size() == 4);
    CHECK_EQ(labels[0], std::string("Bb"));  // flats because the key is Bb major
    CHECK_EQ(labels[1], std::string("Gm"));
    CHECK_EQ(labels[2], std::string("Eb"));
    CHECK_EQ(labels[3], std::string("F"));
    CHECK_EQ(res->getStemCount(), 0u);
    sq_tuner_value tv{};
    tv.struct_size = sizeof tv;
    CHECK(an->getRealtimeValue(&tv) == SQ_E_UNSUPPORTED);
  }
  CHECK(loader.unload("analysis-chords") == SQ_OK);
}
