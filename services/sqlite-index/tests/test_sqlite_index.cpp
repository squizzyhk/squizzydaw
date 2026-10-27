// sqlite-index: IContentIndex behaviour (schema, tags, FTS, paging, batches) + the M3 exit test
// (1000+ items, search under 50 ms).
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/platform.hpp"
#include "sqlite_index.hpp"
#include "sqtest.hpp"

using sq::Ptr;
using namespace sq::sqlite_index;

namespace {

std::string tempDir(const char* tag) {
  sq_guid g{};
  sq_guid_generate(&g);
  std::string d = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), std::string("sqix-") + tag + "-" + std::to_string(g.data1));
  sq_fs_create_dirs(d.c_str());
  return d;
}

struct TempDir {
  std::string path;
  explicit TempDir(const char* tag) : path(tempDir(tag)) {}
  ~TempDir() { sq_fs_remove_all(path.c_str()); }
};

sq_result put(sq::IContentIndex* ix, const char* url, const char* kind, const char* title, const char* tags,
              int64_t mtime = 0, sq::IAttributes* meta = nullptr, uint64_t* id = nullptr) {
  sq_index_item it{};
  it.struct_size = sizeof it;
  it.url = url;
  it.kind = kind;
  it.title = title;
  it.tags = tags;
  it.mtime = mtime;
  it.size = 1000;
  return ix->putItem(&it, meta, id);
}

struct Q {
  sq_index_query q{};
  Q() { q.struct_size = sizeof q; }
  Q& text(const char* t) { q.text = t; return *this; }
  Q& all(const char* t) { q.tags_all = t; return *this; }
  Q& any(const char* t) { q.tags_any = t; return *this; }
  Q& kind(const char* k) { q.kind = k; return *this; }
  Q& prefix(const char* p) { q.url_prefix = p; return *this; }
  Q& sort(int32_t s) { q.sort = s; return *this; }
  Q& page(uint32_t off, uint32_t lim) { q.offset = off; q.limit = lim; return *this; }
};

Ptr<sq::IIndexResult> run(sq::IContentIndex* ix, const Q& q) {
  Ptr<sq::IIndexResult> r;
  if (ix->query(&q.q, r.put()) != SQ_OK) return {};
  return r;
}

std::vector<std::string> urls(sq::IIndexResult* r) {
  std::vector<std::string> v;
  for (uint32_t i = 0; r && i < r->getCount(); ++i) {
    sq_index_row row{};
    row.struct_size = sizeof row;
    if (r->getRow(i, &row) == SQ_OK) v.emplace_back(row.url);
  }
  return v;
}

Ptr<sq::IContentIndex> openMem() {
  auto ix = createContentIndex();
  if (ix->open(":memory:", SQ_INDEX_CREATE) != SQ_OK) return {};
  return ix;
}

}  // namespace

SQ_TEST("sqlite-index: tag and query helpers") {
  auto t = splitTags(" Drums;LOOP;;drums; 120BPM ;");
  REQUIRE_EQ(t.size(), size_t(3));
  CHECK_EQ(t[0], std::string("drums"));
  CHECK_EQ(t[1], std::string("loop"));
  CHECK_EQ(t[2], std::string("120bpm"));
  CHECK_EQ(joinTags(t), std::string("drums;loop;120bpm"));
  CHECK_EQ(ftsExpression("deep kick-drum \"x"), std::string("\"deep\"* \"kick\"* \"drum\"* \"x\"*"));
  CHECK_EQ(ftsExpression(" -- "), std::string());
  CHECK_EQ(prefixUpperBound("C:/a/"), std::string("C:/a0"));
  CHECK_EQ(prefixUpperBound(""), std::string());
}

