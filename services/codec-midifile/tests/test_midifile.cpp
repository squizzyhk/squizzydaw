// codec-midifile: SMF reader/writer through the real module loader, on synthesized files.
#include <cstring>
#include <string>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/codec_midifile/smf_meta.hpp"
#include "sq/cpp/bytestream.hpp"
#include "sq/interfaces/midi.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"
#include "sqtest.hpp"

namespace {

struct Rig {
  sq::app::Settings settings;
  sq::app::ClassRegistry registry;
  sq::app::HostContext host{settings, registry};
  sq::app::ModuleLoader loader{host, registry};
  Rig() {
    std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"),
                                    std::string("codec-midifile") + sq_dylib_suffix());
    auto r = loader.load(path);
    if (!r.ok()) std::printf("load failed: %s\n", r.message.c_str());
  }
  sq::Ptr<sq::IMidiFileReader> reader() { return sq::createInstance<sq::IMidiFileReader>(&registry, sq::midi::kMidiFileReaderClassId); }
  sq::Ptr<sq::IMidiFileWriter> writer() { return sq::createInstance<sq::IMidiFileWriter>(&registry, sq::midi::kMidiFileWriterClassId); }
};

// Owning test-side event (payload copied).
struct Ev {
  uint64_t tick = 0;
  uint8_t status = 0, d1 = 0, d2 = 0, meta = 0;
  std::vector<uint8_t> data;
  bool operator==(const Ev& o) const {
    return tick == o.tick && status == o.status && d1 == o.d1 && d2 == o.d2 && meta == o.meta && data == o.data;
  }
};

Ev chan(uint64_t t, uint8_t s, uint8_t a, uint8_t b = 0) { return Ev{t, s, a, b, 0, {}}; }
Ev meta(uint64_t t, uint8_t type, std::vector<uint8_t> d) { return Ev{t, 0xFF, 0, 0, type, std::move(d)}; }
Ev metaText(uint64_t t, uint8_t type, const std::string& s) { return meta(t, type, std::vector<uint8_t>(s.begin(), s.end())); }
Ev sysex(uint64_t t, uint8_t st, std::vector<uint8_t> d) { return Ev{t, st, 0, 0, 0, std::move(d)}; }
Ev eot(uint64_t t) { return meta(t, 0x2F, {}); }

sq_result add(sq::IMidiFileWriter* w, const Ev& e) {
  sq_midi_file_event f{};
  f.struct_size = sizeof f;
  f.tick = e.tick;
  f.status = e.status;
  f.data1 = e.d1;
  f.data2 = e.d2;
  f.meta_type = e.meta;
  f.size = uint32_t(e.data.size());
  f.data = e.data.empty() ? nullptr : e.data.data();
  return w->addEvent(&f);
}

std::vector<Ev> readTrack(sq::IMidiFileReader* r, uint32_t track) {
  std::vector<Ev> out;
  const uint32_t n = r->getEventCount(track);
  for (uint32_t i = 0; i < n; ++i) {
    sq_midi_file_event f{};
    f.struct_size = sizeof f;
    if (r->getEvent(track, i, &f) != SQ_OK) break;
    Ev e{f.tick, f.status, f.data1, f.data2, f.meta_type, {}};
    if (f.size) e.data.assign(f.data, f.data + f.size);
    out.push_back(std::move(e));
  }
  return out;
}

std::string trackName(sq::IMidiFileReader* r, uint32_t track) {
  uint32_t need = 0;
  r->getTrackName(track, nullptr, 0, &need);
  std::string s(need, '\0');
  if (need == 0 || r->getTrackName(track, s.data(), need, &need) != SQ_OK) return "?";
  s.resize(need - 1);
  return s;
}

std::vector<uint8_t> streamBytes(sq::IByteStream* s) {
  int64_t size = 0;
  s->getSize(&size);
  std::vector<uint8_t> v(static_cast<size_t>(size));
  s->seek(0, SQ_SEEK_SET, nullptr);
  uint32_t got = 0;
  if (size) s->read(v.data(), uint32_t(size), &got);
  return v;
}

