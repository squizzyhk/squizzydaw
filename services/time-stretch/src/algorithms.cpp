// time-stretch frame algorithms (own implementations):
//  - WSOLA: waveform-similarity overlap-add. Each frame is cut from the input near its nominal
//    position at the offset whose start best continues the previously used segment (normalised
//    cross-correlation over the overlap), Hann window, hop N/2.
//  - Phase vocoder: STFT with Hann analysis/synthesis windows, hop N/4, instantaneous-frequency
//    phase propagation with identity phase locking around spectral peaks. One phase rotation per
//    bin is computed and applied to every channel, which keeps the inter-channel phase relations
//    (stereo image) intact. The rotation follows, per bin, the channel that is loudest in that bin
//    (its own previous phase gives the instantaneous frequency); peaks use the summed channel
//    magnitudes. A plain channel sum would cancel for polarity-inverted / wide stereo content and
//    leave the bin without a usable phase. Optional transient phase reset
//    (SQ_STRETCH_PRESERVE_TRANSIENTS).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "stretch_core.hpp"

namespace sq::stretch {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

inline double princarg(double p) { return p - kTwoPi * std::floor((p + kPi) / kTwoPi); }

class Wsola final : public FrameAlgorithm {
 public:
  Wsola(const Settings& s, const sq_vector_ops* ops) : ops_(ops), channels_(s.channels) {
    const bool preview = s.quality <= SQ_STRETCH_QUALITY_PREVIEW;
    n_ = scaledFrameSize(s.sampleRate, preview ? 512.0 : 1024.0);
    hop_ = n_ / 2;
    overlap_ = n_ - hop_;
    radius_ = n_ / 4;
    step_ = preview ? 2 : 1;
    window_.resize(n_);
    sq::simd::hannWindow(window_.data(), n_);
    energies_.assign(2 * radius_ + 1, 0.0);
    reset();
  }
  const char* name() const noexcept override { return "wsola"; }
  uint32_t frameSize() const noexcept override { return n_; }
  uint32_t hop() const noexcept override { return hop_; }
  uint32_t searchRadius() const noexcept override { return radius_; }
  void reset() noexcept override {
    first_ = true;
    prevChosen_ = 0;
  }
  int64_t oldestNeeded(int64_t nextCenter) const noexcept override {
    int64_t lo = nextCenter - static_cast<int64_t>(n_ / 2 + radius_);
    if (!first_) lo = std::min(lo, prevChosen_ - static_cast<int64_t>(n_ / 2) + hop_);
    return lo;
  }

  void frame(const InputView& in, int64_t center, float* const* out) noexcept override {
    int64_t chosen = center;
    if (!first_) {
      const int64_t target = prevChosen_ - static_cast<int64_t>(n_ / 2) + hop_;
      const int64_t start0 = center - static_cast<int64_t>(n_ / 2);
      const int32_t r = static_cast<int32_t>(radius_);
      // sliding energy of the candidate overlap regions, in double
      double energy = 0.0;
      for (uint32_t c = 0; c < channels_; ++c) energy += ops_->sum_squares(in.at(c, start0 - r), overlap_);
      const double eps = 1e-9 * overlap_;
      double best = -1e300;
      int32_t bestD = 0;
      auto score = [&](int32_t d, double e) {
        double corr = 0.0;
        for (uint32_t c = 0; c < channels_; ++c) corr += ops_->dot(in.at(c, target), in.at(c, start0 + d), overlap_);
        return corr / std::sqrt(std::max(e, 0.0) + eps);
      };
      std::vector<double>& energies = energies_;  // sized in the constructor
      for (int32_t d = -r; d <= r; ++d) {
        energies[static_cast<size_t>(d + r)] = energy;
        if (d < r) {
          for (uint32_t c = 0; c < channels_; ++c) {
            const float out0 = *in.at(c, start0 + d);
            const float in0 = *in.at(c, start0 + d + static_cast<int64_t>(overlap_));
            energy += static_cast<double>(in0) * in0 - static_cast<double>(out0) * out0;
          }
        }
      }
      // evaluate offset 0 first so ties (e.g. silence) keep the nominal position
      best = score(0, energies[static_cast<size_t>(r)]);
      for (int32_t d = -r; d <= r; d += step_) {
        if (d == 0) continue;
        double s = score(d, energies[static_cast<size_t>(d + r)]);
        if (s > best) best = s, bestD = d;
      }
      if (step_ > 1) {
        const int32_t base = bestD;
        for (int32_t d = base - 1; d <= base + 1; d += 2) {
          if (d < -r || d > r) continue;
          double s = score(d, energies[static_cast<size_t>(d + r)]);
          if (s > best) best = s, bestD = d;
        }
      }
      chosen = center + bestD;
    }
    const int64_t start = chosen - static_cast<int64_t>(n_ / 2);
    for (uint32_t c = 0; c < channels_; ++c) {
      ops_->copy(out[c], in.at(c, start), n_);
      ops_->mul(out[c], window_.data(), n_);
    }
    prevChosen_ = chosen;
    first_ = false;
  }

