// midi-alsa: the ALSA sequencer event record (snd_seq_event_t, stable kernel/libasound ABI),
// declared here because the build has no ALSA development headers, plus the conversion between
// sequencer events and raw MIDI bytes. Header-only and realtime safe (no allocation, no locks).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sq::alsaseq {

struct SeqAddr {
  uint8_t client;
  uint8_t port;
};

struct SeqNote {
  uint8_t channel, note, velocity, off_velocity;
  uint32_t duration;
};
struct SeqCtrl {
  uint8_t channel;
  uint8_t unused[3];
  uint32_t param;
  int32_t value;
};
#pragma pack(push, 1)
struct SeqExt {  // variable-length payload (sysex): packed in the C ABI
  uint32_t len;
  void* ptr;
};
#pragma pack(pop)
struct SeqConnect {
  SeqAddr sender, dest;
};

struct SeqEvent {
  uint8_t type;
  uint8_t flags;
  uint8_t tag;
  uint8_t queue;
  uint32_t time[2];  // tick or {sec, nsec}
  SeqAddr source;
  SeqAddr dest;
  union {
    SeqNote note;
    SeqCtrl control;
    uint8_t raw8[12];
    uint32_t raw32[3];
    SeqExt ext;
    SeqAddr addr;
    SeqConnect connect;
  } data;
};
static_assert(sizeof(SeqExt) == 4 + sizeof(void*), "packed ext");
static_assert(sizeof(SeqEvent) == 28, "snd_seq_event_t layout");
static_assert(offsetof(SeqEvent, data) == 16, "snd_seq_event_t layout");

// snd_seq_event_type_t values
enum : uint8_t {
  kEvNoteOn = 6,
  kEvNoteOff = 7,
  kEvKeyPress = 8,
  kEvController = 10,
  kEvPgmChange = 11,
  kEvChanPress = 12,
  kEvPitchBend = 13,
  kEvControl14 = 14,
  kEvNonRegParam = 15,
  kEvRegParam = 16,
  kEvSongPos = 20,
  kEvSongSel = 21,
  kEvQFrame = 22,
  kEvStart = 30,
  kEvContinue = 31,
  kEvStop = 32,
  kEvClock = 36,
  kEvTuneRequest = 40,
  kEvReset = 41,
  kEvSensing = 42,
  kEvClientStart = 60,
  kEvClientExit = 61,
  kEvClientChange = 62,
  kEvPortStart = 63,
  kEvPortExit = 64,
  kEvPortChange = 65,
  kEvSysex = 130,
};
constexpr uint8_t kFlagLengthVariable = 1u << 2;
constexpr uint8_t kQueueDirect = 253;

inline bool isAnnounce(uint8_t type) { return type >= kEvClientStart && type <= kEvPortChange; }

// A decoded short message.
struct ShortMsg {
  uint8_t bytes[3];
  uint8_t size;
};

