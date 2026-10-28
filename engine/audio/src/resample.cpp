// Squizzy Studio engine/audio - windowed-sinc sample-rate conversion (see resample.hpp).
#include "sq/engine/audio/resample.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sq::engine::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Kernel half-width in samples of the LOWER rate, Kaiser beta and cutoff (cycles per sample of the
// lower rate): transition band 0.45 .. 0.5, A ~ 100 dB (Kaiser: N - 1 = (A - 7.95) / (2.285 dw)).
constexpr double kHalfWidth = 66.0;
constexpr double kBeta = 10.06;
constexpr double kCutoff = 0.475;
// Fallback (irrational ratio): kernel table resolution per lower-rate sample (linear interp
// error < 1e-6, far below the stopband).
constexpr int kTableRes = 4096;
// Largest polyphase table (phases x taps) used for rational ratios.
constexpr int64_t kMaxPolyphase = int64_t{1} << 22;

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double q = x * x * 0.25;
  for (int k = 1; k < 200; ++k) {
    term *= q / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

// Prototype kernel at u lower-rate samples from the centre (unit DC gain over u).
double kernel(double u) {
  const double a = std::fabs(u);
  if (a >= kHalfWidth) return 0.0;
  const double x = 2.0 * kCutoff * u;
  const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
  const double r = a / kHalfWidth;
  static const double i0b = besselI0(kBeta);
  const double w = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
  return 2.0 * kCutoff * sinc * w;
}

const std::vector<double>& kernelTable() {
  static const std::vector<double> table = [] {
    const size_t n = static_cast<size_t>(kHalfWidth * kTableRes) + 2;
    std::vector<double> t(n);
    for (size_t i = 0; i < n; ++i) t[i] = kernel(static_cast<double>(i) / kTableRes);
    return t;
  }();
  return table;
}

double kernelLookup(const std::vector<double>& table, double u) {
  const double a = std::fabs(u) * kTableRes;
  const size_t i = static_cast<size_t>(a);
  if (i + 1 >= table.size()) return 0.0;
  const double f = a - static_cast<double>(i);
  return table[i] + (table[i + 1] - table[i]) * f;
}

// Weights of the 2H taps k = i - H + 1 .. i + H for an output at input position i + frac,
// normalised to unit sum (exact DC gain).
void tapWeights(double frac, double s, int64_t h, const std::vector<double>* table, double* w) {
  double sum = 0.0;
  for (int64_t j = 0; j < 2 * h; ++j) {
    const double t = static_cast<double>(j - h + 1) - frac;
    const double v = table ? kernelLookup(*table, t * s) : kernel(t * s);
    w[j] = v;
    sum += v;
  }
  if (sum != 0.0)
    for (int64_t j = 0; j < 2 * h; ++j) w[j] /= sum;
}

float convolve(const float* in, int64_t inFrames, int64_t i, int64_t h, const double* w) {
  const int64_t k0 = i - h + 1;
  double acc = 0.0;
  if (k0 >= 0 && i + h < inFrames) {
    const float* x = in + k0;
    for (int64_t j = 0; j < 2 * h; ++j) acc += w[j] * static_cast<double>(x[j]);
  } else {
    for (int64_t j = 0; j < 2 * h; ++j) {
      const int64_t k = k0 + j;
      if (k >= 0 && k < inFrames) acc += w[j] * static_cast<double>(in[k]);
    }
  }
  return static_cast<float>(acc);
}

bool integral(double r, int64_t* out) {
  if (!(r > 0.0) || r > 1e9) return false;
  const double ri = std::round(r);
  if (std::fabs(r - ri) > 1e-9) return false;
  *out = static_cast<int64_t>(ri);
  return true;
}

}  // namespace

std::vector<float> resampleChannel(const float* in, int64_t inFrames, double inRate, double outRate) {
  if (!in || inFrames <= 0) return {};
  if (!(inRate > 0.0) || !(outRate > 0.0) || inRate == outRate) return std::vector<float>(in, in + inFrames);
  const int64_t outFrames = std::max<int64_t>(1, std::llround(static_cast<double>(inFrames) * outRate / inRate));
  std::vector<float> out(static_cast<size_t>(outFrames));
  const double s = std::min(1.0, outRate / inRate);  // input samples -> lower-rate samples
  const int64_t h = static_cast<int64_t>(std::ceil(kHalfWidth / s));
  std::vector<double> w(static_cast<size_t>(2 * h));

  int64_t ir = 0, orr = 0;
  if (integral(inRate, &ir) && integral(outRate, &orr)) {
    const int64_t g = std::gcd(ir, orr);
    const int64_t m = ir / g, l = orr / g;  // output n sits at input n * m / l
    if (l * 2 * h <= kMaxPolyphase) {
      std::vector<double> poly(static_cast<size_t>(l * 2 * h));
      for (int64_t p = 0; p < l; ++p)
        tapWeights(static_cast<double>(p) / static_cast<double>(l), s, h, nullptr, &poly[static_cast<size_t>(p * 2 * h)]);
      for (int64_t n = 0; n < outFrames; ++n) {
        const int64_t pos = n * m;  // exact: n < 2^40, m < 2^23 in practice
        const int64_t i = pos / l, p = pos % l;
        out[static_cast<size_t>(n)] = convolve(in, inFrames, i, h, &poly[static_cast<size_t>(p * 2 * h)]);
      }
      return out;
    }
  }
  const std::vector<double>& table = kernelTable();
  const double step = inRate / outRate;
  for (int64_t n = 0; n < outFrames; ++n) {
    const double x = static_cast<double>(n) * step;
    const double fl = std::floor(x);
    tapWeights(x - fl, s, h, &table, w.data());
    out[static_cast<size_t>(n)] = convolve(in, inFrames, static_cast<int64_t>(fl), h, w.data());
  }
  return out;
}

MediaPtr resampleMedia(const MediaPtr& in, double outRate) {
  if (!in || !(outRate > 0.0) || !(in->sampleRate > 0.0) || std::fabs(in->sampleRate - outRate) < 1e-9) return in;
  auto m = std::make_shared<MediaBuffer>();
  m->sampleRate = outRate;
  m->channels = in->channels;
  m->path = in->path;
  m->data.resize(in->data.size());
  for (size_t c = 0; c < in->data.size(); ++c)
    m->data[c] = resampleChannel(in->data[c].data(), static_cast<int64_t>(in->data[c].size()), in->sampleRate, outRate);
  m->frames = m->data.empty() ? 0 : static_cast<int64_t>(m->data[0].size());
  return m;
}

}  // namespace sq::engine::audio
