// analysis-beat service: two IAnalyzer classes.
//   kBeatAnalyzerClassId  kind "beat"  (offline): SQ_ANALYSIS_TEMPO (time 0, value = bpm),
//                         SQ_ANALYSIS_BEAT (value = beat in bar, 1 = downbeat), SQ_ANALYSIS_ONSET.
//                         Options: "beats_per_bar" (int, 4), "min_bpm" (50), "max_bpm" (220).
//   kTunerClassId         kind "tuner" (realtime): process() is RT-safe, getRealtimeValue() is
//                         wait-free. Options: "reference_hz" (double, 440), "min_hz" (40).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "beat_core.hpp"
#include "sq/engine/simd/analysis_result.hpp"
#include "sq/sdk.hpp"
#include "tuner_core.hpp"

namespace {

// Finite rates only: an infinite/absurd rate would make the frame-size searches loop forever.
bool validRate(double sr) { return sr >= 4000.0 && sr <= 1.0e6; }

// Offline analyzers: non-finite samples (a NaN from a broken plug-in) become silence instead of
// poisoning every spectrum they touch.
void sanitize(float* x, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!std::isfinite(x[i])) x[i] = 0.0f;
}

class BeatAnalyzer final : public sq::Object<BeatAnalyzer, sq::IAnalyzer> {
 public:
  sq_result SQ_CALL getInfo(sq_analyzer_info* out) override {
    return sq::dspkit::fillInfo(out, "beat", "Squizzy Beat and Tempo Detector", SQ_ANALYZER_OFFLINE);
  }
  sq_result SQ_CALL prepare(double sampleRate, uint32_t channels, uint32_t maxBlockFrames, sq::IAttributes* options) override {
    if (!validRate(sampleRate) || channels == 0 || maxBlockFrames == 0) return SQ_E_INVALIDARG;
    sampleRate_ = sampleRate;
    opt_ = sq::beat::Options{};
    if (options) {
      opt_.beatsPerBar = static_cast<int>(std::clamp<int64_t>(options->getInt("beats_per_bar", 4), 1, 32));
      opt_.minBpm = std::clamp(options->getDouble("min_bpm", 50.0), 20.0, 400.0);
      opt_.maxBpm = std::clamp(options->getDouble("max_bpm", 220.0), opt_.minBpm + 1.0, 600.0);
    }
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
      sanitize(mono_.data() + base, block->frames);
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
      sq::beat::Result r = sq::beat::analyze(mono_, sampleRate_, opt_);
      auto* res = new sq::dspkit::AnalysisResult();
      const double total = static_cast<double>(mono_.size()) / sampleRate_;
      if (r.bpm > 0.0) res->events.push_back(sq::dspkit::makeEvent(SQ_ANALYSIS_TEMPO, 0.0, total, r.bpm, r.tempoConfidence));
      std::vector<sq_analysis_event> rest;
      for (const auto& b : r.beats) rest.push_back(sq::dspkit::makeEvent(SQ_ANALYSIS_BEAT, b.time, 0.0, b.indexInBar, b.strength));
      for (const auto& o : r.onsets) rest.push_back(sq::dspkit::makeEvent(SQ_ANALYSIS_ONSET, o.time, 0.0, 0.0, o.strength));
      std::stable_sort(rest.begin(), rest.end(),
                       [](const sq_analysis_event& a, const sq_analysis_event& b) { return a.time < b.time; });
      res->events.insert(res->events.end(), rest.begin(), rest.end());
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
  sq::beat::Options opt_;
  std::vector<float> mono_;
};

class Tuner final : public sq::Object<Tuner, sq::IAnalyzer> {
 public:
  sq_result SQ_CALL getInfo(sq_analyzer_info* out) override {
    return sq::dspkit::fillInfo(out, "tuner", "Squizzy Tuner", SQ_ANALYZER_REALTIME);
  }
  sq_result SQ_CALL prepare(double sampleRate, uint32_t channels, uint32_t maxBlockFrames, sq::IAttributes* options) override {
    if (!validRate(sampleRate) || channels == 0 || maxBlockFrames == 0) return SQ_E_INVALIDARG;
    double ref = options ? options->getDouble("reference_hz", 440.0) : 440.0;
    double minHz = options ? options->getDouble("min_hz", 40.0) : 40.0;
    if (!(ref > 300.0 && ref < 600.0)) ref = 440.0;
    minHz = std::clamp(minHz, 20.0, 500.0);
    try {
      if (!tracker_.prepare(sampleRate, maxBlockFrames, minHz)) return SQ_E_INVALIDARG;
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    reference_.store(ref, std::memory_order_relaxed);
    channels_ = channels;
    return SQ_OK;
  }
  // RT-safe
  sq_result SQ_CALL process(const sq_audio_block* block) override {
    if (!tracker_.prepared()) return SQ_E_FAIL;
    if (!sq::dspkit::blockReadable(block) || block->channel_count == 0) return SQ_E_INVALIDARG;
    if (!block->frames) return SQ_OK;
    tracker_.pushChannels(block->channels, block->channel_count, block->frames);
    return SQ_OK;
  }
  sq_result SQ_CALL finish(sq::IAnalysisResult** out) override {
    if (out) *out = nullptr;
    return SQ_E_UNSUPPORTED;  // realtime only
  }
  // wait-free
  sq_result SQ_CALL getRealtimeValue(sq_tuner_value* out) override {
    if (!out || out->struct_size < sizeof(sq_tuner_value)) return SQ_E_INVALIDARG;
    const sq::tuner::Estimate e = tracker_.latest();
    out->frequency_hz = e.frequency;
    out->confidence = e.confidence;
    if (e.frequency > 0.0) {
      const double midi = 69.0 + 12.0 * std::log2(e.frequency / reference_.load(std::memory_order_relaxed));
      out->midi_note = static_cast<int32_t>(std::lround(midi));
      out->cents = 100.0 * (midi - out->midi_note);
    } else {
      out->midi_note = 0;
      out->cents = 0.0;
    }
    return SQ_OK;
  }
  void SQ_CALL reset() override { tracker_.reset(); }

 private:
  sq::tuner::PitchTracker tracker_;
  std::atomic<double> reference_{440.0};
  uint32_t channels_ = 0;
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<BeatAnalyzer>(sq::dsp::kBeatAnalyzerClassId, sq::dsp::kAnalysisCategory,
                                 "Squizzy Beat and Tempo Detector"),
    sq::classEntry<Tuner>(sq::dsp::kTunerClassId, sq::dsp::kAnalysisCategory, "Squizzy Tuner"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('B', 'E', 'A', 'T'),
    .name = "analysis-beat",
    .description = "Onset, tempo and beat detection; realtime tuner",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
