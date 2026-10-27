// analysis-chords core: chromagram, chord decoding and key estimation (own implementation).
//
//  1. Chromagram: Hann-windowed STFT (~186 ms frames at 44.1 kHz, half overlap); every spectral
//     peak between 55 Hz and 5 kHz adds its magnitude to its pitch class, weighted by how close it
//     lies to an equal-tempered pitch (reference_hz, default 440).
//  2. Chords: each frame is compared (cosine) with harmonic-aware templates for 8 qualities x 12
//     roots plus a "no chord" state; a Viterbi pass with a constant change penalty smooths the
//     sequence; equal neighbours are merged into segments.
//  3. Key: Pearson correlation of the energy-weighted mean chroma with major/minor key profiles
//     built from scale-degree weights (24 candidates).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sq/theory/theory.hpp"

namespace sq::chords {

struct ChordSegment {
  double start = 0.0, duration = 0.0;
  int root = -1;  // -1 = no chord
  sq::theory::ChordQuality quality = sq::theory::ChordQuality::Major;
  float confidence = 0.0f;
};

struct KeyEstimate {
  int tonic = 0;
  bool minor = false;
  float confidence = 0.0f;
};

struct Result {
  std::vector<ChordSegment> chords;
  KeyEstimate key;
  std::vector<std::vector<float>> chroma;  // per frame, 12 values (L2-normalised), for inspection
  double hopSeconds = 0.0;
};

Result analyze(const std::vector<float>& mono, double sampleRate, double referenceHz = 440.0);

// Key from a 12-bin (unnormalised) pitch-class profile.
KeyEstimate estimateKey(const float chroma[12]);

}  // namespace sq::chords
