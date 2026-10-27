// Squizzy Studio - header-only IAnalysisResult implementation shared by the analysis services
// (analysis-beat, analysis-chords, analysis-stems). Events + optional stems held in memory.
// Not RT-safe (allocates); built on the worker that runs IAnalyzer::finish().
#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "sq/cpp/object.hpp"
#include "sq/interfaces/dsp.h"

namespace sq::dspkit {

inline sq_analysis_event makeEvent(int32_t type, double time, double duration, double value, float confidence,
                                   const char* label = nullptr) {
  sq_analysis_event e{};
  e.struct_size = sizeof e;
  e.type = type;
  e.time = time;
  e.duration = duration;
  e.value = value;
  e.confidence = confidence;
  if (label) {
    std::strncpy(e.label, label, sizeof e.label - 1);
    e.label[sizeof e.label - 1] = 0;
  }
  return e;
}

class AnalysisResult final : public sq::Object<AnalysisResult, sq::IAnalysisResult> {
 public:
  std::vector<sq_analysis_event> events;
  struct Stem {
    std::string name;
    std::vector<std::vector<float>> channels;
  };
  std::vector<Stem> stems;

  uint32_t SQ_CALL getEventCount() override { return static_cast<uint32_t>(events.size()); }
  sq_result SQ_CALL getEvent(uint32_t index, sq_analysis_event* out) override {
    if (!out || out->struct_size < sizeof(sq_analysis_event)) return SQ_E_INVALIDARG;
    if (index >= events.size()) return SQ_E_NOTFOUND;
    *out = events[index];
    return SQ_OK;
  }
  uint32_t SQ_CALL getStemCount() override { return static_cast<uint32_t>(stems.size()); }
  sq_result SQ_CALL getStemName(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override {
    if (index >= stems.size()) return SQ_E_NOTFOUND;
    const std::string& n = stems[index].name;
    const uint32_t need = static_cast<uint32_t>(n.size() + 1);
    if (needed) *needed = need;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, n.c_str(), need);
    return SQ_OK;
  }
  sq_result SQ_CALL readStem(uint32_t index, sq_sample_time start, sq_audio_block* dst, uint32_t* framesRead) override {
    if (framesRead) *framesRead = 0;
    if (!dst || !framesRead || (dst->channel_count && !dst->channels)) return SQ_E_INVALIDARG;
    if (index >= stems.size()) return SQ_E_NOTFOUND;
    const Stem& s = stems[index];
    const size_t len = s.channels.empty() ? 0 : s.channels[0].size();
    if (start < 0) return SQ_E_INVALIDARG;
    const size_t from = static_cast<size_t>(start);
    const uint32_t n = from >= len ? 0u : static_cast<uint32_t>(std::min<size_t>(dst->frames, len - from));
    for (uint32_t c = 0; c < dst->channel_count; ++c) {
      float* d = dst->channels[c];
      if (!d) continue;
      if (!s.channels.empty()) {
        const std::vector<float>& src = s.channels[std::min<size_t>(c, s.channels.size() - 1)];
        if (n) std::memcpy(d, src.data() + from, sizeof(float) * n);
      }
      if (n < dst->frames) std::memset(d + n, 0, sizeof(float) * (dst->frames - n));
    }
    *framesRead = n;
    return n ? SQ_OK : SQ_S_FALSE;
  }
};

// True when every channel pointer an analyzer would read is present (a block with 0 frames needs
// none). Allocation- and lock-free.
inline bool blockReadable(const sq_audio_block* b) noexcept {
  if (!b) return false;
  if (!b->frames) return true;
  if (!b->channels) return false;
  for (uint32_t c = 0; c < b->channel_count; ++c)
    if (!b->channels[c]) return false;
  return true;
}

// Copies the analyzer info strings into the POD (helper for IAnalyzer::getInfo).
inline sq_result fillInfo(sq_analyzer_info* out, const char* kind, const char* name, uint32_t flags) {
  if (!out || out->struct_size < sizeof(sq_analyzer_info)) return SQ_E_INVALIDARG;
  std::memset(out->kind, 0, sizeof out->kind);
  std::memset(out->name, 0, sizeof out->name);
  std::strncpy(out->kind, kind, sizeof out->kind - 1);
  std::strncpy(out->name, name, sizeof out->name - 1);
  out->flags = flags;
  return SQ_OK;
}

}  // namespace sq::dspkit