// Sequencer event -> up to 4 short MIDI messages (NRPN/RPN expand to 4 controller messages).
// Returns the count; 0 for sysex (see SysexAssembler), announcements and unknown types.
inline uint32_t decodeShort(const SeqEvent& ev, ShortMsg out[4]) noexcept {
  auto put = [&](uint32_t i, uint8_t s, uint8_t d1, uint8_t d2, uint8_t n) {
    out[i].bytes[0] = s;
    out[i].bytes[1] = d1;
    out[i].bytes[2] = d2;
    out[i].size = n;
  };
  const uint8_t nch = ev.data.note.channel & 0x0F;
  const uint8_t cch = ev.data.control.channel & 0x0F;
  const int32_t v = ev.data.control.value;
  const uint32_t p = ev.data.control.param;
  switch (ev.type) {
    case kEvNoteOn: put(0, uint8_t(0x90 | nch), ev.data.note.note & 0x7F, ev.data.note.velocity & 0x7F, 3); return 1;
    case kEvNoteOff: put(0, uint8_t(0x80 | nch), ev.data.note.note & 0x7F, ev.data.note.velocity & 0x7F, 3); return 1;
    case kEvKeyPress: put(0, uint8_t(0xA0 | nch), ev.data.note.note & 0x7F, ev.data.note.velocity & 0x7F, 3); return 1;
    case kEvController: put(0, uint8_t(0xB0 | cch), uint8_t(p & 0x7F), uint8_t(v & 0x7F), 3); return 1;
    case kEvPgmChange: put(0, uint8_t(0xC0 | cch), uint8_t(v & 0x7F), 0, 2); return 1;
    case kEvChanPress: put(0, uint8_t(0xD0 | cch), uint8_t(v & 0x7F), 0, 2); return 1;
    case kEvPitchBend: {
      int32_t b = v + 8192;  // ALSA: -8192..8191
      b = b < 0 ? 0 : (b > 16383 ? 16383 : b);
      put(0, uint8_t(0xE0 | cch), uint8_t(b & 0x7F), uint8_t((b >> 7) & 0x7F), 3);
      return 1;
    }
    case kEvControl14:
      if (p < 32) {
        put(0, uint8_t(0xB0 | cch), uint8_t(p), uint8_t((v >> 7) & 0x7F), 3);
        put(1, uint8_t(0xB0 | cch), uint8_t(p + 32), uint8_t(v & 0x7F), 3);
        return 2;
      }
      put(0, uint8_t(0xB0 | cch), uint8_t(p & 0x7F), uint8_t(v & 0x7F), 3);
      return 1;
    case kEvNonRegParam:
    case kEvRegParam: {
      const bool nrpn = ev.type == kEvNonRegParam;
      const uint8_t s = uint8_t(0xB0 | cch);
      put(0, s, nrpn ? 99 : 101, uint8_t((p >> 7) & 0x7F), 3);
      put(1, s, nrpn ? 98 : 100, uint8_t(p & 0x7F), 3);
      put(2, s, 6, uint8_t((v >> 7) & 0x7F), 3);
      put(3, s, 38, uint8_t(v & 0x7F), 3);
      return 4;
    }
    case kEvSongPos: put(0, 0xF2, uint8_t(v & 0x7F), uint8_t((v >> 7) & 0x7F), 3); return 1;
    case kEvSongSel: put(0, 0xF3, uint8_t(v & 0x7F), 0, 2); return 1;
    case kEvQFrame: put(0, 0xF1, uint8_t(v & 0x7F), 0, 2); return 1;
    case kEvStart: put(0, 0xFA, 0, 0, 1); return 1;
    case kEvContinue: put(0, 0xFB, 0, 0, 1); return 1;
    case kEvStop: put(0, 0xFC, 0, 0, 1); return 1;
    case kEvClock: put(0, 0xF8, 0, 0, 1); return 1;
    case kEvTuneRequest: put(0, 0xF6, 0, 0, 1); return 1;
    case kEvReset: put(0, 0xFF, 0, 0, 1); return 1;
    case kEvSensing: put(0, 0xFE, 0, 0, 1); return 1;
    default: return 0;
  }
}

