// Squizzy Studio engine/audio - modulation effects: chorus, flanger, phaser, tremolo/autopan.
// Odd channels run their LFO `spread` (or `stereo`) of a half cycle ahead of even channels.
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

// ---------------------------------------------------------------- chorus / flanger
enum ModP : uint32_t { mRate, mDepth, mDelay, mFeedback, mMix, mSpread, mCount };

// Modulated delay line effect. Chorus: two voices per channel, no feedback by default.
// Flanger: one short voice with (signed) feedback.
class ModDelay final : public FxDevice {
 public:
  explicit ModDelay(bool flanger) : FxDevice(infoOf(flanger ? "flanger" : "chorus")), flanger_(flanger) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<ModDelay>(flanger_); }
  bool onPrepare() override {
    lines_.assign(channels(), dsp::FracDelay{});
    for (auto& l : lines_) l.resize(static_cast<size_t>(0.06 * sr()) + 8);
    fbState_.assign(channels(), 0.0f);
    return true;
  }
  void onReset() noexcept override {
    for (auto& l : lines_) l.reset();
    std::fill(fbState_.begin(), fbState_.end(), 0.0f);
    phase_ = 0.0;
  }
  void onParams(bool snap) noexcept override {
    const float s = static_cast<float>(sr());
    const float base = p(mDelay) * 0.001f * s;
    const float depth = p(mDepth) * (flanger_ ? base * 0.95f + 0.004f * s : 0.006f * s);
    const float mix = p(mMix) * 0.01f;
    const float fb = p(mFeedback) * 0.01f;
    if (snap) base_.snap(base), depth_.snap(depth), mix_.snap(mix), fb_.snap(fb);
    else base_.target = base, depth_.target = depth, mix_.target = mix, fb_.target = fb;
  }
  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const double inc = static_cast<double>(p(mRate)) / sr();
    const double spread = p(mSpread) * 0.5;
    const int voices = flanger_ ? 1 : 2;
    for (uint32_t c = 0; c < nch; ++c) {
      dsp::Ramp base = base_, depth = depth_, mix = mix_, fb = fb_;
      base.begin(base_.target, n);
      depth.begin(depth_.target, n);
      mix.begin(mix_.target, n);
      fb.begin(fb_.target, n);
      dsp::FracDelay& line = lines_[c];
      float& fbs = fbState_[c];
      double ph = phase_ + ((c & 1u) ? spread : 0.0);
      const float maxD = static_cast<float>(line.capacity()) - 4.0f;
      for (uint32_t i = 0; i < n; ++i) {
        const float b = base.next(), d = depth.next(), m = mix.next(), g = fb.next();
        const float x = io[c][i];
        line.push(dsp::flush(x + fbs * g));
        float wet = 0.0f;
        for (int v = 0; v < voices; ++v) {
          double pv = ph + 0.5 * v;
          pv -= std::floor(pv);
          const float lfo = flanger_ ? 0.5f - 0.5f * std::cos(dsp::kTwoPiF * static_cast<float>(pv))
                                     : 0.5f + 0.5f * std::sin(dsp::kTwoPiF * static_cast<float>(pv));
          const float dl = dsp::clampv(b + d * lfo + static_cast<float>(v) * 0.0031f * static_cast<float>(sr()), 1.0f, maxD);
          wet += line.read(dl);
        }
        wet /= static_cast<float>(voices);
        fbs = dsp::flush(wet);
        io[c][i] = x * (1.0f - m) + wet * m;
        ph += inc;
      }
    }
    phase_ += inc * n;
    phase_ -= std::floor(phase_);
    base_.end();
    depth_.end();
    mix_.end();
    fb_.end();
  }

 private:
  bool flanger_;
  std::vector<dsp::FracDelay> lines_;
  std::vector<float> fbState_;
  dsp::Ramp base_, depth_, mix_, fb_;
  double phase_ = 0.0;
};

// ---------------------------------------------------------------- phaser
enum PhP : uint32_t { phRate, phDepth, phCenter, phFeedback, phStages, phMix, phSpread, phCount };
constexpr int kMaxStages = 12;

class Phaser final : public FxDevice {
 public:
  Phaser() : FxDevice(infoOf("phaser")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Phaser>(); }
  bool onPrepare() override {
    z_.assign(static_cast<size_t>(channels()) * kMaxStages, 0.0f);
    last_.assign(channels(), 0.0f);
    return true;
  }
  void onReset() noexcept override {
    std::fill(z_.begin(), z_.end(), 0.0f);
    std::fill(last_.begin(), last_.end(), 0.0f);
    phase_ = 0.0;
  }
  void onParams(bool snap) noexcept override {
    const float mix = p(phMix) * 0.01f;
    if (snap) mix_.snap(mix); else mix_.target = mix;
  }
  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const double s = sr();
    const int stages = 2 + 2 * ci(phStages);
    const float fb = p(phFeedback) * 0.01f * 0.95f;
    const double spread = p(phSpread) * 0.5;
    for (uint32_t c = 0; c < nch; ++c) {
      double ph = phase_ + ((c & 1u) ? spread : 0.0);
      const float lfo = std::sin(dsp::kTwoPiF * static_cast<float>(ph - std::floor(ph)));
      const double f = dsp::clampv(p(phCenter) * std::exp2(p(phDepth) * 3.0 * lfo), 20.0, s * 0.45);
      const double tn = std::tan(dsp::kPi * f / s);
      const float a = static_cast<float>((tn - 1.0) / (tn + 1.0));
      float* z = &z_[static_cast<size_t>(c) * kMaxStages];
      dsp::Ramp mix = mix_;
      mix.begin(mix_.target, n);
      for (uint32_t i = 0; i < n; ++i) {
        const float x = io[c][i];
        float y = x + last_[c] * fb;
        for (int k = 0; k < stages; ++k) {
          const float out = a * y + z[k];
          z[k] = y - a * out;
          y = out;
        }
        last_[c] = y;
        const float m = mix.next();
        io[c][i] = x * (1.0f - 0.5f * m) + y * 0.5f * m;
      }
      last_[c] = dsp::flush(last_[c]);
      for (int k = 0; k < stages; ++k) z[k] = dsp::flush(z[k]);
    }
    phase_ += static_cast<double>(p(phRate)) * n / s;
    phase_ -= std::floor(phase_);
    mix_.end();
  }

 private:
  std::vector<float> z_, last_;
  dsp::Ramp mix_;
  double phase_ = 0.0;
};