SQ_TEST("sqlite-index: open modes and store info") {
  TempDir dir("open");
  const std::string path = sq::joinPath(dir.path, "sub/Library.tags.db");
  auto ix = createContentIndex();
  CHECK_EQ(ix->open(path.c_str(), 0), SQ_E_NOTFOUND);
  CHECK_EQ(ix->open(path.c_str(), SQ_INDEX_CREATE | SQ_INDEX_READONLY), SQ_E_INVALIDARG);
  REQUIRE_OK(ix->open(path.c_str(), SQ_INDEX_CREATE));
  sq_index_store_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(ix->getStoreInfo(&info));
  CHECK_EQ(info.schema_version, kSchemaVersion);
  CHECK_EQ(info.item_count, uint64_t(0));
  CHECK_EQ(std::string(info.name), std::string("Library"));
  CHECK(info.created > 1600000000);
  REQUIRE_OK(ix->setStoreName("My Loops"));
  REQUIRE_OK(put(ix.get(), "file:///a.wav", "audio", "Alpha", "x;y"));
  REQUIRE_OK(ix->close());
  CHECK_EQ(ix->close(), SQ_S_FALSE);
  CHECK_EQ(ix->getStoreInfo(&info), SQ_E_CLOSED);

  // Persistence + read-only open.
  REQUIRE_OK(ix->open(path.c_str(), SQ_INDEX_READONLY));
  REQUIRE_OK(ix->getStoreInfo(&info));
  CHECK_EQ(info.item_count, uint64_t(1));
  CHECK_EQ(std::string(info.name), std::string("My Loops"));
  CHECK_EQ(put(ix.get(), "file:///b.wav", "audio", "Beta", nullptr), SQ_E_ACCESS);
  CHECK_EQ(ix->beginBatch(), SQ_E_ACCESS);
  auto r = run(ix.get(), Q().text("alp"));
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 1u);
  r.reset();
  ix->close();

  // A file that is not a database is rejected.
  const std::string junk = sq::joinPath(dir.path, "junk.tags.db");
  sq::writeFile(junk, "this is not sqlite at all, just text padding padding padding padding padding");
  CHECK(SQ_FAILED(ix->open(junk.c_str(), 0)));
  CHECK(SQ_FAILED(ix->open(junk.c_str(), SQ_INDEX_READONLY)));
}

SQ_TEST("sqlite-index: put, replace, remove and tags") {
  auto ix = openMem();
  REQUIRE(ix);
  uint64_t id1 = 0, id1b = 0, id2 = 0;
  REQUIRE_OK(put(ix.get(), "/lib/kick.wav", "audio", "Deep Kick", "Drums;Kick", 10, nullptr, &id1));
  REQUIRE_OK(put(ix.get(), "/lib/snare.wav", "audio", "Snare Crack", "drums;snare", 20, nullptr, &id2));
  CHECK(id1 != 0 && id2 != 0 && id1 != id2);
  // replace keeps the id and rewrites the tags
  REQUIRE_OK(put(ix.get(), "/lib/kick.wav", "audio", "Deep Kick 2", "drums;808", 11, nullptr, &id1b));
  CHECK_EQ(id1, id1b);
  auto r = run(ix.get(), Q().all("kick"));
  CHECK_EQ(r->getCount(), 0u);
  r = run(ix.get(), Q().all("808"));
  REQUIRE_EQ(r->getCount(), 1u);
  sq_index_row row{};
  row.struct_size = sizeof row;
  REQUIRE_OK(r->getRow(0, &row));
  CHECK_EQ(std::string(row.title), std::string("Deep Kick 2"));
  CHECK_EQ(std::string(row.tags), std::string("drums;808"));
  CHECK_EQ(row.mtime, int64_t(11));
  CHECK_EQ(row.id, id1);

  // tag list: "kick" became unused and is gone
  std::vector<std::pair<std::string, uint64_t>> tags;
  auto collect = [](void* u, const char* t, uint64_t n) {
    static_cast<std::vector<std::pair<std::string, uint64_t>>*>(u)->emplace_back(t, n);
  };
  REQUIRE_OK(ix->listTags(nullptr, collect, &tags));
  REQUIRE_EQ(tags.size(), size_t(3));
  CHECK_EQ(tags[0].first, std::string("808"));
  CHECK_EQ(tags[1].first, std::string("drums"));
  CHECK_EQ(tags[1].second, uint64_t(2));
  CHECK_EQ(tags[2].first, std::string("snare"));
  tags.clear();
  REQUIRE_OK(ix->listTags("DR", collect, &tags));
  REQUIRE_EQ(tags.size(), size_t(1));

  // addTags / removeTags
  REQUIRE_OK(ix->addTags("/lib/snare.wav", "Bright;drums"));
  CHECK_EQ(ix->addTags("/lib/snare.wav", "bright"), SQ_S_FALSE);
  CHECK_EQ(ix->addTags("/lib/none.wav", "x"), SQ_E_NOTFOUND);
  r = run(ix.get(), Q().all("bright;snare"));
  CHECK_EQ(r->getCount(), 1u);
  r = run(ix.get(), Q().text("brig"));  // tags are searchable text
  CHECK_EQ(r->getCount(), 1u);
  REQUIRE_OK(ix->removeTags("/lib/snare.wav", "BRIGHT"));
  r = run(ix.get(), Q().all("bright"));
  CHECK_EQ(r->getCount(), 0u);
  r = run(ix.get(), Q().text("bright"));
  CHECK_EQ(r->getCount(), 0u);

  // remove
  CHECK_EQ(ix->removeItem("/lib/missing.wav"), SQ_S_FALSE);
  REQUIRE_OK(ix->removeItem("/lib/kick.wav"));
  r = run(ix.get(), Q().text("deep"));
  CHECK_EQ(r->getCount(), 0u);
  r = run(ix.get(), Q());
  CHECK_EQ(r->getCount(), 1u);
  CHECK_EQ(r->getTotalCount(), uint64_t(1));

  // removeByPrefix only touches the prefix
  REQUIRE_OK(put(ix.get(), "/lib/sub/a.mid", "midi", "A", nullptr));
  REQUIRE_OK(put(ix.get(), "/lib/sub/b.mid", "midi", "B", nullptr));
  REQUIRE_OK(put(ix.get(), "/lib/subway.mid", "midi", "C", nullptr));
  uint64_t removed = 0;
  REQUIRE_OK(ix->removeByPrefix("/lib/sub/", &removed));
  CHECK_EQ(removed, uint64_t(2));
  CHECK_EQ(urls(run(ix.get(), Q().kind("midi")).get()), std::vector<std::string>{"/lib/subway.mid"});

  // invalid input
  CHECK_EQ(put(ix.get(), "", "audio", "x", nullptr), SQ_E_INVALIDARG);
  CHECK_EQ(ix->putItem(nullptr, nullptr, nullptr), SQ_E_INVALIDARG);
}

