// Squizzy Studio engine/audio - "Squizzy Keys": two-operator FM electric piano / bell with a
// separate high-ratio "tine" pair for the attack, velocity -> brightness and stereo tremolo.
#include "device.hpp"
#include "voices.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t {
  kBrightness, kVelBright, kModRatio, kTine, kDecay, kRelease, kModDecay,
  kTremRate, kTremDepth, kTremStereo, kVelSens, kGain, kCount
};
constexpr float kRatios[] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f, 14.0f};
constexpr int kVoices = 16;

struct Voice {
  VoiceSlot slot;
  float vel = 0.0f;
  float freq = 0.0f;
  double cph = 0.0, mph = 0.0, tph = 0.0;
  float amp = 0.0f, attack = 0.0f, ampCoef = 0.0f;
  bool releasing = false;
  float modEnv = 0.0f, modCoef = 0.0f;
  float tine = 0.0f, tineCoef = 0.0f;
  float index = 0.0f;
};

class Keys final : public SynthDevice {
 public:
  Keys() : SynthDevice(infoOf("keys")) {}

 protected:
  std::unique_ptr<SynthDevice> fresh() const override { return std::make_unique<Keys>(); }
  bool onPrepare() override {
    attackInc_ = static_cast<float>(1.0 / std::max(1.0, 0.0015 * sr()));
    return true;
  }
  void onReset() noexcept override {
    for (auto& v : v_) v = Voice{};
    bend_ = 0.0f;
    sustain_ = false;
    lfoPhase_ = 0.0;
  }
  void onParams(bool snap) noexcept override {
    const float g = dsp::dbToLin(p(kGain));
    if (snap) gain_.snap(g); else gain_.target = g;
    relCoef_ = dsp::decayCoef(sr(), p(kRelease));
  }
  void onMidi(const sq_midi_event& e) noexcept override {
    const uint8_t st = midiStatus(e);
    if (isNoteOn(e)) {
      noteOn(e.bytes[1] & 0x7F, static_cast<float>(e.bytes[2]) / 127.0f);
    } else if (isNoteOff(e)) {
      noteOffVoices(v_, kVoices, e.bytes[1] & 0x7F, sustain_, [](Voice& v) { v.releasing = true; });
    } else if (st == 0xE0) {
      bend_ = pitchBendValue(e) * 2.0f;
    } else if (st == 0xB0) {
      const uint8_t cc = e.bytes[1];
      if (cc == 64) {
        sustain_ = e.bytes[2] >= 64;
        if (!sustain_)
          for (auto& v : v_)
            if (v.slot.sustained) v.slot.sustained = false, v.releasing = true;
      } else if (cc == 123) {
        for (auto& v : v_) v.slot.held = v.slot.sustained = false, v.releasing = true;
      } else if (cc == 120) {
        for (auto& v : v_) v = Voice{};
      }
    }
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const double s = sr();
    const float lfo = static_cast<float>(std::sin(dsp::kTwoPi * lfoPhase_));
    lfoPhase_ += static_cast<double>(p(kTremRate)) * n / s;
    lfoPhase_ -= std::floor(lfoPhase_);
    const float depth = p(kTremDepth);
    const float stereo = p(kTremStereo);
    tl_.begin(1.0f - depth * 0.5f * (1.0f + lfo), n);
    tr_.begin(1.0f - depth * 0.5f * (1.0f + lfo * (1.0f - 2.0f * stereo)), n);
    gain_.begin(gain_.target, n);
    const double ratio = kRatios[ci(kModRatio)];
    const double bendMul = std::exp2(static_cast<double>(bend_) / 12.0);
    const float tineLevel = p(kTine);
    for (auto& v : v_) {
      if (!v.slot.active) continue;
      const double f = v.freq * bendMul;
      const double cInc = f / s, mInc = f * ratio / s, tInc = f * 14.0 / s;
      // Phase modulation puts sidebands at f +- k*ratio*f; fade the modulation out as the first
      // upper sideband nears Nyquist so high notes do not fold back into inharmonic partials.
      const float bodyFade = nyquistFade((1.0 + ratio) * f, s);
      const float tineFade = nyquistFade(15.0 * f, s);
      dsp::Ramp a = tl_, b = tr_, g = gain_;
      for (uint32_t i = 0; i < n; ++i) {
        if (v.attack < 1.0f) v.attack = std::min(1.0f, v.attack + attackInc_);
        const float idx = v.index * (0.2f + 0.8f * v.modEnv) * bodyFade;
        const float m = static_cast<float>(std::sin(dsp::kTwoPi * v.mph));
        const float body = static_cast<float>(std::sin(dsp::kTwoPi * v.cph + idx * m));
        const float tm = static_cast<float>(std::sin(dsp::kTwoPi * v.tph));
        const float tine = static_cast<float>(std::sin(dsp::kTwoPi * v.cph + 1.3f * tm)) * v.tine * tineLevel * tineFade;
        const float y = (body + tine) * v.amp * v.attack * v.vel * 0.3f * g.next();
        addStereo(io, nch, i, y * a.next(), y * b.next());
        v.cph += cInc; v.cph -= std::floor(v.cph);
        v.mph += mInc; v.mph -= std::floor(v.mph);
        v.tph += tInc; v.tph -= std::floor(v.tph);
        v.amp *= v.releasing ? relCoef_ : v.ampCoef;
        v.modEnv *= v.modCoef;
        v.tine *= v.tineCoef;
      }
      v.modEnv = dsp::flush(v.modEnv);
      v.tine = dsp::flush(v.tine);
      if (v.amp < 1e-5f) v.slot.active = false;
    }
    tl_.end();
    tr_.end();
    gain_.end();
  }

