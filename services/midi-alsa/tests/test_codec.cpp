// midi-alsa: sequencer event <-> raw MIDI byte conversion with synthetic events, the
// snd_seq_event_t layout, and sysex chunk reassembly. Needs no sequencer and no libasound.
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "seq_event.hpp"
#include "sqtest.hpp"

using namespace sq::alsaseq;

namespace {
SeqEvent note(uint8_t type, uint8_t ch, uint8_t n, uint8_t v) {
  SeqEvent e{};
  e.type = type;
  e.data.note.channel = ch;
  e.data.note.note = n;
  e.data.note.velocity = v;
  return e;
}
SeqEvent ctrl(uint8_t type, uint8_t ch, uint32_t param, int32_t value) {
  SeqEvent e{};
  e.type = type;
  e.data.control.channel = ch;
  e.data.control.param = param;
  e.data.control.value = value;
  return e;
}
std::vector<uint8_t> one(const SeqEvent& e) {
  ShortMsg m[4];
  const uint32_t n = decodeShort(e, m);
  if (n != 1) return {};
  return std::vector<uint8_t>(m[0].bytes, m[0].bytes + m[0].size);
}
using B = std::vector<uint8_t>;
}  // namespace

SQ_TEST("midi-alsa codec: snd_seq_event_t layout matches the C ABI") {
  CHECK_EQ(sizeof(SeqEvent), size_t(28));
  CHECK_EQ(offsetof(SeqEvent, source), size_t(12));
  CHECK_EQ(offsetof(SeqEvent, dest), size_t(14));
  CHECK_EQ(offsetof(SeqEvent, data), size_t(16));
  CHECK_EQ(offsetof(SeqCtrl, param), size_t(4));
  CHECK_EQ(offsetof(SeqCtrl, value), size_t(8));
}

SQ_TEST("midi-alsa codec: channel events decode to raw bytes") {
  CHECK(one(note(kEvNoteOn, 2, 60, 100)) == B({0x92, 60, 100}));
  CHECK(one(note(kEvNoteOn, 0, 61, 0)) == B({0x90, 61, 0}));  // velocity 0 stays a note-on
  CHECK(one(note(kEvNoteOff, 15, 60, 64)) == B({0x8F, 60, 64}));
  CHECK(one(note(kEvKeyPress, 1, 70, 33)) == B({0xA1, 70, 33}));
  CHECK(one(ctrl(kEvController, 0, 7, 99)) == B({0xB0, 7, 99}));
  CHECK(one(ctrl(kEvPgmChange, 9, 0, 42)) == B({0xC9, 42}));
  CHECK(one(ctrl(kEvChanPress, 3, 0, 127)) == B({0xD3, 127}));
  CHECK(one(ctrl(kEvPitchBend, 0, 0, 0)) == B({0xE0, 0x00, 0x40}));       // centre
  CHECK(one(ctrl(kEvPitchBend, 0, 0, -8192)) == B({0xE0, 0x00, 0x00}));   // minimum
  CHECK(one(ctrl(kEvPitchBend, 0, 0, 8191)) == B({0xE0, 0x7F, 0x7F}));    // maximum
  CHECK(one(ctrl(kEvPitchBend, 0, 0, 99999)) == B({0xE0, 0x7F, 0x7F}));   // clamped
}

SQ_TEST("midi-alsa codec: system events decode") {
  SeqEvent e{};
  CHECK(one(ctrl(kEvSongPos, 0, 0, 0x1234)) == B({0xF2, 0x34, 0x24}));
  CHECK(one(ctrl(kEvSongSel, 0, 0, 5)) == B({0xF3, 5}));
  CHECK(one(ctrl(kEvQFrame, 0, 0, 0x31)) == B({0xF1, 0x31}));
  const std::pair<uint8_t, uint8_t> rt[] = {{kEvClock, 0xF8},        {kEvStart, 0xFA}, {kEvContinue, 0xFB}, {kEvStop, 0xFC},
                                            {kEvTuneRequest, 0xF6}, {kEvSensing, 0xFE}, {kEvReset, 0xFF}};
  for (auto [t, b] : rt) {
    e.type = t;
    CHECK(one(e) == B({b}));
  }
  ShortMsg m[4];
  e.type = kEvPortStart;  // announcements and unknown types produce nothing
  CHECK_EQ(decodeShort(e, m), 0u);
  e.type = kEvSysex;
  CHECK_EQ(decodeShort(e, m), 0u);
  e.type = 99;
  CHECK_EQ(decodeShort(e, m), 0u);
}

