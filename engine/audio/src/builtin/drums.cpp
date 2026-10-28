// Squizzy Studio engine/audio - "Squizzy Drums": fully synthesized drum kit on the GM drum map
// (an analog-style kick/snare/clap/hats/toms/cymbals hybrid; nothing is sample based).
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t {
  kKickLevel, kKickTune, kKickDecay, kKickPunch,
  kSnareLevel, kSnareTune, kSnareDecay, kSnareSnappy,
  kClapLevel, kClapTone, kClapDecay,
  kRimLevel,
  kChhLevel, kChhTune, kChhDecay,
  kOhhLevel, kOhhDecay,
  kTomLevel, kTomTune, kTomDecay,
  kCrashLevel, kCrashDecay,
  kRideLevel, kRideDecay,
  kMaster, kCount
};

enum Piece : int { Kick, Snare, Clap, Rim, Chh, Ohh, TomL, TomM, TomH, Crash, Ride, kPieces };

constexpr float kPan[kPieces] = {0.0f, 0.0f, 0.05f, -0.1f, 0.25f, 0.25f, 0.35f, 0.0f, -0.35f, -0.3f, 0.35f};
constexpr uint32_t kLevelParam[kPieces] = {kKickLevel, kSnareLevel, kClapLevel, kRimLevel, kChhLevel, kOhhLevel,
                                           kTomLevel,  kTomLevel,   kTomLevel,  kCrashLevel, kRideLevel};
// Kit output trim: every piece is voiced to peak near full scale on its own, so the summed kit is
// scaled down to leave headroom (kick + snare + hat at full velocity peaks around -3 dBFS) ...
constexpr float kOutTrim = 0.36f;
// ... and the kit bus has a soft knee above kKnee that approaches (never reaches) kCeiling, so
// even every piece hit at once does not go over 0 dBFS, whatever the master level.
constexpr float kKnee = 0.7f, kCeiling = 0.97f;
inline float kitKnee(float x) noexcept {
  const float a = std::fabs(x);
  if (a <= kKnee) return x;
  const float y = kKnee + (kCeiling - kKnee) * std::tanh((a - kKnee) / (kCeiling - kKnee));
  return x < 0.0f ? -y : y;
}

// Inharmonic ratios of the six square oscillators feeding hats and cymbals.
constexpr float kMetal[6] = {1.0f, 1.4471f, 1.6170f, 1.9265f, 2.5028f, 2.6637f};

int pieceOf(int note) noexcept {
  switch (note) {
    case 35: case 36: return Kick;
    case 37: return Rim;
    case 38: case 40: return Snare;
    case 39: return Clap;
    case 42: case 44: return Chh;
    case 46: return Ohh;
    case 41: case 43: return TomL;
    case 45: case 47: return TomM;
    case 48: case 50: return TomH;
    case 49: case 52: case 55: case 57: return Crash;
    case 51: case 53: case 59: return Ride;
    default: return -1;
  }
}

struct Voice {
  bool active = false;
  float vel = 0.0f;
  float amp = 0.0f, ampCoef = 0.0f;      // main envelope
  float body = 0.0f, bodyCoef = 0.0f;    // body (tonal) envelope
  float noise = 0.0f, noiseCoef = 0.0f;  // noise envelope
  float pe = 0.0f, peCoef = 0.0f;        // pitch envelope
  float freq = 0.0f, freq2 = 0.0f;
  float ph = 0.0f, ph2 = 0.0f;
  float metal[6] = {};
  float mix = 0.0f;
  uint32_t t = 0;
  float declick = 0.0f, last = 0.0f;
  dsp::Biquad f1, f2;
  dsp::BiquadCoefs c1, c2;
  float lvl = 0.0f;  // smoothed level (per sub-block ramp)
};

class Drums final : public SynthDevice {
 public:
  Drums() : SynthDevice(infoOf("drums")) {}

 protected:
  std::unique_ptr<SynthDevice> fresh() const override { return std::make_unique<Drums>(); }

  bool onPrepare() override {
    declickCoef_ = dsp::onePoleCoef(sr(), 1.5);
    sr_ = static_cast<float>(sr());
    invSr_ = 1.0f / sr_;
    return true;
  }
  void onReset() noexcept override {
    for (auto& v : v_) v = Voice{};
    rng_ = dsp::Rng{};
  }
  void onParams(bool snap) noexcept override {
    const float m = dsp::dbToLin(p(kMaster));
    if (snap) master_.snap(m); else master_.target = m;
    if (snap)
      for (int i = 0; i < kPieces; ++i) v_[i].lvl = p(kLevelParam[i]);
  }