sq_result openBytes(sq::IMidiFileReader* r, const std::vector<uint8_t>& bytes) {
  auto s = sq::makeMemoryStream(bytes.data(), bytes.size(), false);
  return r->openStream(s.get());
}

// Writes tracks into an in-memory SMF and returns the bytes.
std::vector<uint8_t> writeSmf(Rig& rig, uint32_t format, uint32_t tpq, const std::vector<std::vector<Ev>>& tracks) {
  auto w = rig.writer();
  auto s = sq::makeMemoryStream();
  if (!w || w->createStream(s.get(), format, tpq) != SQ_OK) return {};
  for (const auto& t : tracks) {
    if (w->beginTrack() != SQ_OK) return {};
    for (const Ev& e : t)
      if (add(w.get(), e) != SQ_OK) return {};
    if (w->endTrack() != SQ_OK) return {};
  }
  if (w->finish() != SQ_OK) return {};
  return streamBytes(s.get());
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}
std::vector<uint8_t> header(uint16_t format, uint16_t ntrks, uint16_t division) {
  std::vector<uint8_t> v{'M', 'T', 'h', 'd', 0, 0, 0, 6};
  v.push_back(uint8_t(format >> 8));
  v.push_back(uint8_t(format));
  v.push_back(uint8_t(ntrks >> 8));
  v.push_back(uint8_t(ntrks));
  v.push_back(uint8_t(division >> 8));
  v.push_back(uint8_t(division));
  return v;
}
void appendTrack(std::vector<uint8_t>& v, const std::vector<uint8_t>& body) {
  v.insert(v.end(), {'M', 'T', 'r', 'k'});
  be32(v, uint32_t(body.size()));
  v.insert(v.end(), body.begin(), body.end());
}

// Deterministic PRNG for the randomized round trips.
struct Lcg {
  uint64_t s;
  uint32_t next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(s >> 33);
  }
  uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

}  // namespace

SQ_TEST("midifile: module loads and registers both classes under MidiFile") {
  Rig rig;
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(rig.registry.findClass(&sq::midi::kMidiFileReaderClassId, &ci));
  CHECK_EQ(std::string(ci.category), std::string("MidiFile"));
  REQUIRE_OK(rig.registry.findClass(&sq::midi::kMidiFileWriterClassId, &ci));
  CHECK_EQ(std::string(ci.category), std::string("MidiFile"));
  CHECK(rig.reader());
  CHECK(rig.writer());
}

