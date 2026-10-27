// sqlite-index: metadata readers + incremental folder scanner on synthetic files.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "miniz.h"
#include "sq/cpp/attributes.hpp"
#include "sq/platform.hpp"
#include "sqlite_index.hpp"
#include "sqtest.hpp"

using sq::Ptr;
using namespace sq::sqlite_index;

namespace {

struct TempDir {
  std::string path;
  explicit TempDir(const char* tag) {
    sq_guid g{};
    sq_guid_generate(&g);
    path = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), std::string("sqix-") + tag + "-" + std::to_string(g.data1));
    for (char& c : path)
      if (c == '\\') c = '/';
    sq_fs_create_dirs(path.c_str());
  }
  ~TempDir() { sq_fs_remove_all(path.c_str()); }
  std::string operator/(const std::string& rel) const { return path + "/" + rel; }
};

struct Bytes {
  std::string b;
  Bytes& raw(const void* p, size_t n) { b.append(static_cast<const char*>(p), n); return *this; }
  Bytes& str(const char* s) { b.append(s); return *this; }
  Bytes& u8(uint32_t v) { b += static_cast<char>(v & 0xFF); return *this; }
  Bytes& le16(uint32_t v) { return u8(v).u8(v >> 8); }
  Bytes& le32(uint32_t v) { return le16(v & 0xFFFF).le16(v >> 16); }
  Bytes& be16(uint32_t v) { return u8(v >> 8).u8(v); }
  Bytes& be32(uint32_t v) { return be16(v >> 16).be16(v & 0xFFFF); }
  Bytes& zeros(size_t n) { b.append(n, '\0'); return *this; }
};

std::string riffChunk(const char* id, const std::string& body) {
  Bytes c;
  c.str(id).le32(static_cast<uint32_t>(body.size())).raw(body.data(), body.size());
  if (body.size() & 1) c.u8(0);
  return c.b;
}

std::string makeWav(uint32_t rate, uint16_t channels, uint16_t bits, uint32_t frames, const char* title, bool loop) {
  Bytes fmt;
  fmt.le16(1).le16(channels).le32(rate).le32(rate * channels * bits / 8).le16(channels * bits / 8).le16(bits);
  std::string body = "WAVE" + riffChunk("fmt ", fmt.b);
  if (loop) {
    Bytes smpl;
    smpl.le32(0).le32(0).le32(0).le32(60).le32(0).le32(0).le32(0).le32(1).le32(0);  // root 60, 1 loop
    smpl.le32(0).le32(0).le32(0).le32(frames - 1).le32(0).le32(0);
    body += riffChunk("smpl", smpl.b);
  }
  body += riffChunk("data", std::string(static_cast<size_t>(frames) * channels * bits / 8, '\0'));
  if (title) {
    std::string info = "INFO" + riffChunk("INAM", std::string(title) + '\0') + riffChunk("IART", std::string("Test Artist") + '\0') +
                       riffChunk("IGNR", std::string("Ambient") + '\0');
    body += riffChunk("LIST", info);
  }
  return riffChunk("RIFF", body);
}

std::string makeAiff(uint32_t frames) {
  Bytes comm;
  comm.be16(1).be32(frames).be16(24);
  // 44100 as 80-bit extended: exponent 16383+15, mantissa 44100 << 48
  comm.u8(0x40).u8(0x0E).u8(0xAC).u8(0x44).zeros(6);
  Bytes c;
  c.str("COMM").be32(18).raw(comm.b.data(), comm.b.size());
  c.str("SSND").be32(8 + frames * 3).zeros(8 + frames * 3);
  if ((frames * 3) & 1) c.u8(0);
  Bytes f;
  f.str("FORM").be32(static_cast<uint32_t>(4 + c.b.size())).str("AIFF").raw(c.b.data(), c.b.size());
  return f.b;
}