  void onMidi(const sq_midi_event& e) noexcept override {
    if (isNoteOn(e)) {
      const int piece = pieceOf(e.bytes[1] & 0x7F);
      if (piece >= 0) trigger(piece, static_cast<float>(e.bytes[2]) / 127.0f);
    } else if (midiStatus(e) == 0xB0 && (e.bytes[1] == 120 || e.bytes[1] == 123)) {
      for (auto& v : v_) v.active = false, v.declick = v.last;
    }
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    master_.begin(master_.target, n);
    float busL[kSubBlock] = {}, busR[kSubBlock] = {};  // n <= kSubBlock (Device sub-blocks)
    for (int pc = 0; pc < kPieces; ++pc) {
      Voice& v = v_[pc];
      const float lvlTarget = p(kLevelParam[pc]);
      if (!v.active && std::fabs(v.declick) < 1e-6f) {
        v.lvl = lvlTarget;
        continue;
      }
      const float lstep = (lvlTarget - v.lvl) / static_cast<float>(n);
      const float pan = kPan[pc];
      const float gl = std::sqrt(0.5f * (1.0f - pan)) * 1.41421356f, gr = std::sqrt(0.5f * (1.0f + pan)) * 1.41421356f;
      dsp::Ramp m = master_;
      for (uint32_t i = 0; i < n; ++i) {
        float y = v.active ? tick(pc, v) : 0.0f;
        v.lvl += lstep;
        y *= v.vel * v.lvl;
        v.last = y;
        y += v.declick;
        v.declick *= declickCoef_;
        y *= m.next() * kOutTrim;
        busL[i] += y * gl;
        busR[i] += y * gr;
      }
      v.lvl = lvlTarget;
      v.declick = dsp::flush(v.declick);
      v.amp = dsp::flush(v.amp);
      v.body = dsp::flush(v.body);
      v.noise = dsp::flush(v.noise);
      v.pe = dsp::flush(v.pe);
      v.f1.flushDenormals();
      v.f2.flushDenormals();
    }
    for (uint32_t i = 0; i < n; ++i) addStereo(io, nch, i, kitKnee(busL[i]), kitKnee(busR[i]));
    master_.end();
  }

 private:
  float sine(float& ph, float f) noexcept {
    const float y = std::sin(dsp::kTwoPiF * ph);
    ph += f * invSr_;
    if (ph >= 1.0f) ph -= 1.0f;
    return y;
  }
  float metal(Voice& v) noexcept {
    float s = 0.0f;
    for (int k = 0; k < 6; ++k) {
      v.metal[k] += v.freq * kMetal[k] * invSr_;
      if (v.metal[k] >= 1.0f) v.metal[k] -= 1.0f;
      s += v.metal[k] < 0.5f ? 1.0f : -1.0f;
    }
    return s * (1.0f / 6.0f);
  }

