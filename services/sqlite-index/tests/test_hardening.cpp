// sqlite-index: adversarial / regression tests (verifier pass).
//  - foreign SQLite databases are refused and left untouched
//  - scans with an extension filter only prune items of the filtered types
//  - unreadable sub folders do not get their items pruned
//  - directory link loops (junctions / symlinks) terminate without duplicates
//  - non-ASCII tags are case-insensitive (stored lowercase)
//  - search text made only of separators behaves like no text / is ignored
//  - malformed / truncated media files never crash or hang the readers
//  - other formats go through a loaded ICodecService (mocked)
//  - large skipped FLAC blocks are seeked over, not read
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "sq/app/class_registry.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/cpp/object.hpp"
#include "sq/interfaces/codec.h"
#include "sq/platform.hpp"
#include "sqlite3.h"
#include "sqlite_index.hpp"
#include "sqtest.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// UTF-8 literal as const char* (C++20 makes u8"" char8_t).
#define U8(s) reinterpret_cast<const char*>(u8##s)

using sq::Ptr;
using namespace sq::sqlite_index;

namespace {

struct TempDir {
  std::string path;
  explicit TempDir(const char* tag) {
    sq_guid g{};
    sq_guid_generate(&g);
    path = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), std::string("sqixh-") + tag + "-" + std::to_string(g.data1));
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
  Bytes& le64(uint64_t v) { return le32(static_cast<uint32_t>(v)).le32(static_cast<uint32_t>(v >> 32)); }
  Bytes& be16(uint32_t v) { return u8(v >> 8).u8(v); }
  Bytes& be32(uint32_t v) { return be16(v >> 16).be16(v & 0xFFFF); }
  Bytes& zeros(size_t n) { b.append(n, '\0'); return *this; }
};

std::string chunk(const char* id, const std::string& body) {
  Bytes c;
  c.str(id).le32(static_cast<uint32_t>(body.size())).raw(body.data(), body.size());
  if (body.size() & 1) c.u8(0);
  return c.b;
}

std::string makeWav(uint32_t frames) {
  Bytes fmt;
  fmt.le16(1).le16(1).le32(44100).le32(88200).le16(2).le16(16);
  std::string body = "WAVE" + chunk("fmt ", fmt.b) + chunk("data", std::string(frames * 2, '\0'));
  return chunk("RIFF", body);
}

std::string makeMidi() {
  Bytes t;
  t.u8(0).u8(0xFF).u8(0x51).u8(3).u8(0x07).u8(0xA1).u8(0x20);  // 120 bpm
  t.u8(0).u8(0x90).u8(60).u8(100).u8(0x83).u8(0x60).u8(0x80).u8(60).u8(0);
  t.u8(0).u8(0xFF).u8(0x2F).u8(0);
  Bytes f;
  f.str("MThd").be32(6).be16(0).be16(1).be16(480);
  f.str("MTrk").be32(static_cast<uint32_t>(t.b.size())).raw(t.b.data(), t.b.size());
  return f.b;
}

std::string makeAiff() {
  Bytes comm;
  comm.be16(2).be32(100).be16(16).u8(0x40).u8(0x0E).u8(0xAC).u8(0x44).zeros(6);
  Bytes c;
  c.str("COMM").be32(18).raw(comm.b.data(), comm.b.size());
  c.str("NAME").be32(4).str("Tone");
  Bytes f;
  f.str("FORM").be32(static_cast<uint32_t>(4 + c.b.size())).str("AIFF").raw(c.b.data(), c.b.size());
  return f.b;
}

std::string streamInfo() {
  Bytes si;
  si.be16(4096).be16(4096).zeros(6);
  const uint64_t packed = (uint64_t(48000) << 44) | (uint64_t(1) << 41) | (uint64_t(15) << 36) | 48000;
  for (int k = 7; k >= 0; --k) si.u8(static_cast<uint32_t>(packed >> (k * 8)));
  si.zeros(16);
  return si.b;
}

std::string vorbisComment(const char* entry) {
  Bytes vc;
  vc.le32(1).str("x").le32(1).le32(static_cast<uint32_t>(std::strlen(entry))).str(entry);
  return vc.b;
}