SQ_TEST("midifile: format 1 round trip keeps every event (meta, tempo, time sig, sysex, all channel kinds)") {
  Rig rig;
  uint8_t tempo[3], tsig[4], ksig[2], tempo2[3];
  auto te = sq::midifile::makeTempo(0, sq::midifile::bpmToMicros(120.0), tempo);
  auto ts = sq::midifile::makeTimeSignature(0, 6, 8, tsig);
  auto ks = sq::midifile::makeKeySignature(0, -3, true, ksig);
  auto te2 = sq::midifile::makeTempo(1920, sq::midifile::bpmToMicros(150.0), tempo2);
  auto fromMeta = [](const sq_midi_file_event& e) { return meta(e.tick, e.meta_type, std::vector<uint8_t>(e.data, e.data + e.size)); };

  std::vector<std::vector<Ev>> tracks = {
      {metaText(0, 0x03, "Conductor"), fromMeta(te), fromMeta(ts), fromMeta(ks), metaText(0, 0x06, "Intro"),
       fromMeta(te2), metaText(3840, 0x01, "free text \xC3\xA9")},
      {metaText(0, 0x03, "Keys"), chan(0, 0xC0, 5), chan(0, 0x90, 60, 100), chan(0, 0x90, 64, 90), chan(0, 0x90, 67, 80),
       chan(480, 0x80, 60, 0), chan(480, 0x80, 64, 0), chan(480, 0x90, 67, 0),  // note-on velocity 0 kept as-is
       chan(500, 0xB0, 7, 127), chan(500, 0xB0, 10, 64), chan(600, 0xE0, 0x00, 0x40), chan(601, 0xE0, 0x7F, 0x7F),
       chan(700, 0xD0, 33), chan(701, 0xA0, 60, 12), sysex(800, 0xF0, {0x7E, 0x7F, 0x09, 0x01, 0xF7}),
       sysex(801, 0xF7, {0xF8}), chan(802, 0x9F, 36, 127), chan(900, 0x8F, 36, 64)},
      {},  // an empty track
  };
  auto bytes = writeSmf(rig, 1, 960, tracks);
  REQUIRE(!bytes.empty());

  auto r = rig.reader();
  REQUIRE_OK(openBytes(r.get(), bytes));
  sq_midi_file_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(r->getInfo(&info));
  CHECK_EQ(info.format, 1u);
  CHECK_EQ(info.track_count, 3u);
  CHECK_EQ(info.ticks_per_quarter, 960u);
  CHECK_EQ(info.smpte_fps, 0);

  for (uint32_t t = 0; t < 3; ++t) {
    auto got = readTrack(r.get(), t);
    auto want = tracks[t];
    want.push_back(eot(want.empty() ? 0 : want.back().tick));
    REQUIRE_EQ(got.size(), want.size());
    for (size_t i = 0; i < got.size(); ++i) {
      if (!(got[i] == want[i])) std::printf("track %u event %zu differs\n", t, i);
      CHECK(got[i] == want[i]);
    }
  }
  CHECK_EQ(trackName(r.get(), 0), std::string("Conductor"));
  CHECK_EQ(trackName(r.get(), 1), std::string("Keys"));
  CHECK_EQ(trackName(r.get(), 2), std::string(""));

  // Meta helpers read back what they wrote.
  sq_midi_file_event f{};
  f.struct_size = sizeof f;
  REQUIRE_OK(r->getEvent(0, 1, &f));
  uint32_t us = 0;
  CHECK(sq::midifile::parseTempo(f, &us));
  CHECK_EQ(us, 500000u);
  REQUIRE_OK(r->getEvent(0, 2, &f));
  uint32_t num = 0, den = 0;
  CHECK(sq::midifile::parseTimeSignature(f, &num, &den));
  CHECK_EQ(num, 6u);
  CHECK_EQ(den, 8u);
  REQUIRE_OK(r->getEvent(0, 3, &f));
  int32_t sf = 0;
  bool minor = false;
  CHECK(sq::midifile::parseKeySignature(f, &sf, &minor));
  CHECK_EQ(sf, -3);
  CHECK(minor);

  // Re-writing what was read reproduces the same bytes (explicit End-of-Track metas included).
  std::vector<std::vector<Ev>> again;
  for (uint32_t t = 0; t < 3; ++t) again.push_back(readTrack(r.get(), t));
  CHECK(writeSmf(rig, 1, 960, again) == bytes);
}

SQ_TEST("midifile: writer emits running status and minimal VLQs (byte exact)") {
  Rig rig;
  auto bytes = writeSmf(rig, 0, 96,
                        {{chan(0, 0x90, 60, 100), chan(0x7F, 0x90, 62, 100), chan(0x7F + 0x80, 0x80, 60, 0),
                          metaText(0x7F + 0x80 + 0x3FFF, 0x01, "x"), chan(0x7F + 0x80 + 0x3FFF + 0x4000, 0x80, 62, 0)}});
  std::vector<uint8_t> want = header(0, 1, 96);
  appendTrack(want, {0x00, 0x90, 60, 100,         // status
                     0x7F, 62, 100,               // running status, 1-byte delta
                     0x81, 0x00, 0x80, 60, 0,     // delta 0x80, new status
                     0xFF, 0x7F, 0xFF, 0x01, 0x01, 'x',  // delta 0x3FFF, meta resets running status
                     0x81, 0x80, 0x00, 0x80, 62, 0,      // delta 0x4000 -> full status again
                     0x00, 0xFF, 0x2F, 0x00});
  CHECK(bytes == want);
}

