// Test-only helpers shared by the dsp unit's tests (engine/simd, time-stretch, analysis-*):
// synthesised signals and simple measurements. Header-only; never linked into product code.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace sqdsptest {

inline constexpr double kPi = 3.14159265358979323846;

inline std::vector<float> sine(double freq, double sr, size_t n, float amp = 0.5f, double phase = 0.0) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2.0 * kPi * freq * i / sr + phase));
  return v;
}

// Sum of harmonics 1..count with 1/h amplitudes (a band-limited saw-ish tone).
inline void addTone(std::vector<float>& v, double freq, double sr, size_t start, size_t len, float amp, int harmonics = 1) {
  for (size_t i = 0; i < len && start + i < v.size(); ++i) {
    double t = static_cast<double>(i) / sr;
    double s = 0.0;
    for (int h = 1; h <= harmonics; ++h)
      if (freq * h < sr * 0.45) s += std::sin(2.0 * kPi * freq * h * t) / h;
    // short fades avoid clicks at the tone edges
    double fade = std::min(1.0, std::min(static_cast<double>(i), static_cast<double>(len - i)) / (0.005 * sr));
    v[start + i] += amp * static_cast<float>(s * fade);
  }
}

// Click track: decaying noise bursts at `bpm`, accent on every `accentEvery`th click.
inline std::vector<float> clickTrack(double bpm, double sr, double seconds, double offsetSec = 0.0, int accentEvery = 4,
                                     uint32_t seed = 1) {
  std::vector<float> v(static_cast<size_t>(seconds * sr), 0.0f);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  const double period = 60.0 / bpm;
  const size_t burst = static_cast<size_t>(0.03 * sr);
  int idx = 0;
  for (double t = offsetSec; t * sr < static_cast<double>(v.size()); t += period, ++idx) {
    const size_t s = static_cast<size_t>(std::llround(t * sr));
    const float amp = (accentEvery > 0 && idx % accentEvery == 0) ? 0.9f : 0.5f;
    for (size_t i = 0; i < burst && s + i < v.size(); ++i)
      v[s + i] += amp * d(rng) * static_cast<float>(std::exp(-static_cast<double>(i) / (0.004 * sr)));
  }
  return v;
}

// Frequency from upward zero crossings (linear interpolation) within [from, to).
inline double zeroCrossingFreq(const std::vector<float>& v, double sr, size_t from, size_t to) {
  double first = -1.0, last = -1.0;
  int count = 0;
  for (size_t i = from + 1; i < to && i < v.size(); ++i) {
    if (v[i - 1] < 0.0f && v[i] >= 0.0f) {
      double t = (i - 1) + v[i - 1] / (v[i - 1] - v[i]);
      if (first < 0) first = t;
      last = t;
      ++count;
    }
  }
  if (count < 2) return 0.0;
  return (count - 1) * sr / (last - first);
}

inline double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < v.size(); ++i, ++n) s += static_cast<double>(v[i]) * v[i];
  return n ? std::sqrt(s / n) : 0.0;
}

}  // namespace sqdsptest