 private:
  const sq_vector_ops* ops_;
  uint32_t channels_;
  uint32_t n_ = 0, hop_ = 0, overlap_ = 0, radius_ = 0;
  int32_t step_ = 1;
  std::vector<float> window_;
  std::vector<double> energies_;
  bool first_ = true;
  int64_t prevChosen_ = 0;
};

class PhaseVocoder final : public FrameAlgorithm {
 public:
  PhaseVocoder(const Settings& s, const sq_vector_ops* ops)
      : ops_(ops),
        channels_(s.channels),
        n_(scaledFrameSize(s.sampleRate, s.quality >= SQ_STRETCH_QUALITY_HIGH ? 2048.0 : 1024.0)),
        fft_(n_),
        transients_((s.flags & SQ_STRETCH_PRESERVE_TRANSIENTS) != 0) {
    hop_ = n_ / 4;
    bins_ = n_ / 2 + 1;
    window_.resize(n_);
    sq::simd::hannWindow(window_.data(), n_);
    re_.assign(channels_, std::vector<float>(bins_));
    im_.assign(channels_, std::vector<float>(bins_));
    for (auto* v : {&mag_, &phase_, &prevPhase_, &prevMag_, &synth_, &rotC_, &rotS_, &bestMag_})
      v->assign(bins_, 0.0f);
    chPhase_.assign(channels_, std::vector<float>(bins_, 0.0f));
    chPrevPhase_.assign(channels_, std::vector<float>(bins_, 0.0f));
    peaks_.reserve(bins_);
    time_.assign(n_, 0.0f);
    reset();
  }
  const char* name() const noexcept override { return "phase-vocoder"; }
  uint32_t frameSize() const noexcept override { return n_; }
  uint32_t hop() const noexcept override { return hop_; }
  uint32_t searchRadius() const noexcept override { return 0; }
  int64_t oldestNeeded(int64_t nextCenter) const noexcept override { return nextCenter - static_cast<int64_t>(n_ / 2); }
  void reset() noexcept override {
    first_ = true;
    prevCenter_ = 0;
    std::fill(prevPhase_.begin(), prevPhase_.end(), 0.0f);
    std::fill(prevMag_.begin(), prevMag_.end(), 0.0f);
    for (auto& v : chPrevPhase_) std::fill(v.begin(), v.end(), 0.0f);
    std::fill(synth_.begin(), synth_.end(), 0.0f);
  }