SQ_TEST("midifile: reader handles running status across meta/sysex and unknown chunks") {
  Rig rig;
  std::vector<uint8_t> f = header(1, 2, 480);
  // An unknown chunk between tracks must be skipped.
  f.insert(f.end(), {'X', 'Y', 'Z', 'W', 0, 0, 0, 3, 1, 2, 3});
  appendTrack(f, {0x00, 0x91, 60, 100,  // note on ch2
                  0x10, 61, 101,        // running status
                  0x00, 0xFF, 0x03, 0x02, 'h', 'i',  // meta in between
                  0x05, 62, 102,        // running status continued after meta (lenient)
                  0x00, 0xC3, 7,        // program change: one data byte
                  0x01, 8,              // running program change
                  0x00, 0xF0, 0x03, 0x41, 0x10, 0xF7,  // sysex
                  0x02, 9,              // running status after sysex (lenient)
                  0x00, 0xFF, 0x2F, 0x00,
                  0xDE, 0xAD});         // junk after End-of-Track ignored
  appendTrack(f, {0x00, 0xFF, 0x2F, 0x00});
  auto r = rig.reader();
  REQUIRE_OK(openBytes(r.get(), f));
  auto ev = readTrack(r.get(), 0);
  std::vector<Ev> want = {chan(0, 0x91, 60, 100), chan(16, 0x91, 61, 101), metaText(16, 0x03, "hi"), chan(21, 0x91, 62, 102),
                          chan(21, 0xC3, 7), chan(22, 0xC3, 8), sysex(22, 0xF0, {0x41, 0x10, 0xF7}), chan(24, 0xC3, 9), eot(24)};
  REQUIRE_EQ(ev.size(), want.size());
  for (size_t i = 0; i < ev.size(); ++i) CHECK(ev[i] == want[i]);
  CHECK_EQ(trackName(r.get(), 0), std::string("hi"));
  CHECK_EQ(r->getEventCount(1), 1u);
}

SQ_TEST("midifile: SMPTE division, format 2 read as 1, RMID wrapper") {
  Rig rig;
  {
    std::vector<uint8_t> f = header(0, 1, uint16_t((uint8_t(-25) << 8) | 40));
    appendTrack(f, {0x00, 0x90, 1, 2, 0x00, 0xFF, 0x2F, 0x00});
    auto r = rig.reader();
    REQUIRE_OK(openBytes(r.get(), f));
    sq_midi_file_info info{};
    info.struct_size = sizeof info;
    REQUIRE_OK(r->getInfo(&info));
    CHECK_EQ(info.ticks_per_quarter, 0u);
    CHECK_EQ(info.smpte_fps, 25);
    CHECK_EQ(info.ticks_per_frame, 40u);
    // The writer accepts the raw SMPTE division word too and round-trips it.
    auto back = writeSmf(rig, 0, uint32_t((uint8_t(-25) << 8) | 40), {readTrack(r.get(), 0)});
    CHECK(back == f);
  }
  {
    std::vector<uint8_t> f = header(2, 2, 192);
    appendTrack(f, {0x00, 0xFF, 0x2F, 0x00});
    appendTrack(f, {0x00, 0xFF, 0x2F, 0x00});
    auto r = rig.reader();
    REQUIRE_OK(openBytes(r.get(), f));
    sq_midi_file_info info{};
    info.struct_size = sizeof info;
    REQUIRE_OK(r->getInfo(&info));
    CHECK_EQ(info.format, 1u);
    CHECK_EQ(info.track_count, 2u);
  }
  {
    std::vector<uint8_t> smf = header(0, 1, 120);
    appendTrack(smf, {0x00, 0x90, 64, 64, 0x00, 0xFF, 0x2F, 0x00});
    std::vector<uint8_t> riff{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'R', 'M', 'I', 'D', 'd', 'a', 't', 'a'};
    const uint32_t n = uint32_t(smf.size());
    riff.insert(riff.end(), {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)});
    riff.insert(riff.end(), smf.begin(), smf.end());
    auto r = rig.reader();
    REQUIRE_OK(openBytes(r.get(), riff));
    CHECK_EQ(r->getEventCount(0), 2u);
  }
}