// Length of a short message from its status byte (0 for sysex / data bytes).
inline uint32_t shortLength(uint8_t status) noexcept {
  if (status < 0x80) return 0;
  if (status < 0xF0) return (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
  switch (status) {
    case 0xF1:
    case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF0:
    case 0xF7: return 0;
    default: return 1;
  }
}

// Short MIDI message -> sequencer event (type + data; addressing is left to the caller).
// false for messages the sequencer has no event for (undefined system bytes, sysex).
inline bool encodeShort(const uint8_t* b, uint32_t size, SeqEvent& ev) noexcept {
  std::memset(&ev, 0, sizeof ev);
  if (size == 0 || size < shortLength(b[0]) || shortLength(b[0]) == 0) return false;
  const uint8_t s = b[0], hi = s & 0xF0, ch = s & 0x0F;
  const uint8_t d1 = size > 1 ? b[1] & 0x7F : 0, d2 = size > 2 ? b[2] & 0x7F : 0;
  switch (hi) {
    case 0x80: ev.type = kEvNoteOff; break;
    case 0x90: ev.type = kEvNoteOn; break;
    case 0xA0: ev.type = kEvKeyPress; break;
    case 0xB0:
      ev.type = kEvController;
      ev.data.control.channel = ch;
      ev.data.control.param = d1;
      ev.data.control.value = d2;
      return true;
    case 0xC0:
    case 0xD0:
      ev.type = hi == 0xC0 ? kEvPgmChange : kEvChanPress;
      ev.data.control.channel = ch;
      ev.data.control.value = d1;
      return true;
    case 0xE0:
      ev.type = kEvPitchBend;
      ev.data.control.channel = ch;
      ev.data.control.value = int32_t(d1 | (d2 << 7)) - 8192;
      return true;
    default: break;
  }
  if (hi >= 0x80 && hi <= 0xA0) {
    ev.data.note.channel = ch;
    ev.data.note.note = d1;
    ev.data.note.velocity = d2;
    return true;
  }
  switch (s) {  // system messages
    case 0xF1: ev.type = kEvQFrame; ev.data.control.value = d1; return true;
    case 0xF2: ev.type = kEvSongPos; ev.data.control.value = int32_t(d1 | (d2 << 7)); return true;
    case 0xF3: ev.type = kEvSongSel; ev.data.control.value = d1; return true;
    case 0xF6: ev.type = kEvTuneRequest; return true;
    case 0xF8: ev.type = kEvClock; return true;
    case 0xFA: ev.type = kEvStart; return true;
    case 0xFB: ev.type = kEvContinue; return true;
    case 0xFC: ev.type = kEvStop; return true;
    case 0xFE: ev.type = kEvSensing; return true;
    case 0xFF: ev.type = kEvReset; return true;
    default: return false;
  }
}

// Turns a variable-length sysex chunk into a sequencer event pointing at `data` (no copy).
inline void encodeSysexChunk(const uint8_t* data, uint32_t size, SeqEvent& ev) noexcept {
  std::memset(&ev, 0, sizeof ev);
  ev.type = kEvSysex;
  ev.flags = kFlagLengthVariable;
  ev.data.ext.len = size;
  ev.data.ext.ptr = const_cast<uint8_t*>(data);
}

// Reassembles sysex that arrives in several SYSEX events (raw MIDI ports deliver 256-byte
// chunks): the first starts with F0, the last ends with F7. Fixed capacity, no allocation.
class SysexAssembler {
 public:
  static constexpr uint32_t kCapacity = 65536;
  // Feeds one chunk. Returns true when a complete F0..F7 message is ready in data()/size()
  // (valid until the next feed). Oversized messages are dropped.
  bool feed(const uint8_t* p, uint32_t n) noexcept {
    if (n == 0 || !p) return false;
    if (p[0] == 0xF0) {  // a new message always restarts
      len_ = 0;
      active_ = true;
      overflow_ = false;
    }
    if (!active_) return false;  // continuation without a start: ignore
    if (!overflow_) {
      if (len_ + n > kCapacity) overflow_ = true;
      else {
        std::memcpy(buf_ + len_, p, n);
        len_ += n;
      }
    }
    if (p[n - 1] == 0xF7) {
      active_ = false;
      if (overflow_) {
        overflow_ = false;
        ++dropped_;
        return false;
      }
      return true;
    }
    return false;
  }
  const uint8_t* data() const noexcept { return buf_; }
  uint32_t size() const noexcept { return len_; }
  uint32_t dropped() const noexcept { return dropped_; }

 private:
  uint8_t buf_[kCapacity];
  uint32_t len_ = 0;
  uint32_t dropped_ = 0;
  bool active_ = false;
  bool overflow_ = false;
};

}  // namespace sq::alsaseq
