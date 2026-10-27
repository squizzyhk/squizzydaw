// analysis-chords service: IAnalyzer "chord" (offline chord sequence + key).
// Options: "reference_hz" (double, default 440).
// Result events: one SQ_ANALYSIS_KEY (time 0, duration = whole input, value = tonic pitch class,
// label "C major"), then SQ_ANALYSIS_CHORD segments (value = root pitch class, label "Am", spelled
// with flats in flat keys).
#include <cmath>
#include <vector>

#include "chords_core.hpp"
#include "sq/engine/simd/analysis_result.hpp"
#include "sq/sdk.hpp"

namespace {

class ChordAnalyzer final : public sq::Object<ChordAnalyzer, sq::IAnalyzer> {
 public:
  sq_result SQ_CALL getInfo(sq_analyzer_info* out) override {
    return sq::dspkit::fillInfo(out, "chord", "Squizzy Chord and Key Detector", SQ_ANALYZER_OFFLINE);
  }
  sq_result SQ_CALL prepare(double sampleRate, uint32_t channels, uint32_t maxBlockFrames, sq::IAttributes* options) override {
    // finite rates only (the frame-size search would never end for an infinite one)
    if (!(sampleRate >= 4000.0 && sampleRate <= 1.0e6) || channels == 0 || maxBlockFrames == 0) return SQ_E_INVALIDARG;
    sampleRate_ = sampleRate;
    channels_ = channels;
    referenceHz_ = options ? options->getDouble("reference_hz", 440.0) : 440.0;
    mono_.clear();
    prepared_ = true;
    return SQ_OK;
  }
  sq_result SQ_CALL process(const sq_audio_block* block) override {
    if (!prepared_) return SQ_E_FAIL;
    if (!sq::dspkit::blockReadable(block) || block->channel_count == 0) return SQ_E_INVALIDARG;
    if (!block->frames) return SQ_OK;
    try {
      const size_t base = mono_.size();
      mono_.resize(base + block->frames, 0.0f);
      const float g = 1.0f / static_cast<float>(block->channel_count);
      for (uint32_t c = 0; c < block->channel_count; ++c)
        sq_dsp_vector_ops()->mix(mono_.data() + base, block->channels[c], g, block->frames);
      for (size_t i = base; i < mono_.size(); ++i)  // a NaN sample would poison every frame it touches
        if (!std::isfinite(mono_[i])) mono_[i] = 0.0f;
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL finish(sq::IAnalysisResult** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!prepared_) return SQ_E_FAIL;
    try {
      sq::chords::Result r = sq::chords::analyze(mono_, sampleRate_, referenceHz_);
      auto* res = new sq::dspkit::AnalysisResult();
      const double total = static_cast<double>(mono_.size()) / sampleRate_;
      const bool flats = sq::theory::keyPrefersFlats(r.key.tonic, r.key.minor);
      if (!r.chords.empty()) {
        const std::string key = sq::theory::keyName(r.key.tonic, r.key.minor);
        res->events.push_back(sq::dspkit::makeEvent(SQ_ANALYSIS_KEY, 0.0, total, r.key.tonic, r.key.confidence, key.c_str()));
      }
      for (const auto& c : r.chords) {
        const std::string name = sq::theory::chordName(c.root, c.quality, -1, flats);
        res->events.push_back(
            sq::dspkit::makeEvent(SQ_ANALYSIS_CHORD, c.start, c.duration, c.root, c.confidence, name.c_str()));
      }
      *out = res;
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL getRealtimeValue(sq_tuner_value*) override { return SQ_E_UNSUPPORTED; }
  void SQ_CALL reset() override { mono_.clear(); }

 private:
  bool prepared_ = false;
  double sampleRate_ = 44100.0;
  uint32_t channels_ = 0;
  double referenceHz_ = 440.0;
  std::vector<float> mono_;
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<ChordAnalyzer>(sq::dsp::kChordAnalyzerClassId, sq::dsp::kAnalysisCategory,
                                  "Squizzy Chord and Key Detector"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'H', 'R', 'D'),
    .name = "analysis-chords",
    .description = "Chromagram chord and key detection",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
