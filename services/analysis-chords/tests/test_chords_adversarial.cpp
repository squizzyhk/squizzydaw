// analysis-chords: hostile inputs and theory edge cases.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "chords_core.hpp"
#include "dsp_test_signals.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/interfaces/dsp.h"
#include "sq/platform.hpp"
#include "sq/theory/theory.hpp"
#include "sqtest.hpp"

namespace {
std::string modulePath(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}
}  // namespace

SQ_TEST("theory adversarial: roman numerals of chromatic roots never wrap to the tonic") {
  using sq::theory::ChordQuality;
  // C minor: B diminished is the leading-tone chord, not a "flat one".
  const std::string lt = sq::theory::romanNumeral(0, true, 11, ChordQuality::Diminished);
  std::printf("  C minor, Bdim -> %s\n", lt.c_str());
  CHECK_EQ(lt, std::string("viio"));
  // G major in C minor (harmonic-minor dominant) is plain V
  CHECK_EQ(sq::theory::romanNumeral(0, true, 7, ChordQuality::Major), std::string("V"));
  // A in C minor is the raised sixth
  CHECK_EQ(sq::theory::romanNumeral(0, true, 9, ChordQuality::Diminished), std::string("#vio"));
  // every root/quality/key gives a non-empty numeral whose degree is not "I"/"i" unless the root
  // is the tonic
  for (int minor = 0; minor < 2; ++minor)
    for (int root = 0; root < 12; ++root) {
      const std::string s = sq::theory::romanNumeral(5, minor == 1, 5 + root, ChordQuality::Major);
      REQUIRE(!s.empty());
      const bool tonicNumeral = s == "I" || s == "bI" || s == "#I";
      CHECK(tonicNumeral == (root == 0));
    }
  // existing conventions stay
  CHECK_EQ(sq::theory::romanNumeral(0, false, 10, ChordQuality::Major), std::string("bVII"));
  CHECK_EQ(sq::theory::romanNumeral(0, true, 8, ChordQuality::Major), std::string("VI"));
}

SQ_TEST("theory adversarial: parsing junk never crashes and is rejected") {
  int root = 0, bass = 0;
  sq::theory::ChordQuality q{};
  for (const char* s : {"", "/", "C/", "H", "Cxyz", "C/H", "\xFF\xFE", "b", "#", "C#####"})
    (void)sq::theory::parseChordName(s, root, q, bass);
  CHECK(!sq::theory::parseChordName("", root, q, bass));
  CHECK(!sq::theory::parseChordName("C/", root, q, bass));
  CHECK_EQ(sq::theory::parseMidiNote("A"), -1);
  CHECK_EQ(sq::theory::parseMidiNote("A-"), -1);
  CHECK_EQ(sq::theory::parseMidiNote("A999"), -1);
  CHECK_EQ(sq::theory::quantizeTime(1.0, 0.0), 1.0);
  CHECK_EQ(sq::theory::quantizeTime(1.0, std::nan("")), 1.0);
}

SQ_TEST("analysis-chords adversarial: non-finite sample rate rejected, NaN samples tolerated") {
  // core: must return (not hang) on absurd rates
  std::vector<float> one(4096, 0.1f);
  CHECK(sq::chords::analyze(one, std::numeric_limits<double>::infinity()).chords.empty());
  CHECK(sq::chords::analyze(one, 1.0e300).chords.empty());

  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath("analysis-chords")).ok());
  {
    auto an = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kChordAnalyzerClassId);
    REQUIRE(an);
    CHECK(an->prepare(std::numeric_limits<double>::infinity(), 1, 512, nullptr) == SQ_E_INVALIDARG);
    CHECK(an->prepare(1.0e300, 1, 512, nullptr) == SQ_E_INVALIDARG);
    REQUIRE(an->prepare(44100.0, 1, 4096, nullptr) == SQ_OK);
    // 4 s of A minor with a couple of NaN samples
    std::vector<float> x(44100 * 4, 0.0f);
    for (double f : {220.0, 261.63, 329.63}) sqdsptest::addTone(x, f, 44100.0, 0, x.size(), 0.2f, 4);
    x[30000] = std::numeric_limits<float>::quiet_NaN();
    x[100000] = -std::numeric_limits<float>::infinity();
    for (size_t pos = 0; pos < x.size(); pos += 4096) {
      float* ch[1] = {x.data() + pos};
      sq_audio_block b{ch, 1, static_cast<uint32_t>(std::min<size_t>(4096, x.size() - pos)), 0};
      REQUIRE(an->process(&b) == SQ_OK);
    }
    sq::Ptr<sq::IAnalysisResult> res;
    REQUIRE(an->finish(res.put()) == SQ_OK);
    int chords = 0, am = 0;
    for (uint32_t i = 0; i < res->getEventCount(); ++i) {
      sq_analysis_event e{};
      e.struct_size = sizeof e;
      REQUIRE(res->getEvent(i, &e) == SQ_OK);
      if (e.type != SQ_ANALYSIS_CHORD) continue;
      ++chords;
      if (std::string(e.label) == "Am") ++am;
      std::printf("  %.2f s +%.2f: %s\n", e.time, e.duration, e.label);
    }
    CHECK(chords >= 1);
    CHECK_EQ(am, chords);
  }
  CHECK(loader.unload("analysis-chords") == SQ_OK);
}
