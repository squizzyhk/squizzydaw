// analysis-beat core (see beat_core.hpp).
#include "beat_core.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "sq/engine/simd/fft.hpp"

namespace sq::beat {

namespace {

std::vector<float> movingMean(const std::vector<float>& v, int radius) {
  std::vector<float> out(v.size());
  double acc = 0.0;
  int count = 0;
  const int n = static_cast<int>(v.size());
  int lo = 0, hi = -1;
  for (int i = 0; i < n; ++i) {
    while (hi < std::min(n - 1, i + radius)) acc += v[static_cast<size_t>(++hi)], ++count;
    while (lo < i - radius) acc -= v[static_cast<size_t>(lo++)], --count;
    out[static_cast<size_t>(i)] = count ? static_cast<float>(acc / count) : 0.0f;
  }
  return out;
}

// Refines an onset near sample `centre` to the start of the energy rise (1 ms energy envelope).
double refineOnset(const std::vector<float>& x, double sr, int64_t centre, int64_t radius) {
  const int64_t n = static_cast<int64_t>(x.size());
  const int64_t a = std::max<int64_t>(0, centre - radius), b = std::min<int64_t>(n, centre + radius);
  if (b - a < 8) return static_cast<double>(centre) / sr;
  const int64_t w = std::max<int64_t>(4, static_cast<int64_t>(0.001 * sr));
  std::vector<double> e(static_cast<size_t>(b - a), 0.0);
  double acc = 0.0;
  for (int64_t i = a; i < b; ++i) {
    acc += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    if (i - w >= a) acc -= static_cast<double>(x[static_cast<size_t>(i - w)]) * x[static_cast<size_t>(i - w)];
    e[static_cast<size_t>(i - a)] = std::max(0.0, acc);
  }
  const size_t peak = static_cast<size_t>(std::max_element(e.begin(), e.end()) - e.begin());
  double base = e[peak];
  for (size_t i = 0; i < peak; ++i) base = std::min(base, e[i]);
  const double thr = base + 0.2 * (e[peak] - base);
  size_t i = peak;
  while (i > 0 && e[i - 1] > thr) --i;
  // e[i] is the energy of the window ending at sample a+i: its start is the rise point
  return static_cast<double>(std::max<int64_t>(a, a + static_cast<int64_t>(i) - w / 2)) / sr;
}

}  // namespace

Result analyze(const std::vector<float>& mono, double sampleRate, const Options& opt) {
  Result r;
  if (mono.empty() || !(sampleRate >= 4000.0 && sampleRate <= 1.0e6)) return r;
  uint32_t n = 256;
  while (n < 1024.0 * sampleRate / 44100.0 * 0.75) n <<= 1;
  const uint32_t hop = n / 8;
  const double fps = sampleRate / hop;
  r.envelopeRate = fps;
  sq::simd::RealFft fft(n);
  const uint32_t bins = n / 2 + 1;
  std::vector<float> window(n), buf(n), re(bins), im(bins), logm(bins), prevLog(bins, 0.0f);
  sq::simd::hannWindow(window.data(), n);
  const size_t total = mono.size();
  const size_t frames = total / hop + 1;
  r.envelope.assign(frames, 0.0f);
  const float norm = 4.0f / static_cast<float>(n);
  for (size_t t = 0; t < frames; ++t) {
    const int64_t start = static_cast<int64_t>(t * hop) - static_cast<int64_t>(n / 2);
    for (uint32_t i = 0; i < n; ++i) {
      const int64_t idx = start + i;
      buf[i] = (idx >= 0 && idx < static_cast<int64_t>(total)) ? mono[static_cast<size_t>(idx)] * window[i] : 0.0f;
    }
    fft.forward(buf.data(), re.data(), im.data());
    double flux = 0.0;
    for (uint32_t k = 1; k < bins; ++k) {
      const float m = std::sqrt(re[k] * re[k] + im[k] * im[k]) * norm;
      logm[k] = std::log1p(100.0f * m);
      const float d = logm[k] - prevLog[k];
      if (d > 0.0f && t > 0) flux += d;
    }
    prevLog.swap(logm);
    r.envelope[t] = static_cast<float>(flux);
  }

  // ---- onsets
  const float envMax = *std::max_element(r.envelope.begin(), r.envelope.end());
  if (envMax <= 0.0f) return r;
  const int meanRadius = std::max(2, static_cast<int>(0.1 * fps));
  const std::vector<float> localMean = movingMean(r.envelope, meanRadius);
  const int peakRadius = std::max(1, static_cast<int>(0.02 * fps));
  const int minGap = std::max(1, static_cast<int>(0.05 * fps));
  int lastOnset = -minGap - 1;
  for (int t = 1; t + 1 < static_cast<int>(frames); ++t) {
    const float v = r.envelope[static_cast<size_t>(t)];
    if (v < 1.5f * localMean[static_cast<size_t>(t)] + 0.05f * envMax) continue;
    bool isMax = true;
    for (int d = -peakRadius; d <= peakRadius && isMax; ++d) {
      const int j = t + d;
      if (d == 0 || j < 0 || j >= static_cast<int>(frames)) continue;
      if (r.envelope[static_cast<size_t>(j)] > v || (d < 0 && r.envelope[static_cast<size_t>(j)] == v)) isMax = false;
    }
    if (!isMax || t - lastOnset < minGap) continue;
    lastOnset = t;
    const double time = refineOnset(mono, sampleRate, static_cast<int64_t>(t) * hop, n / 2);
    r.onsets.push_back({time, v / envMax});
  }

  // ---- tempo: autocorrelation of the mean-removed, half-wave rectified envelope
  const std::vector<float> slowMean = movingMean(r.envelope, std::max(2, static_cast<int>(0.25 * fps)));
  std::vector<float> e(frames);
  for (size_t t = 0; t < frames; ++t) e[t] = std::max(0.0f, r.envelope[t] - slowMean[t]);
  const int lagMin = std::max(2, static_cast<int>(std::floor(60.0 * fps / opt.maxBpm)));
  const int lagMax = static_cast<int>(std::ceil(60.0 * fps / opt.minBpm));
  const int acMax = std::min(static_cast<int>(frames) - 1, 4 * lagMax + 2);
  if (acMax <= lagMin * 2) return r;
  std::vector<double> ac(static_cast<size_t>(acMax + 1), 0.0);
  for (int l = 0; l <= acMax; ++l) {
    double s = 0.0;
    for (size_t t = 0; t + static_cast<size_t>(l) < frames; ++t) s += static_cast<double>(e[t]) * e[t + static_cast<size_t>(l)];
    ac[static_cast<size_t>(l)] = s / static_cast<double>(frames - static_cast<size_t>(l));
  }
  auto acAt = [&](double l) {
    if (l < 0 || l >= acMax) return 0.0;
    const size_t i = static_cast<size_t>(l);
    const double f = l - static_cast<double>(i);
    return ac[i] * (1.0 - f) + ac[i + 1] * f;
  };
  double bestScore = -1.0, bestLag = 0.0;
  std::vector<double> scores(static_cast<size_t>(lagMax + 1), 0.0);
  for (int l = lagMin; l <= lagMax && l < acMax; ++l) {
    double s = 0.0;
    for (int k = 1; k <= 4; ++k) s += acAt(static_cast<double>(l) * k) / k;
    const double bpm = 60.0 * fps / l;
    const double oct = std::log2(bpm / 120.0);
    s *= std::exp(-0.5 * oct * oct);
    scores[static_cast<size_t>(l)] = s;
    if (s > bestScore) bestScore = s, bestLag = l;
  }
  if (bestLag <= 0.0 || bestScore <= 0.0) return r;
  {
    const int l = static_cast<int>(bestLag);
    if (l > lagMin && l < lagMax) {
      const double a = scores[static_cast<size_t>(l - 1)], b = scores[static_cast<size_t>(l)], c = scores[static_cast<size_t>(l + 1)];
      const double den = a - 2 * b + c;
      if (den < 0) bestLag = l + 0.5 * (a - c) / den;
    }
  }
  const double period = bestLag;

  // ---- beat tracking by dynamic programming
  double mean = 0.0, sq = 0.0;
  for (float v : r.envelope) mean += v, sq += static_cast<double>(v) * v;
  mean /= frames;
  const double sd = std::sqrt(std::max(1e-12, sq / frames - mean * mean));
  std::vector<double> o(frames), score(frames);
  std::vector<int> back(frames, -1);
  for (size_t t = 0; t < frames; ++t) o[t] = r.envelope[t] / sd;
  const double tightness = 60.0;
  const int lo = static_cast<int>(std::round(period * 0.5)), hi = static_cast<int>(std::round(period * 2.0));
  for (int t = 0; t < static_cast<int>(frames); ++t) {
    double best = 0.0;
    int arg = -1;
    for (int tau = t - hi; tau <= t - lo; ++tau) {
      if (tau < 0) continue;
      const double dev = std::log(static_cast<double>(t - tau) / period);
      const double s = score[static_cast<size_t>(tau)] - tightness * dev * dev;
      if (arg < 0 || s > best) best = s, arg = tau;
    }
    score[static_cast<size_t>(t)] = o[static_cast<size_t>(t)] + (arg >= 0 && best > 0.0 ? best : 0.0);
    back[static_cast<size_t>(t)] = (arg >= 0 && best > 0.0) ? arg : -1;
  }
  // best final beat within the last period
  int last = static_cast<int>(frames) - 1;
  {
    const int from = std::max(0, static_cast<int>(frames) - 1 - static_cast<int>(std::ceil(period)));
    for (int t = from; t < static_cast<int>(frames); ++t)
      if (score[static_cast<size_t>(t)] > score[static_cast<size_t>(last)]) last = t;
  }
  std::vector<int> beatFrames;
  for (int t = last; t >= 0; t = back[static_cast<size_t>(t)]) beatFrames.push_back(t);
  std::reverse(beatFrames.begin(), beatFrames.end());
  if (beatFrames.size() < 2) {
    r.bpm = 60.0 * fps / period;
    return r;
  }

  // snap to refined onsets within 50 ms
  std::vector<double> times;
  std::vector<float> strength;
  for (int bf : beatFrames) {
    double t = static_cast<double>(bf) / fps;
    double bestD = 0.05;
    for (const Onset& on : r.onsets) {
      const double d = std::fabs(on.time - t);
      if (d < bestD) bestD = d, t = on.time;
    }
    times.push_back(t);
    strength.push_back(std::min(1.0f, r.envelope[static_cast<size_t>(bf)] / envMax));
  }

  // tempo from a least-squares line through the beat times
  const size_t nb = times.size();
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t i = 0; i < nb; ++i) {
    const double x = static_cast<double>(i);
    sx += x, sy += times[i], sxx += x * x, sxy += x * times[i];
  }
  const double slope = (nb * sxy - sx * sy) / (nb * sxx - sx * sx);
  const double icept = (sy - slope * sx) / nb;
  double res2 = 0.0;
  for (size_t i = 0; i < nb; ++i) {
    const double d = times[i] - (icept + slope * static_cast<double>(i));
    res2 += d * d;
  }
  const double rstd = std::sqrt(res2 / nb);
  r.bpm = slope > 0 ? 60.0 / slope : 60.0 * fps / period;
  r.tempoConfidence = static_cast<float>(std::clamp(1.0 - 4.0 * rstd / std::max(1e-6, slope), 0.0, 1.0));

  // downbeat phase
  const int bpb = std::max(1, opt.beatsPerBar);
  int phase = 0;
  double bestAcc = -1.0;
  for (int m = 0; m < bpb; ++m) {
    double acc = 0.0;
    int count = 0;
    for (size_t i = static_cast<size_t>(m); i < nb; i += static_cast<size_t>(bpb)) acc += strength[i], ++count;
    if (count) acc /= count;
    if (acc > bestAcc + 1e-9) bestAcc = acc, phase = m;
  }
  for (size_t i = 0; i < nb; ++i) {
    const int idx = static_cast<int>(((static_cast<int>(i) - phase) % bpb + bpb) % bpb) + 1;
    r.beats.push_back({times[i], idx, strength[i]});
  }
  return r;
}

}  // namespace sq::beat
