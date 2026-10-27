// Squizzy Studio - header-only helpers for Standard MIDI File meta events, for consumers of the
// IMidiFileReader / IMidiFileWriter interfaces (sq/interfaces/midi.h). No link dependency on the
// codec-midifile service: include this header and talk to the service through the interfaces.
//
//   uint8_t buf[3];
//   sq_midi_file_event e = sq::midifile::makeTempo(0, sq::midifile::bpmToMicros(120.0), buf);
//   writer->addEvent(&e);
//
//   sq::midifile::TempoMap map(480);            // ticks per quarter
//   map.add(0, 500000); map.add(1920, 400000);  // from Set Tempo metas (tick, us per quarter)
//   double sec = map.tickToSeconds(2400);
//
// Allocates (TempoMap); not for the audio thread.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sq/interfaces/midi.h"

namespace sq::midifile {

// Meta event types (SMF 1.0).
enum MetaType : uint8_t {
  kMetaSequenceNumber = 0x00,
  kMetaText = 0x01,
  kMetaCopyright = 0x02,
  kMetaTrackName = 0x03,
  kMetaInstrumentName = 0x04,
  kMetaLyric = 0x05,
  kMetaMarker = 0x06,
  kMetaCuePoint = 0x07,
  kMetaChannelPrefix = 0x20,
  kMetaPortPrefix = 0x21,
  kMetaEndOfTrack = 0x2F,
  kMetaSetTempo = 0x51,
  kMetaSmpteOffset = 0x54,
  kMetaTimeSignature = 0x58,
  kMetaKeySignature = 0x59,
  kMetaSequencerSpecific = 0x7F,
};

inline uint32_t bpmToMicros(double bpm) {
  if (!(bpm > 0.0)) return 500000u;
  double us = std::round(60000000.0 / bpm);
  if (us < 1.0) us = 1.0;
  if (us > 16777215.0) us = 16777215.0;
  return static_cast<uint32_t>(us);
}
inline double microsToBpm(uint32_t usPerQuarter) { return usPerQuarter ? 60000000.0 / usPerQuarter : 120.0; }

inline sq_midi_file_event makeMeta(uint64_t tick, uint8_t type, const uint8_t* data, uint32_t size) {
  sq_midi_file_event e{};
  e.struct_size = sizeof e;
  e.tick = tick;
  e.status = SQ_MIDI_STATUS_META;
  e.meta_type = type;
  e.data = data;
  e.size = size;
  return e;
}

// storage must outlive the addEvent() call.
inline sq_midi_file_event makeTempo(uint64_t tick, uint32_t usPerQuarter, uint8_t (&storage)[3]) {
  storage[0] = uint8_t(usPerQuarter >> 16);
  storage[1] = uint8_t(usPerQuarter >> 8);
  storage[2] = uint8_t(usPerQuarter);
  return makeMeta(tick, kMetaSetTempo, storage, 3);
}

// denominator is the musical value (2, 4, 8, ...), stored as a power of two.
inline sq_midi_file_event makeTimeSignature(uint64_t tick, uint8_t numerator, uint32_t denominator, uint8_t (&storage)[4],
                                            uint8_t clocksPerClick = 24, uint8_t thirtySecondsPerQuarter = 8) {
  uint8_t pow2 = 0;
  while ((1u << pow2) < denominator && pow2 < 7) ++pow2;
  storage[0] = numerator;
  storage[1] = pow2;
  storage[2] = clocksPerClick;
  storage[3] = thirtySecondsPerQuarter;
  return makeMeta(tick, kMetaTimeSignature, storage, 4);
}

// sharpsFlats: -7..7 (negative = flats); minor: false = major.
inline sq_midi_file_event makeKeySignature(uint64_t tick, int8_t sharpsFlats, bool minor, uint8_t (&storage)[2]) {
  storage[0] = static_cast<uint8_t>(sharpsFlats);
  storage[1] = minor ? 1 : 0;
  return makeMeta(tick, kMetaKeySignature, storage, 2);
}

inline bool isMeta(const sq_midi_file_event& e, uint8_t type) { return e.status == SQ_MIDI_STATUS_META && e.meta_type == type; }

inline bool parseTempo(const sq_midi_file_event& e, uint32_t* usPerQuarter) {
  if (!isMeta(e, kMetaSetTempo) || e.size < 3 || !e.data) return false;
  uint32_t v = (uint32_t(e.data[0]) << 16) | (uint32_t(e.data[1]) << 8) | e.data[2];
  if (v == 0) return false;
  if (usPerQuarter) *usPerQuarter = v;
  return true;
}

inline bool parseTimeSignature(const sq_midi_file_event& e, uint32_t* numerator, uint32_t* denominator) {
  if (!isMeta(e, kMetaTimeSignature) || e.size < 2 || !e.data || e.data[1] > 31) return false;
  if (numerator) *numerator = e.data[0];
  if (denominator) *denominator = 1u << e.data[1];
  return true;
}

inline bool parseKeySignature(const sq_midi_file_event& e, int32_t* sharpsFlats, bool* minor) {
  if (!isMeta(e, kMetaKeySignature) || e.size < 2 || !e.data) return false;
  if (sharpsFlats) *sharpsFlats = static_cast<int8_t>(e.data[0]);
  if (minor) *minor = e.data[1] != 0;
  return true;
}

// Tick <-> seconds for PPQ files. Tempo changes are piecewise constant; the default before the
// first change is 120 BPM (500000 us per quarter) as the SMF spec prescribes.
class TempoMap {
 public:
  explicit TempoMap(uint32_t ticksPerQuarter) : tpq_(ticksPerQuarter ? ticksPerQuarter : 480) {}

