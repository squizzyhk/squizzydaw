// Squizzy Studio engine/audio - "Reverb": 8-line feedback delay network with a Hadamard mixing
// matrix, input predelay + low cut + allpass diffusion, in-loop HF damping and slow line
// modulation. Channel pairs (0,1), (2,3)... each get their own network; an odd last channel runs
// a network in mono.
#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t { kSize, kDecay, kDamping, kPredelay, kWidth, kMix, kLowCut, kDiffusion, kModulation, kCount };

constexpr int kLines = 8;
constexpr double kLineMs[kLines] = {29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.7, 73.1};
constexpr double kDiffMs[4] = {4.7, 3.6, 12.7, 9.3};
constexpr double kMaxScale = 1.6;
constexpr double kMaxPredelayMs = 250.0;
constexpr float kOutL[kLines] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr float kOutR[kLines] = {1, 1, -1, -1, 1, 1, -1, -1};

inline double sizeScale(double size) noexcept { return 0.4 + 1.2 * size; }

struct Net {
  dsp::FracDelay pre, diff[4], line[kLines];
  dsp::Biquad lowcut;
  float damp[kLines] = {};
  double lfo[kLines] = {};
  void reset() noexcept {
    pre.reset();
    for (auto& d : diff) d.reset();
    for (auto& l : line) l.reset();
    lowcut.reset();
    for (int i = 0; i < kLines; ++i) {
      damp[i] = 0.0f;
      lfo[i] = static_cast<double>(i) / kLines;
    }
  }
};

inline void hadamard8(float* x) noexcept {
  for (int h = 1; h < kLines; h <<= 1)
    for (int i = 0; i < kLines; i += 2 * h)
      for (int j = i; j < i + h; ++j) {
        const float a = x[j], b = x[j + h];
        x[j] = a + b;
        x[j + h] = a - b;
      }
  const float k = 0.35355339f;  // 1/sqrt(8)
  for (int i = 0; i < kLines; ++i) x[i] *= k;
}

