// Squizzy Studio engine/audio - dynamics: compressor, lookahead brickwall limiter, gate.
// All detectors are stereo linked (one gain for every channel).
#include "device.hpp"
#include "meter.hpp"

namespace sq::engine::audio::builtin {
namespace {

// ---------------------------------------------------------------- compressor
enum CompP : uint32_t { cThresh, cRatio, cAttack, cRelease, cKnee, cMakeup, cAuto, cScHpf, cMix, cDetector, cCount };

class Compressor final : public FxDevice, public MeterSource {
 public:
  Compressor() : FxDevice(infoOf("compressor")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Compressor>(); }
  bool onPrepare() override {
    sc_.assign(channels(), dsp::Biquad{});
    rmsCoef_ = dsp::onePoleCoef(sr(), 8.0);
    return true;
  }
  void onReset() noexcept override {
    for (auto& b : sc_) b.reset();
    env_ = 0.0f;
    ms_ = 0.0;
    setMeter(0.0f);
  }
  void onParams(bool snap) noexcept override {
    const double s = sr();
    thr_ = p(cThresh);
    ratio_ = p(cRatio);
    knee_ = p(cKnee);
    att_ = dsp::onePoleCoef(s, p(cAttack));
    rel_ = dsp::onePoleCoef(s, p(cRelease));
    scOn_ = p(cScHpf) > 20.5f;
    if (scOn_) scC_ = dsp::designBiquad(dsp::BqType::Highpass, s, p(cScHpf), 0.7071, 0);
    float makeup = p(cMakeup);
    if (on(cAuto)) makeup += -thr_ * (1.0f - 1.0f / ratio_) * 0.5f;
    makeupDb_ = makeup;
    const float mix = p(cMix) * 0.01f;
    if (snap) mix_.snap(mix); else mix_.target = mix;
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const bool rms = ci(cDetector) == 1;
    const float slope = 1.0f / ratio_ - 1.0f;
    mix_.begin(mix_.target, n);
    for (uint32_t i = 0; i < n; ++i) {
      float det = 0.0f;
      for (uint32_t c = 0; c < nch; ++c) {
        const float xs = scOn_ ? sc_[c].process(scC_, io[c][i]) : io[c][i];
        if (rms) det += xs * xs;
        else det = std::max(det, std::fabs(xs));
      }
      float levelDb;
      if (rms) {
        ms_ = det / static_cast<double>(nch) + (ms_ - det / static_cast<double>(nch)) * rmsCoef_;
        levelDb = ms_ > 1e-16 ? static_cast<float>(10.0 * std::log10(ms_)) : -160.0f;
      } else {
        levelDb = dsp::linToDb(det);
      }
      const float over = levelDb - thr_;
      float red = 0.0f;
      if (knee_ > 0.0f && 2.0f * std::fabs(over) <= knee_) {
        const float k = over + knee_ * 0.5f;
        red = -slope * k * k / (2.0f * knee_);
      } else if (over > 0.0f) {
        red = -slope * over;
      }
      env_ = red > env_ ? red + (env_ - red) * att_ : red + (env_ - red) * rel_;
      const float g = dsp::dbToLin(makeupDb_ - env_);
      const float m = mix_.next();
      const float k = (1.0f - m) + m * g;
      for (uint32_t c = 0; c < nch; ++c) io[c][i] *= k;
    }
    mix_.end();
    env_ = dsp::flush(env_);
    for (auto& b : sc_) b.flushDenormals();
    setMeter(env_);
  }