SQ_TEST("sqlite-index: metadata round trip and metadata text search") {
  auto ix = openMem();
  REQUIRE(ix);
  auto meta = sq::makeAttributes();
  meta->setString("artist", "Nebula Orchestra");
  meta->setInt("audio.sample_rate", 48000);
  meta->setDouble("audio.duration", 2.5);
  meta->setBool("audio.loop", SQ_TRUE);
  sq_guid g{};
  meta->setGuid("ignored", &g);
  REQUIRE_OK(put(ix.get(), "/x/pad.wav", "audio", "Warm Pad", "pad", 0, meta.get()));
  REQUIRE_OK(put(ix.get(), "/x/other.wav", "audio", "Other", "pad"));

  auto r = run(ix.get(), Q().text("nebula orch"));
  REQUIRE(r);
  REQUIRE_EQ(r->getCount(), 1u);
  auto out = sq::makeAttributes();
  REQUIRE_OK(r->getMetadata(0, out.get()));
  CHECK_EQ(sq::getAttrString(out.get(), "artist"), std::string("Nebula Orchestra"));
  CHECK_EQ(out->getType("audio.sample_rate"), SQ_VALUE_INT);
  CHECK_EQ(out->getInt("audio.sample_rate", 0), int64_t(48000));
  CHECK_EQ(out->getType("audio.duration"), SQ_VALUE_DOUBLE);
  CHECK(out->getDouble("audio.duration", 0) == 2.5);
  CHECK_EQ(out->getType("audio.loop"), SQ_VALUE_BOOL);
  CHECK_EQ(out->getType("ignored"), SQ_VALUE_NONE);
  // key order preserved
  char key[64];
  uint32_t need = 0;
  REQUIRE_OK(out->getKey(0, key, sizeof key, &need));
  CHECK_EQ(std::string(key), std::string("artist"));
  CHECK_EQ(r->getMetadata(5, out.get()), SQ_E_NOTFOUND);
  // the snapshot refuses lazy reads once the store was closed
  ix->close();
  auto out2 = sq::makeAttributes();
  CHECK_EQ(r->getMetadata(0, out2.get()), SQ_OK);  // cached
  CHECK_EQ(ix->query(nullptr, r.put()), SQ_E_CLOSED);
}

