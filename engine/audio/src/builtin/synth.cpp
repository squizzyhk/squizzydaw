// Squizzy Studio engine/audio - "Squizzy Synth": polyphonic subtractive synthesizer (the default
// instrument). 2 PolyBLEP oscillators with unison, sub + noise, ZDF state-variable filter (12/24 dB
// LP, HP, BP) with key tracking and its own ADSR, amp ADSR, global LFO, glide, mono/legato mode.
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t {
  kOsc1Wave, kOsc1Level, kPulseWidth, kOsc2Wave, kOsc2Level, kOsc2Semi, kOsc2Detune,
  kUnison, kUniDetune, kUniWidth, kSubLevel, kNoiseLevel,
  kFltType, kCutoff, kReso, kKeyTrack, kFEnvAmt, kVelFilter, kFAttack, kFDecay, kFSustain, kFRelease,
  kAAttack, kADecay, kASustain, kARelease,
  kLfoWave, kLfoRate, kLfoPitch, kLfoFilter, kLfoAmp, kModWheel,
  kGlide, kVelSens, kBendRange, kPolyphony, kDrive, kGain, kCount
};

constexpr int kVoices = 16;
constexpr int kMaxUnison = 4;

struct Voice {
  bool active = false;   // sounding (amp env not idle)
  bool held = false;     // key down
  bool sustained = false;
  uint8_t note = 0;
  float vel = 0.0f;
  uint64_t age = 0;
  float pitch = 60.0f;   // current (gliding) pitch in semitones
  float target = 60.0f;
  dsp::BlepOsc o1[kMaxUnison], o2[kMaxUnison], sub;
  dsp::Adsr amp, flt;
  dsp::Svf f[2][2];  // [channel][stage]
  dsp::SvfCoefs ca, cb;
  float dt1[kMaxUnison] = {}, dt2[kMaxUnison] = {}, dtSub = 0.0f;
};

class Synth final : public SynthDevice {
 public:
  Synth() : SynthDevice(infoOf("synth")) {}

 protected:
  std::unique_ptr<SynthDevice> fresh() const override { return std::make_unique<Synth>(); }

  void onReset() noexcept override {
    for (auto& v : voices_) v = Voice{};
    bend_ = 0.0f;
    modWheel_ = 0.0f;
    sustain_ = false;
    stackSize_ = 0;
    lfo_ = dsp::Lfo{};
    lastPitch_ = -1.0f;
  }

  void onParams(bool snap) noexcept override {
    const double s = sr();
    ampP_.set(s, p(kAAttack), p(kADecay), p(kASustain), p(kARelease));
    fltP_.set(s, p(kFAttack), p(kFDecay), p(kFSustain), p(kFRelease));
    const float g = dsp::dbToLin(p(kGain));
    if (snap) gain_.snap(g); else gain_.target = g;
    const int poly = ci(kPolyphony);
    if (poly != poly_) {
      for (int i = poly; i < kVoices; ++i) voices_[i].amp.gateOff(), voices_[i].flt.gateOff(), voices_[i].held = false;
      poly_ = poly;
    }
  }