std::string makeFlac(size_t paddingBytes) {
  Bytes f;
  f.str("fLaC");
  const std::string si = streamInfo();
  f.u8(0x00).u8(0).u8(0).u8(34).raw(si.data(), si.size());
  if (paddingBytes) {
    const uint32_t n = static_cast<uint32_t>(paddingBytes);
    f.u8(0x01).u8(n >> 16).u8(n >> 8).u8(n).zeros(paddingBytes);
  }
  const std::string vc = vorbisComment("TITLE=After Padding");
  const uint32_t n = static_cast<uint32_t>(vc.size());
  f.u8(0x84).u8(n >> 16).u8(n >> 8).u8(n).raw(vc.data(), vc.size());
  return f.b;
}

Ptr<sq::IIndexResult> runQuery(sq::IContentIndex* ix, const char* text, const char* tagsAll = nullptr,
                               const char* kind = nullptr) {
  sq_index_query q{};
  q.struct_size = sizeof q;
  q.text = text;
  q.tags_all = tagsAll;
  q.kind = kind;
  Ptr<sq::IIndexResult> r;
  if (ix->query(&q, r.put()) != SQ_OK) return {};
  return r;
}

sq_index_scan_status scan(sq::IIndexScanner* sc, sq::IContentIndex* ix, const std::string& root, uint32_t flags,
                          const char* exts = nullptr) {
  sq_index_scan_options o{};
  o.struct_size = sizeof o;
  o.root = root.c_str();
  o.flags = flags;
  o.extensions = exts;
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

bool put(sq::IContentIndex* ix, const char* url, const char* title, const char* tags, const char* kind = "audio") {
  sq_index_item it{};
  it.struct_size = sizeof it;
  it.url = url;
  it.kind = kind;
  it.title = title;
  it.tags = tags;
  return ix->putItem(&it, nullptr, nullptr) == SQ_OK;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

SQ_TEST("hardening: a foreign SQLite database is refused and left untouched") {
  TempDir d("foreign");
  const std::string path = d / "other.db";
  {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "CREATE TABLE Settings(k TEXT, v TEXT); INSERT INTO Settings VALUES('a','b');", nullptr,
                         nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
  }
  auto ix = createContentIndex();
  CHECK_EQ(ix->open(path.c_str(), 0), SQ_E_PARSE);
  CHECK_EQ(ix->open(path.c_str(), SQ_INDEX_CREATE), SQ_E_PARSE);
  CHECK_EQ(ix->open(path.c_str(), SQ_INDEX_READONLY), SQ_E_PARSE);
  {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* s = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master", -1, &s, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(s) == SQLITE_ROW);
    CHECK_EQ(sqlite3_column_int(s, 0), 1);  // only the foreign table: no index schema was added
    sqlite3_finalize(s);
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &s, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(s) == SQLITE_ROW);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, 0))), std::string("delete"));
    sqlite3_finalize(s);
    sqlite3_close(db);
  }
  // An empty (zero-byte or freshly created) sqlite file is still fine to initialise.
  const std::string empty = d / "empty.tags.db";
  sq::writeFile(empty, "");
  CHECK_OK(ix->open(empty.c_str(), 0));
  CHECK_OK(ix->close());
}

SQ_TEST("hardening: non-ASCII tags are case-insensitive") {
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
  REQUIRE(put(ix.get(), "a", "Strings", U8("Ärger;СИНТЕЗ;Δέλτα")));
  auto r = runQuery(ix.get(), nullptr, U8("ärger"));
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 1u);
  r = runQuery(ix.get(), nullptr, U8("синтез"));  // cyrillic lowercase
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 1u);
  sq_index_row row{};
  row.struct_size = sizeof row;
  REQUIRE_OK(r->getRow(0, &row));
  CHECK_EQ(std::string(row.tags), std::string(U8("ärger;синтез;δέλτα")));
  // listTags prefix folds case the same way
  int seen = 0;
  CHECK_OK(ix->listTags(U8("ÄR"), [](void* u, const char*, uint64_t) { ++*static_cast<int*>(u); }, &seen));
  CHECK_EQ(seen, 1);
  // splitTags dedupes across case
  CHECK_EQ(splitTags(U8("Été;éTÉ")).size(), size_t(1));
}