SQ_TEST("midifile: malformed input is rejected or recovered, never crashes") {
  Rig rig;
  auto r = rig.reader();
  CHECK_EQ(openBytes(r.get(), {}), SQ_E_PARSE);
  CHECK_EQ(openBytes(r.get(), {'R', 'I', 'F', 'F'}), SQ_E_PARSE);
  {
    std::vector<uint8_t> f = header(3, 1, 96);  // unknown format
    appendTrack(f, {0x00, 0xFF, 0x2F, 0x00});
    CHECK_EQ(openBytes(r.get(), f), SQ_E_PARSE);
  }
  {
    std::vector<uint8_t> f = header(0, 1, 96);
    appendTrack(f, {0x00, 40, 50});  // data byte with no status
    CHECK_EQ(openBytes(r.get(), f), SQ_E_PARSE);
    sq_midi_file_info info{};
    info.struct_size = sizeof info;
    CHECK_EQ(r->getInfo(&info), SQ_E_CLOSED);
    CHECK_EQ(r->getEventCount(0), 0u);
  }
  {
    std::vector<uint8_t> f = header(0, 1, 96);
    appendTrack(f, {0x00, 0xF8});  // realtime byte is not valid in a file
    CHECK_EQ(openBytes(r.get(), f), SQ_E_PARSE);
  }
  {
    // Missing End-of-Track and a truncated final event: recovered, EOT synthesized.
    std::vector<uint8_t> f = header(0, 1, 96);
    std::vector<uint8_t> body{0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0x10, 0x90, 61};
    f.insert(f.end(), {'M', 'T', 'r', 'k'});
    be32(f, 100);  // declared longer than the file
    f.insert(f.end(), body.begin(), body.end());
    REQUIRE_OK(openBytes(r.get(), f));
    auto ev = readTrack(r.get(), 0);
    REQUIRE_EQ(ev.size(), size_t(3));
    CHECK(ev[1] == chan(0x60, 0x80, 60, 0));
    CHECK(ev[2] == eot(0x70));
  }
  // Random mutations of a valid file: every outcome must be a clean result code.
  std::vector<std::vector<Ev>> tracks = {{metaText(0, 3, "name"), chan(0, 0x90, 1, 2), sysex(5, 0xF0, {1, 2, 0xF7}), chan(9, 0xB0, 3, 4)},
                                         {chan(1, 0xE0, 0, 64), chan(2, 0xC0, 3)}};
  auto good = writeSmf(rig, 1, 480, tracks);
  REQUIRE(!good.empty());
  Lcg rng{12345};
  int ok = 0;
  for (int i = 0; i < 3000; ++i) {
    auto m = good;
    const uint32_t flips = 1 + rng.below(4);
    for (uint32_t k = 0; k < flips; ++k) m[rng.below(uint32_t(m.size()))] = uint8_t(rng.next());
    if (rng.below(4) == 0) m.resize(rng.below(uint32_t(m.size())));
    sq_result res = openBytes(r.get(), m);
    CHECK(res == SQ_OK || res == SQ_E_PARSE);
    if (res == SQ_OK) {
      ++ok;
      sq_midi_file_info info{};
      info.struct_size = sizeof info;
      CHECK_OK(r->getInfo(&info));
      for (uint32_t t = 0; t < info.track_count; ++t) readTrack(r.get(), t);
    }
  }
  CHECK(ok > 0);
}

