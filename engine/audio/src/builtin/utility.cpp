// Squizzy Studio engine/audio - "Utility": gain, balance, stereo width, per-side phase invert,
// mono switch and DC filter. Toggles crossfade over ~5 ms.
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t { kGain, kPan, kWidth, kInvL, kInvR, kMono, kDc, kCount };

class Utility final : public FxDevice {
 public:
  Utility() : FxDevice(infoOf("utility")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Utility>(); }
  bool onPrepare() override {
    dc_.assign(channels(), dsp::DcBlocker{});
    for (auto& d : dc_) d.prepare(sr());
    slew_ = dsp::onePoleCoef(sr(), 5.0);
    return true;
  }
  void onReset() noexcept override {
    for (auto& d : dc_) d.reset();
  }
  void onParams(bool snap) noexcept override {
    const float g = dsp::dbToLin(p(kGain));
    float bl = 1.0f, br = 1.0f;
    panGainsStereo(p(kPan) * 0.01f, &bl, &br);
    if (snap) {
      gainL_.snap(g * bl);
      gainR_.snap(g * br);
      gainM_.snap(g);
      width_.snap(p(kWidth) * 0.01f);
    } else {
      gainL_.target = g * bl;
      gainR_.target = g * br;
      gainM_.target = g;
      width_.target = p(kWidth) * 0.01f;
    }
    tInvL_ = on(kInvL) ? -1.0f : 1.0f;
    tInvR_ = on(kInvR) ? -1.0f : 1.0f;
    tMono_ = on(kMono) ? 1.0f : 0.0f;
    if (snap) invL_ = tInvL_, invR_ = tInvR_, mono_ = tMono_;
  }
  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const bool dc = on(kDc);
    float iL = invL_, iR = invR_, mo = mono_;
    for (uint32_t c = 0; c < nch; c += 2) {
      float* L = io[c];
      float* R = c + 1 < nch ? io[c + 1] : nullptr;
      dsp::Ramp gl = gainL_, gr = gainR_, gm = gainM_, w = width_;
      gl.begin(gainL_.target, n);
      gr.begin(gainR_.target, n);
      gm.begin(gainM_.target, n);
      w.begin(width_.target, n);
      iL = invL_;
      iR = invR_;
      mo = mono_;
      for (uint32_t i = 0; i < n; ++i) {
        iL = tInvL_ + (iL - tInvL_) * slew_;
        iR = tInvR_ + (iR - tInvR_) * slew_;
        mo = tMono_ + (mo - tMono_) * slew_;
        if (R) {
          float l = L[i] * iL, r = R[i] * iR;
          if (dc) l = dc_[c].process(l), r = dc_[c + 1].process(r);
          const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * w.next() * (1.0f - mo);
          L[i] = (mid + side) * gl.next();
          R[i] = (mid - side) * gr.next();
        } else {
          float l = L[i] * iL;
          if (dc) l = dc_[c].process(l);
          L[i] = l * gm.next();
        }
      }
    }
    invL_ = iL;
    invR_ = iR;
    mono_ = mo;
    gainL_.end();
    gainR_.end();
    gainM_.end();
    width_.end();
  }

 private:
  std::vector<dsp::DcBlocker> dc_;
  dsp::Ramp gainL_, gainR_, gainM_, width_;
  float invL_ = 1.0f, invR_ = 1.0f, mono_ = 0.0f, tInvL_ = 1.0f, tInvR_ = 1.0f, tMono_ = 0.0f, slew_ = 0.0f;
};

}  // namespace

DeviceInfo UtilityInfo() {
  return makeInfo("utility", "Utility", "Utility", false,
                  {
                      pCont("gain", "Gain", -60, 24, 0, "dB"),
                      pCont("pan", "Balance", -100, 100, 0, "%"),
                      pCont("width", "Width", 0, 200, 100, "%"),
                      pToggle("invert_l", "Invert Left", false),
                      pToggle("invert_r", "Invert Right", false),
                      pToggle("mono", "Mono", false),
                      pToggle("dc_filter", "DC Filter", false),
                  });
}
ProcessorPtr makeUtility() { return std::make_unique<Utility>(); }

}  // namespace sq::engine::audio::builtin