  void onMidi(const sq_midi_event& e) noexcept override {
    const uint8_t st = midiStatus(e);
    if (isNoteOn(e)) {
      noteOn(e.bytes[1] & 0x7F, static_cast<float>(e.bytes[2]) / 127.0f);
    } else if (isNoteOff(e)) {
      noteOff(e.bytes[1] & 0x7F);
    } else if (st == 0xE0) {
      bend_ = pitchBendValue(e);
    } else if (st == 0xB0) {
      const uint8_t cc = e.bytes[1], val = e.bytes[2];
      if (cc == 1) {
        modWheel_ = static_cast<float>(val) / 127.0f;
      } else if (cc == 64) {
        sustain_ = val >= 64;
        if (!sustain_)
          for (auto& v : voices_)
            if (v.sustained) release(v);
      } else if (cc == 120) {
        for (auto& v : voices_) v.amp.kill(), v.flt.kill(), v.active = v.held = v.sustained = false;
        stackSize_ = 0;
      } else if (cc == 123) {
        for (auto& v : voices_) release(v);
        stackSize_ = 0;
      } else if (cc == 121) {
        bend_ = 0.0f;
        modWheel_ = 0.0f;
        sustain_ = false;  // pedal up: release what it was holding, like CC64 off
        for (auto& v : voices_)
          if (v.sustained) release(v);
      }
    }
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const double s = sr();
    gain_.begin(gain_.target, n);
    // global modulation (control rate)
    const auto lw = static_cast<dsp::LfoWave>(ci(kLfoWave));
    const float lfo = lfo_.value(lw);
    lfo_.advance(static_cast<double>(p(kLfoRate)) * n / s);
    const float lfoPitch = lfo * (p(kLfoPitch) + modWheel_ * p(kModWheel));
    const float lfoFilterSemis = lfo * p(kLfoFilter) * 48.0f;
    const float ampMod = 1.0f - p(kLfoAmp) * 0.5f * (1.0f - lfo);
    tremolo_.begin(ampMod, n);
    const float bendSemis = bend_ * p(kBendRange);
    const int uni = ci(kUnison);
    const float uniNorm = 1.0f / std::sqrt(static_cast<float>(uni));
    float uniOff[kMaxUnison], panL[kMaxUnison], panR[kMaxUnison];
    for (int u = 0; u < uni; ++u) {
      const float x = uni > 1 ? static_cast<float>(u) / static_cast<float>(uni - 1) * 2.0f - 1.0f : 0.0f;
      uniOff[u] = x * p(kUniDetune) * 0.01f;
      const float pan = x * p(kUniWidth);
      panL[u] = std::sqrt(0.5f * (1.0f - pan)) * uniNorm;
      panR[u] = std::sqrt(0.5f * (1.0f + pan)) * uniNorm;
    }
    const auto w1 = static_cast<dsp::Wave>(ci(kOsc1Wave));
    const auto w2 = static_cast<dsp::Wave>(ci(kOsc2Wave));
    const float l1 = p(kOsc1Level), l2 = p(kOsc2Level), lsub = p(kSubLevel), lnoise = p(kNoiseLevel);
    const float pw = p(kPulseWidth);
    const float osc2Off = p(kOsc2Semi) + p(kOsc2Detune) * 0.01f;
    const float glideMs = p(kGlide);
    const float glideCoef = glideMs > 0.5f ? static_cast<float>(std::exp(-1000.0 * n / (glideMs * 0.25 * s))) : 0.0f;
    const int ftype = ci(kFltType);
    const double kRes = dsp::SvfCoefs::dampFromRes(p(kReso));
    const float drive = p(kDrive);
    const float driveGain = 1.0f + 5.0f * drive;
    const float driveOut = 2.0f / (1.0f + 2.0f * drive);
    const float velSens = p(kVelSens);
    const double cutoff = p(kCutoff);
    const float keyTrack = p(kKeyTrack), fenvAmt = p(kFEnvAmt), velFilter = p(kVelFilter);
    const float inv = static_cast<float>(1.0 / s);

    for (int vi = 0; vi < kVoices; ++vi) {
      Voice& v = voices_[vi];
      if (!v.active) continue;
      // control-rate voice update
      v.pitch = v.target + (v.pitch - v.target) * glideCoef;
      const float base = v.pitch + bendSemis + lfoPitch;
      for (int u = 0; u < uni; ++u) {
        v.dt1[u] = static_cast<float>(dsp::noteToHz(base + uniOff[u])) * inv;
        v.dt2[u] = static_cast<float>(dsp::noteToHz(base + osc2Off - uniOff[u])) * inv;
      }
      v.dtSub = static_cast<float>(dsp::noteToHz(base - 12.0f)) * inv;
      const float semis = keyTrack * (static_cast<float>(v.note) - 60.0f) + fenvAmt * v.flt.level +
                          velFilter * (v.vel - 1.0f) * 24.0f + lfoFilterSemis;
      const double fc = cutoff * std::exp2(static_cast<double>(semis) / 12.0);
      if (ftype == 1) {  // LP24: butterworth stage + resonant stage
        v.ca.set(s, fc, 1.41421356);
        v.cb.set(s, fc, kRes);
      } else {
        v.ca.set(s, fc, kRes);
      }
      const float velGain = (1.0f - velSens) + velSens * v.vel;
      const float vout = 0.28f * velGain;
      dsp::Ramp trem = tremolo_;
      dsp::Ramp gr = gain_;
      for (uint32_t i = 0; i < n; ++i) {
        float sl = 0.0f, sr2 = 0.0f;
        for (int u = 0; u < uni; ++u) {
          const float a = l1 * v.o1[u].tick(w1, v.dt1[u], pw, rng_);
          const float b = l2 * v.o2[u].tick(w2, v.dt2[u], pw, rng_);
          sl += a * panL[u] + b * panR[u];
          sr2 += a * panR[u] + b * panL[u];
        }
        const float common = lsub * v.sub.tick(dsp::Wave::Sine, v.dtSub, 0.5f, rng_) + lnoise * rng_.bipolar();
        sl += common;
        sr2 += common;
        sl = dsp::softClip(sl * driveGain * 0.5f) * driveOut;
        sr2 = dsp::softClip(sr2 * driveGain * 0.5f) * driveOut;
        sl = filter(v, 0, ftype, sl);
        sr2 = filter(v, 1, ftype, sr2);
        const float env = v.amp.tick(ampP_);
        v.flt.tick(fltP_);
        const float g = env * vout * trem.next() * gr.next();
        addStereo(io, nch, i, sl * g, sr2 * g);
      }
      for (auto& ch : v.f)
        for (auto& stg : ch) stg.flushDenormals();
      if (!v.amp.active()) {
        v.active = false;
        v.held = v.sustained = false;
      }
    }
    gain_.end();
    tremolo_.end();
  }

