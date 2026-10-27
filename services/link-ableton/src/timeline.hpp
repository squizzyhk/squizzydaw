// link-ableton: own local tempo/beat timeline (portable, no SDK) and its wait-free publisher.
//
// Timeline model: the session beat is a straight line in host time,
//   beat(t) = beatOrigin + (t - timeOrigin) * bpm / 60e9,
// re-anchored at every tempo change so the beat stays continuous. The phase is the beat modulo
// the quantum (always in [0, quantum)). Play state carries the host time it changed at.
//
// StatePublisher: one writer (serialised by the caller), any number of RT readers. The writer
// fills the next of kSlots slots and publishes its index; a slot is a sequence counter plus
// atomic 64-bit words, so a reader copies it with relaxed atomic loads and validates the
// sequence (seqlock per slot). A reader only retries when the writer lapped all slots during one
// copy; retries are bounded (kMaxRetries) so read() never blocks: it then returns false.
#pragma once

#include <atomic>
#include <cstdint>

#include "sq/interfaces/remote.h"

namespace sq::tempo {

struct Timeline {
  double bpm = 120.0;
  double beatOrigin = 0.0;
  uint64_t timeOrigin = 0;  // host ns
  double quantum = 4.0;
  bool playing = false;
  uint64_t playTime = 0;    // host ns of the last play-state change
};

double clampTempo(double bpm);
double clampQuantum(double beats);  // [1, 64] beats; NaN -> 4
// All noexcept and allocation-free (RT-safe).
double beatAt(const Timeline& tl, uint64_t hostNs) noexcept;
double phaseOf(double beat, double quantum) noexcept;
// Host time at which the timeline reaches `beat` (inverse of beatAt).
uint64_t timeAtBeat(const Timeline& tl, double beat) noexcept;
Timeline withTempo(const Timeline& tl, double bpm, uint64_t hostNs) noexcept;
Timeline withPlaying(const Timeline& tl, bool playing, uint64_t hostNs) noexcept;
void project(const Timeline& tl, uint64_t hostNs, uint32_t peers, sq_tempo_sync_state& out) noexcept;

class StatePublisher {
 public:
  static constexpr int kSlots = 4;
  static constexpr int kMaxRetries = 16;

  explicit StatePublisher(const Timeline& initial = Timeline{}) noexcept { write(initial); }
  void write(const Timeline& tl) noexcept;  // single writer (caller serialises)
  bool read(Timeline& out) const noexcept;  // RT-safe, bounded

 private:
  static constexpr int kWords = 6;
  struct Slot {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint64_t> w[kWords];
  };
  Slot slots_[kSlots];
  std::atomic<uint32_t> current_{0};
  uint32_t next_ = 0;  // writer-only
};

}  // namespace sq::tempo
