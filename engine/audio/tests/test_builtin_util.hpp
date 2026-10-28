// engine/audio - helpers shared by the built-in device tests.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "sq/engine/audio/builtin.hpp"

namespace bt {

using namespace sq::engine::audio;
using Channels = std::vector<std::vector<float>>;

struct Note {
  int64_t frame;
  uint8_t status, d1, d2;
};

inline sq_midi_event midi(uint32_t off, uint8_t st, uint8_t d1, uint8_t d2) {
  sq_midi_event e{};
  e.frame_offset = off;
  e.bytes[0] = st;
  e.bytes[1] = d1;
  e.bytes[2] = d2;
  e.size = 3;
  return e;
}

// Runs a prepared processor over `input` (or silence when empty) in blocks; MIDI events are
// given in absolute frames. Returns the processed channels.
inline Channels run(AudioProcessor& p, uint32_t channels, int64_t frames, uint32_t block, const Channels& input = {},
                    const std::vector<Note>& notes = {}) {
  Channels out(channels, std::vector<float>(static_cast<size_t>(frames), 0.0f));
  if (!input.empty())
    for (uint32_t c = 0; c < channels; ++c) out[c] = input[c % input.size()];
  std::vector<float*> ptr(channels);
  std::vector<sq_midi_event> ev;
  for (int64_t pos = 0; pos < frames; pos += block) {
    const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(block, frames - pos));
    for (uint32_t c = 0; c < channels; ++c) ptr[c] = out[c].data() + pos;
    ev.clear();
    for (const Note& nt : notes)
      if (nt.frame >= pos && nt.frame < pos + n) ev.push_back(midi(static_cast<uint32_t>(nt.frame - pos), nt.status, nt.d1, nt.d2));
    ProcessContext ctx;
    ctx.frames = n;
    ctx.outputFrame = pos;
    ctx.playing = true;
    ctx.midi = ev.empty() ? nullptr : ev.data();
    ctx.midiCount = static_cast<uint32_t>(ev.size());
    p.process(ctx, ptr.data(), channels);
  }
  return out;
}

inline std::unique_ptr<AudioProcessor> instance(const std::string& id, const std::vector<std::pair<uint32_t, double>>& params = {}) {
  std::string err;
  ProcessorProto proto = makeBuiltin(id, params, &err);
  if (!proto) return nullptr;
  return proto->clone();
}

inline double rms(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  if (to > v.size()) to = v.size();
  if (to <= from) return 0.0;
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

inline float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  if (to > v.size()) to = v.size();
  float m = 0.0f;
  for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(v[i]));
  return m;
}

// True if every sample is finite and not subnormal.
inline bool clean(const Channels& ch) {
  for (const auto& c : ch)
    for (float x : c)
      if (!std::isfinite(x) || std::fpclassify(x) == FP_SUBNORMAL) return false;
  return true;
}

inline Channels testSignal(uint32_t channels, int64_t frames, double sr, float amp = 0.5f) {
  Channels in(channels, std::vector<float>(static_cast<size_t>(frames)));
  uint32_t s = 12345u;
  for (uint32_t c = 0; c < channels; ++c)
    for (int64_t i = 0; i < frames; ++i) {
      s = s * 1664525u + 1013904223u;
      const float noise = static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f;
      const float sine = static_cast<float>(std::sin(2.0 * 3.14159265358979 * (220.0 + 110.0 * c) * static_cast<double>(i) / sr));
      in[c][static_cast<size_t>(i)] = amp * (0.5f * sine + 0.5f * noise);
    }
  return in;
}

inline std::vector<Note> chordNotes(int64_t onFrame, int64_t offFrame) {
  std::vector<Note> n;
  for (uint8_t k : std::initializer_list<uint8_t>{36, 38, 42, 48, 55, 60, 64, 67}) n.push_back({onFrame, 0x90, k, 100});
  for (uint8_t k : std::initializer_list<uint8_t>{36, 38, 42, 48, 55, 60, 64, 67}) n.push_back({offFrame, 0x80, k, 0});
  return n;
}

}  // namespace bt