SQ_TEST("hardening: separator-only words in search text are ignored") {
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
  REQUIRE(put(ix.get(), "1", "Warm Pad", "pad"));
  REQUIRE(put(ix.get(), "2", "Kick", "drums"));
  auto base = runQuery(ix.get(), "pad");
  REQUIRE(base);
  CHECK_EQ(base->getCount(), 1u);
  for (const char* t : {U8("pad —"), U8("— pad §"), U8("pad 　"), "pad \"*^-:()", U8("pad ’")}) {
    auto r = runQuery(ix.get(), t);
    REQUIRE(r);
    CHECK_EQ(r->getCount(), 1u);
  }
  // text with no word at all: same as no text
  auto all = runQuery(ix.get(), U8("— …"));
  REQUIRE(all);
  CHECK_EQ(all->getCount(), 2u);
  // Non-ASCII words still match (diacritics folded)
  REQUIRE(put(ix.get(), "3", U8("Café Brücke"), ""));
  auto u = runQuery(ix.get(), "bruck");
  REQUIRE(u);
  CHECK_EQ(u->getCount(), 1u);
}

SQ_TEST("hardening: extension-filtered scans prune only their own types") {
  TempDir lib("prune");
  sq::writeFile(lib / "a.wav", makeWav(10));
  sq::writeFile(lib / "b.mid", makeMidi());
  sq::writeFile(lib / "c.aif", makeAiff());
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
  auto sc = createIndexScanner();
  auto st = scan(sc.get(), ix.get(), lib.path, SQ_INDEX_SCAN_DEFAULT);
  CHECK_EQ(st.files_indexed, uint64_t(3));
  // re-scan WAV only, with pruning: the MIDI and AIFF items must stay
  st = scan(sc.get(), ix.get(), lib.path, SQ_INDEX_SCAN_DEFAULT, "wav");
  CHECK_EQ(st.state, SQ_INDEX_SCAN_DONE);
  CHECK_EQ(st.files_removed, uint64_t(0));
  auto r = runQuery(ix.get(), nullptr);
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 3u);
  // a removed wav is still pruned by the filtered scan
  sq_fs_remove((lib / "a.wav").c_str());
  st = scan(sc.get(), ix.get(), lib.path, SQ_INDEX_SCAN_DEFAULT, "wav");
  CHECK_EQ(st.files_removed, uint64_t(1));
  r = runQuery(ix.get(), nullptr);
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 2u);
}

#ifdef _WIN32
SQ_TEST("hardening: directory junction loops terminate without duplicates") {
  TempDir lib("loop");
  sq_fs_create_dirs((lib / "Sub").c_str());
  sq::writeFile(lib / "Sub/one.wav", makeWav(10));
  std::string target = lib.path, link = lib / "Sub/Back";
  for (char& c : target)
    if (c == '/') c = '\\';
  for (char& c : link)
    if (c == '/') c = '\\';
  const std::string cmd = "cmd /c mklink /J \"" + link + "\" \"" + target + "\" >NUL 2>&1";
  if (std::system(cmd.c_str()) != 0 || !sq_fs_is_dir((lib / "Sub/Back").c_str())) {
    std::printf("  (junction could not be created, skipping loop check)\n");
    CHECK(false);  // never pass silently
    return;
  }
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
  auto sc = createIndexScanner();
  const auto t0 = std::chrono::steady_clock::now();
  auto st = scan(sc.get(), ix.get(), lib.path, SQ_INDEX_SCAN_DEFAULT);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK_EQ(st.state, SQ_INDEX_SCAN_DONE);
  CHECK(secs < 20.0);
  auto r = runQuery(ix.get(), nullptr);
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 1u);  // one.wav once, not once per loop level
  // remove the junction itself (not its target) before TempDir cleans up
  RemoveDirectoryA(link.c_str());
}

