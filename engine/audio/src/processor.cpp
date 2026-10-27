// Squizzy Studio engine/audio - processors, RT markers.
#include "sq/engine/audio/processor.hpp"

#include <cmath>
#include <cstring>

namespace sq::engine::audio {

// ---- rt ----
namespace {
thread_local bool t_inRt = false;
}
namespace rt {
bool inRealtimeContext() noexcept { return t_inRt; }
Scope::Scope() noexcept : prev_(t_inRt) { t_inRt = true; }
Scope::~Scope() { t_inRt = prev_; }
Unscope::Unscope() noexcept : prev_(t_inRt) { t_inRt = false; }
Unscope::~Unscope() { t_inRt = prev_; }
}  // namespace rt

// ---- laws ----
float dbToGain(double db) noexcept {
  if (db <= -144.0) return 0.0f;
  return static_cast<float>(std::pow(10.0, db / 20.0));
}

void panGainsMono(float pan, float* left, float* right) noexcept {
  if (pan < -1.0f) pan = -1.0f;
  if (pan > 1.0f) pan = 1.0f;
  const double theta = (static_cast<double>(pan) + 1.0) * 0.25 * 3.14159265358979323846;
  *left = static_cast<float>(std::cos(theta));
  *right = static_cast<float>(std::sin(theta));
}

void panGainsStereo(float pan, float* left, float* right) noexcept {
  if (pan < -1.0f) pan = -1.0f;
  if (pan > 1.0f) pan = 1.0f;
  *left = pan > 0.0f ? 1.0f - pan : 1.0f;
  *right = pan < 0.0f ? 1.0f + pan : 1.0f;
}

// ---- chain ----
bool ProcessorChain::prepare(double sr, uint32_t maxFrames, uint32_t channels) {
  for (auto& p : procs_)
    if (!p->prepare(sr, maxFrames, channels)) return false;
  return true;
}
void ProcessorChain::reset() noexcept {
  for (auto& p : procs_) p->reset();
}
void ProcessorChain::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  for (auto& p : procs_) p->process(ctx, io, channels);
}
uint32_t ProcessorChain::latency() const noexcept {
  uint32_t l = 0;
  for (auto& p : procs_) l += p->latency();
  return l;
}
uint32_t ProcessorChain::tail() const noexcept {
  uint32_t t = 0;
  for (auto& p : procs_) t += p->tail();
  return t;
}
ProcessorChain ProcessorChain::cloneChain() const {
  ProcessorChain c;
  for (auto& p : procs_) c.add(p->clone());
  return c;
}

// ---- GainFx ----
void GainFx::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  for (uint32_t c = 0; c < channels; ++c)
    for (uint32_t i = 0; i < ctx.frames; ++i) io[c][i] *= gain_;
}

// ---- DelayFx ----
bool DelayFx::prepare(double, uint32_t maxFrames, uint32_t channels) {
  line_.prepare(channels, frames_, maxFrames);
  return true;
}
void DelayFx::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  line_.process(io, channels, ctx.frames);
}

// ---- ParallelProcessor ----
ProcessorPtr ParallelProcessor::clone() const {
  auto p = std::make_unique<ParallelProcessor>();
  p->protoBranches_ = protoBranches_;
  return p;
}

bool ParallelProcessor::prepare(double sr, uint32_t maxFrames, uint32_t channels) {
  channels_ = channels;
  maxFrames_ = maxFrames;
  branches_.clear();
  for (auto& pb : protoBranches_) {
    ProcessorChain ch;
    for (auto& proto : pb) ch.add(proto->clone());
    if (!ch.prepare(sr, maxFrames, channels)) return false;
    branches_.push_back(std::move(ch));
  }
  latency_ = 0;
  for (auto& b : branches_) latency_ = b.latency() > latency_ ? b.latency() : latency_;
  comp_.assign(branches_.size(), DelayLine{});
  for (size_t i = 0; i < branches_.size(); ++i) comp_[i].prepare(channels, latency_ - branches_[i].latency(), maxFrames);
  scratch_.assign(static_cast<size_t>(channels) * maxFrames * 2, 0.0f);
  scratchPtrs_.assign(static_cast<size_t>(channels) * 2, nullptr);
  for (uint32_t c = 0; c < channels * 2; ++c) scratchPtrs_[c] = &scratch_[static_cast<size_t>(c) * maxFrames];
  return true;
}

void ParallelProcessor::reset() noexcept {
  for (auto& b : branches_) b.reset();
  for (auto& d : comp_) d.reset();
}

void ParallelProcessor::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  if (branches_.empty()) return;
  const uint32_t nch = channels < channels_ ? channels : channels_;
  float* const* dry = scratchPtrs_.data();          // copy of the input
  float* const* work = scratchPtrs_.data() + channels_;
  for (uint32_t c = 0; c < nch; ++c) std::memcpy(dry[c], io[c], sizeof(float) * ctx.frames);
  for (uint32_t c = 0; c < nch; ++c) std::memset(io[c], 0, sizeof(float) * ctx.frames);
  for (size_t b = 0; b < branches_.size(); ++b) {
    for (uint32_t c = 0; c < nch; ++c) std::memcpy(work[c], dry[c], sizeof(float) * ctx.frames);
    branches_[b].process(ctx, work, nch);
    comp_[b].process(work, nch, ctx.frames);
    for (uint32_t c = 0; c < nch; ++c)
      for (uint32_t i = 0; i < ctx.frames; ++i) io[c][i] += work[c][i];
  }
}

// ---- SineSynth ----
bool SineSynth::prepare(double sr, uint32_t, uint32_t) {
  sr_ = sr;
  reset();
  return true;
}
void SineSynth::reset() noexcept {
  for (auto& v : voices_) v = Voice{};
}
void SineSynth::handle(const sq_midi_event& e) noexcept {
  const uint8_t st = e.bytes[0] & 0xF0u;
  const uint8_t note = e.bytes[1];
  const uint8_t vel = e.bytes[2];
  if (st == 0x90 && vel > 0) {
    int slot = -1;
    for (int i = 0; i < kVoices; ++i)
      if (!voices_[i].active) { slot = i; break; }
    if (slot < 0) slot = 0;
    Voice& v = voices_[slot];
    v.active = true;
    v.note = note;
    v.phase = 0.0;
    v.inc = 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0) / sr_;
    v.amp = 0.25f * static_cast<float>(vel) / 127.0f;
  } else if (st == 0x80 || (st == 0x90 && vel == 0)) {
    for (auto& v : voices_)
      if (v.active && v.note == note) v.active = false;
  } else if (st == 0xB0 && (note == 120 || note == 123)) {
    reset();
  }
}
void SineSynth::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  uint32_t ev = 0;
  for (uint32_t i = 0; i < ctx.frames; ++i) {
    while (ev < ctx.midiCount && ctx.midi[ev].frame_offset <= i) handle(ctx.midi[ev++]);
    float s = 0.0f;
    for (auto& v : voices_) {
      if (!v.active) continue;
      s += v.amp * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * v.phase));
      v.phase += v.inc;
      if (v.phase >= 1.0) v.phase -= 1.0;
    }
    for (uint32_t c = 0; c < channels; ++c) io[c][i] += s;
  }
  while (ev < ctx.midiCount) handle(ctx.midi[ev++]);
}

}  // namespace sq::engine::audio
