// analysis-beat core: onset detection, tempo estimation and beat tracking (own implementation).
//
//  - Onset envelope: log-compressed spectral flux (half-wave rectified) of a 1024-point STFT with
//    a 128-sample hop (at 44.1 kHz; scaled with the rate).
//  - Onsets: local maxima above a moving-average + offset threshold, refined in the time domain
//    to the start of the energy rise.
//  - Tempo: autocorrelation of the envelope with harmonic summation and a broad prior around
//    120 BPM; parabolic refinement.
//  - Beats: dynamic programming over the envelope (each beat = onset strength + best predecessor
//    roughly one period earlier, penalised by log-deviation from the period), then a least-squares
//    fit of the beat times gives the final tempo. Downbeat = the bar phase with the strongest
//    onsets (beatsPerBar, default 4).
#pragma once

#include <cstdint>
#include <vector>

namespace sq::beat {

struct Onset {
  double time;
  float strength;  // 0..1 (relative to the strongest onset)
};

struct Beat {
  double time;
  int indexInBar;  // 1 = downbeat
  float strength;
};

struct Result {
  double bpm = 0.0;
  float tempoConfidence = 0.0f;
  std::vector<Onset> onsets;
  std::vector<Beat> beats;
  std::vector<float> envelope;  // onset envelope per hop (for inspection)
  double envelopeRate = 0.0;    // frames per second
};

struct Options {
  double minBpm = 50.0;
  double maxBpm = 220.0;
  int beatsPerBar = 4;
};

Result analyze(const std::vector<float>& mono, double sampleRate, const Options& opt = {});

}  // namespace sq::beat
