// Squizzy Studio engine/audio - private base for built-in devices: parameter storage, smoothing,
// sub-block scheduling (sample-accurate MIDI) and clone().
#pragma once

#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dsp.hpp"
#include "sq/engine/audio/builtin.hpp"
#include "sq/engine/audio/nodes.hpp"

namespace sq::engine::audio::builtin {

inline constexpr uint32_t kMaxParams = 64;
inline constexpr uint32_t kSubBlock = 16;   // control-rate granularity (frames)
inline constexpr double kSmoothMs = 25.0;   // parameter smoothing time constant

// ---- ParamInfo builders (catalog definitions) ----
inline ParamInfo pCont(const char* id, const char* name, float mn, float mx, float def, const char* unit = "", bool log = false) {
  ParamInfo p;
  p.id = id; p.name = name; p.min = mn; p.max = mx; p.def = def; p.unit = unit; p.logScale = log;
  return p;
}
inline ParamInfo pLog(const char* id, const char* name, float mn, float mx, float def, const char* unit) {
  return pCont(id, name, mn, mx, def, unit, true);
}
inline ParamInfo pInt(const char* id, const char* name, float mn, float mx, float def, const char* unit = "") {
  ParamInfo p = pCont(id, name, mn, mx, def, unit);
  p.kind = ParamKind::Integer;
  return p;
}
inline ParamInfo pToggle(const char* id, const char* name, bool def) {
  ParamInfo p = pCont(id, name, 0, 1, def ? 1.0f : 0.0f);
  p.kind = ParamKind::Toggle;
  return p;
}
inline ParamInfo pChoice(const char* id, const char* name, std::initializer_list<const char*> labels, int def) {
  ParamInfo p;
  p.id = id; p.name = name; p.kind = ParamKind::Choice;
  for (const char* l : labels) p.choices.emplace_back(l);
  p.min = 0; p.max = static_cast<float>(p.choices.size() - 1); p.def = static_cast<float>(def);
  return p;
}
inline DeviceInfo makeInfo(const char* id, const char* name, const char* category, bool instrument, std::vector<ParamInfo> params) {
  DeviceInfo d;
  d.id = id; d.name = name; d.category = category; d.instrument = instrument; d.params = std::move(params);
  return d;
}
// Clamp (and round for discrete kinds) a plain value into a parameter's range.
inline float clampParam(const ParamInfo& p, double v) noexcept {
  if (!std::isfinite(v)) return p.def;
  double c = v < p.min ? p.min : (v > p.max ? p.max : v);
  if (p.kind != ParamKind::Continuous) c = std::floor(c + 0.5);
  return static_cast<float>(c);
}
const DeviceInfo& infoOf(const char* id);  // catalog entry (must exist)

// ---- base ----
template <class Base>
class Device : public Base {
 public:
  explicit Device(const DeviceInfo& info) : info_(&info) {
    count_ = static_cast<uint32_t>(info.params.size() < kMaxParams ? info.params.size() : kMaxParams);
    for (uint32_t i = 0; i < count_; ++i) {
      const ParamInfo& p = info.params[i];
      target_[i] = p.def;
      cur_[i] = p.def;
      logDomain_[i] = p.logScale && p.min > 0.0f && p.kind == ParamKind::Continuous;
      smooth_[i] = toDomain(i, p.def);
      eps_[i] = logDomain_[i] ? 1e-5f : 1e-6f * (p.max - p.min);
    }
  }

  uint32_t parameterCount() const noexcept override { return count_; }
  void setParameter(uint32_t index, float plain) noexcept override {
    if (index >= count_) return;
    target_[index] = clampParam(info_->params[index], plain);
  }
  float parameter(uint32_t index) const noexcept override { return index < count_ ? target_[index] : 0.0f; }

  ProcessorPtr clone() const final {
    std::unique_ptr<Device> d = fresh();
    for (uint32_t i = 0; i < count_; ++i) d->setParameter(i, target_[i]);
    d->snapAll();
    return d;
  }

  bool prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) final {
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxFrames_ = maxFrames;
    channels_ = channels < kMaxChannels ? channels : kMaxChannels;
    if (channels_ == 0) channels_ = 1;
    coef1_ = std::exp(-1000.0 / (kSmoothMs * sr_));
    coefSub_ = std::pow(coef1_, static_cast<double>(kSubBlock));
    snapAll();
    if (!onPrepare()) return false;
    prepared_ = true;
    snap_ = true;
    onReset();
    return true;
  }
  void reset() noexcept final {
    if (!prepared_) return;
    snapAll();
    snap_ = true;
    onReset();
  }

  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept final {
    if (!prepared_) return;
    onContext(ctx);
    const uint32_t nch = channels < channels_ ? channels : channels_;
    uint32_t ev = 0;
    uint32_t pos = 0;
    float* sub[kMaxChannels];
    while (pos < ctx.frames) {
      uint32_t evEnd = ev;
      while (evEnd < ctx.midiCount && ctx.midi[evEnd].frame_offset <= pos) ++evEnd;
      uint32_t n = ctx.frames - pos;
      if (n > kSubBlock) n = kSubBlock;
      if (evEnd < ctx.midiCount && ctx.midi[evEnd].frame_offset - pos < n) n = ctx.midi[evEnd].frame_offset - pos;
      smoothStep(n);  // parameters first, so events see the current (discrete) settings
      while (ev < evEnd) onMidi(ctx.midi[ev++]);
      for (uint32_t c = 0; c < nch; ++c) sub[c] = io[c] + pos;
      render(sub, nch, n);
      pos += n;
    }
    while (ev < ctx.midiCount) onMidi(ctx.midi[ev++]);
  }

  const DeviceInfo& info() const noexcept { return *info_; }