// ---------------------------------------------------------------- tremolo / autopan
enum TrP : uint32_t { tRate, tDepth, tWave, tStereo, tCount };

class Tremolo final : public FxDevice {
 public:
  Tremolo() : FxDevice(infoOf("tremolo")) {}

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Tremolo>(); }
  bool onPrepare() override {
    smooth_.assign(channels(), 1.0f);
    sqCoef_ = dsp::onePoleCoef(sr(), 2.0);
    return true;
  }
  void onReset() noexcept override {
    std::fill(smooth_.begin(), smooth_.end(), 1.0f);
    phase_ = 0.0;
  }
  void onParams(bool snap) noexcept override {
    if (snap) depth_.snap(p(tDepth)); else depth_.target = p(tDepth);
  }
  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const double inc = static_cast<double>(p(tRate)) / sr();
    const double off = p(tStereo) / 360.0;
    const int wave = ci(tWave);
    for (uint32_t c = 0; c < nch; ++c) {
      double ph = phase_ + ((c & 1u) ? off : 0.0);
      dsp::Ramp depth = depth_;
      depth.begin(depth_.target, n);
      float& sm = smooth_[c];
      for (uint32_t i = 0; i < n; ++i) {
        const float pf = static_cast<float>(ph - std::floor(ph));
        float lfo;  // 0..1
        switch (wave) {
          case 1: lfo = pf < 0.5f ? 2.0f * pf : 2.0f - 2.0f * pf; break;
          case 2: lfo = pf < 0.5f ? 1.0f : 0.0f; break;
          case 3: lfo = 1.0f - pf; break;
          default: lfo = 0.5f + 0.5f * std::sin(dsp::kTwoPiF * pf); break;
        }
        const float g = 1.0f - depth.next() * (1.0f - lfo);
        sm = g + (sm - g) * sqCoef_;  // de-click square/saw edges
        io[c][i] *= sm;
        ph += inc;
      }
    }
    phase_ += inc * n;
    phase_ -= std::floor(phase_);
    depth_.end();
  }

 private:
  std::vector<float> smooth_;
  dsp::Ramp depth_;
  float sqCoef_ = 0.0f;
  double phase_ = 0.0;
};

}  // namespace

DeviceInfo ChorusInfo() {
  return makeInfo("chorus", "Chorus", "Modulation", false,
                  {
                      pLog("rate", "Rate", 0.05f, 10, 0.8f, "Hz"),
                      pCont("depth", "Depth", 0, 1, 0.4f),
                      pCont("delay", "Delay", 1, 30, 8, "ms"),
                      pCont("feedback", "Feedback", -90, 90, 0, "%"),
                      pCont("mix", "Mix", 0, 100, 50, "%"),
                      pCont("spread", "Stereo Spread", 0, 1, 1),
                  });
}
ProcessorPtr makeChorus() { return std::make_unique<ModDelay>(false); }

DeviceInfo FlangerInfo() {
  return makeInfo("flanger", "Flanger", "Modulation", false,
                  {
                      pLog("rate", "Rate", 0.02f, 10, 0.25f, "Hz"),
                      pCont("depth", "Depth", 0, 1, 0.7f),
                      pCont("delay", "Delay", 0.1f, 10, 1.5f, "ms"),
                      pCont("feedback", "Feedback", -95, 95, 50, "%"),
                      pCont("mix", "Mix", 0, 100, 50, "%"),
                      pCont("spread", "Stereo Spread", 0, 1, 0.5f),
                  });
}
ProcessorPtr makeFlanger() { return std::make_unique<ModDelay>(true); }

DeviceInfo PhaserInfo() {
  return makeInfo("phaser", "Phaser", "Modulation", false,
                  {
                      pLog("rate", "Rate", 0.02f, 10, 0.4f, "Hz"),
                      pCont("depth", "Depth", 0, 1, 0.7f),
                      pLog("center", "Center", 100, 5000, 800, "Hz"),
                      pCont("feedback", "Feedback", -100, 100, 50, "%"),
                      pChoice("stages", "Stages", {"2", "4", "6", "8", "10", "12"}, 2),
                      pCont("mix", "Mix", 0, 100, 100, "%"),
                      pCont("spread", "Stereo Spread", 0, 1, 0.5f),
                  });
}
ProcessorPtr makePhaser() { return std::make_unique<Phaser>(); }

DeviceInfo TremoloInfo() {
  return makeInfo("tremolo", "Tremolo", "Modulation", false,
                  {
                      pLog("rate", "Rate", 0.05f, 20, 4, "Hz"),
                      pCont("depth", "Depth", 0, 1, 0.5f),
                      pChoice("wave", "Wave", {"Sine", "Triangle", "Square", "Saw"}, 0),
                      pCont("stereo", "Stereo Phase", 0, 180, 0, "deg"),
                  });
}
ProcessorPtr makeTremolo() { return std::make_unique<Tremolo>(); }

}  // namespace sq::engine::audio::builtin
