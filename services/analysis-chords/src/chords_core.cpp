// analysis-chords core (see chords_core.hpp).
#include "chords_core.hpp"

#include <algorithm>
#include <cmath>

#include "sq/engine/simd/fft.hpp"

namespace sq::chords {

namespace {

using sq::theory::ChordQuality;

constexpr ChordQuality kDetect[] = {ChordQuality::Major,     ChordQuality::Minor,  ChordQuality::Diminished,
                                    ChordQuality::Augmented, ChordQuality::Sus4,   ChordQuality::Dominant7,
                                    ChordQuality::Major7,    ChordQuality::Minor7};
constexpr int kQ = static_cast<int>(sizeof kDetect / sizeof kDetect[0]);
constexpr int kStates = 12 * kQ + 1;  // last = no chord

// Expected chroma of a chord made of harmonic tones: partial h of pitch class p lands on
// p + round(12 log2 h); amplitudes fall off geometrically.
void buildTemplate(int root, ChordQuality q, float out[12]) {
  for (int i = 0; i < 12; ++i) out[i] = 0.0f;
  const uint16_t mask = sq::theory::chordMask(root, q);
  static const int kPartialOffset[4] = {0, 12, 19, 24};
  for (int pc = 0; pc < 12; ++pc) {
    if (!((mask >> pc) & 1)) continue;
    float a = 1.0f;
    for (int h = 0; h < 4; ++h, a *= 0.6f) out[(pc + kPartialOffset[h]) % 12] += a;
  }
  float n = 0.0f;
  for (int i = 0; i < 12; ++i) n += out[i] * out[i];
  n = std::sqrt(n);
  for (int i = 0; i < 12; ++i) out[i] /= n;
}

// Scale-degree weights used as key profiles (own choice: tonic > fifth > third > other diatonic
// degrees > chromatic notes).
constexpr float kMajorProfile[12] = {6.0f, 1.0f, 3.0f, 1.0f, 4.5f, 3.5f, 1.0f, 5.0f, 1.0f, 3.0f, 1.0f, 2.5f};
constexpr float kMinorProfile[12] = {6.0f, 1.0f, 3.0f, 4.5f, 1.0f, 3.5f, 1.0f, 5.0f, 3.0f, 1.0f, 2.5f, 2.0f};

double pearson(const float* a, const float* b, int n) {
  double ma = 0, mb = 0;
  for (int i = 0; i < n; ++i) ma += a[i], mb += b[i];
  ma /= n, mb /= n;
  double num = 0, da = 0, db = 0;
  for (int i = 0; i < n; ++i) {
    num += (a[i] - ma) * (b[i] - mb);
    da += (a[i] - ma) * (a[i] - ma);
    db += (b[i] - mb) * (b[i] - mb);
  }
  return (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.0;
}

}  // namespace

KeyEstimate estimateKey(const float chroma[12]) {
  KeyEstimate best;
  double bestR = -2.0, second = -2.0;
  for (int mode = 0; mode < 2; ++mode)
    for (int tonic = 0; tonic < 12; ++tonic) {
      float rotated[12];
      for (int i = 0; i < 12; ++i) rotated[i] = chroma[(tonic + i) % 12];
      const double r = pearson(rotated, mode ? kMinorProfile : kMajorProfile, 12);
      if (r > bestR) {
        second = bestR;
        bestR = r;
        best.tonic = tonic;
        best.minor = mode == 1;
      } else if (r > second) {
        second = r;
      }
    }
  best.confidence = static_cast<float>(std::clamp((bestR - second) * 4.0 + std::max(0.0, bestR) * 0.5, 0.0, 1.0));
  return best;
}

Result analyze(const std::vector<float>& mono, double sampleRate, double referenceHz) {
  Result res;
  if (!(sampleRate >= 4000.0 && sampleRate <= 1.0e6) || mono.empty()) return res;
  if (!(referenceHz > 300.0 && referenceHz < 600.0)) referenceHz = 440.0;
  uint32_t n = 1024;
  while (n < 8192.0 * sampleRate / 44100.0 * 0.75) n <<= 1;
  const uint32_t hop = n / 2;
  res.hopSeconds = hop / sampleRate;
  sq::simd::RealFft fft(n);
  std::vector<float> window(n), buf(n), re(n / 2 + 1), im(n / 2 + 1), mag(n / 2 + 1);
  sq::simd::hannWindow(window.data(), n);

  // pitch class + weight per bin
  const uint32_t bins = n / 2 + 1;
  std::vector<int> binPc(bins, -1);
  std::vector<float> binW(bins, 0.0f);
  for (uint32_t k = 1; k < bins; ++k) {
    const double f = k * sampleRate / n;
    if (f < 55.0 || f > 5000.0) continue;
    const double midi = sq::theory::frequencyToMidi(f, referenceHz);
    const double nearest = std::round(midi);
    const double dist = std::fabs(midi - nearest);
    binPc[k] = sq::theory::wrapPc(static_cast<int>(nearest));
    binW[k] = static_cast<float>(std::max(0.0, 1.0 - 2.0 * dist));
  }

  std::vector<float> energy;
  const size_t total = mono.size();
  for (size_t start = 0; start < total; start += hop) {
    for (uint32_t i = 0; i < n; ++i) {
      const size_t idx = start + i;
      buf[i] = idx < total ? mono[idx] * window[i] : 0.0f;
    }
    fft.forward(buf.data(), re.data(), im.data());
    for (uint32_t k = 0; k < bins; ++k) mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
    std::vector<float> c(12, 0.0f);
    for (uint32_t k = 2; k + 1 < bins; ++k) {
      if (binPc[k] < 0 || mag[k] < mag[k - 1] || mag[k] < mag[k + 1]) continue;
      c[static_cast<size_t>(binPc[k])] += mag[k] * binW[k];
    }
    float e = 0.0f;
    for (float v : c) e += v * v;
    e = std::sqrt(e);
    energy.push_back(e);
    if (e > 0.0f)
      for (float& v : c) v /= e;
    res.chroma.push_back(std::move(c));
  }
  const size_t frames = res.chroma.size();
  if (!frames) return res;
  const float maxE = *std::max_element(energy.begin(), energy.end());

  // templates
  std::vector<float> tmpl(static_cast<size_t>(12 * kQ) * 12);
  for (int qi = 0; qi < kQ; ++qi)
    for (int r = 0; r < 12; ++r) buildTemplate(r, kDetect[qi], &tmpl[static_cast<size_t>((qi * 12 + r) * 12)]);

  // Viterbi: maximise sum(score) - penalty * changes
  const double penalty = 0.5;
  std::vector<double> score(kStates), prev(kStates);
  std::vector<int> back(frames * kStates);
  for (size_t t = 0; t < frames; ++t) {
    const bool quiet = energy[t] < 0.02f * maxE || energy[t] <= 0.0f;
    for (int s = 0; s < kStates - 1; ++s) {
      double d = 0.0;
      if (!quiet)
        for (int i = 0; i < 12; ++i) d += res.chroma[t][static_cast<size_t>(i)] * tmpl[static_cast<size_t>(s * 12 + i)];
      // seventh chords must beat their triad clearly
      const ChordQuality q = kDetect[s / 12];
      if (q == ChordQuality::Dominant7 || q == ChordQuality::Major7 || q == ChordQuality::Minor7) d -= 0.02;
      if (q == ChordQuality::Augmented || q == ChordQuality::Diminished || q == ChordQuality::Sus4) d -= 0.01;
      score[static_cast<size_t>(s)] = d;
    }
    score[kStates - 1] = quiet ? 1.0 : 0.3;
    if (t == 0) {
      prev = score;
      continue;
    }
    int bestPrev = 0;
    for (int s = 1; s < kStates; ++s)
      if (prev[static_cast<size_t>(s)] > prev[static_cast<size_t>(bestPrev)]) bestPrev = s;
    std::vector<double> cur(kStates);
    for (int s = 0; s < kStates; ++s) {
      const double stay = prev[static_cast<size_t>(s)];
      const double jump = prev[static_cast<size_t>(bestPrev)] - penalty;
      if (stay >= jump) {
        cur[static_cast<size_t>(s)] = stay + score[static_cast<size_t>(s)];
        back[t * kStates + static_cast<size_t>(s)] = s;
      } else {
        cur[static_cast<size_t>(s)] = jump + score[static_cast<size_t>(s)];
        back[t * kStates + static_cast<size_t>(s)] = bestPrev;
      }
    }
    prev.swap(cur);
  }
  std::vector<int> path(frames);
  int s = 0;
  for (int i = 1; i < kStates; ++i)
    if (prev[static_cast<size_t>(i)] > prev[static_cast<size_t>(s)]) s = i;
  for (size_t t = frames; t-- > 0;) {
    path[t] = s;
    if (t > 0) s = back[t * kStates + static_cast<size_t>(s)];
  }

  // segments (frame t covers its centre region [t*hop, (t+1)*hop) shifted by half a frame)
  const double frameSec = hop / sampleRate;
  const double totalSec = static_cast<double>(total) / sampleRate;
  size_t segStart = 0;
  for (size_t t = 1; t <= frames; ++t) {
    if (t < frames && path[t] == path[segStart]) continue;
    const int st = path[segStart];
    if (st != kStates - 1) {
      ChordSegment seg;
      seg.start = std::max(0.0, segStart * frameSec + 0.5 * n / sampleRate - 0.5 * frameSec);
      if (segStart == 0) seg.start = 0.0;
      double end = t * frameSec + 0.5 * n / sampleRate - 0.5 * frameSec;
      if (t == frames) end = totalSec;
      seg.duration = std::max(0.0, std::min(end, totalSec) - seg.start);
      seg.root = st % 12;
      seg.quality = kDetect[st / 12];
      double conf = 0.0;
      for (size_t k = segStart; k < t; ++k)
        for (int i = 0; i < 12; ++i)
          conf += res.chroma[k][static_cast<size_t>(i)] * tmpl[static_cast<size_t>(st * 12 + i)];
      seg.confidence = static_cast<float>(std::clamp(conf / static_cast<double>(t - segStart), 0.0, 1.0));
      res.chords.push_back(seg);
    }
    segStart = t;
  }

  // key from the energy-weighted mean chroma
  float acc[12] = {};
  for (size_t t = 0; t < frames; ++t)
    for (int i = 0; i < 12; ++i) acc[i] += res.chroma[t][static_cast<size_t>(i)] * energy[t];
  res.key = estimateKey(acc);
  return res;
}

}  // namespace sq::chords
