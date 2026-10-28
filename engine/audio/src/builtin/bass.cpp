// Squizzy Studio engine/audio - "Squizzy Bass": monophonic bass synth (saw/square + sub, 24 dB
// resonant lowpass with decay envelope, glide, legato, drive).
#include "device.hpp"
#include "voices.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t {
  kWave, kSub, kCutoff, kReso, kEnvAmt, kEnvDecay, kAccent,
  kAttack, kDecay, kSustain, kRelease, kGlide, kLegato, kDrive, kGain, kCount
};

class Bass final : public SynthDevice {
 public:
  Bass() : SynthDevice(infoOf("bass")) {}

 protected:
  std::unique_ptr<SynthDevice> fresh() const override { return std::make_unique<Bass>(); }
  void onReset() noexcept override {
    osc_.reset(0.0f);
    osc2_.reset(0.0f);
    sub_.reset(0.0f);
    amp_.kill();
    fenv_ = 0.0f;
    for (auto& f : f_) f.reset();
    stack_.clear();
    pitch_ = target_ = 36.0f;
    vel_ = 0.0f;
    bend_ = 0.0f;
    sustain_ = false;
    gateHeld_ = false;
  }
  void onParams(bool snap) noexcept override {
    const double s = sr();
    ampP_.set(s, p(kAttack), p(kDecay), p(kSustain), p(kRelease));
    fenvCoef_ = dsp::decayCoef(s, p(kEnvDecay));
    const float g = dsp::dbToLin(p(kGain));
    if (snap) gain_.snap(g); else gain_.target = g;
  }
  void onMidi(const sq_midi_event& e) noexcept override {
    const uint8_t st = midiStatus(e);
    if (isNoteOn(e)) {
      const uint8_t note = e.bytes[1] & 0x7F;
      const bool legato = amp_.active() && gateHeld_ && !stack_.empty() && on(kLegato);
      stack_.push(note);
      target_ = static_cast<float>(note);
      if (!amp_.active() || t(kGlide) < 0.5f) pitch_ = target_;
      gateHeld_ = true;
      if (!legato) {
        vel_ = static_cast<float>(e.bytes[2]) / 127.0f;
        if (!amp_.active()) {
          osc_.reset(0.0f);
          osc2_.reset(0.0f);
          sub_.reset(0.0f);
        }
        amp_.gateOn();
        fenv_ = 1.0f;
      }
    } else if (isNoteOff(e)) {
      const uint8_t note = e.bytes[1] & 0x7F;
      stack_.remove(note);
      if (!stack_.empty()) {
        target_ = static_cast<float>(stack_.top());
        if (t(kGlide) < 0.5f) pitch_ = target_;
        if (!on(kLegato)) fenv_ = 1.0f, amp_.gateOn();
      } else if (!sustain_) {
        gateHeld_ = false;
        amp_.gateOff();
      }
    } else if (st == 0xE0) {
      bend_ = pitchBendValue(e) * 2.0f;
    } else if (st == 0xB0) {
      const uint8_t cc = e.bytes[1];
      if (cc == 64) {
        sustain_ = e.bytes[2] >= 64;
        if (!sustain_ && stack_.empty()) gateHeld_ = false, amp_.gateOff();
      } else if (cc == 123) {
        stack_.clear();
        gateHeld_ = false;
        amp_.gateOff();
      } else if (cc == 120) {
        stack_.clear();
        gateHeld_ = false;
        amp_.kill();
      }
    }
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    gain_.begin(gain_.target, n);
    if (!amp_.active()) {
      gain_.end();
      return;
    }
    const double s = sr();
    const float glideMs = p(kGlide);
    if (glideMs > 0.5f) pitch_ = target_ + (pitch_ - target_) * static_cast<float>(std::exp(-1000.0 * n / (glideMs * 0.25 * s)));
    else pitch_ = target_;
    const float semis = pitch_ + bend_;
    const float dt = static_cast<float>(dsp::noteToHz(semis) / s);
    const int wave = ci(kWave);
    const float subL = p(kSub);
    const double fc = p(kCutoff) * std::exp2((p(kEnvAmt) * fenv_ * (1.0f - p(kAccent) + p(kAccent) * vel_) +
                                              (static_cast<float>(semis) - 36.0f) * 0.3f) / 12.0);
    ca_.set(s, fc, 1.41421356);
    cb_.set(s, fc, dsp::SvfCoefs::dampFromRes(p(kReso) * 0.97f));
    const float drive = 1.0f + 6.0f * p(kDrive);
    const float out = 0.65f * (0.6f + 0.4f * vel_) * 1.6f / (1.0f + p(kDrive));
    for (uint32_t i = 0; i < n; ++i) {
      float x = 0.0f;
      if (wave == 0 || wave == 2) x += osc_.tick(dsp::Wave::Saw, dt, 0.5f, rng_);
      if (wave == 1 || wave == 2) x += osc2_.tick(dsp::Wave::Square, dt, 0.5f, rng_) * 0.8f;
      if (wave == 2) x *= 0.6f;
      x += subL * sub_.tick(dsp::Wave::Square, dt * 0.5f, 0.5f, rng_) * 0.7f;
      f_[0].tick(ca_, x);
      f_[1].tick(cb_, f_[0].lp);
      const float y = dsp::softClip(f_[1].lp * drive * 0.6f) * amp_.tick(ampP_) * out * gain_.next();
      addStereo(io, nch, i, y, y);
      fenv_ *= fenvCoef_;
    }
    gain_.end();
    fenv_ = dsp::flush(fenv_);
    for (auto& f : f_) f.flushDenormals();
  }

 private:
  dsp::BlepOsc osc_, osc2_, sub_;
  dsp::Adsr amp_;
  dsp::AdsrParams ampP_;
  dsp::Svf f_[2];
  dsp::SvfCoefs ca_, cb_;
  dsp::Ramp gain_;
  dsp::Rng rng_;
  NoteStack stack_;
  float fenv_ = 0.0f, fenvCoef_ = 0.0f;
  float pitch_ = 36.0f, target_ = 36.0f, vel_ = 0.0f, bend_ = 0.0f;
  bool sustain_ = false, gateHeld_ = false;
};

}  // namespace

DeviceInfo BassInfo() {
  return makeInfo("bass", "Squizzy Bass", "Instrument", true,
                  {
                      pChoice("wave", "Wave", {"Saw", "Square", "Saw + Square"}, 0),
                      pCont("sub", "Sub Level", 0, 1, 0.5f),
                      pLog("cutoff", "Cutoff", 30, 12000, 320, "Hz"),
                      pCont("resonance", "Resonance", 0, 1, 0.35f),
                      pCont("env_amount", "Env Amount", 0, 72, 36, "st"),
                      pLog("env_decay", "Env Decay", 10, 5000, 260, "ms"),
                      pCont("accent", "Velocity > Filter", 0, 1, 0.5f),
                      pLog("attack", "Attack", 0.5f, 5000, 2, "ms"),
                      pLog("decay", "Decay", 1, 10000, 600, "ms"),
                      pCont("sustain", "Sustain", 0, 1, 0.8f),
                      pLog("release", "Release", 1, 5000, 60, "ms"),
                      pCont("glide", "Glide", 0, 1000, 40, "ms"),
                      pToggle("legato", "Legato", true),
                      pCont("drive", "Drive", 0, 1, 0.25f),
                      pCont("gain", "Gain", -36, 6, -3, "dB"),
                  });
}

ProcessorPtr makeBass() { return std::make_unique<Bass>(); }

}  // namespace sq::engine::audio::builtin
