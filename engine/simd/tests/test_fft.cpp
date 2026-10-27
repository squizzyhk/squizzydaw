// engine/simd: FFT against a naive DFT, real FFT round trips, Hann window properties.
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <vector>

#include "sq/engine/simd/fft.hpp"
#include "sqtest.hpp"

namespace {
constexpr double kPi = 3.14159265358979323846;

void naiveDft(const std::vector<double>& xr, const std::vector<double>& xi, std::vector<double>& yr,
              std::vector<double>& yi) {
  const size_t n = xr.size();
  yr.assign(n, 0.0);
  yi.assign(n, 0.0);
  for (size_t k = 0; k < n; ++k)
    for (size_t t = 0; t < n; ++t) {
      double a = -2.0 * kPi * double(k * t % n) / double(n);
      yr[k] += xr[t] * std::cos(a) - xi[t] * std::sin(a);
      yi[k] += xr[t] * std::sin(a) + xi[t] * std::cos(a);
    }
}
}  // namespace

SQ_TEST("fft: complex forward matches naive DFT, inverse round-trips") {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> d(-1.f, 1.f);
  for (uint32_t n : {2u, 4u, 8u, 64u, 256u}) {
    sq::simd::Fft fft(n);
    std::vector<float> re(n), im(n);
    std::vector<double> xr(n), xi(n), yr, yi;
    for (uint32_t i = 0; i < n; ++i) xr[i] = re[i] = d(rng), xi[i] = im[i] = d(rng);
    naiveDft(xr, xi, yr, yi);
    fft.forward(re.data(), im.data());
    double err = 0;
    for (uint32_t i = 0; i < n; ++i) err = std::max(err, std::fabs(re[i] - yr[i]) + std::fabs(im[i] - yi[i]));
    CHECK(err < 1e-3 * n);
    fft.inverse(re.data(), im.data());
    double err2 = 0;
    for (uint32_t i = 0; i < n; ++i) err2 = std::max(err2, std::fabs(re[i] / n - xr[i]) + std::fabs(im[i] / n - xi[i]));
    CHECK(err2 < 1e-5);
  }
}

SQ_TEST("fft: real forward matches naive DFT, inverse round-trips") {
  std::mt19937 rng(9);
  std::uniform_real_distribution<float> d(-1.f, 1.f);
  for (uint32_t n : {4u, 8u, 16u, 512u, 2048u}) {
    sq::simd::RealFft fft(n);
    std::vector<float> x(n), re(n / 2 + 1), im(n / 2 + 1), back(n);
    std::vector<double> xr(n), xi(n, 0.0), yr, yi;
    for (uint32_t i = 0; i < n; ++i) xr[i] = x[i] = d(rng);
    naiveDft(xr, xi, yr, yi);
    fft.forward(x.data(), re.data(), im.data());
    double err = 0;
    for (uint32_t k = 0; k <= n / 2; ++k) err = std::max(err, std::fabs(re[k] - yr[k]) + std::fabs(im[k] - yi[k]));
    CHECK(err < 1e-4 * n);
    fft.inverse(re.data(), im.data(), back.data());
    double err2 = 0;
    for (uint32_t i = 0; i < n; ++i) err2 = std::max(err2, double(std::fabs(back[i] / n - x[i])));
    CHECK(err2 < 1e-5);
  }
}

SQ_TEST("fft: hann window overlap-adds to a constant") {
  const uint32_t n = 1024;
  std::vector<float> w(n);
  sq::simd::hannWindow(w.data(), n);
  CHECK(std::fabs(w[0]) < 1e-7f);
  CHECK(std::fabs(w[n / 2] - 1.0f) < 1e-6f);
  for (uint32_t i = 0; i < n / 2; ++i) CHECK(std::fabs(w[i] + w[i + n / 2] - 1.0f) < 1e-5f);
  for (uint32_t i = 0; i < n / 4; ++i) {
    float s = 0;
    for (uint32_t k = 0; k < 4; ++k) s += w[i + k * n / 4] * w[i + k * n / 4];
    CHECK(std::fabs(s - 1.5f) < 1e-4f);
  }
}

SQ_TEST("fft: rejects sizes that are not powers of two") {
  bool threw = false;
  try {
    sq::simd::Fft f(12);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}