std::string makeFlac() {
  Bytes si;
  // min/max block, min/max frame
  si.be16(4096).be16(4096).u8(0).u8(0).u8(0).u8(0).u8(0).u8(0);
  // 96000 Hz (20 bits) | channels-1 = 1 (3 bits) | bps-1 = 23 (5 bits) | total samples 96000 (36 bits)
  const uint64_t rate = 96000, ch = 1, bps = 23, total = 96000;
  const uint64_t packed = (rate << 44) | (ch << 41) | (bps << 36) | total;
  for (int k = 7; k >= 0; --k) si.u8(static_cast<uint32_t>(packed >> (k * 8)));
  si.zeros(16);  // md5
  Bytes vc;
  const char* vendor = "synthetic";
  vc.le32(static_cast<uint32_t>(std::strlen(vendor))).str(vendor).le32(2);
  const char* c1 = "TITLE=Night Drive";
  const char* c2 = "BPM=92";
  vc.le32(static_cast<uint32_t>(std::strlen(c1))).str(c1).le32(static_cast<uint32_t>(std::strlen(c2))).str(c2);
  Bytes f;
  f.str("fLaC");
  f.u8(0x00).u8(0).u8(0).u8(34).raw(si.b.data(), si.b.size());
  const uint32_t n = static_cast<uint32_t>(vc.b.size());
  f.u8(0x84).u8(n >> 16).u8(n >> 8).u8(n).raw(vc.b.data(), vc.b.size());
  return f.b;
}

std::string makeMidi() {
  Bytes t0;  // conductor: name, tempo 100 bpm (600000 us), 3/4, A minor, end at 1920
  t0.u8(0).u8(0xFF).u8(0x03).u8(4).str("Song");
  t0.u8(0).u8(0xFF).u8(0x51).u8(3).u8(0x09).u8(0x27).u8(0xC0);
  t0.u8(0).u8(0xFF).u8(0x58).u8(4).u8(3).u8(2).u8(24).u8(8);
  t0.u8(0).u8(0xFF).u8(0x59).u8(2).u8(0).u8(1);
  t0.u8(0x8F).u8(0x00).u8(0xFF).u8(0x2F).u8(0);  // delta 1920 = 0x8F 0x00
  Bytes t1;  // 4 notes, running status
  t1.u8(0).u8(0xFF).u8(0x03).u8(12).str("Piano Melody");
  for (int n = 0; n < 4; ++n) {
    t1.u8(0).u8(0x90).u8(60 + n).u8(100);
    t1.u8(0x83).u8(0x60).u8(60 + n).u8(0);  // delta 480, running status note-off via velocity 0
  }
  t1.u8(0).u8(0xFF).u8(0x2F).u8(0);
  Bytes f;
  f.str("MThd").be32(6).be16(1).be16(2).be16(480);
  f.str("MTrk").be32(static_cast<uint32_t>(t0.b.size())).raw(t0.b.data(), t0.b.size());
  f.str("MTrk").be32(static_cast<uint32_t>(t1.b.size())).raw(t1.b.data(), t1.b.size());
  return f.b;
}

bool writeZipPreset(const std::string& path, const std::string& metainfo) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_writer_init_file(&zip, path.c_str(), 0)) return false;
  bool ok = mz_zip_writer_add_mem(&zip, "metainfo.xml", metainfo.data(), metainfo.size(), static_cast<mz_uint>(MZ_DEFAULT_LEVEL)) &&
            mz_zip_writer_add_mem(&zip, "preset.xml", "<Preset/>", 9, static_cast<mz_uint>(MZ_DEFAULT_LEVEL));
  ok = mz_zip_writer_finalize_archive(&zip) && ok;
  mz_zip_writer_end(&zip);
  return ok;
}

const MetaValue* meta(const FileInfo& fi, const char* key) { return fi.find(key); }
bool hasTag(const FileInfo& fi, const char* t) {
  for (const auto& x : fi.tags)
    if (x == t) return true;
  return false;
}

struct Library {
  TempDir dir{"scan"};
  Library() {
    sq_fs_create_dirs((dir / "Loops/Sub").c_str());
    sq_fs_create_dirs((dir / ".hidden").c_str());
    sq::writeFile(dir / "pad.wav", makeWav(48000, 2, 16, 4800, "Sunrise Pad", true));
    sq::writeFile(dir / "Loops/Drum Loop 128bpm.wav", makeWav(44100, 1, 16, 441, nullptr, false));
    sq::writeFile(dir / "Loops/Sub/tone.aif", makeAiff(1000));
    sq::writeFile(dir / "night.flac", makeFlac());
    sq::writeFile(dir / "melody.mid", makeMidi());
    sq::writeFile(dir / "broken.mp3", "not really an mp3");
    writeZipPreset(dir / "Glass Keys.preset",
                   "<?xml version=\"1.0\"?><MetaInfo><Attribute id=\"Document:Title\" value=\"Glass Keys\"/>"
                   "<Attribute id=\"Document:Tags\" value=\"Bright;Keys\"/>"
                   "<Attribute id=\"Preset:Category\" value=\"Piano\"/></MetaInfo>");
    sq::writeFile(dir / "plain.trackpreset", "<TrackPreset><name>Vocal Chain</name><category>Vocals</category></TrackPreset>");
    sq::writeFile(dir / "notes.txt", "ignored");
    sq::writeFile(dir / ".hidden/secret.wav", makeWav(48000, 1, 16, 10, nullptr, false));
  }
};

