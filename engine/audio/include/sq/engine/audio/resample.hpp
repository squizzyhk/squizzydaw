// Squizzy Studio engine/audio - offline sample-rate conversion of decoded media (control / worker
// thread only; allocates).
//
// Band-limited interpolation with a Kaiser-windowed sinc (beta 10.06, 133 taps of the lower rate;
// measured stopband attenuation > 100 dB, passband deviation < 0.02 dB): passband to 0.45 x the
// lower of the two rates (19.8 kHz at 44.1 kHz), stopband from 0.5 x the lower rate, so a
// conversion in either direction neither images nor aliases audibly. Rational ratios of integer
// rates (44.1k <-> 48k, 22.05k -> 48k, ...) use an exact polyphase table (one weight set per
// phase, computed once per conversion); any other ratio evaluates the windowed sinc per output
// frame from a finely sampled table. Frame t
// of the output is source time t / outRate, i.e. timing and pitch are preserved and a source of
// N frames becomes round(N * outRate / inRate) frames.
#pragma once

#include <cstdint>
#include <vector>

#include "sq/engine/audio/nodes.hpp"

namespace sq::engine::audio {

// Resamples one channel. in: inFrames samples at inRate; returns round(inFrames * outRate / inRate)
// samples at outRate. Identity copy when the rates are equal (or either is not positive).
std::vector<float> resampleChannel(const float* in, int64_t inFrames, double inRate, double outRate);

// Resamples every channel of a media buffer to outRate (same path / channel count). Returns `in`
// itself when the rates already match.
MediaPtr resampleMedia(const MediaPtr& in, double outRate);

}  // namespace sq::engine::audio