SQ_TEST("midifile: writer validates state, ordering and data bytes") {
  Rig rig;
  auto w = rig.writer();
  auto s = sq::makeMemoryStream();
  CHECK_EQ(w->beginTrack(), SQ_E_FAIL);  // before create
  CHECK_EQ(w->createStream(s.get(), 2, 480), SQ_E_UNSUPPORTED);
  CHECK_EQ(w->createStream(s.get(), 1, 0), SQ_E_INVALIDARG);
  CHECK_EQ(w->createStream(s.get(), 1, 0x8000 | 40), SQ_E_INVALIDARG);  // fps 0 is not SMPTE
  REQUIRE_OK(w->createStream(s.get(), 0, 480));
  CHECK_EQ(add(w.get(), chan(0, 0x90, 1, 1)), SQ_E_FAIL);  // outside a track
  REQUIRE_OK(w->beginTrack());
  CHECK_EQ(w->beginTrack(), SQ_E_FAIL);  // nested
  REQUIRE_OK(add(w.get(), chan(10, 0x90, 1, 1)));
  CHECK_EQ(add(w.get(), chan(9, 0x80, 1, 0)), SQ_E_INVALIDARG);    // tick goes backwards
  CHECK_EQ(add(w.get(), chan(10, 0x90, 0x80, 1)), SQ_E_INVALIDARG); // data byte with bit 7
  CHECK_EQ(add(w.get(), chan(10, 0xF2, 1, 1)), SQ_E_INVALIDARG);    // system common is not an SMF event
  CHECK_EQ(add(w.get(), meta(10, 0x80, {})), SQ_E_INVALIDARG);
  CHECK_EQ(add(w.get(), chan(10 + 0x10000000ull, 0x80, 1, 0)), SQ_E_INVALIDARG);  // delta too large for a VLQ
  sq_midi_file_event small{};
  small.struct_size = 4;
  CHECK_EQ(w->addEvent(&small), SQ_E_INVALIDARG);
  REQUIRE_OK(add(w.get(), eot(1000)));  // explicit End-of-Track sets the track length
  CHECK_EQ(add(w.get(), chan(1000, 0x80, 1, 0)), SQ_E_FAIL);  // nothing after End-of-Track
  REQUIRE_OK(w->endTrack());
  CHECK_EQ(w->beginTrack(), SQ_E_UNSUPPORTED);  // format 0 = one track
  REQUIRE_OK(w->finish());
  CHECK_EQ(w->finish(), SQ_E_FAIL);
  auto r = rig.reader();
  REQUIRE_OK(openBytes(r.get(), streamBytes(s.get())));
  auto ev = readTrack(r.get(), 0);
  REQUIRE_EQ(ev.size(), size_t(2));
  CHECK(ev[1] == eot(1000));
}