 private:
  std::vector<dsp::Biquad> sc_;
  dsp::BiquadCoefs scC_;
  dsp::Ramp mix_;
  double ms_ = 0.0;
  float rmsCoef_ = 0.0f, env_ = 0.0f;
  float thr_ = -18.0f, ratio_ = 4.0f, knee_ = 6.0f, att_ = 0.0f, rel_ = 0.0f, makeupDb_ = 0.0f;
  bool scOn_ = false;
};

// ---------------------------------------------------------------- limiter
enum LimP : uint32_t { lGain, lCeiling, lRelease, lCount };
constexpr double kLookaheadMs = 1.5;

class Limiter final : public FxDevice, public MeterSource {
 public:
  Limiter() : FxDevice(infoOf("limiter")) {}
  uint32_t latency() const noexcept override { return la_; }

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Limiter>(); }
  bool onPrepare() override {
    la_ = static_cast<uint32_t>(std::lround(kLookaheadMs * 0.001 * sr()));
    if (la_ < 1) la_ = 1;
    win_ = la_ + 1;
    delay_.assign(static_cast<size_t>(channels()) * la_, 0.0f);
    qv_.assign(win_ + 1, 1.0f);
    qi_.assign(win_ + 1, 0);
    box_.assign(win_, 1.0f);
    return true;
  }
  void onReset() noexcept override {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    std::fill(box_.begin(), box_.end(), 1.0f);
    sum_ = static_cast<double>(win_);
    qHead_ = qTail_ = 0;
    pos_ = dpos_ = 0;
    n_ = 0;
    held_ = 1.0f;
    setMeter(0.0f);
  }
  void onParams(bool snap) noexcept override {
    const float g = dsp::dbToLin(p(lGain));
    const float c = dsp::dbToLin(p(lCeiling));
    if (snap) gain_.snap(g), ceil_.snap(c);
    else gain_.target = g, ceil_.target = c;
    rel_ = 1.0f - dsp::onePoleCoef(sr(), p(lRelease));
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    gain_.begin(gain_.target, n);
    ceil_.begin(ceil_.target, n);
    const uint32_t q = win_ + 1;
    float minGain = 1.0f;
    for (uint32_t i = 0; i < n; ++i) {
      const float g = gain_.next();
      const float ceil = ceil_.next();
      float peak = 0.0f;
      for (uint32_t c = 0; c < nch; ++c) peak = std::max(peak, std::fabs(io[c][i] * g));
      const float r = peak > ceil ? ceil / peak : 1.0f;
      // sliding minimum of r over the last win_ samples (monotonic queue)
      while (qTail_ != qHead_ && qv_[(qTail_ + q - 1) % q] >= r) qTail_ = (qTail_ + q - 1) % q;
      qv_[qTail_] = r;
      qi_[qTail_] = n_;
      qTail_ = (qTail_ + 1) % q;
      while (n_ - qi_[qHead_] >= win_) qHead_ = (qHead_ + 1) % q;
      const float h = qv_[qHead_];
      held_ = h < held_ ? h : held_ + (h - held_) * rel_;
      // moving average over win_ samples
      sum_ += static_cast<double>(held_) - box_[pos_];
      box_[pos_] = held_;
      const float gr = std::min(1.0f, static_cast<float>(sum_ / win_));
      minGain = std::min(minGain, gr);
      for (uint32_t c = 0; c < nch; ++c) {
        float* d = &delay_[static_cast<size_t>(c) * la_];
        const float in = io[c][i] * g;
        const float out = d[dpos_];  // written la_ samples ago
        d[dpos_] = in;
        io[c][i] = dsp::clampv(out * gr, -ceil, ceil);
      }
      pos_ = pos_ + 1 == win_ ? 0 : pos_ + 1;
      dpos_ = dpos_ + 1 == la_ ? 0 : dpos_ + 1;
      ++n_;
    }
    if ((n_ & 0xFFFFu) < n) {  // re-sum occasionally against drift
      double s = 0.0;
      for (float v : box_) s += v;
      sum_ = s;
    }
    gain_.end();
    ceil_.end();
    setMeter(-dsp::linToDb(minGain));
  }