SQ_TEST("sqlite-index: filters, sorting and paging") {
  auto ix = openMem();
  REQUIRE(ix);
  REQUIRE_OK(ix->beginBatch());
  REQUIRE_OK(put(ix.get(), "/a/1", "audio", "Charlie", "drums;loop", 300));
  REQUIRE_OK(put(ix.get(), "/a/2", "audio", "alpha", "drums", 100));
  REQUIRE_OK(put(ix.get(), "/a/3", "midi", "Bravo", "loop;keys", 200));
  REQUIRE_OK(put(ix.get(), "/b/4", "preset", "Delta drums", "synth", 400));
  REQUIRE_OK(ix->commitBatch());
  CHECK_EQ(ix->commitBatch(), SQ_S_FALSE);

  CHECK_EQ(urls(run(ix.get(), Q().sort(SQ_INDEX_SORT_TITLE)).get()),
           (std::vector<std::string>{"/a/2", "/a/3", "/a/1", "/b/4"}));
  CHECK_EQ(urls(run(ix.get(), Q().sort(SQ_INDEX_SORT_MTIME)).get()),
           (std::vector<std::string>{"/b/4", "/a/1", "/a/3", "/a/2"}));
  CHECK_EQ(urls(run(ix.get(), Q().all("drums;loop")).get()), std::vector<std::string>{"/a/1"});
  CHECK_EQ(urls(run(ix.get(), Q().any("keys;synth").sort(SQ_INDEX_SORT_TITLE)).get()),
           (std::vector<std::string>{"/a/3", "/b/4"}));
  CHECK_EQ(urls(run(ix.get(), Q().kind("audio").sort(SQ_INDEX_SORT_TITLE)).get()),
           (std::vector<std::string>{"/a/2", "/a/1"}));
  CHECK_EQ(urls(run(ix.get(), Q().prefix("/b/")).get()), std::vector<std::string>{"/b/4"});
  // text matches title words OR tags; title hit ranks above a tag-only hit
  auto r = run(ix.get(), Q().text("drums"));
  auto u = urls(r.get());
  REQUIRE_EQ(u.size(), size_t(3));
  CHECK_EQ(u[0], std::string("/b/4"));
  sq_index_row row{};
  row.struct_size = sizeof row;
  REQUIRE_OK(r->getRow(0, &row));
  CHECK(row.score > 0.0);
  // paging + total
  r = run(ix.get(), Q().sort(SQ_INDEX_SORT_TITLE).page(1, 2));
  CHECK_EQ(urls(r.get()), (std::vector<std::string>{"/a/3", "/a/1"}));
  CHECK_EQ(r->getTotalCount(), uint64_t(4));
  r = run(ix.get(), Q().page(10, 2));
  CHECK_EQ(r->getCount(), 0u);
  CHECK_EQ(r->getTotalCount(), uint64_t(4));
  r = run(ix.get(), Q().page(3, 0));
  CHECK_EQ(r->getCount(), 1u);
  CHECK_EQ(r->getTotalCount(), uint64_t(4));

  // rollback discards the whole batch
  REQUIRE_OK(ix->beginBatch());
  REQUIRE_OK(put(ix.get(), "/c/5", "audio", "Echo", nullptr));
  REQUIRE_OK(ix->removeItem("/a/1"));
  REQUIRE_OK(ix->rollbackBatch());
  CHECK_EQ(run(ix.get(), Q())->getTotalCount(), uint64_t(4));
  CHECK_EQ(run(ix.get(), Q().text("echo"))->getCount(), 0u);
}

