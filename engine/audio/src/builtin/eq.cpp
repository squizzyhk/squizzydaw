// Squizzy Studio engine/audio - "Parametric EQ": low cut, low shelf, three peaking bands, high
// shelf, high cut (RBJ biquads; cut filters are Butterworth cascades of 12/24/48 dB/oct).
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t {
  kLcOn, kLcFreq, kLcSlope,
  kLsFreq, kLsGain, kLsQ,
  kP1Freq, kP1Gain, kP1Q,
  kP2Freq, kP2Gain, kP2Q,
  kP3Freq, kP3Gain, kP3Q,
  kHsFreq, kHsGain, kHsQ,
  kHcOn, kHcFreq, kHcSlope,
  kOutput, kCount
};
constexpr int kCutStages = 4;
constexpr int kBands = 5;  // ls, p1, p2, p3, hs
// Butterworth section Qs for 2/4/8 poles.
constexpr double kQ2[1] = {0.70710678};
constexpr double kQ4[2] = {0.54119610, 1.30656296};
constexpr double kQ8[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};

struct Cut {
  int stages = 0;
  dsp::BiquadCoefs c[kCutStages];
  float mix = 0.0f, target = 0.0f, next = 0.0f;  // 0 = bypassed, 1 = on (crossfaded toggle)
};

class Eq final : public FxDevice {
 public:
  Eq() : FxDevice(infoOf("eq")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Eq>(); }
  bool onPrepare() override {
    const size_t ch = channels();
    band_.assign(ch * kBands, dsp::Biquad{});
    lc_.assign(ch * kCutStages, dsp::Biquad{});
    hc_.assign(ch * kCutStages, dsp::Biquad{});
    fadeStep_ = static_cast<float>(1.0 / (0.01 * sr()));
    return true;
  }
  void onReset() noexcept override {
    for (auto& b : band_) b.reset();
    for (auto& b : lc_) b.reset();
    for (auto& b : hc_) b.reset();
  }
  void onParams(bool snap) noexcept override {
    const double s = sr();
    const uint32_t f[kBands] = {kLsFreq, kP1Freq, kP2Freq, kP3Freq, kHsFreq};
    const dsp::BqType ty[kBands] = {dsp::BqType::LowShelf, dsp::BqType::Peak, dsp::BqType::Peak, dsp::BqType::Peak,
                                    dsp::BqType::HighShelf};
    for (int b = 0; b < kBands; ++b) {
      const float g = p(f[b] + 1);
      bandOn_[b] = std::fabs(g) > 1e-4f;
      if (bandOn_[b]) bc_[b] = dsp::designBiquad(ty[b], s, p(f[b]), p(f[b] + 2), g);
    }
    setupCut(lcCut_, lc_, dsp::BqType::Highpass, p(kLcFreq), ci(kLcSlope), on(kLcOn), snap);
    setupCut(hcCut_, hc_, dsp::BqType::Lowpass, p(kHcFreq), ci(kHcSlope), on(kHcOn), snap);
    const float o = dsp::dbToLin(p(kOutput));
    if (snap) out_.snap(o); else out_.target = o;
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    out_.begin(out_.target, n);
    for (uint32_t c = 0; c < nch; ++c) {
      float* x = io[c];
      runCut(lcCut_, &lc_[c * kCutStages], x, n);
      for (int b = 0; b < kBands; ++b) {
        if (!bandOn_[b]) continue;
        dsp::Biquad& st = band_[c * kBands + b];
        for (uint32_t i = 0; i < n; ++i) x[i] = st.process(bc_[b], x[i]);
        st.flushDenormals();
      }
      runCut(hcCut_, &hc_[c * kCutStages], x, n);
      dsp::Ramp o = out_;
      for (uint32_t i = 0; i < n; ++i) x[i] *= o.next();
    }
    lcCut_.mix = lcCut_.next;
    hcCut_.mix = hcCut_.next;
    out_.end();
    for (int b = 0; b < kBands; ++b)
      if (!bandOn_[b])
        for (uint32_t c = 0; c < nch; ++c) band_[c * kBands + b].reset();
  }