Ptr<sq::IIndexResult> queryAll(sq::IContentIndex* ix, const char* kind = nullptr, const char* text = nullptr) {
  sq_index_query q{};
  q.struct_size = sizeof q;
  q.kind = kind;
  q.text = text;
  q.sort = SQ_INDEX_SORT_TITLE;
  Ptr<sq::IIndexResult> r;
  ix->query(&q, r.put());
  return r;
}

sq_index_scan_status scanAndWait(sq::IIndexScanner* sc, sq::IContentIndex* ix, const std::string& root, uint32_t flags,
                                 const char* exts = nullptr, const char* extra = nullptr) {
  sq_index_scan_options o{};
  o.struct_size = sizeof o;
  o.root = root.c_str();
  o.flags = flags;
  o.extensions = exts;
  o.extra_tags = extra;
  o.batch_size = 3;
  sq_index_scan_status st{};
  st.struct_size = sizeof st;
  if (sc->start(ix, &o) != SQ_OK) {
    st.state = -1;
    return st;
  }
  sc->wait(-1);
  sc->getStatus(&st);
  return st;
}

}  // namespace

SQ_TEST("scanner: wav metadata (fmt, LIST/INFO, smpl)") {
  Library lib;
  FileInfo fi;
  REQUIRE(extractFileInfo(lib.dir / "pad.wav", fi));
  CHECK_EQ(fi.kind, std::string("audio"));
  CHECK_EQ(fi.title, std::string("Sunrise Pad"));
  REQUIRE(meta(fi, "audio.sample_rate"));
  CHECK_EQ(meta(fi, "audio.sample_rate")->i, int64_t(48000));
  CHECK_EQ(meta(fi, "audio.channels")->i, int64_t(2));
  CHECK_EQ(meta(fi, "audio.bits")->i, int64_t(16));
  CHECK_EQ(meta(fi, "audio.frames")->i, int64_t(4800));
  CHECK(meta(fi, "audio.duration") && meta(fi, "audio.duration")->d == 0.1);
  CHECK_EQ(meta(fi, "audio.root_note")->i, int64_t(60));
  REQUIRE(meta(fi, "artist"));
  CHECK_EQ(meta(fi, "artist")->s, std::string("Test Artist"));
  CHECK(hasTag(fi, "loop"));
  CHECK(hasTag(fi, "ambient"));
  CHECK(hasTag(fi, "audio"));
  CHECK(hasTag(fi, "wav"));

  FileInfo loop;
  REQUIRE(extractFileInfo(lib.dir / "Loops/Drum Loop 128bpm.wav", loop));
  CHECK_EQ(loop.title, std::string("Drum Loop 128bpm"));
  CHECK(hasTag(loop, "128bpm"));
  CHECK(hasTag(loop, "loop"));
  REQUIRE(meta(loop, "bpm"));
  CHECK_EQ(meta(loop, "bpm")->s, std::string("128"));
}

