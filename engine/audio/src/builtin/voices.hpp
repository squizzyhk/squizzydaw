// Squizzy Studio engine/audio - shared polyphonic voice bookkeeping for the built-in instruments.
#pragma once

#include <cstdint>

namespace sq::engine::audio::builtin {

// Common per-voice state the allocator needs. Instruments embed it in their voice struct.
struct VoiceSlot {
  bool active = false;
  bool held = false;
  bool sustained = false;
  uint8_t note = 0;
  uint64_t age = 0;
};

// Picks a voice for `note`: the voice already playing that note, a free one, the quietest
// released one (level() callback), else the oldest. Returns the index.
template <class V, class Level>
int allocateVoice(V* voices, int count, uint8_t note, Level level) noexcept {
  for (int i = 0; i < count; ++i)
    if (voices[i].slot.active && voices[i].slot.note == note) return i;
  for (int i = 0; i < count; ++i)
    if (!voices[i].slot.active) return i;
  int best = -1;
  float bestLevel = 1e9f;
  for (int i = 0; i < count; ++i) {
    const VoiceSlot& s = voices[i].slot;
    if (!s.held && !s.sustained && level(voices[i]) < bestLevel) {
      bestLevel = level(voices[i]);
      best = i;
    }
  }
  if (best >= 0) return best;
  uint64_t oldest = ~0ull;
  for (int i = 0; i < count; ++i)
    if (voices[i].slot.age < oldest) {
      oldest = voices[i].slot.age;
      best = i;
    }
  return best < 0 ? 0 : best;
}

// Sustain-pedal aware note-off: calls release(v) for matching held voices (or marks sustained).
template <class V, class Release>
void noteOffVoices(V* voices, int count, uint8_t note, bool sustain, Release release) noexcept {
  for (int i = 0; i < count; ++i) {
    VoiceSlot& s = voices[i].slot;
    if (!s.active || !s.held || s.note != note) continue;
    s.held = false;
    if (sustain) {
      s.sustained = true;
    } else {
      release(voices[i]);
    }
  }
}

// Monophonic last-note-priority key stack.
struct NoteStack {
  static constexpr int kSize = 16;
  uint8_t notes[kSize] = {};
  int size = 0;
  void push(uint8_t n) noexcept {
    remove(n);
    if (size == kSize) {
      for (int i = 1; i < kSize; ++i) notes[i - 1] = notes[i];
      --size;
    }
    notes[size++] = n;
  }
  void remove(uint8_t n) noexcept {
    int w = 0;
    for (int i = 0; i < size; ++i)
      if (notes[i] != n) notes[w++] = notes[i];
    size = w;
  }
  bool empty() const noexcept { return size == 0; }
  uint8_t top() const noexcept { return notes[size - 1]; }
  void clear() noexcept { size = 0; }
};

}  // namespace sq::engine::audio::builtin