 protected:
  // Derived hooks.
  virtual std::unique_ptr<Device> fresh() const = 0;
  virtual bool onPrepare() { return true; }       // allocate (control thread)
  virtual void onReset() noexcept {}              // clear runtime state
  // Called before render() whenever a smoothed value moved (and once after prepare/reset with
  // snap = true: jump ramps to their targets).
  virtual void onParams(bool snap) noexcept = 0;
  virtual void onMidi(const sq_midi_event& e) noexcept { (void)e; }
  // Called at the start of every process() with the block context (song tempo etc.), before
  // any parameter smoothing / onParams of the block.
  virtual void onContext(const ProcessContext& ctx) noexcept { (void)ctx; }
  virtual void render(float* const* io, uint32_t nch, uint32_t n) noexcept = 0;

  // Smoothed plain value (updated once per sub-block).
  float p(uint32_t i) const noexcept { return cur_[i]; }
  // Raw target (discrete parameters, values read at note-on).
  float t(uint32_t i) const noexcept { return target_[i]; }
  int ci(uint32_t i) const noexcept { return static_cast<int>(target_[i] + 0.5f); }
  bool on(uint32_t i) const noexcept { return target_[i] >= 0.5f; }
  double sr() const noexcept { return sr_; }
  uint32_t channels() const noexcept { return channels_; }
  uint32_t maxFrames() const noexcept { return maxFrames_; }
  bool prepared() const noexcept { return prepared_; }

 private:
  float toDomain(uint32_t i, float v) const noexcept { return logDomain_[i] ? std::log(v) : v; }
  void snapAll() noexcept {
    for (uint32_t i = 0; i < count_; ++i) {
      smooth_[i] = toDomain(i, target_[i]);
      cur_[i] = target_[i];
    }
  }
  void smoothStep(uint32_t n) noexcept {
    const double a = n == kSubBlock ? coefSub_ : std::pow(coef1_, static_cast<double>(n));
    const float k = static_cast<float>(1.0 - a);
    bool changed = snap_;
    for (uint32_t i = 0; i < count_; ++i) {
      const float tgt = toDomain(i, target_[i]);
      float s = smooth_[i];
      if (s == tgt) continue;
      if (info_->params[i].kind != ParamKind::Continuous || snap_) {
        s = tgt;
      } else {
        s += (tgt - s) * k;
        if (std::fabs(tgt - s) <= eps_[i]) s = tgt;
      }
      smooth_[i] = s;
      cur_[i] = s == tgt ? target_[i] : (logDomain_[i] ? std::exp(s) : s);
      changed = true;
    }
    if (changed) onParams(snap_);
    snap_ = false;
  }

  const DeviceInfo* info_;
  uint32_t count_ = 0;
  std::array<float, kMaxParams> target_{};
  std::array<float, kMaxParams> smooth_{};
  std::array<float, kMaxParams> cur_{};
  std::array<float, kMaxParams> eps_{};
  std::array<bool, kMaxParams> logDomain_{};
  double sr_ = 48000.0;
  double coef1_ = 0.0, coefSub_ = 0.0;
  uint32_t maxFrames_ = 0;
  uint32_t channels_ = 0;
  bool prepared_ = false;
  bool snap_ = true;
};

using FxDevice = Device<FxProcessor>;
using SynthDevice = Device<SynthProcessor>;

// Output helper for stereo-rendering instruments: L to even channels, R to odd, mono = average.
inline void addStereo(float* const* io, uint32_t nch, uint32_t i, float l, float r) noexcept {
  if (nch == 1) {
    io[0][i] += 0.5f * (l + r);
    return;
  }
  for (uint32_t c = 0; c < nch; ++c) io[c][i] += (c & 1u) ? r : l;
}

// MIDI helpers.
inline uint8_t midiStatus(const sq_midi_event& e) noexcept { return static_cast<uint8_t>(e.bytes[0] & 0xF0u); }
inline bool isNoteOn(const sq_midi_event& e) noexcept { return midiStatus(e) == 0x90 && e.bytes[2] > 0; }
inline bool isNoteOff(const sq_midi_event& e) noexcept {
  return midiStatus(e) == 0x80 || (midiStatus(e) == 0x90 && e.bytes[2] == 0);
}
inline float pitchBendValue(const sq_midi_event& e) noexcept {  // -1..1
  const int v = (static_cast<int>(e.bytes[2] & 0x7F) << 7) | (e.bytes[1] & 0x7F);
  return static_cast<float>(v - 8192) / 8192.0f;
}

// ---- per-device catalog entries + factories (one pair per source file) ----
#define SQ_BUILTIN_DECL(name) \
  DeviceInfo name##Info();    \
  ProcessorPtr make##name()
SQ_BUILTIN_DECL(Synth);
SQ_BUILTIN_DECL(Drums);
SQ_BUILTIN_DECL(Keys);
SQ_BUILTIN_DECL(Bass);
SQ_BUILTIN_DECL(Sampler);
SQ_BUILTIN_DECL(Eq);
SQ_BUILTIN_DECL(Compressor);
SQ_BUILTIN_DECL(Limiter);
SQ_BUILTIN_DECL(Gate);
SQ_BUILTIN_DECL(Reverb);
SQ_BUILTIN_DECL(Delay);
SQ_BUILTIN_DECL(Chorus);
SQ_BUILTIN_DECL(Flanger);
SQ_BUILTIN_DECL(Phaser);
SQ_BUILTIN_DECL(Distortion);
SQ_BUILTIN_DECL(Filter);
SQ_BUILTIN_DECL(Utility);
SQ_BUILTIN_DECL(Tremolo);
#undef SQ_BUILTIN_DECL
ProcessorPtr makeSamplerWithMedia(MediaPtr media);

}  // namespace sq::engine::audio::builtin
