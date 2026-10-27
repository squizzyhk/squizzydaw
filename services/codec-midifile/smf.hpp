// Standard MIDI File (SMF type 0/1, type 2 read as 1) core: parser + writer.
// Internal to the codec-midifile service; the module exposes it through midi.h.
// Not thread-safe; allocates (never use on the audio thread).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sq/types.h"

namespace sq::smf {

struct Event {
  uint64_t tick = 0;
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  uint8_t metaType = 0;
  uint32_t size = 0;    // payload bytes (meta / sysex)
  uint32_t offset = 0;  // payload offset into File::payload
};

struct Track {
  std::vector<Event> events;  // always ends with an End-of-Track meta (synthesized if missing)
  std::string name;           // first Track Name meta (0x03), "" if none
};

struct File {
  uint32_t format = 0;     // as stored in the header (0, 1 or 2)
  uint32_t declaredTracks = 0;
  uint16_t division = 0;   // raw header division word
  std::vector<Track> tracks;
  std::vector<uint8_t> payload;
};

// Number of data bytes following a channel status (0x80..0xEF).
inline uint32_t channelDataBytes(uint8_t status) {
  const uint8_t hi = status & 0xF0u;
  return (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
}

// Parses a whole SMF image (optionally wrapped in a RIFF "RMID" container).
// Lenient where real-world files are sloppy: missing End-of-Track, truncated last track, running
// status continued across meta/sysex events, unknown chunks. Hard errors (SQ_E_PARSE): not an SMF,
// format > 2, a data byte with no running status, an invalid status byte (0xF1..0xFE).
sq_result parse(const uint8_t* data, size_t size, File& out, std::string* err = nullptr);

// Appends a MIDI variable-length quantity (value must be <= 0x0FFFFFFF).
void writeVlq(std::vector<uint8_t>& out, uint32_t value);

// Builds an SMF in memory. Channel messages use running status; meta/sysex reset it.
class Writer {
 public:
  // format 0 or 1. division: ticks per quarter (1..0x7FFF), or a raw SMPTE division word
  // (high byte -24/-25/-29/-30 as two's complement, low byte ticks per frame).
  sq_result begin(uint32_t format, uint32_t division);
  sq_result beginTrack();
  // Meta 0x2F (End-of-Track) is not written directly: it sets the track end tick and closes the
  // track for further events; endTrack() writes it.
  sq_result addEvent(uint64_t tick, uint8_t status, uint8_t data1, uint8_t data2, uint8_t metaType,
                     const uint8_t* data, uint32_t size);
  sq_result endTrack();
  // Closes an open track, then produces the complete file.
  sq_result finish(std::vector<uint8_t>& out);

  bool started() const { return started_; }
  bool inTrack() const { return inTrack_; }

 private:
  bool started_ = false;
  bool inTrack_ = false;
  bool trackEnded_ = false;
  uint32_t format_ = 1;
  uint16_t division_ = 480;
  uint32_t trackCount_ = 0;
  uint64_t lastTick_ = 0;
  uint64_t endTick_ = 0;
  uint8_t running_ = 0;
  std::vector<uint8_t> track_;
  std::vector<uint8_t> chunks_;
};

}  // namespace sq::smf