SQ_TEST("scanner: aiff, flac, midi and preset metadata") {
  Library lib;
  FileInfo a;
  REQUIRE(extractFileInfo(lib.dir / "Loops/Sub/tone.aif", a));
  REQUIRE(meta(a, "audio.sample_rate"));
  CHECK_EQ(meta(a, "audio.sample_rate")->i, int64_t(44100));
  CHECK_EQ(meta(a, "audio.channels")->i, int64_t(1));
  CHECK_EQ(meta(a, "audio.bits")->i, int64_t(24));
  CHECK_EQ(meta(a, "audio.frames")->i, int64_t(1000));

  FileInfo f;
  REQUIRE(extractFileInfo(lib.dir / "night.flac", f));
  CHECK_EQ(f.title, std::string("Night Drive"));
  REQUIRE(meta(f, "audio.sample_rate"));
  CHECK_EQ(meta(f, "audio.sample_rate")->i, int64_t(96000));
  CHECK_EQ(meta(f, "audio.channels")->i, int64_t(2));
  CHECK_EQ(meta(f, "audio.bits")->i, int64_t(24));
  CHECK(meta(f, "audio.duration")->d == 1.0);
  CHECK(hasTag(f, "92bpm"));

  FileInfo m;
  REQUIRE(extractFileInfo(lib.dir / "melody.mid", m));
  CHECK_EQ(m.kind, std::string("midi"));
  CHECK_EQ(m.title, std::string("melody"));
  REQUIRE(meta(m, "midi.ppq"));
  CHECK_EQ(meta(m, "midi.format")->i, int64_t(1));
  CHECK_EQ(meta(m, "midi.tracks")->i, int64_t(2));
  CHECK_EQ(meta(m, "midi.ppq")->i, int64_t(480));
  CHECK(meta(m, "midi.tempo")->d == 100.0);
  CHECK_EQ(meta(m, "midi.signature")->s, std::string("3/4"));
  CHECK_EQ(meta(m, "midi.key")->s, std::string("A minor"));
  CHECK_EQ(meta(m, "midi.length_ticks")->i, int64_t(1920));
  CHECK_EQ(meta(m, "midi.notes")->i, int64_t(4));
  CHECK(meta(m, "midi.duration")->d > 2.399 && meta(m, "midi.duration")->d < 2.401);
  CHECK_EQ(meta(m, "midi.track_names")->s, std::string("Song; Piano Melody"));
  CHECK(hasTag(m, "100bpm"));

  FileInfo p;
  REQUIRE(extractFileInfo(lib.dir / "Glass Keys.preset", p));
  CHECK_EQ(p.kind, std::string("preset"));
  CHECK_EQ(p.title, std::string("Glass Keys"));
  CHECK(hasTag(p, "bright"));
  CHECK(hasTag(p, "keys"));
  CHECK(hasTag(p, "piano"));
  REQUIRE(meta(p, "preset.Preset:Category"));

  FileInfo x;
  REQUIRE(extractFileInfo(lib.dir / "plain.trackpreset", x));
  CHECK_EQ(x.title, std::string("Vocal Chain"));
  CHECK(hasTag(x, "vocals"));

  FileInfo bad;
  REQUIRE(extractFileInfo(lib.dir / "broken.mp3", bad));  // no codec loaded: file info only
  CHECK_EQ(bad.kind, std::string("audio"));
  CHECK_EQ(bad.title, std::string("broken"));
  FileInfo txt;
  CHECK(!extractFileInfo(lib.dir / "notes.txt", txt));
  CHECK(!extractFileInfo(lib.dir / "missing.wav", txt));
}

SQ_TEST("scanner: full, incremental and pruning scans") {
  Library lib;
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open((lib.dir / "lib.tags.db").c_str(), SQ_INDEX_CREATE));
  auto sc = createIndexScanner();

  sq_index_scan_status st = scanAndWait(sc.get(), ix.get(), lib.dir.path, SQ_INDEX_SCAN_DEFAULT, nullptr, "Factory");
  CHECK_EQ(st.state, SQ_INDEX_SCAN_DONE);
  CHECK_EQ(st.result, SQ_OK);
  CHECK_EQ(st.files_seen, uint64_t(8));  // .txt and .hidden/ skipped
  CHECK_EQ(st.files_indexed, uint64_t(8));
  CHECK_EQ(st.errors, uint64_t(0));
  auto r = queryAll(ix.get());
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 8u);

  // Query through the index: kinds, tags, text over metadata.
  CHECK_EQ(queryAll(ix.get(), "midi")->getCount(), 1u);
  CHECK_EQ(queryAll(ix.get(), "preset")->getCount(), 2u);
  CHECK_EQ(queryAll(ix.get(), "audio")->getCount(), 5u);
  r = queryAll(ix.get(), nullptr, "piano melody");  // MIDI track name (metadata text)
  REQUIRE_EQ(r->getCount(), 1u);
  sq_index_row row{};
  row.struct_size = sizeof row;
  REQUIRE_OK(r->getRow(0, &row));
  CHECK_EQ(std::string(row.url), lib.dir / "melody.mid");
  CHECK(std::string(row.tags).find("factory") != std::string::npos);
  auto md = sq::makeAttributes();
  REQUIRE_OK(r->getMetadata(0, md.get()));
  CHECK_EQ(md->getInt("midi.ppq", 0), int64_t(480));
  CHECK(row.mtime > 1600000000);
  CHECK(row.size > 0);

  // Rescan: everything unchanged.
  st = scanAndWait(sc.get(), ix.get(), lib.dir.path, SQ_INDEX_SCAN_DEFAULT);
  CHECK_EQ(st.files_unchanged, uint64_t(8));
  CHECK_EQ(st.files_indexed, uint64_t(0));

  // Modify one (size changes), delete one, add one.
  sq::writeFile(lib.dir / "pad.wav", makeWav(48000, 2, 16, 9600, "Sunrise Pad Long", true));
  sq_fs_remove((lib.dir / "night.flac").c_str());
  sq::writeFile(lib.dir / "Loops/new.mid", makeMidi());
  st = scanAndWait(sc.get(), ix.get(), lib.dir.path, SQ_INDEX_SCAN_DEFAULT);
  CHECK_EQ(st.state, SQ_INDEX_SCAN_DONE);
  CHECK_EQ(st.files_seen, uint64_t(8));
  CHECK_EQ(st.files_indexed, uint64_t(2));
  CHECK_EQ(st.files_unchanged, uint64_t(6));
  CHECK_EQ(st.files_removed, uint64_t(1));
  CHECK_EQ(queryAll(ix.get(), nullptr, "sunrise long")->getCount(), 1u);
  CHECK_EQ(queryAll(ix.get(), nullptr, "night")->getCount(), 0u);
  CHECK_EQ(queryAll(ix.get())->getCount(), 8u);

  // FORCE re-reads everything.
  st = scanAndWait(sc.get(), ix.get(), lib.dir.path, SQ_INDEX_SCAN_DEFAULT | SQ_INDEX_SCAN_FORCE);
  CHECK_EQ(st.files_indexed, uint64_t(8));

  // Non-recursive pruning only removes direct children, and an extension filter limits both the
  // candidates and what may be pruned (other types under the root are left alone).
  sq_fs_remove((lib.dir / "melody.mid").c_str());
  sq_fs_remove((lib.dir / "Loops/new.mid").c_str());
  st = scanAndWait(sc.get(), ix.get(), lib.dir.path, SQ_INDEX_SCAN_PRUNE, "mid");
  CHECK_EQ(st.files_seen, uint64_t(0));
  CHECK_EQ(st.files_removed, uint64_t(1));  // melody.mid; Loops/new.mid is nested, wav/presets not "mid"
  CHECK_EQ(queryAll(ix.get())->getCount(), 7u);
  CHECK_EQ(queryAll(ix.get(), "midi")->getCount(), 1u);

  ix->close();
}