  void frame(const InputView& in, int64_t center, float* const* out) noexcept override {
    const int64_t start = center - static_cast<int64_t>(n_ / 2);
    std::fill(mag_.begin(), mag_.end(), 0.0f);
    std::fill(bestMag_.begin(), bestMag_.end(), -1.0f);
    for (uint32_t c = 0; c < channels_; ++c) {
      ops_->copy(time_.data(), in.at(c, start), n_);
      ops_->mul(time_.data(), window_.data(), n_);
      float* re = re_[c].data();
      float* im = im_[c].data();
      fft_.forward(time_.data(), re, im);
      float* ph = chPhase_[c].data();
      const float* prevPh = chPrevPhase_[c].data();
      for (uint32_t k = 0; k < bins_; ++k) {
        const float m = std::sqrt(re[k] * re[k] + im[k] * im[k]);
        ph[k] = std::atan2(im[k], re[k]);
        mag_[k] += m;
        if (m > bestMag_[k]) {  // reference channel of this bin: the loudest one
          bestMag_[k] = m;
          phase_[k] = ph[k];
          prevPhase_[k] = prevPh[k];  // that channel's own previous phase
        }
      }
    }
    double total = 0.0, flux = 0.0;
    for (uint32_t k = 0; k < bins_; ++k) {
      total += mag_[k];
      const float inc = mag_[k] - prevMag_[k];
      if (inc > 0.0f) flux += inc;
    }
    const int64_t ha = center - prevCenter_;
    bool resetPhase = first_;
    if (transients_ && !first_ && total > 1e-6 * bins_ && flux > 0.5 * total) resetPhase = true;

    if (resetPhase) {
      for (uint32_t k = 0; k < bins_; ++k) synth_[k] = phase_[k];
    } else {
      // instantaneous frequency per bin (stored in rotC_ temporarily)
      float* inst = rotC_.data();
      for (uint32_t k = 0; k < bins_; ++k) {
        const double omega = kTwoPi * k / n_;
        if (ha > 0) {
          const double dev = princarg(static_cast<double>(phase_[k]) - prevPhase_[k] - omega * static_cast<double>(ha));
          inst[k] = static_cast<float>(omega + dev / static_cast<double>(ha));
        } else {
          inst[k] = static_cast<float>(omega);
        }
      }
      // peaks: local maxima over +-2 bins
      peaks_.clear();
      for (uint32_t k = 2; k + 2 < bins_; ++k) {
        const float m = mag_[k];
        if (m > mag_[k - 1] && m >= mag_[k + 1] && m > mag_[k - 2] && m >= mag_[k + 2] && m > 1e-9f) peaks_.push_back(k);
      }
      if (peaks_.empty()) {
        for (uint32_t k = 0; k < bins_; ++k)
          synth_[k] = static_cast<float>(princarg(synth_[k] + static_cast<double>(inst[k]) * hop_));
      } else {
        for (size_t i = 0; i < peaks_.size(); ++i) {
          const uint32_t p = peaks_[i];
          const uint32_t lo = i == 0 ? 0u : (peaks_[i - 1] + p + 1) / 2;
          const uint32_t hi = i + 1 == peaks_.size() ? bins_ : (p + peaks_[i + 1] + 1) / 2;
          const double sp = princarg(synth_[p] + static_cast<double>(inst[p]) * hop_);
          for (uint32_t k = lo; k < hi; ++k)
            synth_[k] = static_cast<float>(princarg(sp + static_cast<double>(phase_[k]) - phase_[p]));
        }
      }
    }
    for (uint32_t k = 0; k < bins_; ++k) {
      const double rot = static_cast<double>(synth_[k]) - phase_[k];
      rotC_[k] = static_cast<float>(std::cos(rot));
      rotS_[k] = static_cast<float>(std::sin(rot));
      prevMag_[k] = mag_[k];
    }
    chPrevPhase_.swap(chPhase_);
    const float norm = 1.0f / (static_cast<float>(n_) * 1.5f);
    for (uint32_t c = 0; c < channels_; ++c) {
      float* re = re_[c].data();
      float* im = im_[c].data();
      for (uint32_t k = 0; k < bins_; ++k) {
        const float r = re[k] * rotC_[k] - im[k] * rotS_[k];
        const float i = re[k] * rotS_[k] + im[k] * rotC_[k];
        re[k] = r;
        im[k] = i;
      }
      im[0] = 0.0f;
      im[bins_ - 1] = 0.0f;
      fft_.inverse(re, im, out[c]);
      ops_->mul(out[c], window_.data(), n_);
      ops_->scale(out[c], norm, n_);
    }
    prevCenter_ = center;
    first_ = false;
  }

 private:
  const sq_vector_ops* ops_;
  uint32_t channels_;
  uint32_t n_;
  sq::simd::RealFft fft_;
  bool transients_;
  uint32_t hop_ = 0, bins_ = 0;
  std::vector<float> window_, time_;
  std::vector<std::vector<float>> re_, im_;
  std::vector<float> mag_, phase_, prevPhase_, prevMag_, synth_, rotC_, rotS_, bestMag_;
  std::vector<std::vector<float>> chPhase_, chPrevPhase_;  // per channel, this / previous frame
  std::vector<uint32_t> peaks_;
  bool first_ = true;
  int64_t prevCenter_ = 0;
};

}  // namespace

std::unique_ptr<FrameAlgorithm> makeWsola(const Settings& s, const sq_vector_ops* ops) {
  return std::make_unique<Wsola>(s, ops);
}

std::unique_ptr<FrameAlgorithm> makePhaseVocoder(const Settings& s, const sq_vector_ops* ops) {
  return std::make_unique<PhaseVocoder>(s, ops);
}

}  // namespace sq::stretch
