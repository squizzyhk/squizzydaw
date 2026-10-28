// Squizzy Studio engine/audio - "Distortion": drive into soft clip / hard clip / asymmetric tube /
// foldback / bit crusher, tone lowpass, dry/wet mix, output gain, optional 2x oversampling
// (6th-order Butterworth IIR up/down filters; reported latency stays 0).
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t { kDrive, kType, kTone, kBits, kDownsample, kMix, kOutput, kOversample, kCount };
enum Type : int { SoftClip, HardClip, Tube, Foldback, Bitcrush };
constexpr double kOsQ[3] = {0.51763809, 0.70710678, 1.93185165};

struct Chan {
  dsp::Biquad up[3], down[3], tone;
  dsp::DcBlocker dc;
  float held = 0.0f;
  uint32_t count = 0;
  void reset() noexcept {
    for (auto& b : up) b.reset();
    for (auto& b : down) b.reset();
    tone.reset();
    dc.reset();
    held = 0.0f;
    count = 0;
  }
};

inline float shape(int type, float x) noexcept {
  switch (type) {
    case HardClip: return dsp::clampv(x, -1.0f, 1.0f);
    case Tube: return std::tanh(x + 0.3f) - 0.29131261f;  // tanh(0.3): no static offset at rest
    case Foldback: {
      float u = (x + 1.0f) * 0.25f;
      u -= std::floor(u);
      return 1.0f - 4.0f * std::fabs(u - 0.5f);
    }
    default: return std::tanh(x);
  }
}

class Distortion final : public FxDevice {
 public:
  Distortion() : FxDevice(infoOf("distortion")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Distortion>(); }
  bool onPrepare() override {
    ch_.assign(channels(), Chan{});
    for (auto& c : ch_) c.dc.prepare(sr());
    for (int i = 0; i < 3; ++i) osC_[i] = dsp::designBiquad(dsp::BqType::Lowpass, sr() * 2.0, sr() * 0.45, kOsQ[i], 0);
    return true;
  }
  void onReset() noexcept override {
    for (auto& c : ch_) c.reset();
  }
  void onParams(bool snap) noexcept override {
    toneC_ = dsp::designBiquad(dsp::BqType::Lowpass, sr(), p(kTone), 0.7071, 0);
    const float drive = dsp::dbToLin(p(kDrive));
    const float out = dsp::dbToLin(p(kOutput));
    const float mix = p(kMix) * 0.01f;
    if (snap) drive_.snap(drive), out_.snap(out), mix_.snap(mix);
    else drive_.target = drive, out_.target = out, mix_.target = mix;
  }
  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const int type = ci(kType);
    const bool os = on(kOversample) && type != Bitcrush;
    const float q = std::exp2(static_cast<float>(ci(kBits)) - 1.0f);
    const uint32_t hold = static_cast<uint32_t>(ci(kDownsample));
    for (uint32_t c = 0; c < nch; ++c) {
      Chan& st = ch_[c];
      dsp::Ramp drive = drive_, out = out_, mix = mix_;
      drive.begin(drive_.target, n);
      out.begin(out_.target, n);
      mix.begin(mix_.target, n);
      for (uint32_t i = 0; i < n; ++i) {
        const float x = io[c][i];
        const float d = x * drive.next();
        float y;
        if (type == Bitcrush) {
          if (st.count == 0) st.held = std::round(dsp::clampv(d, -1.0f, 1.0f) * q) / q;
          st.count = st.count + 1 >= hold ? 0 : st.count + 1;
          y = st.held;
        } else if (os) {
          float a = 2.0f * d, b = 0.0f;
          for (int k = 0; k < 3; ++k) a = st.up[k].process(osC_[k], a);
          for (int k = 0; k < 3; ++k) b = st.up[k].process(osC_[k], b);
          a = shape(type, a);
          b = shape(type, b);
          for (int k = 0; k < 3; ++k) a = st.down[k].process(osC_[k], a);
          for (int k = 0; k < 3; ++k) b = st.down[k].process(osC_[k], b);
          y = b;  // keep every second sample
          (void)a;
        } else {
          y = shape(type, d);
        }
        y = st.dc.process(st.tone.process(toneC_, y));
        const float m = mix.next();
        io[c][i] = (x * (1.0f - m) + y * m) * out.next();
      }
      for (auto& b : st.up) b.flushDenormals();
      for (auto& b : st.down) b.flushDenormals();
      st.tone.flushDenormals();
    }
    drive_.end();
    out_.end();
    mix_.end();
  }

 private:
  std::vector<Chan> ch_;
  dsp::BiquadCoefs osC_[3], toneC_;
  dsp::Ramp drive_, out_, mix_;
};

}  // namespace

DeviceInfo DistortionInfo() {
  return makeInfo("distortion", "Distortion", "Distortion", false,
                  {
                      pCont("drive", "Drive", 0, 48, 12, "dB"),
                      pChoice("type", "Type", {"Soft Clip", "Hard Clip", "Tube", "Foldback", "Bitcrush"}, 0),
                      pLog("tone", "Tone", 500, 20000, 12000, "Hz"),
                      pInt("bits", "Bits", 2, 16, 8),
                      pInt("downsample", "Downsample", 1, 32, 1, "x"),
                      pCont("mix", "Mix", 0, 100, 100, "%"),
                      pCont("output", "Output", -24, 12, -6, "dB"),
                      pToggle("oversample", "Oversample 2x", true),
                  });
}
ProcessorPtr makeDistortion() { return std::make_unique<Distortion>(); }

}  // namespace sq::engine::audio::builtin
