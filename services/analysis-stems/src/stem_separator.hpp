// analysis-stems: model-agnostic stem separation driver over INNModel (own implementation).
//
// Model convention ("sq stem model v1"): a model file plus a sidecar `<model>.json`:
//   { "format": "sq-stems-1", "sample_rate": 44100, "channels": 2,
//     "stems": ["vocals", "drums", "bass", "other"], "chunk_frames": 262144, "overlap_frames": 8192 }
// Input 0: float [1, channels, chunk_frames]; output 0: float [1, stems, channels, chunk_frames].
// Long inputs are cut into overlapping chunks and recombined with linear cross-fades.
#pragma once

#include <string>
#include <vector>

#include "sq/cpp/ptr.hpp"
#include "sq/interfaces/dsp.h"

namespace sq::stems {

// Sidecar sanity limits (a chunk of 2^22 frames is ~95 s at 44.1 kHz; the output tensor of one
// chunk may hold at most 2^28 floats = 1 GB).
inline constexpr uint32_t kMaxChunkFrames = 1u << 22;
inline constexpr uint64_t kMaxOutputElements = 1ull << 28;

struct ModelSpec {
  double sampleRate = 44100.0;
  uint32_t channels = 2;
  std::vector<std::string> stems;
  uint32_t chunkFrames = 0;
  uint32_t overlapFrames = 0;
};

// Parses the sidecar JSON text. Returns false with a reason on error.
bool parseModelSpec(const std::string& json, ModelSpec& out, std::string& err);

struct Separated {
  std::vector<std::string> names;
  std::vector<std::vector<std::vector<float>>> audio;  // [stem][channel][frame]
};

// Runs `model` over `input` ([channel][frame], any channel count) and returns stems with the input's
// channel count. `wantStems` > 0 and < model stems: keeps the first wantStems-1 stems and sums the
// rest into "accompaniment" (2) / "other" (> 2). Returns an sq_result.
sq_result separate(sq::INNModel* model, const ModelSpec& spec, const std::vector<std::vector<float>>& input,
                   uint32_t wantStems, Separated& out);

}  // namespace sq::stems