  float tick(int pc, Voice& v) noexcept {
    float y = 0.0f;
    switch (pc) {
      case Kick: {
        const float f = v.freq * (1.0f + v.mix * 6.0f * v.pe);
        y = sine(v.ph, f) * v.amp;
        y += v.f1.process(v.c1, rng_.bipolar()) * v.noise * 0.6f * v.mix;
        y = dsp::softClip(y * 1.3f);
        break;
      }
      case Snare: {
        const float f = v.freq * (1.0f + 0.35f * v.pe);
        const float body = (sine(v.ph, f) + 0.6f * sine(v.ph2, f * 1.62f)) * 0.6f * v.body;
        const float nz = v.f2.process(v.c2, v.f1.process(v.c1, rng_.bipolar())) * v.noise * 1.6f;
        y = body * (1.0f - 0.5f * v.mix) + nz * v.mix;
        break;
      }
      case Clap: {
        const uint32_t spacing = static_cast<uint32_t>(0.0095f * sr_);
        float env;
        if (v.t < 3 * spacing) {
          env = std::exp(-static_cast<float>(v.t % spacing) / (0.0022f * sr_));
        } else {
          env = v.amp;
          v.amp *= v.ampCoef;
        }
        y = v.f1.process(v.c1, rng_.bipolar()) * env * 2.2f;
        break;
      }
      case Rim: {
        y = (0.7f * sine(v.ph, 1720.0f) + 0.5f * sine(v.ph2, 480.0f)) * v.amp;
        y = v.f1.process(v.c1, y + rng_.bipolar() * v.noise * 0.6f);
        break;
      }
      case Chh:
      case Ohh: {
        const float src = metal(v) + 0.35f * rng_.bipolar();
        y = v.f2.process(v.c2, v.f1.process(v.c1, src)) * v.amp * 1.4f;
        break;
      }
      case TomL:
      case TomM:
      case TomH: {
        const float f = v.freq * (1.0f + 0.45f * v.pe);
        y = sine(v.ph, f) * v.amp + v.f1.process(v.c1, rng_.bipolar()) * v.noise * 0.25f;
        break;
      }
      case Crash:
      case Ride: {
        const float src = metal(v) * v.mix + rng_.bipolar() * (1.0f - v.mix);
        const float att = v.t < 64 ? static_cast<float>(v.t) / 64.0f : 1.0f;
        y = v.f2.process(v.c2, v.f1.process(v.c1, src)) * v.amp * att * 1.2f;
        if (pc == Ride) y += 0.18f * sine(v.ph, v.freq * 6.1f) * v.body;
        break;
      }
      default: break;
    }
    v.amp *= pc == Clap ? 1.0f : v.ampCoef;
    v.body *= v.bodyCoef;
    v.noise *= v.noiseCoef;
    v.pe *= v.peCoef;
    ++v.t;
    if (v.t > 64 && v.amp < 1e-4f && v.noise < 1e-4f && v.body < 1e-4f) {
      if (pc != Clap || v.t > static_cast<uint32_t>(0.03f * sr_)) v.active = false;
    }
    return y;
  }

  void trigger(int pc, float vel) noexcept {
    Voice& v = v_[pc];
    const float dk = v.declick + (v.active ? v.last : 0.0f);
    const float lvl = v.lvl;
    v = Voice{};
    v.lvl = lvl;
    v.declick = dk;
    v.active = true;
    v.vel = 0.25f + 0.75f * vel;
    const double s = sr();
    auto dc = [s](double ms) { return dsp::decayCoef(s, ms); };
    switch (pc) {
      case Kick:
        v.freq = t(kKickTune);
        v.mix = t(kKickPunch);
        v.amp = 1.0f; v.ampCoef = dc(t(kKickDecay));
        v.pe = 1.0f; v.peCoef = dc(45.0);
        v.noise = 1.0f; v.noiseCoef = dc(6.0);
        v.c1 = dsp::designBiquad(dsp::BqType::Highpass, s, 2500.0, 0.7, 0);
        break;
      case Snare: {
        const double dec = t(kSnareDecay);
        v.freq = t(kSnareTune);
        v.mix = t(kSnareSnappy);
        v.body = 1.0f; v.bodyCoef = dc(dec * 0.45);
        v.noise = 1.0f; v.noiseCoef = dc(dec);
        v.pe = 1.0f; v.peCoef = dc(25.0);
        v.ampCoef = 1.0f;
        v.c1 = dsp::designBiquad(dsp::BqType::Highpass, s, 1400.0, 0.7, 0);
        v.c2 = dsp::designBiquad(dsp::BqType::Lowpass, s, 9000.0, 0.7, 0);
        break;
      }
      case Clap:
        v.amp = 0.75f; v.ampCoef = dc(t(kClapDecay));
        v.c1 = dsp::designBiquad(dsp::BqType::Bandpass, s, t(kClapTone), 1.6, 0);
        break;
      case Rim:
        v.amp = 1.0f; v.ampCoef = dc(28.0);
        v.noise = 1.0f; v.noiseCoef = dc(4.0);
        v.c1 = dsp::designBiquad(dsp::BqType::Highpass, s, 380.0, 0.8, 0);
        break;
      case Chh:
      case Ohh: {
        if (pc == Chh && v_[Ohh].active) v_[Ohh].ampCoef = dc(12.0);  // choke the open hat
        const float tune = t(kChhTune);
        v.freq = 410.0f * tune;
        for (int k = 0; k < 6; ++k) v.metal[k] = rng_.unipolar();
        v.amp = 1.0f; v.ampCoef = dc(pc == Chh ? t(kChhDecay) : t(kOhhDecay));
        v.c1 = dsp::designBiquad(dsp::BqType::Bandpass, s, 9500.0 * tune, 0.9, 0);
        v.c2 = dsp::designBiquad(dsp::BqType::Highpass, s, 6800.0 * tune, 0.7, 0);
        break;
      }
      case TomL:
      case TomM:
      case TomH: {
        const float mul = pc == TomL ? 0.72f : (pc == TomM ? 1.0f : 1.36f);
        v.freq = t(kTomTune) * mul;
        v.amp = 1.0f; v.ampCoef = dc(t(kTomDecay) * (pc == TomL ? 1.2 : (pc == TomH ? 0.85 : 1.0)));
        v.pe = 1.0f; v.peCoef = dc(90.0);
        v.noise = 1.0f; v.noiseCoef = dc(12.0);
        v.c1 = dsp::designBiquad(dsp::BqType::Bandpass, s, v.freq * 4.0, 1.0, 0);
        break;
      }
      case Crash:
      case Ride: {
        const bool ride = pc == Ride;
        v.freq = ride ? 520.0f : 330.0f;
        v.mix = ride ? 0.8f : 0.55f;
        for (int k = 0; k < 6; ++k) v.metal[k] = rng_.unipolar();
        v.amp = 1.0f; v.ampCoef = dc(ride ? t(kRideDecay) : t(kCrashDecay));
        v.body = 1.0f; v.bodyCoef = dc(ride ? 900.0 : 10.0);
        v.c1 = dsp::designBiquad(dsp::BqType::Highpass, s, ride ? 3200.0 : 4200.0, 0.7, 0);
        v.c2 = dsp::designBiquad(dsp::BqType::Lowpass, s, ride ? 11000.0 : 15000.0, 0.7, 0);
        break;
      }
      default: break;
    }
  }

