#include "timeline.hpp"

#include <cmath>
#include <cstring>

#include "sq/services/link_ableton/link_ableton.h"

namespace sq::tempo {
namespace {

uint64_t bits(double d) noexcept {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}
double real(uint64_t u) noexcept {
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

// Signed difference a - b of two host times, in seconds.
double secondsBetween(uint64_t a, uint64_t b) noexcept {
  return a >= b ? static_cast<double>(a - b) * 1e-9 : -static_cast<double>(b - a) * 1e-9;
}

}  // namespace

double clampTempo(double bpm) {
  if (!(bpm == bpm)) return 120.0;  // NaN
  if (bpm < tempo_sync::kMinTempo) return tempo_sync::kMinTempo;
  if (bpm > tempo_sync::kMaxTempo) return tempo_sync::kMaxTempo;
  return bpm;
}

double clampQuantum(double beats) {
  if (!(beats == beats)) return 4.0;
  if (beats < 1.0) return 1.0;
  if (beats > 64.0) return 64.0;
  return beats;
}

double beatAt(const Timeline& tl, uint64_t hostNs) noexcept {
  return tl.beatOrigin + secondsBetween(hostNs, tl.timeOrigin) * tl.bpm / 60.0;
}

double phaseOf(double beat, double quantum) noexcept {
  if (quantum <= 0.0) return 0.0;
  double p = std::fmod(beat, quantum);
  if (p < 0.0) p += quantum;
  if (p >= quantum) p = 0.0;  // fmod rounding at the boundary
  return p;
}

uint64_t timeAtBeat(const Timeline& tl, double beat) noexcept {
  double secs = (beat - tl.beatOrigin) * 60.0 / tl.bpm;
  double ns = secs * 1e9;
  if (ns >= 0) return tl.timeOrigin + static_cast<uint64_t>(ns + 0.5);
  uint64_t back = static_cast<uint64_t>(-ns + 0.5);
  return back > tl.timeOrigin ? 0 : tl.timeOrigin - back;
}

Timeline withTempo(const Timeline& tl, double bpm, uint64_t hostNs) noexcept {
  Timeline n = tl;
  n.beatOrigin = beatAt(tl, hostNs);
  n.timeOrigin = hostNs;
  n.bpm = clampTempo(bpm);
  return n;
}

Timeline withPlaying(const Timeline& tl, bool playing, uint64_t hostNs) noexcept {
  Timeline n = tl;
  if (n.playing != playing) {
    n.playing = playing;
    n.playTime = hostNs;
  }
  return n;
}

void project(const Timeline& tl, uint64_t hostNs, uint32_t peers, sq_tempo_sync_state& out) noexcept {
  out.struct_size = sizeof(sq_tempo_sync_state);
  out.bpm = tl.bpm;
  out.beat = beatAt(tl, hostNs);
  out.quantum = tl.quantum;
  out.phase = phaseOf(out.beat, tl.quantum);
  out.playing = tl.playing ? SQ_TRUE : SQ_FALSE;
  out.peers = peers;
  out.host_time_ns = hostNs;
}

void StatePublisher::write(const Timeline& tl) noexcept {
  uint32_t idx = next_;
  next_ = (next_ + 1) % kSlots;
  Slot& s = slots_[idx];
  uint32_t seq = s.seq.load(std::memory_order_relaxed);
  s.seq.store(seq + 1, std::memory_order_relaxed);  // odd: being written
  std::atomic_thread_fence(std::memory_order_release);
  s.w[0].store(bits(tl.bpm), std::memory_order_relaxed);
  s.w[1].store(bits(tl.beatOrigin), std::memory_order_relaxed);
  s.w[2].store(tl.timeOrigin, std::memory_order_relaxed);
  s.w[3].store(bits(tl.quantum), std::memory_order_relaxed);
  s.w[4].store(tl.playing ? 1u : 0u, std::memory_order_relaxed);
  s.w[5].store(tl.playTime, std::memory_order_relaxed);
  s.seq.store(seq + 2, std::memory_order_release);  // even: stable
  current_.store(idx, std::memory_order_release);
}

bool StatePublisher::read(Timeline& out) const noexcept {
  for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
    const Slot& s = slots_[current_.load(std::memory_order_acquire)];
    uint32_t a = s.seq.load(std::memory_order_acquire);
    if (a & 1u) continue;
    Timeline t;
    t.bpm = real(s.w[0].load(std::memory_order_relaxed));
    t.beatOrigin = real(s.w[1].load(std::memory_order_relaxed));
    t.timeOrigin = s.w[2].load(std::memory_order_relaxed);
    t.quantum = real(s.w[3].load(std::memory_order_relaxed));
    t.playing = s.w[4].load(std::memory_order_relaxed) != 0;
    t.playTime = s.w[5].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) == a) {
      out = t;
      return true;
    }
  }
  return false;
}

}  // namespace sq::tempo