SQ_TEST("midifile: file path round trip; release without finish writes nothing") {
  Rig rig;
  sq_guid g{};
  sq_guid_generate(&g);
  std::string dir = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-midifile-" + std::to_string(g.data1));
  REQUIRE_OK(sq_fs_create_dirs(dir.c_str()));
  std::string path = sq::joinPath(dir, "song.mid");
  std::string discarded = sq::joinPath(dir, "never.mid");
  {
    auto w = rig.writer();
    REQUIRE_OK(w->create(discarded.c_str(), 1, 480));
    REQUIRE_OK(w->beginTrack());
    REQUIRE_OK(add(w.get(), chan(0, 0x90, 60, 1)));
  }
  CHECK(!sq_fs_exists(discarded.c_str()));
  {
    auto w = rig.writer();
    REQUIRE_OK(w->create(path.c_str(), 1, 480));
    REQUIRE_OK(w->beginTrack());
    REQUIRE_OK(add(w.get(), metaText(0, 3, "A")));
    REQUIRE_OK(w->endTrack());
    REQUIRE_OK(w->beginTrack());
    REQUIRE_OK(add(w.get(), chan(0, 0x90, 60, 1)));
    REQUIRE_OK(w->finish());  // closes the open track itself
  }
  auto r = rig.reader();
  CHECK_EQ(r->open(sq::joinPath(dir, "missing.mid").c_str()), SQ_E_NOTFOUND);
  REQUIRE_OK(r->open(path.c_str()));
  sq_midi_file_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(r->getInfo(&info));
  CHECK_EQ(info.track_count, 2u);
  CHECK_EQ(trackName(r.get(), 0), std::string("A"));
  char tiny[1];
  uint32_t need = 0;
  CHECK_EQ(r->getTrackName(0, tiny, sizeof tiny, &need), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(need, 2u);
  CHECK_EQ(r->getTrackName(7, tiny, sizeof tiny, &need), SQ_E_NOTFOUND);
  r = nullptr;
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("midifile: randomized multi-track round trips are lossless") {
  Rig rig;
  Lcg rng{0xC0FFEE};
  for (int iter = 0; iter < 40; ++iter) {
    std::vector<std::vector<Ev>> tracks(1 + rng.below(6));
    for (auto& t : tracks) {
      uint64_t tick = 0;
      const uint32_t n = rng.below(300);
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t gap = rng.below(8) == 0 ? rng.below(0x0FFFFFFF) : rng.below(200);
        tick += rng.below(3) == 0 ? 0 : gap;
        const uint32_t kind = rng.below(10);
        if (kind < 6) {
          static const uint8_t kinds[] = {0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0};
          t.push_back(chan(tick, uint8_t(kinds[rng.below(7)] | rng.below(16)), uint8_t(rng.below(128)), 0));
        } else if (kind < 8) {
          std::vector<uint8_t> d(rng.below(40));
          for (auto& b : d) b = uint8_t(rng.next());
          uint8_t type = uint8_t(rng.below(0x7F));
          if (type == 0x2F) type = 0x01;
          t.push_back(meta(tick, type, d));
        } else {
          std::vector<uint8_t> d(rng.below(100));
          for (auto& b : d) b = uint8_t(rng.below(128));
          t.push_back(sysex(tick, rng.below(2) ? 0xF0 : 0xF7, d));
        }
      }
    }
    // Fill second data bytes only for two-byte messages (one-byte ones read back as 0).
    for (auto& t : tracks)
      for (auto& e : t) {
        const uint8_t hi = e.status & 0xF0;
        if (e.status < 0xF0 && hi != 0xC0 && hi != 0xD0) e.d2 = uint8_t(rng.below(128));
      }
    const uint32_t format = tracks.size() == 1 ? rng.below(2) : 1;
    auto bytes = writeSmf(rig, format, 1 + rng.below(0x7FFF), tracks);
    REQUIRE(!bytes.empty());
    auto r = rig.reader();
    REQUIRE_OK(openBytes(r.get(), bytes));
    sq_midi_file_info info{};
    info.struct_size = sizeof info;
    REQUIRE_OK(r->getInfo(&info));
    REQUIRE_EQ(info.track_count, uint32_t(tracks.size()));
    CHECK_EQ(info.format, format);
    for (uint32_t ti = 0; ti < tracks.size(); ++ti) {
      auto got = readTrack(r.get(), ti);
      auto want = tracks[ti];
      want.push_back(eot(want.empty() ? 0 : want.back().tick));
      REQUIRE_EQ(got.size(), want.size());
      bool same = true;
      for (size_t i = 0; i < got.size(); ++i) same = same && got[i] == want[i];
      CHECK(same);
    }
  }
}

SQ_TEST("midifile: tempo map converts ticks and seconds across tempo changes") {
  sq::midifile::TempoMap map(480);
  CHECK(std::abs(map.tickToSeconds(960) - 1.0) < 1e-9);  // default 120 BPM
  map.add(0, sq::midifile::bpmToMicros(120.0));
  map.add(1920, sq::midifile::bpmToMicros(60.0));
  CHECK(std::abs(map.tickToSeconds(1920) - 2.0) < 1e-9);
  CHECK(std::abs(map.tickToSeconds(2400) - 3.0) < 1e-9);
  CHECK(std::abs(map.secondsToTick(3.0) - 2400.0) < 1e-6);
  CHECK(std::abs(map.secondsToTick(0.5) - 480.0) < 1e-6);
  CHECK(std::abs(sq::midifile::microsToBpm(sq::midifile::bpmToMicros(93.0)) - 93.0) < 1e-3);
}
