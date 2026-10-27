// Realtime pitch tracker (see tuner_core.hpp).
#include "tuner_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sq/interfaces/dsp.h"

namespace sq::tuner {

namespace {
uint64_t pack(float f, float c) {
  uint32_t a = 0, b = 0;
  std::memcpy(&a, &f, 4);
  std::memcpy(&b, &c, 4);
  return (static_cast<uint64_t>(b) << 32) | a;
}
void unpack(uint64_t v, float& f, float& c) {
  uint32_t a = static_cast<uint32_t>(v), b = static_cast<uint32_t>(v >> 32);
  std::memcpy(&f, &a, 4);
  std::memcpy(&c, &b, 4);
}
}  // namespace

bool PitchTracker::prepare(double sampleRate, uint32_t maxBlock, double minHz) {
  if (!(sampleRate >= 4000.0 && sampleRate <= 1.0e6) || maxBlock == 0 || !(minHz > 10.0 && minHz < 20000.0)) return false;
  sr_ = sampleRate;
  // lag range up to w/2 must reach the lowest frequency
  uint32_t w = 512;
  while (w / 2 < sampleRate / minHz + 2) w <<= 1;
  w_ = w;
  hop_ = w / 4;
  ring_.assign(w, 0.0f);
  frame_.assign(w, 0.0f);
  mono_.assign(maxBlock, 0.0f);
  fft_ = std::make_unique<sq::simd::RealFft>(2 * w);
  a_.assign(2 * w, 0.0f);
  b_.assign(2 * w, 0.0f);
  corr_.assign(2 * w, 0.0f);
  are_.assign(w + 1, 0.0f);
  aim_.assign(w + 1, 0.0f);
  bre_.assign(w + 1, 0.0f);
  bim_.assign(w + 1, 0.0f);
  d_.assign(w / 2, 0.0);
  cmnd_.assign(w / 2, 0.0);
  (void)sq_dsp_vector_ops();  // select kernels now, not on the audio thread
  reset();
  return true;
}

void PitchTracker::reset() noexcept {
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  pos_ = sinceLast_ = filled_ = 0;
  packed_.store(0, std::memory_order_relaxed);
}

void PitchTracker::pushChannels(const float* const* ch, uint32_t channels, uint32_t n) noexcept {
  if (!fft_ || !channels) return;
  const sq_vector_ops* ops = sq_dsp_vector_ops();
  const float g = 1.0f / static_cast<float>(channels);
  uint32_t done = 0;
  while (done < n) {
    const uint32_t m = std::min<uint32_t>(n - done, static_cast<uint32_t>(mono_.size()));
    ops->clear(mono_.data(), m);
    for (uint32_t c = 0; c < channels; ++c)
      if (ch[c]) ops->mix(mono_.data(), ch[c] + done, g, m);
    push(mono_.data(), m);
    done += m;
  }
}

void PitchTracker::push(const float* x, uint32_t n) noexcept {
  if (!fft_) return;
  for (uint32_t i = 0; i < n; ++i) {
    ring_[pos_] = std::isfinite(x[i]) ? x[i] : 0.0f;  // a NaN would poison the whole window
    pos_ = (pos_ + 1) % w_;
    if (filled_ < w_) ++filled_;
    if (++sinceLast_ >= hop_ && filled_ == w_) {
      sinceLast_ = 0;
      analyse();
    }
  }
}

void PitchTracker::analyse() noexcept {
  // unwrap the ring (oldest first)
  std::memcpy(frame_.data(), ring_.data() + pos_, sizeof(float) * (w_ - pos_));
  std::memcpy(frame_.data() + (w_ - pos_), ring_.data(), sizeof(float) * pos_);
  Estimate e = estimate(frame_.data());
  packed_.store(pack(static_cast<float>(e.frequency), e.confidence), std::memory_order_release);
}

Estimate PitchTracker::estimate(const float* x) noexcept {
  Estimate out;
  if (!fft_) return out;
  const uint32_t w = w_, half = w_ / 2;
  const sq_vector_ops* ops = sq_dsp_vector_ops();
  const float energy = ops->sum_squares(x, w);
  if (!(energy / static_cast<float>(w) >= 1e-8f) || !std::isfinite(energy)) return out;  // silence / non-finite
  // r(tau) = sum_{j<half} x[j] x[j+tau] via FFT: corr = IFFT(conj(FFT(x[0..half))) * FFT(x[0..w)))
  std::memset(a_.data(), 0, sizeof(float) * 2 * w);
  std::memset(b_.data(), 0, sizeof(float) * 2 * w);
  std::memcpy(a_.data(), x, sizeof(float) * w);
  std::memcpy(b_.data(), x, sizeof(float) * half);
  fft_->forward(a_.data(), are_.data(), aim_.data());
  fft_->forward(b_.data(), bre_.data(), bim_.data());
  for (uint32_t k = 0; k <= w; ++k) {
    const float r = are_[k] * bre_[k] + aim_[k] * bim_[k];
    const float i = aim_[k] * bre_[k] - are_[k] * bim_[k];
    are_[k] = r;
    aim_[k] = i;
  }
  fft_->inverse(are_.data(), aim_.data(), corr_.data());
  const double scale = 1.0 / (2.0 * w);
  // d(tau) = e0 + e(tau) - 2 r(tau), e(tau) = energy of x[tau..tau+half)
  double e0 = 0.0;
  for (uint32_t j = 0; j < half; ++j) e0 += static_cast<double>(x[j]) * x[j];
  double et = e0;
  d_[0] = 0.0;
  for (uint32_t tau = 1; tau < half; ++tau) {
    et += static_cast<double>(x[tau + half - 1]) * x[tau + half - 1] - static_cast<double>(x[tau - 1]) * x[tau - 1];
    d_[tau] = std::max(0.0, e0 + et - 2.0 * corr_[tau] * scale);
  }
  cmnd_[0] = 1.0;
  double running = 0.0;
  for (uint32_t tau = 1; tau < half; ++tau) {
    running += d_[tau];
    cmnd_[tau] = running > 0.0 ? d_[tau] * tau / running : 1.0;
  }
  // absolute threshold, then walk down to the local minimum
  const double threshold = 0.15;
  const uint32_t minTau = std::max<uint32_t>(2, static_cast<uint32_t>(sr_ / 2000.0));  // <= 2 kHz
  uint32_t tau = 0;
  for (uint32_t t = minTau; t < half - 1; ++t) {
    if (cmnd_[t] < threshold) {
      while (t + 1 < half - 1 && cmnd_[t + 1] < cmnd_[t]) ++t;
      tau = t;
      break;
    }
  }
  if (tau == 0) {
    uint32_t best = minTau;
    for (uint32_t t = minTau; t < half - 1; ++t)
      if (cmnd_[t] < cmnd_[best]) best = t;
    if (cmnd_[best] > 0.35) return out;  // unvoiced / noise
    tau = best;
  }
  double ft = tau;
  if (tau > 1 && tau + 1 < half) {
    // refine on the raw difference function (the normalisation skews short periods)
    const double a = d_[tau - 1], b = d_[tau], c = d_[tau + 1];
    const double den = a - 2.0 * b + c;
    if (den > 0.0) ft = tau + 0.5 * (a - c) / den;
  }
  out.frequency = sr_ / ft;
  out.confidence = static_cast<float>(std::clamp(1.0 - cmnd_[tau], 0.0, 1.0));
  return out;
}

Estimate PitchTracker::latest() const noexcept {
  float f = 0.0f, c = 0.0f;
  unpack(packed_.load(std::memory_order_acquire), f, c);
  return {static_cast<double>(f), c};
}

}  // namespace sq::tuner