 private:
  // 1 while `hz` is below 0.35 * sr, falling linearly to 0 at 0.45 * sr.
  static float nyquistFade(double hz, double rate) noexcept {
    return static_cast<float>(std::clamp((0.45 * rate - hz) / (0.1 * rate), 0.0, 1.0));
  }

  void noteOn(uint8_t note, float vel) noexcept {
    const int i = allocateVoice(v_, kVoices, note, [](const Voice& v) { return v.amp; });
    Voice& v = v_[i];
    const bool fresh = !v.slot.active;
    const double s = sr();
    v.slot.active = true;
    v.slot.held = true;
    v.slot.sustained = false;
    v.slot.note = note;
    v.slot.age = ++clock_;
    v.releasing = false;
    v.freq = static_cast<float>(dsp::noteToHz(note));
    const float vs = p(kVelSens);
    v.vel = (1.0f - vs) + vs * vel;
    const float keyScale = dsp::clampv(1.25f - (static_cast<float>(note) - 36.0f) / 80.0f, 0.3f, 1.25f);
    const float bright = dsp::clampv(p(kBrightness) + p(kVelBright) * (vel - 0.6f), 0.0f, 1.2f);
    v.index = bright * 4.0f * keyScale;
    const double keyDecay = std::exp2(-(static_cast<double>(note) - 60.0) / 24.0);
    v.ampCoef = dsp::decayCoef(s, p(kDecay) * keyDecay);
    v.modCoef = dsp::decayCoef(s, p(kModDecay) * keyDecay);
    v.tineCoef = dsp::decayCoef(s, 70.0 * keyDecay);
    v.modEnv = 1.0f;
    v.tine = 0.6f + 0.4f * vel;
    if (fresh) {
      v.cph = v.mph = v.tph = 0.0;
      v.attack = 0.0f;
      v.amp = 1.0f;
    } else {
      // retrigger: continue from the current level and ramp up to full again (no click)
      v.attack = dsp::clampv(v.amp * v.attack, 0.0f, 1.0f);
      v.amp = 1.0f;
    }
  }

  Voice v_[kVoices];
  dsp::Ramp tl_, tr_, gain_;
  double lfoPhase_ = 0.0;
  float bend_ = 0.0f, relCoef_ = 0.0f, attackInc_ = 0.01f;
  bool sustain_ = false;
  uint64_t clock_ = 0;
};

}  // namespace

DeviceInfo KeysInfo() {
  return makeInfo("keys", "Squizzy Keys", "Instrument", true,
                  {
                      pCont("brightness", "Brightness", 0, 1, 0.4f),
                      pCont("velocity_brightness", "Velocity > Brightness", 0, 1, 0.6f),
                      pChoice("mod_ratio", "Modulator Ratio", {"0.5", "1", "2", "3", "4", "5", "7", "14"}, 1),
                      pCont("tine", "Tine", 0, 1, 0.35f),
                      pLog("decay", "Decay", 200, 20000, 3500, "ms"),
                      pLog("release", "Release", 10, 5000, 300, "ms"),
                      pLog("mod_decay", "Brightness Decay", 20, 10000, 900, "ms"),
                      pLog("tremolo_rate", "Tremolo Rate", 0.1f, 12, 4.5f, "Hz"),
                      pCont("tremolo_depth", "Tremolo Depth", 0, 1, 0.25f),
                      pCont("tremolo_stereo", "Tremolo Stereo", 0, 1, 1),
                      pCont("velocity", "Velocity Sensitivity", 0, 1, 0.7f),
                      pCont("gain", "Gain", -36, 6, -3, "dB"),
                  });
}

ProcessorPtr makeKeys() { return std::make_unique<Keys>(); }

}  // namespace sq::engine::audio::builtin
