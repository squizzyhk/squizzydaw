// Squizzy Studio - engine/simd: small radix-2 FFT used by time-stretch and the analyzers.
// Own implementation (iterative decimation-in-time). Construction allocates; transforms do not
// (RT-safe after construction). Not thread-safe per instance.
#pragma once

#include <cstdint>
#include <vector>

namespace sq::simd {

// Complex FFT on split real/imag arrays, size n = power of two (>= 2).
class Fft {
 public:
  explicit Fft(uint32_t n);
  uint32_t size() const noexcept { return n_; }
  // In place, unnormalised: forward uses e^{-i...}; inverse(forward(x)) == n * x.
  void forward(float* re, float* im) const noexcept;
  void inverse(float* re, float* im) const noexcept;

 private:
  void run(float* re, float* im, float sign) const noexcept;
  uint32_t n_;
  std::vector<uint32_t> rev_;
  std::vector<float> cos_, sin_;
};

// Real FFT of size n (power of two >= 4) via one complex FFT of size n/2.
class RealFft {
 public:
  explicit RealFft(uint32_t n);
  uint32_t size() const noexcept { return n_; }
  uint32_t bins() const noexcept { return n_ / 2 + 1; }
  // in[n] -> re/im[n/2+1] (unnormalised).
  void forward(const float* in, float* re, float* im) noexcept;
  // re/im[n/2+1] -> out[n]; inverse(forward(x)) == n * x. re/im are not modified.
  void inverse(const float* re, const float* im, float* out) noexcept;

 private:
  uint32_t n_;
  Fft half_;
  std::vector<float> wr_, wi_;  // e^{-2 pi i k / n}, k < n/2
  std::vector<float> zr_, zi_;  // scratch n/2
};

// Periodic Hann window (sums to a constant at hop n/2 and, squared, at hop n/4).
void hannWindow(float* w, uint32_t n) noexcept;

inline bool isPowerOfTwo(uint32_t n) noexcept { return n >= 1 && (n & (n - 1)) == 0; }

}  // namespace sq::simd