 private:
  static float filter(Voice& v, int ch, int type, float x) noexcept {
    dsp::Svf& a = v.f[ch][0];
    a.tick(v.ca, x);
    switch (type) {
      case 0: return a.lp;
      case 1: {
        dsp::Svf& b = v.f[ch][1];
        b.tick(v.cb, a.lp);
        return b.lp;
      }
      case 2: return a.hp;
      default: return a.bp;
    }
  }

  void trigger(Voice& v, uint8_t note, float vel, bool retrigger) noexcept {
    const bool fresh = !v.active;
    v.note = note;
    v.vel = vel;
    v.age = ++clock_;
    v.target = static_cast<float>(note);
    if (fresh) {
      v.pitch = (t(kGlide) > 0.5f && lastPitch_ >= 0.0f) ? lastPitch_ : v.target;
      const bool rnd = ci(kUnison) > 1;
      for (int u = 0; u < kMaxUnison; ++u) {
        v.o1[u].reset(rnd ? rng_.unipolar() : 0.0f);
        v.o2[u].reset(rnd ? rng_.unipolar() : 0.0f);
      }
      v.sub.reset(0.0f);
      for (auto& ch : v.f)
        for (auto& stg : ch) stg.reset();
    } else if (t(kGlide) <= 0.5f && poly_ > 1) {
      v.pitch = v.target;
    }
    if (retrigger || fresh) {
      v.amp.gateOn();
      v.flt.gateOn();
    }
    v.active = true;
    v.held = true;
    v.sustained = false;
    lastPitch_ = v.target;
  }

  void release(Voice& v) noexcept {
    v.held = false;
    v.sustained = false;
    v.amp.gateOff();
    v.flt.gateOff();
  }

  void noteOn(uint8_t note, float vel) noexcept {
    if (poly_ <= 1) {
      pushStack(note);
      Voice& v = voices_[0];
      const bool legato = v.active && v.held;
      trigger(v, note, legato ? v.vel : vel, !legato);
      return;
    }
    Voice* slot = nullptr;
    for (int i = 0; i < poly_; ++i)
      if (voices_[i].active && voices_[i].note == note) { slot = &voices_[i]; break; }
    if (!slot)
      for (int i = 0; i < poly_; ++i)
        if (!voices_[i].active) { slot = &voices_[i]; break; }
    if (!slot) {  // steal: quietest releasing voice, else the oldest
      float best = 2.0f;
      for (int i = 0; i < poly_; ++i)
        if (!voices_[i].held && !voices_[i].sustained && voices_[i].amp.level < best) {
          best = voices_[i].amp.level;
          slot = &voices_[i];
        }
      if (!slot) {
        uint64_t oldest = ~0ull;
        for (int i = 0; i < poly_; ++i)
          if (voices_[i].age < oldest) { oldest = voices_[i].age; slot = &voices_[i]; }
      }
    }
    trigger(*slot, note, vel, true);
  }

  void noteOff(uint8_t note) noexcept {
    if (poly_ <= 1) {
      removeStack(note);
      Voice& v = voices_[0];
      if (!v.active || v.note != note) return;
      if (stackSize_ > 0) {
        const uint8_t prev = stack_[stackSize_ - 1];
        v.note = prev;
        v.target = static_cast<float>(prev);
        if (t(kGlide) <= 0.5f) v.pitch = v.target;
        lastPitch_ = v.target;
        return;
      }
      if (sustain_) v.sustained = true, v.held = false; else release(v);
      return;
    }
    for (auto& v : voices_) {
      if (!v.active || !v.held || v.note != note) continue;
      if (sustain_) {
        v.held = false;
        v.sustained = true;
      } else {
        release(v);
      }
    }
  }