class Reverb final : public FxDevice {
 public:
  Reverb() : FxDevice(infoOf("reverb")) {}
  uint32_t tail() const noexcept override {
    if (!prepared()) return 0;
    const double sec = static_cast<double>(t(kDecay)) * 1.1 + t(kPredelay) * 0.001 + kLineMs[kLines - 1] * kMaxScale * 0.001;
    return static_cast<uint32_t>(sec * sr());
  }

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Reverb>(); }
  bool onPrepare() override {
    const double s = sr();
    nets_.clear();
    nets_.resize((channels() + 1) / 2);
    modMax_ = 16.0 * s / 48000.0;
    for (auto& n : nets_) {
      n.pre.resize(static_cast<size_t>(kMaxPredelayMs * 0.001 * s) + 4);
      for (int i = 0; i < 4; ++i) n.diff[i].resize(static_cast<size_t>(kDiffMs[i] * 0.001 * s) + 4);
      for (int i = 0; i < kLines; ++i)
        n.line[i].resize(static_cast<size_t>(kLineMs[i] * 0.001 * s * kMaxScale + 2.0 * modMax_) + 8);
    }
    for (int i = 0; i < 4; ++i) diffLen_[i] = std::max<uint32_t>(1, static_cast<uint32_t>(kDiffMs[i] * 0.001 * s));
    return true;
  }
  void onReset() noexcept override {
    for (auto& n : nets_) n.reset();
  }
  void onParams(bool snap) noexcept override {
    const double s = sr();
    const double scale = sizeScale(p(kSize));
    const double t60 = std::max(0.05f, p(kDecay));
    for (int i = 0; i < kLines; ++i) {
      len_[i] = static_cast<float>(kLineMs[i] * 0.001 * s * scale);
      fb_[i] = static_cast<float>(std::pow(10.0, -3.0 * (len_[i] / s) / t60));
    }
    const double fc = 800.0 * std::exp2((1.0 - p(kDamping)) * 4.5);
    dampA_ = static_cast<float>(std::exp(-dsp::kTwoPi * std::min(fc, s * 0.45) / s));
    pre_ = std::max(1.0f, static_cast<float>(p(kPredelay) * 0.001 * s));
    lowC_ = dsp::designBiquad(dsp::BqType::Highpass, s, p(kLowCut), 0.7071, 0);
    diffG_ = 0.45f + 0.3f * p(kDiffusion);
    modDepth_ = static_cast<float>(p(kModulation) * modMax_);
    const float mix = p(kMix) * 0.01f;
    const float dry = std::min(1.0f, 2.0f * (1.0f - mix)), wet = std::min(1.0f, 2.0f * mix);
    if (snap) dry_.snap(dry), wet_.snap(wet), width_.snap(p(kWidth));
    else dry_.target = dry, wet_.target = wet, width_.target = p(kWidth);
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const double s = sr();
    double lfoInc[kLines];
    for (int i = 0; i < kLines; ++i) lfoInc[i] = (0.31 + 0.13 * i) / s;
    for (size_t pi = 0; pi < nets_.size(); ++pi) {
      const uint32_t cl = static_cast<uint32_t>(pi * 2);
      if (cl >= nch) break;
      float* L = io[cl];
      float* R = cl + 1 < nch ? io[cl + 1] : nullptr;
      Net& net = nets_[pi];
      dsp::Ramp dry = dry_, wet = wet_, width = width_;
      dry.begin(dry_.target, n);
      wet.begin(wet_.target, n);
      width.begin(width_.target, n);
      for (uint32_t i = 0; i < n; ++i) {
        const float inL = L[i], inR = R ? R[i] : L[i];
        net.pre.push(0.5f * (inL + inR));
        float x = net.pre.readLinear(pre_);
        x = net.lowcut.process(lowC_, x);
        for (int d = 0; d < 4; ++d) {
          const float wd = net.diff[d].at(diffLen_[d]);
          const float w = x + diffG_ * wd;
          x = wd - diffG_ * w;
          net.diff[d].push(dsp::flush(w));
        }
        float o[kLines];
        for (int k = 0; k < kLines; ++k) {
          const float mod = modDepth_ * static_cast<float>(std::sin(dsp::kTwoPi * net.lfo[k]));
          net.lfo[k] += lfoInc[k];
          if (net.lfo[k] >= 1.0) net.lfo[k] -= 1.0;
          const float v = net.line[k].read(len_[k] + modDepth_ + mod + 1.0f);
          net.damp[k] = v + dampA_ * (net.damp[k] - v);
          o[k] = net.damp[k];
        }
        float outL = 0.0f, outR = 0.0f;
        for (int k = 0; k < kLines; ++k) {
          outL += o[k] * kOutL[k];
          outR += o[k] * kOutR[k];
        }
        hadamard8(o);
        for (int k = 0; k < kLines; ++k) net.line[k].push(dsp::flush(o[k] * fb_[k] + ((k & 1) ? -x : x)));
        outL *= 0.3f;
        outR *= 0.3f;
        const float wd = width.next();
        const float mid = 0.5f * (outL + outR), side = 0.5f * (outL - outR) * wd;
        const float dg = dry.next(), wg = wet.next();
        if (R) {
          L[i] = inL * dg + (mid + side) * wg;
          R[i] = inR * dg + (mid - side) * wg;
        } else {
          L[i] = inL * dg + mid * wg;
        }
      }
      for (auto& d : net.damp) d = dsp::flush(d);
      net.lowcut.flushDenormals();
    }
    dry_.end();
    wet_.end();
    width_.end();
  }

 private:
  std::vector<Net> nets_;
  float len_[kLines] = {}, fb_[kLines] = {};
  uint32_t diffLen_[4] = {1, 1, 1, 1};
  float dampA_ = 0.0f, pre_ = 1.0f, diffG_ = 0.6f, modDepth_ = 0.0f;
  double modMax_ = 16.0;
  dsp::BiquadCoefs lowC_;
  dsp::Ramp dry_, wet_, width_;
};

}  // namespace

DeviceInfo ReverbInfo() {
  return makeInfo("reverb", "Reverb", "Reverb", false,
                  {
                      pCont("size", "Size", 0, 1, 0.55f),
                      pLog("decay", "Decay", 0.2f, 20, 2.2f, "s"),
                      pCont("damping", "Damping", 0, 1, 0.45f),
                      pCont("predelay", "Predelay", 0, 250, 12, "ms"),
                      pCont("width", "Width", 0, 1, 1),
                      pCont("mix", "Mix", 0, 100, 25, "%"),
                      pLog("lowcut", "Low Cut", 20, 1000, 120, "Hz"),
                      pCont("diffusion", "Diffusion", 0, 1, 0.7f),
                      pCont("modulation", "Modulation", 0, 1, 0.3f),
                  });
}
ProcessorPtr makeReverb() { return std::make_unique<Reverb>(); }

}  // namespace sq::engine::audio::builtin
