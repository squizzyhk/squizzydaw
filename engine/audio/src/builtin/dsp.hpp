// Squizzy Studio engine/audio - private DSP building blocks for the built-in devices.
// Everything here is header-only, allocation free after prepare/resize and RT safe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sq::engine::audio::dsp {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;
inline constexpr float kPiF = 3.14159265358979323846f;
inline constexpr float kTwoPiF = 2.0f * kPiF;

template <class T>
inline T clampv(T v, T lo, T hi) noexcept {
  return v < lo ? lo : (v > hi ? hi : v);
}
inline float dbToLin(float db) noexcept { return db <= -144.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }
inline float linToDb(float lin) noexcept { return lin <= 1e-8f ? -160.0f : 20.0f * std::log10(lin); }
inline double noteToHz(double note) noexcept { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }
inline float flush(float v) noexcept { return std::fabs(v) < 1e-20f ? 0.0f : v; }
inline double flush(double v) noexcept { return std::fabs(v) < 1e-30 ? 0.0 : v; }
// Coefficient of a one-pole smoother reaching ~63% in `ms` milliseconds.
inline float onePoleCoef(double sr, double ms) noexcept {
  if (ms <= 0.0) return 0.0f;
  return static_cast<float>(std::exp(-1000.0 / (ms * sr)));
}
// Per-sample multiplier that decays by 60 dB in `ms` milliseconds.
inline float decayCoef(double sr, double ms) noexcept {
  if (ms <= 0.0) return 0.0f;
  return static_cast<float>(std::exp(std::log(0.001) * 1000.0 / (ms * sr)));
}
// Fast smooth saturator (bounded to +-1, unity slope at 0).
inline float softClip(float x) noexcept {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// xorshift32 noise.
struct Rng {
  uint32_t s = 0x9E3779B9u;
  uint32_t next() noexcept {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float bipolar() noexcept { return static_cast<float>(static_cast<int32_t>(next())) * (1.0f / 2147483648.0f); }
  float unipolar() noexcept { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

// Linear per-sample ramp toward a value set once per sub-block.
struct Ramp {
  float cur = 0.0f;
  float step = 0.0f;
  float target = 0.0f;
  void snap(float v) noexcept { cur = target = v; step = 0.0f; }
  void begin(float t, uint32_t n) noexcept {
    target = t;
    step = n ? (t - cur) / static_cast<float>(n) : 0.0f;
    if (!n) cur = t;
  }
  float next() noexcept {
    cur += step;
    return cur;
  }
  void end() noexcept {
    cur = target;
    step = 0.0f;
  }
};

// ---- filters ---------------------------------------------------------------------------------

// RBJ cookbook biquad, transposed direct form II with double state.
struct BiquadCoefs {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
enum class BqType : uint8_t { Lowpass, Highpass, Bandpass, Notch, Peak, LowShelf, HighShelf, Allpass };
inline BiquadCoefs designBiquad(BqType type, double sr, double freq, double q, double gainDb) noexcept {
  freq = clampv(freq, 1.0, sr * 0.49);
  q = std::max(q, 0.025);
  const double w0 = kTwoPi * freq / sr;
  const double cw = std::cos(w0), sw = std::sin(w0);
  const double alpha = sw / (2.0 * q);
  const double A = std::pow(10.0, gainDb / 40.0);
  double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  switch (type) {
    case BqType::Lowpass:
      b0 = (1 - cw) * 0.5; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BqType::Highpass:
      b0 = (1 + cw) * 0.5; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BqType::Bandpass:  // 0 dB peak gain
      b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BqType::Notch:
      b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BqType::Peak:
      b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
      break;
    case BqType::LowShelf: {
      const double s = 2.0 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cw + s);
      b1 = 2 * A * ((A - 1) - (A + 1) * cw);
      b2 = A * ((A + 1) - (A - 1) * cw - s);
      a0 = (A + 1) + (A - 1) * cw + s;
      a1 = -2 * ((A - 1) + (A + 1) * cw);
      a2 = (A + 1) + (A - 1) * cw - s;
      break;
    }
    case BqType::HighShelf: {
      const double s = 2.0 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cw + s);
      b1 = -2 * A * ((A - 1) + (A + 1) * cw);
      b2 = A * ((A + 1) + (A - 1) * cw - s);
      a0 = (A + 1) - (A - 1) * cw + s;
      a1 = 2 * ((A - 1) - (A + 1) * cw);
      a2 = (A + 1) - (A - 1) * cw - s;
      break;
    }
    case BqType::Allpass:
      b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
  }
  BiquadCoefs c;
  c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
  return c;
}

struct Biquad {
  double z1 = 0, z2 = 0;
  void reset() noexcept { z1 = z2 = 0; }
  float process(const BiquadCoefs& c, float xf) noexcept {
    const double x = xf;
    const double y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return static_cast<float>(y);
  }
  void flushDenormals() noexcept {
    z1 = flush(z1);
    z2 = flush(z2);
  }
};

// Trapezoidal (zero-delay feedback) state variable filter: simultaneous LP/BP/HP outputs.
struct SvfCoefs {
  float g = 0, k = 2, a1 = 0, a2 = 0, a3 = 0;
  void set(double sr, double freq, double kDamp) noexcept {
    freq = clampv(freq, 5.0, sr * 0.47);
    g = static_cast<float>(std::tan(kPi * freq / sr));
    k = static_cast<float>(kDamp);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  // resonance 0..1 -> damping 2..~0.05
  static double dampFromRes(double res) noexcept { return 2.0 - 1.95 * clampv(res, 0.0, 1.0); }
};
struct Svf {
  float ic1 = 0, ic2 = 0;
  float lp = 0, bp = 0, hp = 0;
  void reset() noexcept { ic1 = ic2 = lp = bp = hp = 0; }
  void tick(const SvfCoefs& c, float v0) noexcept {
    const float v3 = v0 - ic2;
    const float v1 = c.a1 * ic1 + c.a2 * v3;
    const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    lp = v2;
    bp = v1;
    hp = v0 - c.k * v1 - v2;
  }
  void flushDenormals() noexcept {
    ic1 = flush(ic1);
    ic2 = flush(ic2);
  }
};

// One-pole lowpass / highpass (6 dB/oct).
struct OnePole {
  float a = 0, z = 0;
  void setLowpass(double sr, double freq) noexcept {
    a = static_cast<float>(std::exp(-kTwoPi * clampv(freq, 1.0, sr * 0.49) / sr));
  }
  float lowpass(float x) noexcept {
    z = x + a * (z - x);
    return z;
  }
  float highpass(float x) noexcept { return x - lowpass(x); }
  void reset() noexcept { z = 0; }
  void flushDenormals() noexcept { z = flush(z); }
};

// DC blocker (~10 Hz).
struct DcBlocker {
  float x1 = 0, y1 = 0, r = 0.9987f;
  void prepare(double sr) noexcept { r = static_cast<float>(1.0 - kTwoPi * 10.0 / sr); }
  float process(float x) noexcept {
    const float y = x - x1 + r * y1;
    x1 = x;
    y1 = flush(y);
    return y;
  }
  void reset() noexcept { x1 = y1 = 0; }
};

// ---- oscillators -----------------------------------------------------------------------------

inline float polyBlep(float t, float dt) noexcept {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

enum class Wave : uint8_t { Sine, Triangle, Saw, Square, Noise };

// Band-limited (PolyBLEP) oscillator; triangle is a leaky-integrated band-limited square.
struct BlepOsc {
  float phase = 0.0f;
  float tri = 0.0f;
  void reset(float ph) noexcept {
    phase = ph;
    tri = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
  }
  float tick(Wave w, float dt, float pw, Rng& rng) noexcept {
    dt = clampv(dt, 0.0f, 0.45f);
    float y = 0.0f;
    switch (w) {
      case Wave::Sine: y = std::sin(kTwoPiF * phase); break;
      case Wave::Saw: y = 2.0f * phase - 1.0f - polyBlep(phase, dt); break;
      case Wave::Square:
      case Wave::Triangle: {
        const float p = w == Wave::Triangle ? 0.5f : pw;
        float sq = phase < p ? 1.0f : -1.0f;
        sq += polyBlep(phase, dt);
        float t2 = phase - p;
        if (t2 < 0.0f) t2 += 1.0f;
        sq -= polyBlep(t2, dt);
        if (w == Wave::Square) {
          y = sq;
        } else {
          tri = tri * 0.9995f + 4.0f * dt * sq;
          y = tri;
        }
        break;
      }
      case Wave::Noise: y = rng.bipolar(); break;
    }
    phase += dt;
    if (phase >= 1.0f) phase -= 1.0f;
    return y;
  }
};

// ---- envelopes / LFO -------------------------------------------------------------------------

struct AdsrParams {
  float attackInc = 1.0f;  // per sample
  float decayCoef = 0.0f;
  float sustain = 1.0f;
  float releaseCoef = 0.0f;
  void set(double sr, double aMs, double dMs, double s, double rMs) noexcept {
    attackInc = static_cast<float>(1.0 / std::max(1.0, aMs * 0.001 * sr));
    decayCoef = decayCoef_(sr, dMs);
    sustain = static_cast<float>(clampv(s, 0.0, 1.0));
    releaseCoef = decayCoef_(sr, rMs);
  }
  static float decayCoef_(double sr, double ms) noexcept { return dsp::decayCoef(sr, std::max(ms, 0.5)); }
};

struct Adsr {
  enum Stage : uint8_t { Idle, Attack, Decay, Release };
  Stage stage = Idle;
  float level = 0.0f;
  void gateOn() noexcept { stage = Attack; }
  void gateOff() noexcept {
    if (stage != Idle) stage = Release;
  }
  void kill() noexcept {
    stage = Idle;
    level = 0.0f;
  }
  bool active() const noexcept { return stage != Idle; }
  float tick(const AdsrParams& p) noexcept {
    switch (stage) {
      case Attack:
        level += p.attackInc;
        if (level >= 1.0f) {
          level = 1.0f;
          stage = Decay;
        }
        break;
      case Decay: level = flush(p.sustain + (level - p.sustain) * p.decayCoef); break;
      case Release:
        level *= p.releaseCoef;
        if (level < 1e-5f) kill();
        break;
      case Idle: break;
    }
    return level;
  }
};

enum class LfoWave : uint8_t { Sine, Triangle, Saw, Square, SampleHold };
struct Lfo {
  double phase = 0.0;
  float held = 0.0f;
  Rng rng;
  // Value in -1..1 at the given phase offset (0..1); advance() moves on.
  float value(LfoWave w, double offset = 0.0) noexcept {
    double ph = phase + offset;
    ph -= std::floor(ph);
    const float p = static_cast<float>(ph);
    switch (w) {
      case LfoWave::Sine: return std::sin(kTwoPiF * p);
      case LfoWave::Triangle: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
      case LfoWave::Saw: return 2.0f * p - 1.0f;
      case LfoWave::Square: return p < 0.5f ? 1.0f : -1.0f;
      case LfoWave::SampleHold: return held;
    }
    return 0.0f;
  }
  void advance(double inc) noexcept {
    phase += inc;
    if (phase >= 1.0) {
      phase -= std::floor(phase);
      held = rng.bipolar();
    }
  }
};

// ---- delay lines -----------------------------------------------------------------------------

// Power-of-two circular delay with fractional (cubic Hermite) reads.
struct FracDelay {
  std::vector<float> buf;
  uint32_t mask = 0;
  uint32_t w = 0;
  void resize(size_t minLen) {
    size_t n = 4;
    while (n < minLen + 4) n <<= 1;
    buf.assign(n, 0.0f);
    mask = static_cast<uint32_t>(n - 1);
    w = 0;
  }
  void reset() noexcept {
    std::fill(buf.begin(), buf.end(), 0.0f);
    w = 0;
  }
  size_t capacity() const noexcept { return buf.size() > 4 ? buf.size() - 4 : 0; }
  void push(float x) noexcept {
    buf[w] = x;
    w = (w + 1) & mask;
  }
  // Integer read: d >= 1 means the sample pushed d pushes ago (d=1 -> most recent).
  float at(uint32_t d) const noexcept { return buf[(w - d) & mask]; }
  // Fractional read, d in [1, capacity-2].
  float read(float d) const noexcept {
    const float di = std::floor(d);
    const float f = d - di;
    const uint32_t i = static_cast<uint32_t>(di);
    const float xm1 = at(i - 1 == 0 ? 1 : i - 1);
    const float x0 = at(i);
    const float x1 = at(i + 1);
    const float x2 = at(i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
  }
  float readLinear(float d) const noexcept {
    const float di = std::floor(d);
    const float f = d - di;
    const uint32_t i = static_cast<uint32_t>(di);
    return at(i) + f * (at(i + 1) - at(i));
  }
};

}  // namespace sq::engine::audio::dsp
