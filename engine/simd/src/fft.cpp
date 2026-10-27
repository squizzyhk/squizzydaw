// Iterative radix-2 FFT (own implementation).
#include "sq/engine/simd/fft.hpp"

#include <cmath>
#include <stdexcept>

namespace sq::simd {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
}

Fft::Fft(uint32_t n) : n_(n) {
  if (n < 2 || !isPowerOfTwo(n)) throw std::invalid_argument("Fft: size must be a power of two >= 2");
  uint32_t bits = 0;
  while ((1u << bits) < n) ++bits;
  rev_.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t r = 0;
    for (uint32_t b = 0; b < bits; ++b)
      if (i & (1u << b)) r |= 1u << (bits - 1 - b);
    rev_[i] = r;
  }
  cos_.resize(n / 2);
  sin_.resize(n / 2);
  for (uint32_t k = 0; k < n / 2; ++k) {
    double a = kTwoPi * k / n;
    cos_[k] = static_cast<float>(std::cos(a));
    sin_[k] = static_cast<float>(std::sin(a));
  }
}

void Fft::run(float* re, float* im, float sign) const noexcept {
  const uint32_t n = n_;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t j = rev_[i];
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (uint32_t len = 2; len <= n; len <<= 1) {
    const uint32_t half = len >> 1;
    const uint32_t step = n / len;
    for (uint32_t base = 0; base < n; base += len) {
      for (uint32_t k = 0; k < half; ++k) {
        const float wr = cos_[k * step];
        const float wi = sign * sin_[k * step];
        const uint32_t a = base + k, b = a + half;
        const float xr = re[b] * wr - im[b] * wi;
        const float xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
}

void Fft::forward(float* re, float* im) const noexcept { run(re, im, -1.0f); }
void Fft::inverse(float* re, float* im) const noexcept { run(re, im, 1.0f); }

RealFft::RealFft(uint32_t n) : n_(n), half_(n >= 4 ? n / 2 : 2) {
  if (n < 4 || !isPowerOfTwo(n)) throw std::invalid_argument("RealFft: size must be a power of two >= 4");
  wr_.resize(n / 2);
  wi_.resize(n / 2);
  for (uint32_t k = 0; k < n / 2; ++k) {
    double a = -kTwoPi * k / n;
    wr_[k] = static_cast<float>(std::cos(a));
    wi_[k] = static_cast<float>(std::sin(a));
  }
  zr_.resize(n / 2);
  zi_.resize(n / 2);
}

void RealFft::forward(const float* in, float* re, float* im) noexcept {
  const uint32_t m = n_ / 2;
  for (uint32_t k = 0; k < m; ++k) {
    zr_[k] = in[2 * k];
    zi_[k] = in[2 * k + 1];
  }
  half_.forward(zr_.data(), zi_.data());
  // X[k] = E[k] + W^k O[k];  E = (Z[k] + conj Z[m-k]) / 2,  O = (Z[k] - conj Z[m-k]) / 2i
  for (uint32_t k = 0; k <= m / 2; ++k) {
    const uint32_t j = (m - k) & (m - 1);
    const float ar = zr_[k], ai = zi_[k];
    const float br = zr_[j], bi = -zi_[j];  // conj Z[m-k]
    const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
    // (a - b) / 2i = (ai - bi)/2 - i (ar - br)/2
    const float orr = 0.5f * (ai - bi), oi = -0.5f * (ar - br);
    const float wr = wr_[k], wi = wi_[k];
    re[k] = er + (orr * wr - oi * wi);
    im[k] = ei + (orr * wi + oi * wr);
    // mirror bin m-k uses the same pair swapped: E' = conj(E), O' = conj(O) ... compute directly
    if (k != 0 && k != m - k) {
      const uint32_t q = m - k;
      const float er2 = er, ei2 = -ei;   // E[m-k] = conj(E[k])
      const float or2 = orr, oi2 = -oi;  // O[m-k] = conj(O[k])
      const float w2r = wr_[q], w2i = wi_[q];
      re[q] = er2 + (or2 * w2r - oi2 * w2i);
      im[q] = ei2 + (or2 * w2i + oi2 * w2r);
    }
  }
  // Nyquist: X[m] = E[0] - O[0]
  {
    const float er = zr_[0], orr = zi_[0];
    re[m] = er - orr;
    im[m] = 0.0f;
    re[0] = er + orr;
    im[0] = 0.0f;
  }
}

void RealFft::inverse(const float* re, const float* im, float* out) noexcept {
  const uint32_t m = n_ / 2;
  // E[k] = (X[k] + conj X[m-k]) / 2 ; O[k] = (X[k] - conj X[m-k]) * W^-k / 2 ; Z = E + i O
  for (uint32_t k = 0; k < m; ++k) {
    const uint32_t j = m - k;
    const float ar = re[k], ai = im[k];
    const float br = re[j], bi = -im[j];
    const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
    const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
    // multiply by conj(W^k)
    const float wr = wr_[k], wi = -wi_[k];
    const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;
    // Z = E + i O
    zr_[k] = er - oi;
    zi_[k] = ei + orr;
  }
  half_.inverse(zr_.data(), zi_.data());
  for (uint32_t k = 0; k < m; ++k) {
    out[2 * k] = 2.0f * zr_[k];
    out[2 * k + 1] = 2.0f * zi_[k];
  }
}

void hannWindow(float* w, uint32_t n) noexcept {
  for (uint32_t i = 0; i < n; ++i) w[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / n));
}

}  // namespace sq::simd