SQ_TEST("hardening: an unreadable sub folder does not get its items pruned") {
  TempDir lib("acl");
  sq_fs_create_dirs((lib / "Locked").c_str());
  sq::writeFile(lib / "Locked/keep.wav", makeWav(10));
  sq::writeFile(lib / "top.wav", makeWav(10));
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
  auto sc = createIndexScanner();
  auto st = scan(sc.get(), ix.get(), lib.path, SQ_INDEX_SCAN_DEFAULT);
  CHECK_EQ(st.files_indexed, uint64_t(2));
  std::string locked = lib / "Locked";
  for (char& c : locked)
    if (c == '/') c = '\\';
  const std::string deny = "icacls \"" + locked + "\" /deny *S-1-1-0:(RD) >NUL 2>&1";
  const std::string undo = "icacls \"" + locked + "\" /remove:d *S-1-1-0 >NUL 2>&1";
  if (std::system(deny.c_str()) != 0) {
    std::printf("  (icacls failed, skipping)\n");
    CHECK(false);
    return;
  }
  st = scan(sc.get(), ix.get(), lib.path, SQ_INDEX_SCAN_DEFAULT | SQ_INDEX_SCAN_FORCE);
  std::system(undo.c_str());
  CHECK_EQ(st.state, SQ_INDEX_SCAN_DONE);
  CHECK_EQ(st.files_removed, uint64_t(0));
  CHECK(st.errors >= 1u);  // the folder that could not be listed is reported
  auto r = runQuery(ix.get(), nullptr);
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 2u);
}
#endif

SQ_TEST("hardening: malformed and truncated files never crash or hang the readers") {
  TempDir d("fuzz");
  std::vector<std::pair<std::string, std::string>> seeds = {
      {"wav", makeWav(64)}, {"aif", makeAiff()}, {"flac", makeFlac(0)}, {"mid", makeMidi()},
      {"preset", "<MetaInfo><Attribute id=\"Document:Title\" value=\"X\"/></MetaInfo>"}};
  // RF64 with a hostile ds64 (data size near 2^64) followed by a LIST chunk
  {
    Bytes ds64;
    ds64.le64(0).le64(0xFFFFFFFFFFFFFFF0ull).le64(0).le32(0);
    Bytes fmt;
    fmt.le16(1).le16(1).le32(44100).le32(44100).le16(1).le16(8);
    std::string body = "WAVE" + chunk("ds64", ds64.b) + chunk("fmt ", fmt.b);
    Bytes data;
    data.str("data").le32(0xFFFFFFFFu).zeros(16);
    body += data.b + chunk("LIST", "INFO" + chunk("INAM", "Evil"));
    Bytes f;
    f.str("RF64").le32(0xFFFFFFFFu).raw(body.data(), body.size());
    seeds.emplace_back("wav", f.b);
  }
  std::mt19937 rng(1234);
  int files = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& [ext, good] : seeds) {
    for (int v = 0; v < 120; ++v) {
      std::string bytes = good;
      if (v < 40) {
        bytes.resize(bytes.size() * static_cast<size_t>(v) / 40);  // truncations
      } else {
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips && !bytes.empty(); ++k) bytes[rng() % bytes.size()] = static_cast<char>(rng());
      }
      const std::string p = d / ("f" + std::to_string(files++) + "." + ext);
      sq::writeFile(p, bytes);
      FileInfo fi;
      extractFileInfo(p, fi);  // must return, whatever the content
      if (const MetaValue* m = fi.find("audio.frames")) CHECK(m->i >= 0);
      if (const MetaValue* m = fi.find("audio.duration")) CHECK(m->d >= 0.0 && m->d == m->d);
      if (const MetaValue* m = fi.find("midi.duration")) CHECK(m->d >= 0.0 && m->d == m->d);
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  %d malformed files read in %.2f s\n", files, secs);
  CHECK(secs < 30.0);
  // the hostile RF64: data size clamped to what the file holds, never negative / wrapped
  FileInfo evil;
  const std::string ep = d / "evil.wav";
  sq::writeFile(ep, seeds.back().second);
  REQUIRE(extractFileInfo(ep, evil));
  CHECK_EQ(evil.title, std::string("evil"));  // data runs to EOF: nothing after it is trusted
  REQUIRE(evil.find("audio.frames"));
  CHECK(evil.find("audio.frames")->i >= 0);
  CHECK(evil.find("audio.frames")->i <= static_cast<int64_t>(seeds.back().second.size()));
}