  void add(uint64_t tick, uint32_t usPerQuarter) {
    if (!usPerQuarter) return;
    auto it = std::lower_bound(points_.begin(), points_.end(), tick, [](const Point& p, uint64_t t) { return p.tick < t; });
    if (it != points_.end() && it->tick == tick) {
      it->us = usPerQuarter;  // last one at a tick wins
    } else {
      points_.insert(it, Point{tick, usPerQuarter, 0.0});
    }
    rebuild();
  }

  double tickToSeconds(uint64_t tick) const {
    const Point* seg = segmentForTick(tick);
    const uint64_t t0 = seg ? seg->tick : 0;
    const double s0 = seg ? seg->sec : 0.0;
    const uint32_t us = seg ? seg->us : 500000u;
    return s0 + double(tick - t0) * us / (1e6 * tpq_);
  }

  double secondsToTick(double seconds) const {
    if (seconds <= 0.0) return 0.0;
    uint64_t t0 = 0;
    double s0 = 0.0;
    uint32_t us = 500000u;
    for (const Point& p : points_) {
      if (p.sec > seconds) break;
      t0 = p.tick;
      s0 = p.sec;
      us = p.us;
    }
    return double(t0) + (seconds - s0) * 1e6 * tpq_ / us;
  }

  uint32_t ticksPerQuarter() const { return tpq_; }
  size_t size() const { return points_.size(); }

 private:
  struct Point {
    uint64_t tick;
    uint32_t us;
    double sec;  // absolute seconds at `tick`
  };

  const Point* segmentForTick(uint64_t tick) const {
    const Point* best = nullptr;
    for (const Point& p : points_) {
      if (p.tick > tick) break;
      best = &p;
    }
    return best;
  }

  void rebuild() {
    uint64_t t = 0;
    double s = 0.0;
    uint32_t us = 500000u;
    for (Point& p : points_) {
      s += double(p.tick - t) * us / (1e6 * tpq_);
      p.sec = s;
      t = p.tick;
      us = p.us;
    }
  }

  uint32_t tpq_;
  std::vector<Point> points_;
};

}  // namespace sq::midifile