// M3 exit criterion: index 1000+ synthetic items; a search returns correct results in < 50 ms.
SQ_TEST("sqlite-index: 5000 synthetic items, searches correct and under 50 ms") {
  TempDir dir("perf");
  const std::string path = sq::joinPath(dir.path, "perf.tags.db");
  auto ix = createContentIndex();
  REQUIRE_OK(ix->open(path.c_str(), SQ_INDEX_CREATE));
  static const char* kInstr[] = {"kick", "snare", "hat", "bass", "pad", "lead", "pluck", "vocal", "fx", "chord"};
  static const char* kGenre[] = {"house", "techno", "ambient", "hiphop", "jazz"};
  static const char* kKinds[] = {"audio", "midi", "preset"};
  const int N = 5000;
  const uint64_t t0 = sq_time_monotonic_ns();
  REQUIRE_OK(ix->beginBatch());
  for (int i = 0; i < N; ++i) {
    const char* instr = kInstr[i % 10];
    const char* genre = kGenre[(i / 10) % 5];
    const char* kind = kKinds[i % 3];
    const int bpm = 80 + (i % 7) * 10;
    char url[128], title[128], tags[128];
    std::snprintf(url, sizeof url, "/library/%s/%s_%05d.wav", genre, instr, i);
    std::snprintf(title, sizeof title, "%s %s %d", genre, instr, i);
    std::snprintf(tags, sizeof tags, "%s;%s;%dbpm%s", instr, genre, bpm, (i % 4 == 3) ? ";loop" : "");
    auto meta = sq::makeAttributes();
    meta->setString("artist", (i % 100 == 0) ? "Zyxwv Unique" : "Studio Library");
    meta->setInt("audio.sample_rate", 48000);
    REQUIRE_OK(put(ix.get(), url, kind, title, tags, 1000 + i, meta.get()));
  }
  REQUIRE_OK(ix->commitBatch());
  const double indexMs = (sq_time_monotonic_ns() - t0) / 1e6;
  std::printf("  indexed %d items in %.1f ms\n", N, indexMs);

  sq_index_store_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(ix->getStoreInfo(&info));
  CHECK_EQ(info.item_count, uint64_t(N));

  // Brute-force expectation for: text "techno bass", tags_all "loop", kind "audio".
  std::set<std::string> expected;
  for (int i = 0; i < N; ++i) {
    if (std::string(kGenre[(i / 10) % 5]) == "techno" && std::string(kInstr[i % 10]) == "bass" && i % 4 == 3 &&
        i % 3 == 0) {
      char url[128];
      std::snprintf(url, sizeof url, "/library/techno/bass_%05d.wav", i);
      expected.insert(url);
    }
  }
  REQUIRE(!expected.empty());

  double worst = 0.0;
  for (int rep = 0; rep < 5; ++rep) {
    const uint64_t q0 = sq_time_monotonic_ns();
    auto r = run(ix.get(), Q().text("techno bas").all("loop").kind("audio").page(0, 50));
    const double ms = (sq_time_monotonic_ns() - q0) / 1e6;
    worst = std::max(worst, ms);
    REQUIRE(r);
    auto got = urls(r.get());
    CHECK_EQ(r->getTotalCount(), uint64_t(expected.size()));
    CHECK_EQ(std::set<std::string>(got.begin(), got.end()), expected);
  }
  std::printf("  text+tags+kind search: worst %.2f ms (%zu hits)\n", worst, expected.size());
  CHECK(worst < 50.0);

  // Broad query with paging: 1/10 of the library, first page of 100, total must be exact.
  uint64_t q0 = sq_time_monotonic_ns();
  auto r = run(ix.get(), Q().any("pad;lead").sort(SQ_INDEX_SORT_TITLE).page(100, 100));
  double ms = (sq_time_monotonic_ns() - q0) / 1e6;
  REQUIRE(r);
  CHECK_EQ(r->getCount(), 100u);
  CHECK_EQ(r->getTotalCount(), uint64_t(N / 5));
  std::printf("  tags_any page query: %.2f ms\n", ms);
  CHECK(ms < 50.0);

  // Metadata text search: the rare artist.
  q0 = sq_time_monotonic_ns();
  r = run(ix.get(), Q().text("zyxwv"));
  ms = (sq_time_monotonic_ns() - q0) / 1e6;
  REQUIRE(r);
  CHECK_EQ(r->getCount(), uint32_t(N / 100));
  std::printf("  metadata text query: %.2f ms\n", ms);
  CHECK(ms < 50.0);

  // Tag counts.
  uint64_t loopCount = 0;
  REQUIRE_OK(ix->listTags("loop", [](void* u, const char* t, uint64_t n) {
    if (std::string(t) == "loop") *static_cast<uint64_t*>(u) = n;
  }, &loopCount));
  CHECK_EQ(loopCount, uint64_t(N / 4));
  ix->close();
}