  void pushStack(uint8_t note) noexcept {
    removeStack(note);
    if (stackSize_ == kStack) {
      for (int i = 1; i < kStack; ++i) stack_[i - 1] = stack_[i];
      --stackSize_;
    }
    stack_[stackSize_++] = note;
  }
  void removeStack(uint8_t note) noexcept {
    int w = 0;
    for (int i = 0; i < stackSize_; ++i)
      if (stack_[i] != note) stack_[w++] = stack_[i];
    stackSize_ = w;
  }

  static constexpr int kStack = 16;
  Voice voices_[kVoices];
  dsp::AdsrParams ampP_, fltP_;
  dsp::Ramp gain_, tremolo_;
  dsp::Lfo lfo_;
  dsp::Rng rng_;
  float bend_ = 0.0f, modWheel_ = 0.0f, lastPitch_ = -1.0f;
  bool sustain_ = false;
  int poly_ = kVoices;
  uint64_t clock_ = 0;
  uint8_t stack_[kStack] = {};
  int stackSize_ = 0;
};

}  // namespace

DeviceInfo SynthInfo() {
  const auto waves = {"Sine", "Triangle", "Saw", "Square", "Noise"};
  return makeInfo("synth", "Squizzy Synth", "Instrument", true,
                  {
                      pChoice("osc1_wave", "Osc 1 Wave", waves, 2),
                      pCont("osc1_level", "Osc 1 Level", 0, 1, 0.8f),
                      pCont("pulse_width", "Pulse Width", 0.05f, 0.95f, 0.5f),
                      pChoice("osc2_wave", "Osc 2 Wave", waves, 2),
                      pCont("osc2_level", "Osc 2 Level", 0, 1, 0.6f),
                      pInt("osc2_semi", "Osc 2 Semitones", -24, 24, 0, "st"),
                      pCont("osc2_detune", "Osc 2 Detune", -100, 100, 7, "ct"),
                      pInt("unison", "Unison", 1, 4, 2),
                      pCont("unison_detune", "Unison Detune", 0, 50, 12, "ct"),
                      pCont("unison_width", "Unison Width", 0, 1, 0.6f),
                      pCont("sub_level", "Sub Level", 0, 1, 0.25f),
                      pCont("noise_level", "Noise Level", 0, 1, 0),
                      pChoice("filter_type", "Filter Type", {"Lowpass 12", "Lowpass 24", "Highpass", "Bandpass"}, 1),
                      pLog("cutoff", "Cutoff", 20, 20000, 1800, "Hz"),
                      pCont("resonance", "Resonance", 0, 1, 0.2f),
                      pCont("key_track", "Key Tracking", 0, 1, 0.5f),
                      pCont("filter_env", "Filter Env Amount", -48, 48, 24, "st"),
                      pCont("velocity_filter", "Velocity > Filter", 0, 1, 0.3f),
                      pLog("filter_attack", "Filter Attack", 0.5f, 10000, 5, "ms"),
                      pLog("filter_decay", "Filter Decay", 1, 10000, 700, "ms"),
                      pCont("filter_sustain", "Filter Sustain", 0, 1, 0.35f),
                      pLog("filter_release", "Filter Release", 1, 10000, 400, "ms"),
                      pLog("amp_attack", "Amp Attack", 0.5f, 10000, 8, "ms"),
                      pLog("amp_decay", "Amp Decay", 1, 10000, 400, "ms"),
                      pCont("amp_sustain", "Amp Sustain", 0, 1, 0.8f),
                      pLog("amp_release", "Amp Release", 1, 10000, 350, "ms"),
                      pChoice("lfo_wave", "LFO Wave", {"Sine", "Triangle", "Saw", "Square", "Sample & Hold"}, 0),
                      pLog("lfo_rate", "LFO Rate", 0.05f, 20, 5, "Hz"),
                      pCont("lfo_pitch", "LFO > Pitch", 0, 12, 0, "st"),
                      pCont("lfo_filter", "LFO > Filter", 0, 1, 0),
                      pCont("lfo_amp", "LFO > Amp", 0, 1, 0),
                      pCont("mod_wheel", "Mod Wheel > Vibrato", 0, 2, 0.5f, "st"),
                      pCont("glide", "Glide", 0, 2000, 0, "ms"),
                      pCont("velocity", "Velocity Sensitivity", 0, 1, 0.6f),
                      pInt("bend_range", "Pitch Bend Range", 0, 24, 2, "st"),
                      pInt("polyphony", "Polyphony", 1, kVoices, kVoices),
                      pCont("drive", "Drive", 0, 1, 0.1f),
                      pCont("gain", "Gain", -36, 6, -3, "dB"),
                  });
}

ProcessorPtr makeSynth() { return std::make_unique<Synth>(); }

}  // namespace sq::engine::audio::builtin