SQ_TEST("scanner: errors, busy and cancel") {
  Library lib;
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
  auto sc = createIndexScanner();
  sq_index_scan_options o{};
  o.struct_size = sizeof o;
  o.root = "Z:/definitely/not/here";
  CHECK_EQ(sc->start(ix.get(), &o), SQ_E_NOTFOUND);
  CHECK_EQ(sc->start(nullptr, &o), SQ_E_INVALIDARG);
  CHECK_EQ(sc->cancel(), SQ_S_FALSE);
  CHECK_OK(sc->wait(0));  // idle

  // Larger tree so a cancel lands mid-scan.
  for (int d = 0; d < 20; ++d)
    for (int f = 0; f < 50; ++f)
      sq::writeFile(lib.dir / ("bulk/d" + std::to_string(d) + "/f" + std::to_string(f) + ".mid"), makeMidi());
  o.root = lib.dir.path.c_str();
  o.flags = SQ_INDEX_SCAN_DEFAULT;
  REQUIRE_OK(sc->start(ix.get(), &o));
  CHECK_EQ(sc->start(ix.get(), &o), SQ_E_BUSY);
  sc->cancel();
  REQUIRE_OK(sc->wait(30000));
  sq_index_scan_status st{};
  st.struct_size = sizeof st;
  REQUIRE_OK(sc->getStatus(&st));
  CHECK(st.state == SQ_INDEX_SCAN_CANCELLED || st.state == SQ_INDEX_SCAN_DONE);
  // the index is usable (no dangling batch) and a full scan afterwards completes
  CHECK_OK(ix->beginBatch());
  CHECK_OK(ix->commitBatch());
  CHECK_EQ(ix->commitBatch(), SQ_S_FALSE);
  st = scanAndWait(sc.get(), ix.get(), lib.dir.path, SQ_INDEX_SCAN_DEFAULT);
  CHECK_EQ(st.state, SQ_INDEX_SCAN_DONE);
  CHECK_EQ(queryAll(ix.get())->getCount(), 1008u);
  std::printf("  1008-file scan (partly incremental): %lld ms\n", static_cast<long long>(st.elapsed_ms));

  // Releasing a scanner mid-scan cancels and joins.
  auto sc2 = createIndexScanner();
  o.flags |= SQ_INDEX_SCAN_FORCE;
  REQUIRE_OK(sc2->start(ix.get(), &o));
  sc2.reset();
  CHECK_OK(ix->beginBatch());
  CHECK_OK(ix->commitBatch());
}