 private:
  void setupCut(Cut& cut, std::vector<dsp::Biquad>& state, dsp::BqType type, double freq, int slope, bool enabled, bool snap) noexcept {
    const int stages = slope == 0 ? 1 : (slope == 1 ? 2 : 4);
    const double* qs = slope == 0 ? kQ2 : (slope == 1 ? kQ4 : kQ8);
    if (stages != cut.stages)
      for (auto& b : state) b.reset();
    cut.stages = stages;
    for (int i = 0; i < stages; ++i) cut.c[i] = dsp::designBiquad(type, sr(), freq, qs[i], 0);
    cut.target = enabled ? 1.0f : 0.0f;
    if (snap) cut.mix = cut.next = cut.target;
  }
  void runCut(Cut& cut, dsp::Biquad* st, float* x, uint32_t n) noexcept {
    cut.next = cut.mix;
    if (cut.mix <= 0.0f && cut.target <= 0.0f) return;
    float m = cut.mix;
    for (uint32_t i = 0; i < n; ++i) {
      float y = x[i];
      for (int s = 0; s < cut.stages; ++s) y = st[s].process(cut.c[s], y);
      m = m < cut.target ? std::min(cut.target, m + fadeStep_) : std::max(cut.target, m - fadeStep_);
      x[i] += (y - x[i]) * m;
    }
    cut.next = m;
    for (int s = 0; s < cut.stages; ++s) st[s].flushDenormals();
    if (m <= 0.0f)
      for (int s = 0; s < kCutStages; ++s) st[s].reset();
  }

  std::vector<dsp::Biquad> band_, lc_, hc_;
  dsp::BiquadCoefs bc_[kBands];
  bool bandOn_[kBands] = {};
  Cut lcCut_, hcCut_;
  float fadeStep_ = 0.01f;
  dsp::Ramp out_;
};

}  // namespace

DeviceInfo EqInfo() {
  const auto slopes = {"12 dB/oct", "24 dB/oct", "48 dB/oct"};
  return makeInfo("eq", "Parametric EQ", "EQ", false,
                  {
                      pToggle("lowcut_on", "Low Cut", false),
                      pLog("lowcut_freq", "Low Cut Freq", 20, 2000, 30, "Hz"),
                      pChoice("lowcut_slope", "Low Cut Slope", slopes, 1),
                      pLog("lowshelf_freq", "Low Shelf Freq", 20, 2000, 100, "Hz"),
                      pCont("lowshelf_gain", "Low Shelf Gain", -24, 24, 0, "dB"),
                      pLog("lowshelf_q", "Low Shelf Q", 0.3f, 2.0f, 0.71f, ""),
                      pLog("peak1_freq", "Band 1 Freq", 20, 20000, 250, "Hz"),
                      pCont("peak1_gain", "Band 1 Gain", -24, 24, 0, "dB"),
                      pLog("peak1_q", "Band 1 Q", 0.1f, 18, 1, ""),
                      pLog("peak2_freq", "Band 2 Freq", 20, 20000, 1000, "Hz"),
                      pCont("peak2_gain", "Band 2 Gain", -24, 24, 0, "dB"),
                      pLog("peak2_q", "Band 2 Q", 0.1f, 18, 1, ""),
                      pLog("peak3_freq", "Band 3 Freq", 20, 20000, 4000, "Hz"),
                      pCont("peak3_gain", "Band 3 Gain", -24, 24, 0, "dB"),
                      pLog("peak3_q", "Band 3 Q", 0.1f, 18, 1, ""),
                      pLog("highshelf_freq", "High Shelf Freq", 1000, 20000, 8000, "Hz"),
                      pCont("highshelf_gain", "High Shelf Gain", -24, 24, 0, "dB"),
                      pLog("highshelf_q", "High Shelf Q", 0.3f, 2.0f, 0.71f, ""),
                      pToggle("highcut_on", "High Cut", false),
                      pLog("highcut_freq", "High Cut Freq", 1000, 22000, 18000, "Hz"),
                      pChoice("highcut_slope", "High Cut Slope", slopes, 1),
                      pCont("output", "Output", -24, 24, 0, "dB"),
                  });
}

ProcessorPtr makeEq() { return std::make_unique<Eq>(); }

}  // namespace sq::engine::audio::builtin