SQ_TEST("hardening: large skipped FLAC blocks are seeked over") {
  TempDir d("flac");
  const std::string p = d / "pad.flac";
  sq::writeFile(p, makeFlac(15u * 1024u * 1024u));  // 15 MB padding block before the tags
  FileInfo fi;
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(extractFileInfo(p, fi));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  flac with 15 MB padding: %.2f ms\n", ms);
  CHECK_EQ(fi.title, std::string("After Padding"));
  REQUIRE(fi.find("audio.sample_rate"));
  CHECK_EQ(fi.find("audio.sample_rate")->i, int64_t(48000));
  // a block length running past the end of file stops cleanly
  std::string trunc = makeFlac(0);
  trunc += std::string("\x81\xFF\xFF\xFF", 4);
  sq::writeFile(p, trunc);
  FileInfo t;
  CHECK(extractFileInfo(p, t));
}

// ---- codec fallback through a mocked ICodecService -------------------------------------------

namespace {

class FakeReader final : public sq::Object<FakeReader, sq::IAudioFileReader> {
 public:
  sq_result SQ_CALL open(const char* path) override { return path ? SQ_OK : SQ_E_INVALIDARG; }
  sq_result SQ_CALL openStream(sq::IByteStream*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override {
    if (!out) return SQ_E_INVALIDARG;
    out->sample_rate = 44100.0;
    out->channels = 2;
    out->bits_per_sample = 16;
    out->frame_count = 88200;
    std::strcpy(out->container, "mp3");
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block*, uint32_t*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL seek(sq_sample_time) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL tell(sq_sample_time*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getMetadataCount() override { return 3; }
  sq_result SQ_CALL getMetadataKey(uint32_t i, char* buf, uint32_t size, uint32_t* need) override {
    static const char* keys[] = {"tag:title", "tag:genre", "chunk:ID3 "};
    if (i >= 3) return SQ_E_NOTFOUND;
    const uint32_t n = static_cast<uint32_t>(std::strlen(keys[i]) + 1);
    if (need) *need = n;
    if (!buf || size < n) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, keys[i], n);
    return SQ_OK;
  }
  sq_result SQ_CALL getMetadata(const char* key, void* buf, uint32_t size, uint32_t* need) override {
    std::string v;
    if (std::strcmp(key, "tag:title") == 0) v = "Codec Title";
    else if (std::strcmp(key, "tag:genre") == 0) v = "Techno";
    else if (std::strcmp(key, "chunk:ID3 ") == 0) v = "raw";
    else return SQ_E_NOTFOUND;
    if (need) *need = static_cast<uint32_t>(v.size());
    if (!buf || size < v.size()) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, v.data(), v.size());
    return SQ_OK;
  }
  uint32_t SQ_CALL getMarkerCount() override { return 0; }
  sq_result SQ_CALL getMarker(uint32_t, sq_audio_file_marker*) override { return SQ_E_NOTFOUND; }
};

class FakeCodec final : public sq::Object<FakeCodec, sq::ICodecService> {
 public:
  static int live;
  FakeCodec() { ++live; }
  ~FakeCodec() override { --live; }
  sq_result SQ_CALL getCodecInfo(sq_codec_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getFormatCount() override { return 0; }
  sq_result SQ_CALL getFormatInfo(uint32_t, sq_codec_format_info*) override { return SQ_E_NOTFOUND; }
  sq_bool SQ_CALL supports(const char* ext, const char*, uint32_t flags) override {
    return (ext && std::strcmp(ext, "mp3") == 0 && (flags & SQ_CODEC_READ)) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void*, uint32_t) override { return 0; }
  sq_result SQ_CALL createReader(sq::IAudioFileReader** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = new FakeReader();
    return SQ_OK;
  }
  sq_result SQ_CALL createWriter(const char*, sq::IAudioFileWriter** out) override {
    if (out) *out = nullptr;
    return SQ_E_NOTIMPL;
  }
};
int FakeCodec::live = 0;

class FakeFactory final : public sq::Object<FakeFactory, sq::IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid*, const sq_guid* riid, void** out) override {
    sq::Ptr<sq::ICodecService> c = sq::Ptr<sq::ICodecService>::adopt(new FakeCodec());
    return c->queryInterface(riid, out);
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

}  // namespace

SQ_TEST("hardening: formats without a built-in reader use a loaded codec service") {
  sq::app::ClassRegistry reg;
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  ci.class_id = sq::codec::kMp3CodecClassId;
  ci.category = sq::codec::kCategory;
  ci.name = "Fake codec";
  auto factory = sq::Ptr<sq::IClassFactory>::adopt(new FakeFactory());
  REQUIRE_OK(reg.registerClass(&ci, factory.get()));
  setClassRegistry(&reg);
  TempDir d("codec");
  const std::string p = d / "song.mp3";
  sq::writeFile(p, "ID3-ish bytes");
  FileInfo fi;
  REQUIRE(extractFileInfo(p, fi));
  setClassRegistry(nullptr);
  CHECK_EQ(fi.title, std::string("Codec Title"));
  REQUIRE(fi.find("audio.sample_rate"));
  CHECK_EQ(fi.find("audio.sample_rate")->i, int64_t(44100));
  CHECK(fi.find("audio.duration") && fi.find("audio.duration")->d == 2.0);
  REQUIRE(fi.find("genre"));
  CHECK_EQ(fi.find("genre")->s, std::string("Techno"));
  CHECK(std::find(fi.tags.begin(), fi.tags.end(), std::string("techno")) != fi.tags.end());
  CHECK_EQ(FakeCodec::live, 0);  // no leaked codec references
  CHECK_OK(reg.unregisterClass(&ci.class_id));
}

SQ_TEST("hardening: results outlive their index and refuse metadata after close") {
  Ptr<sq::IIndexResult> r;
  {
    auto ix = createContentIndex();
    REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
    Ptr<sq::IAttributes> md = sq::makeAttributes();
    md->setString("artist", "Someone");
    sq_index_item it{};
    it.struct_size = sizeof it;
    it.url = "x";
    it.title = "X";
    REQUIRE_OK(ix->putItem(&it, md.get(), nullptr));
    r = runQuery(ix.get(), nullptr);
    REQUIRE(r);
  }  // index pointer released: the result keeps it alive
  Ptr<sq::IAttributes> out = sq::makeAttributes();
  CHECK_OK(r->getMetadata(0, out.get()));
  CHECK_EQ(sq::getAttrString(out.get(), "artist"), std::string("Someone"));
  sq_index_row row{};
  row.struct_size = sizeof row;
  CHECK_OK(r->getRow(0, &row));
  CHECK_EQ(std::string(row.url), std::string("x"));
  row.struct_size = 4;
  CHECK_EQ(r->getRow(0, &row), SQ_E_INVALIDARG);
}

SQ_TEST("hardening: page-sized results are metadata snapshots") {
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(":memory:", SQ_INDEX_CREATE));
  Ptr<sq::IAttributes> md = sq::makeAttributes();
  md->setString("artist", "Before");
  sq_index_item it{};
  it.struct_size = sizeof it;
  it.url = "snap";
  it.title = "Snap";
  REQUIRE_OK(ix->putItem(&it, md.get(), nullptr));
  auto r = runQuery(ix.get(), nullptr);
  REQUIRE(r);
  REQUIRE_EQ(r->getCount(), 1u);
  md->setString("artist", "After");
  REQUIRE_OK(ix->putItem(&it, md.get(), nullptr));  // same id, new metadata
  REQUIRE_OK(ix->removeItem("snap"));
  REQUIRE_OK(ix->close());
  Ptr<sq::IAttributes> out = sq::makeAttributes();
  CHECK_OK(r->getMetadata(0, out.get()));
  CHECK_EQ(sq::getAttrString(out.get(), "artist"), std::string("Before"));
  CHECK_EQ(r->getMetadata(1, out.get()), SQ_E_NOTFOUND);
  CHECK_EQ(r->getMetadata(0, nullptr), SQ_E_INVALIDARG);
}