SQ_TEST("midi-alsa codec: 14-bit controllers and (N)RPN expand to several messages") {
  ShortMsg m[4];
  REQUIRE_EQ(decodeShort(ctrl(kEvControl14, 1, 7, (100 << 7) | 5), m), 2u);
  CHECK(B(m[0].bytes, m[0].bytes + 3) == B({0xB1, 7, 100}));
  CHECK(B(m[1].bytes, m[1].bytes + 3) == B({0xB1, 39, 5}));
  REQUIRE_EQ(decodeShort(ctrl(kEvNonRegParam, 0, (3 << 7) | 4, (10 << 7) | 11), m), 4u);
  CHECK(B(m[0].bytes, m[0].bytes + 3) == B({0xB0, 99, 3}));
  CHECK(B(m[1].bytes, m[1].bytes + 3) == B({0xB0, 98, 4}));
  CHECK(B(m[2].bytes, m[2].bytes + 3) == B({0xB0, 6, 10}));
  CHECK(B(m[3].bytes, m[3].bytes + 3) == B({0xB0, 38, 11}));
  REQUIRE_EQ(decodeShort(ctrl(kEvRegParam, 2, 0, 2 << 7), m), 4u);
  CHECK_EQ(m[0].bytes[1], 101);
  CHECK_EQ(m[1].bytes[1], 100);
}

SQ_TEST("midi-alsa codec: bytes -> event -> bytes round trip for every short message kind") {
  const B msgs[] = {{0x90, 60, 100}, {0x80, 60, 0},  {0xA5, 10, 20}, {0xBF, 64, 127}, {0xC0, 5},  {0xD7, 99},
                    {0xE3, 0x12, 0x34}, {0xE0, 0, 0x40}, {0xF1, 0x22}, {0xF2, 1, 2},    {0xF3, 9},  {0xF6},
                    {0xF8},             {0xFA},          {0xFB},       {0xFC},          {0xFE},     {0xFF}};
  for (const B& b : msgs) {
    SeqEvent e;
    REQUIRE(encodeShort(b.data(), uint32_t(b.size()), e));
    CHECK(one(e) == b);
  }
  SeqEvent e;
  const uint8_t undef[1] = {0xF4}, data[1] = {0x40}, shortNote[2] = {0x90, 60};
  CHECK(!encodeShort(undef, 1, e));
  CHECK(!encodeShort(data, 1, e));
  CHECK(!encodeShort(shortNote, 2, e));  // truncated
  const uint8_t sx[3] = {0xF0, 1, 0xF7};
  CHECK(!encodeShort(sx, 3, e));  // sysex goes through encodeSysexChunk
  encodeSysexChunk(sx, 3, e);
  CHECK_EQ(e.type, kEvSysex);
  CHECK(e.flags & kFlagLengthVariable);
  CHECK_EQ(e.data.ext.len, 3u);
  CHECK(e.data.ext.ptr == static_cast<const void*>(sx));
}

SQ_TEST("midi-alsa codec: sysex chunks are reassembled into one F0..F7 message") {
  auto a = std::make_unique<SysexAssembler>();
  std::vector<uint8_t> msg(600);
  for (size_t i = 0; i < msg.size(); ++i) msg[i] = uint8_t(i % 100);
  msg.front() = 0xF0;
  msg.back() = 0xF7;
  // 256-byte chunks like a raw MIDI port delivers them
  bool done = false;
  for (size_t off = 0; off < msg.size(); off += 256) {
    const size_t n = std::min<size_t>(256, msg.size() - off);
    done = a->feed(msg.data() + off, uint32_t(n));
    CHECK_EQ(done, off + n == msg.size());
  }
  REQUIRE(done);
  REQUIRE_EQ(a->size(), uint32_t(msg.size()));
  CHECK(std::memcmp(a->data(), msg.data(), msg.size()) == 0);
  // a single-chunk message
  const uint8_t small[4] = {0xF0, 0x7D, 0x10, 0xF7};
  CHECK(a->feed(small, 4));
  CHECK_EQ(a->size(), 4u);
  // continuation without a start is ignored; a new F0 restarts an unfinished message
  const uint8_t orphan[2] = {0x01, 0xF7};
  CHECK(!a->feed(orphan, 2));
  const uint8_t start[2] = {0xF0, 0x01};
  CHECK(!a->feed(start, 2));
  CHECK(a->feed(small, 4));
  CHECK_EQ(a->size(), 4u);
  // oversized messages are dropped, the next one works
  std::vector<uint8_t> big(SysexAssembler::kCapacity + 10, 0x11);
  big.front() = 0xF0;
  big.back() = 0xF7;
  CHECK(!a->feed(big.data(), uint32_t(big.size())));
  CHECK_EQ(a->dropped(), 1u);
  CHECK(a->feed(small, 4));
}
