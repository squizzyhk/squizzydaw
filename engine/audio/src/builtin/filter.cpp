// Squizzy Studio engine/audio - "Auto Filter": resonant multimode state-variable filter (12/24
// dB) with drive, LFO (stereo phase offset) and an envelope follower (auto-wah).
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t { kType, kSlope, kCutoff, kReso, kDrive, kLfoRate, kLfoDepth, kLfoWave, kLfoStereo, kEnvAmt, kMix, kCount };

class AutoFilter final : public FxDevice {
 public:
  AutoFilter() : FxDevice(infoOf("filter")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<AutoFilter>(); }
  bool onPrepare() override {
    f_.assign(static_cast<size_t>(channels()) * 2, dsp::Svf{});
    envAtt_ = dsp::onePoleCoef(sr(), 5.0);
    envRel_ = dsp::onePoleCoef(sr(), 150.0);
    return true;
  }
  void onReset() noexcept override {
    for (auto& f : f_) f.reset();
    env_ = 0.0f;
    lfo_ = dsp::Lfo{};
  }
  void onParams(bool snap) noexcept override {
    const float drive = dsp::dbToLin(p(kDrive));
    const float mix = p(kMix) * 0.01f;
    if (snap) drive_.snap(drive), mix_.snap(mix);
    else drive_.target = drive, mix_.target = mix;
  }
  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const double s = sr();
    // envelope follower (linked)
    float peak = 0.0f;
    for (uint32_t c = 0; c < nch; ++c)
      for (uint32_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(io[c][i]));
    const float k = static_cast<float>(n);
    env_ = peak > env_ ? peak + (env_ - peak) * std::pow(envAtt_, k) : peak + (env_ - peak) * std::pow(envRel_, k);
    const float envSemis = p(kEnvAmt) * 48.0f * std::sqrt(dsp::clampv(env_, 0.0f, 1.0f));
    const auto wave = static_cast<dsp::LfoWave>(ci(kLfoWave));
    const int type = ci(kType);
    const bool steep = ci(kSlope) == 1;
    const double kDamp = dsp::SvfCoefs::dampFromRes(p(kReso));
    const bool drive = p(kDrive) > 0.01f;
    for (uint32_t c = 0; c < nch; ++c) {
      const float lfo = lfo_.value(wave, (c & 1u) ? p(kLfoStereo) / 360.0 : 0.0);
      const double semis = static_cast<double>(lfo * p(kLfoDepth) * 48.0f + envSemis);
      dsp::SvfCoefs co;
      co.set(s, p(kCutoff) * std::exp2(semis / 12.0), steep ? std::max(kDamp, 0.1) : kDamp);
      dsp::Svf& a = f_[c * 2];
      dsp::Svf& b = f_[c * 2 + 1];
      dsp::Ramp dr = drive_, mx = mix_;
      dr.begin(drive_.target, n);
      mx.begin(mix_.target, n);
      for (uint32_t i = 0; i < n; ++i) {
        const float x = io[c][i];
        const float g = dr.next();
        float y = drive ? dsp::softClip(x * g) : x;
        a.tick(co, y);
        y = pick(a, co, type, y);
        if (steep) {
          const float in2 = y;
          b.tick(co, in2);
          y = pick(b, co, type, in2);
        }
        const float m = mx.next();
        io[c][i] = x * (1.0f - m) + y * m;
      }
      a.flushDenormals();
      b.flushDenormals();
    }
    lfo_.advance(static_cast<double>(p(kLfoRate)) * n / s);
    drive_.end();
    mix_.end();
  }

 private:
  static float pick(const dsp::Svf& f, const dsp::SvfCoefs& c, int type, float in) noexcept {
    switch (type) {
      case 1: return f.hp;
      case 2: return f.bp * c.k;  // unity gain at the centre
      case 3: return in - c.k * f.bp;
      default: return f.lp;
    }
  }
  std::vector<dsp::Svf> f_;
  dsp::Lfo lfo_;
  dsp::Ramp drive_, mix_;
  float env_ = 0.0f, envAtt_ = 0.0f, envRel_ = 0.0f;
};

}  // namespace

DeviceInfo FilterInfo() {
  return makeInfo("filter", "Auto Filter", "Filter", false,
                  {
                      pChoice("type", "Type", {"Lowpass", "Highpass", "Bandpass", "Notch"}, 0),
                      pChoice("slope", "Slope", {"12 dB/oct", "24 dB/oct"}, 0),
                      pLog("cutoff", "Cutoff", 20, 20000, 1500, "Hz"),
                      pCont("resonance", "Resonance", 0, 1, 0.3f),
                      pCont("drive", "Drive", 0, 24, 0, "dB"),
                      pLog("lfo_rate", "LFO Rate", 0.05f, 20, 1, "Hz"),
                      pCont("lfo_depth", "LFO Depth", 0, 1, 0),
                      pChoice("lfo_wave", "LFO Wave", {"Sine", "Triangle", "Saw", "Square", "Sample & Hold"}, 0),
                      pCont("lfo_stereo", "LFO Stereo Phase", 0, 180, 0, "deg"),
                      pCont("env_amount", "Envelope Amount", -1, 1, 0),
                      pCont("mix", "Mix", 0, 100, 100, "%"),
                  });
}
ProcessorPtr makeFilter() { return std::make_unique<AutoFilter>(); }

}  // namespace sq::engine::audio::builtin