 private:
  std::vector<float> delay_, qv_, box_;
  std::vector<uint64_t> qi_;
  uint32_t la_ = 0, win_ = 1, qHead_ = 0, qTail_ = 0, pos_ = 0, dpos_ = 0;
  uint64_t n_ = 0;
  double sum_ = 0.0;
  float held_ = 1.0f, rel_ = 0.0f;
  dsp::Ramp gain_, ceil_;
};

// ---------------------------------------------------------------- gate
enum GateP : uint32_t { gThresh, gAttack, gHold, gRelease, gRange, gCount };

class Gate final : public FxDevice, public MeterSource {
 public:
  Gate() : FxDevice(infoOf("gate")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Gate>(); }
  bool onPrepare() override {
    detRel_ = dsp::onePoleCoef(sr(), 10.0);
    return true;
  }
  void onReset() noexcept override {
    det_ = 0.0f;
    gain_ = dsp::dbToLin(p(gRange));
    open_ = false;
    hold_ = 0;
    setMeter(0.0f);
  }
  void onParams(bool) noexcept override {
    const double s = sr();
    openLin_ = dsp::dbToLin(p(gThresh));
    closeLin_ = dsp::dbToLin(p(gThresh) - 3.0f);
    att_ = dsp::onePoleCoef(s, p(gAttack));
    rel_ = dsp::onePoleCoef(s, p(gRelease));
    holdFrames_ = static_cast<uint32_t>(p(gHold) * 0.001 * s);
    floor_ = dsp::dbToLin(p(gRange));
  }
  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    for (uint32_t i = 0; i < n; ++i) {
      float peak = 0.0f;
      for (uint32_t c = 0; c < nch; ++c) peak = std::max(peak, std::fabs(io[c][i]));
      det_ = peak > det_ ? peak : det_ * detRel_;
      if (det_ >= openLin_) {
        open_ = true;
        hold_ = holdFrames_;
      } else if (open_ && det_ < closeLin_) {
        if (hold_ > 0) --hold_;
        else open_ = false;
      }
      gain_ = open_ ? 1.0f + (gain_ - 1.0f) * att_ : floor_ + (gain_ - floor_) * rel_;
      for (uint32_t c = 0; c < nch; ++c) io[c][i] *= gain_;
    }
    det_ = dsp::flush(det_);
    setMeter(-dsp::linToDb(gain_));
  }

 private:
  float det_ = 0.0f, detRel_ = 0.0f, gain_ = 0.0f, openLin_ = 0.0f, closeLin_ = 0.0f, att_ = 0.0f, rel_ = 0.0f, floor_ = 0.0f;
  uint32_t hold_ = 0, holdFrames_ = 0;
  bool open_ = false;
};

}  // namespace

DeviceInfo CompressorInfo() {
  return makeInfo("compressor", "Compressor", "Dynamics", false,
                  {
                      pCont("threshold", "Threshold", -60, 0, -18, "dB"),
                      pLog("ratio", "Ratio", 1, 20, 4, ":1"),
                      pLog("attack", "Attack", 0.1f, 200, 10, "ms"),
                      pLog("release", "Release", 5, 2000, 120, "ms"),
                      pCont("knee", "Knee", 0, 24, 6, "dB"),
                      pCont("makeup", "Makeup", 0, 24, 0, "dB"),
                      pToggle("auto_makeup", "Auto Makeup", false),
                      pLog("sidechain_hpf", "Sidechain HPF", 20, 500, 20, "Hz"),
                      pCont("mix", "Mix", 0, 100, 100, "%"),
                      pChoice("detector", "Detector", {"Peak", "RMS"}, 1),
                  });
}
ProcessorPtr makeCompressor() { return std::make_unique<Compressor>(); }

DeviceInfo LimiterInfo() {
  return makeInfo("limiter", "Limiter", "Dynamics", false,
                  {
                      pCont("gain", "Gain", 0, 24, 0, "dB"),
                      pCont("ceiling", "Ceiling", -24, 0, -0.3f, "dB"),
                      pLog("release", "Release", 1, 1000, 80, "ms"),
                  });
}
ProcessorPtr makeLimiter() { return std::make_unique<Limiter>(); }

DeviceInfo GateInfo() {
  return makeInfo("gate", "Gate", "Dynamics", false,
                  {
                      pCont("threshold", "Threshold", -80, 0, -45, "dB"),
                      pLog("attack", "Attack", 0.05f, 100, 0.5f, "ms"),
                      pCont("hold", "Hold", 0, 1000, 25, "ms"),
                      pLog("release", "Release", 5, 4000, 150, "ms"),
                      pCont("range", "Range", -80, 0, -80, "dB"),
                  });
}
ProcessorPtr makeGate() { return std::make_unique<Gate>(); }

}  // namespace sq::engine::audio::builtin