  Voice v_[kPieces];
  dsp::Rng rng_;
  dsp::Ramp master_;
  float declickCoef_ = 0.0f;
  float sr_ = 48000.0f, invSr_ = 1.0f / 48000.0f;
};

}  // namespace

DeviceInfo DrumsInfo() {
  return makeInfo("drums", "Squizzy Drums", "Instrument", true,
                  {
                      pCont("kick_level", "Kick Level", 0, 1, 0.9f),
                      pLog("kick_tune", "Kick Tune", 30, 120, 50, "Hz"),
                      pLog("kick_decay", "Kick Decay", 50, 2000, 450, "ms"),
                      pCont("kick_punch", "Kick Punch", 0, 1, 0.5f),
                      pCont("snare_level", "Snare Level", 0, 1, 0.8f),
                      pLog("snare_tune", "Snare Tune", 100, 400, 190, "Hz"),
                      pLog("snare_decay", "Snare Decay", 50, 1000, 220, "ms"),
                      pCont("snare_snappy", "Snare Snappy", 0, 1, 0.6f),
                      pCont("clap_level", "Clap Level", 0, 1, 0.7f),
                      pLog("clap_tone", "Clap Tone", 500, 4000, 1150, "Hz"),
                      pLog("clap_decay", "Clap Decay", 50, 1500, 280, "ms"),
                      pCont("rim_level", "Rim Level", 0, 1, 0.6f),
                      pCont("chh_level", "Closed Hat Level", 0, 1, 0.55f),
                      pCont("hat_tune", "Hat Tune", 0.5f, 2.0f, 1.0f, "x"),
                      pLog("chh_decay", "Closed Hat Decay", 10, 500, 70, "ms"),
                      pCont("ohh_level", "Open Hat Level", 0, 1, 0.5f),
                      pLog("ohh_decay", "Open Hat Decay", 50, 3000, 450, "ms"),
                      pCont("tom_level", "Tom Level", 0, 1, 0.75f),
                      pLog("tom_tune", "Tom Tune", 50, 400, 140, "Hz"),
                      pLog("tom_decay", "Tom Decay", 50, 2000, 380, "ms"),
                      pCont("crash_level", "Crash Level", 0, 1, 0.45f),
                      pLog("crash_decay", "Crash Decay", 200, 6000, 1600, "ms"),
                      pCont("ride_level", "Ride Level", 0, 1, 0.4f),
                      pLog("ride_decay", "Ride Decay", 200, 6000, 2200, "ms"),
                      pCont("master", "Master", -24, 6, 0, "dB"),
                  });
}

ProcessorPtr makeDrums() { return std::make_unique<Drums>(); }

}  // namespace sq::engine::audio::builtin
