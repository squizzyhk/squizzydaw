// engine/doc: adversarial regression tests (verifier pass). Each case targets a defect found in
// review: transaction-level escapes by external actions, merge-key leakage, routing cycles,
// malformed archives (cycles / non-finite numbers / bad kinds), UTF-8 truncation, observer cycles.
#include <cmath>
#include <limits>

#include "doc_test_util.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/platform/text.h"

using namespace doctest;
using sq::Ptr;

namespace {

constexpr sq_guid kOverCommit = sq::guid("{D2AE5B07-17F4-43D5-BC18-8B56D50F3F42}");
constexpr sq_guid kOverCancel = sq::guid("{E228D8D2-DF71-4C84-9498-30C3C81F051C}");
constexpr sq_guid kCloser = sq::guid("{58378652-8FD6-4DF1-932C-802A05431375}");

sq::edoc::DocumentManagerImpl* gMgr = nullptr;  // for kCloser

// External macro that misbehaves with the transaction stack.
class BadMacro final : public sq::Object<BadMacro, sq::IAction> {
 public:
  explicit BadMacro(sq_guid kind) : kind_(kind) {}
  sq_result SQ_CALL perform(sq::IDocument* doc, sq::IEditor* ed, sq::IAttributes*, sq::IAttributes*) override {
    addTrack(ed, SQ_TRACK_AUDIO, "Leak");
    if (kind_ == kOverCommit) {
      lastInner = ed->commitTransaction();   // tries to close the level execute() opened
      lastInner2 = ed->commitTransaction();  // ... and the implicit outer one (commits the partial edit)
      return SQ_E_FAIL;                      // ... and then fails
    }
    if (kind_ == kOverCancel) {
      lastInner = ed->cancelTransaction();
      lastInner2 = ed->cancelTransaction();  // would cancel the caller's own transaction
      return SQ_OK;
    }
    lastInner = gMgr ? gMgr->closeDocument(doc) : SQ_E_FAIL;
    return SQ_OK;
  }
  static inline sq_result lastInner = SQ_OK, lastInner2 = SQ_OK;

 private:
  sq_guid kind_;
};

class BadFactory final : public sq::Object<BadFactory, sq::IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override {
    Ptr<BadMacro> m = Ptr<BadMacro>::adopt(new BadMacro(*classId));
    return m->queryInterface(riid, out);
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

struct RegFx {
  sq::app::ClassRegistry reg;
  Ptr<BadFactory> fac = Ptr<BadFactory>::adopt(new BadFactory());
  Ptr<sq::edoc::DocumentManagerImpl> mgr;
  Ptr<sq::IDocument> doc;
  Ptr<sq::IEditor> ed;
  sq::edoc::DocumentImpl* impl = nullptr;
  RegFx() {
    for (const sq_guid& id : {kOverCommit, kOverCancel, kCloser}) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      ci.class_id = id;
      ci.category = "Action";
      ci.name = "Bad macro";
      REQUIRE_OK(reg.registerClass(&ci, fac.get()));
    }
    mgr = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl(&reg));
    REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, doc.put()));
    REQUIRE_OK(doc->getEditor(ed.put()));
    impl = sq::edoc::DocumentImpl::from(doc.get());
  }
};

// Re-serializes a (deliberately corrupted) store and reads it back.
sq_result roundTrip(const sq::edoc::Store& s, sq::edoc::Store& out) {
  const std::string bytes = sq::edoc::writeArchive(s);
  REQUIRE(!bytes.empty());
  return sq::edoc::readArchive(bytes, out);
}

}  // namespace

SQ_TEST("verify: an external action cannot commit the level execute() opened (no partial edit leaks)") {
  RegFx f;
  const std::string h0 = f.impl->stateHash();
  const size_t tracks = f.impl->store().trackOrder.size();
  CHECK(SQ_FAILED(f.ed->execute(&kOverCommit, nullptr, nullptr)));
  CHECK(SQ_FAILED(BadMacro::lastInner));  // the over-commits themselves are refused
  CHECK(SQ_FAILED(BadMacro::lastInner2));
  CHECK_EQ(f.impl->store().trackOrder.size(), tracks);
  CHECK_EQ(f.ed->getUndoCount(), 0u);
  CHECK(!f.ed->inTransaction());
  CHECK_EQ(f.impl->stateHash(), h0);
  CHECK(!f.impl->modified());
}

SQ_TEST("verify: an external action cannot cancel the caller's transaction") {
  RegFx f;
  REQUIRE_OK(f.ed->beginTransaction("outer"));
  const sq_guid keep = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Keep");
  const sq_result r = f.ed->execute(&kOverCancel, nullptr, nullptr);
  CHECK(SQ_FAILED(BadMacro::lastInner));
  CHECK(SQ_FAILED(BadMacro::lastInner2));
  CHECK_OK(r);
  CHECK(f.ed->inTransaction() == SQ_TRUE);
  REQUIRE_OK(f.ed->commitTransaction());
  CHECK(f.impl->store().track(keep) != nullptr);
  CHECK_EQ(f.ed->getUndoCount(), 1u);
  char desc[64] = {0};
  CHECK_OK(f.ed->getUndoDescription(0, desc, sizeof desc, nullptr));
  CHECK_EQ(std::string(desc), std::string("outer"));
}

SQ_TEST("verify: closing a document from inside one of its actions is refused") {
  RegFx f;
  gMgr = f.mgr.get();
  const sq_result r = f.ed->execute(&kCloser, nullptr, nullptr);
  gMgr = nullptr;
  CHECK_EQ(BadMacro::lastInner, SQ_E_BUSY);
  CHECK_OK(r);
  CHECK_EQ(f.mgr->getDocumentCount(), 1u);
  CHECK_EQ(f.ed->getUndoCount(), 1u);
  CHECK(!f.ed->inTransaction());
}

SQ_TEST("verify: a merge key only merges transactions made solely of that merge edit") {
  Fx f;
  const sq_guid t = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "T");
  const uint32_t base = f.ed->getUndoCount();
  CHECK_OK(setProp(f.ed.get(), t, "volumeDb", -1.0, true));
  CHECK_OK(setProp(f.ed.get(), t, "volumeDb", -2.0, true));
  CHECK_EQ(f.ed->getUndoCount(), base + 1);  // the legit drag merge still works
  // a grouped transaction that also adds a marker must not be swallowed by the drag step
  REQUIRE_OK(f.ed->beginTransaction("drag+marker"));
  CHECK_OK(setProp(f.ed.get(), t, "volumeDb", -3.0, true));
  auto mk = sq::makeAttributes();
  mk->setDouble("position", 1.0);
  CHECK_OK(exec(f.ed.get(), sq::doc::kAddMarker, mk.get()));
  REQUIRE_OK(f.ed->commitTransaction());
  CHECK_EQ(f.ed->getUndoCount(), base + 2);
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.st().markers.size(), 0u);
  CHECK_EQ(f.st().track(t)->volumeDb, -2.0);  // only the grouped step was undone
}

SQ_TEST("verify: output routing cycles are rejected") {
  Fx f;
  const sq_guid a = addTrack(f.ed.get(), SQ_TRACK_BUS, "A");
  const sq_guid b = addTrack(f.ed.get(), SQ_TRACK_BUS, "B");
  const sq_guid c = addTrack(f.ed.get(), SQ_TRACK_BUS, "C");
  auto route = [&](const sq_guid& from, const sq_guid& to) {
    auto p = sq::makeAttributes();
    p->setGuid("node", &from);
    p->setString("key", "output");
    p->setGuid("value", &to);
    return exec(f.ed.get(), sq::doc::kSetProperty, p.get());
  };
  CHECK_OK(route(a, b));
  CHECK_OK(route(b, c));
  CHECK_EQ(route(c, a), SQ_E_INVALIDARG);  // A -> B -> C -> A
  CHECK_EQ(route(b, a), SQ_E_INVALIDARG);  // A -> B -> A
  CHECK(sq_guid_is_null(&f.st().track(c)->output));
}

SQ_TEST("verify: getMarker never cuts a UTF-8 sequence in half") {
  Fx f;
  std::string name(126, 'a');
  name += "\xC3\xA9";  // 128 bytes: the 2-byte char straddles the 127-byte limit
  auto mk = sq::makeAttributes();
  mk->setDouble("position", 0.0);
  mk->setString("name", name.c_str());
  CHECK_OK(exec(f.ed.get(), sq::doc::kAddMarker, mk.get()));
  sq_doc_marker m{};
  m.struct_size = sizeof m;
  REQUIRE_OK(f.doc->getMarker(0, &m));
  const size_t len = std::strlen(m.name);
  CHECK_EQ(len, 126u);
  CHECK(sq_utf8_validate(m.name, len) == SQ_TRUE);
}

SQ_TEST("verify: malformed archives with structural cycles are rejected, dangling refs dropped") {
  Fx f;
  const sq_guid fa = addTrack(f.ed.get(), SQ_TRACK_FOLDER, "FA");
  const sq_guid fb = addTrack(f.ed.get(), SQ_TRACK_FOLDER, "FB");
  const sq_guid au = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Au");
  const sq_guid b1 = addTrack(f.ed.get(), SQ_TRACK_BUS, "B1");
  const sq_guid b2 = addTrack(f.ed.get(), SQ_TRACK_BUS, "B2");
  sq::edoc::Store out;
  {  // folder parent cycle FA <-> FB (previously: descendants() spun forever on the next RemoveNode)
    sq::edoc::Store s = f.st();
    s.tracks[fa].parent = fb;
    s.tracks[fb].parent = fa;
    CHECK_EQ(roundTrip(s, out), SQ_E_PARSE);
  }
  {  // parent that is not a folder
    sq::edoc::Store s = f.st();
    s.tracks[fa].parent = au;
    CHECK_EQ(roundTrip(s, out), SQ_E_PARSE);
  }
  {  // output cycle B1 <-> B2
    sq::edoc::Store s = f.st();
    s.tracks[b1].output = b2;
    s.tracks[b2].output = b1;
    CHECK_EQ(roundTrip(s, out), SQ_E_PARSE);
  }
  {  // dangling parent / output: dropped, document still opens
    sq::edoc::Store s = f.st();
    s.tracks[au].parent = sq::edoc::newGuid();
    s.tracks[au].output = sq::edoc::newGuid();
    REQUIRE_OK(roundTrip(s, out));
    CHECK(sq_guid_is_null(&out.tracks.at(au).parent));
    CHECK(sq_guid_is_null(&out.tracks.at(au).output));
  }
}

SQ_TEST("verify: malformed archives with non-finite or out-of-range numbers are rejected") {
  Fx f;
  const sq_guid au = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Au");
  const sq_guid clip = addClip(f.ed.get(), au, SQ_CLIP_AUDIO, 1.0, 2.0, "x.wav");
  auto mk = sq::makeAttributes();
  mk->setDouble("position", 1.0);
  sq_guid marker{};
  CHECK_OK(exec(f.ed.get(), sq::doc::kAddMarker, mk.get(), &marker));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  sq::edoc::Store out;
  auto expectReject = [&](auto mutate, const char* what) {
    sq::edoc::Store s = f.st();
    mutate(s);
    const sq_result r = roundTrip(s, out);
    if (r != SQ_E_PARSE) std::printf("  not rejected: %s (%d)\n", what, r);
    CHECK_EQ(r, SQ_E_PARSE);
  };
  expectReject([&](sq::edoc::Store& s) { s.clips[clip].start = nan; }, "clip start NaN");
  expectReject([&](sq::edoc::Store& s) { s.clips[clip].start = -1.0; }, "clip start < 0");
  expectReject([&](sq::edoc::Store& s) { s.clips[clip].length = 0.0; }, "clip length 0");
  expectReject([&](sq::edoc::Store& s) { s.clips[clip].length = inf; }, "clip length inf");
  expectReject([&](sq::edoc::Store& s) { s.clips[clip].kind = 7; }, "clip kind 7");
  expectReject([&](sq::edoc::Store& s) { s.clips[clip].gainDb = nan; }, "clip gain NaN");
  expectReject([&](sq::edoc::Store& s) { s.markers[marker].position = nan; }, "marker NaN");
  expectReject([&](sq::edoc::Store& s) { s.tempo.tempos[0].bpm = 0.0; }, "bpm 0");
  expectReject([&](sq::edoc::Store& s) { s.tempo.tempos.push_back({4.0, nan, 0}); }, "bpm NaN");
  expectReject([&](sq::edoc::Store& s) { s.tempo.tempos.push_back({nan, 100.0, 0}); }, "tempo pos NaN");
  expectReject([&](sq::edoc::Store& s) { s.tempo.sigs[0].den = 0; }, "sig den 0");
  expectReject([&](sq::edoc::Store& s) { s.tempo.sigs[0].num = 0; }, "sig num 0");
  expectReject([&](sq::edoc::Store& s) { s.tracks[au].kind = 42; }, "track kind 42");
  expectReject([&](sq::edoc::Store& s) { s.tracks[au].channels = 0; }, "channels 0");
  expectReject([&](sq::edoc::Store& s) { s.tracks[au].volumeDb = nan; }, "volume NaN");
  expectReject([&](sq::edoc::Store& s) { s.tracks[au].automation["v"] = {{nan, 1.0}}; }, "automation NaN");
  expectReject([&](sq::edoc::Store& s) {
    s.clips[clip].kind = SQ_CLIP_MIDI;
    s.clips[clip].events.push_back({nan, 1.0, {0x90, 60, 100, 0}, 3});
  }, "event NaN");
  // and the untouched store still opens
  REQUIRE_OK(roundTrip(f.st(), out));
}

SQ_TEST("verify: remove-overlaps ignores edits that do not place a clip (rename)") {
  Fx f;
  const sq_guid t = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "T");
  const sq_guid a = addClip(f.ed.get(), t, SQ_CLIP_AUDIO, 0.0, 4.0, "a.wav");
  const sq_guid b = addClip(f.ed.get(), t, SQ_CLIP_AUDIO, 2.0, 4.0, "b.wav");  // overlaps a (feature off)
  CHECK_OK(setPropB(f.ed.get(), f.st().props.id, "removeOverlaps", true));
  CHECK_OK(setPropS(f.ed.get(), b, "name", "renamed"));
  CHECK_EQ(f.st().clip(a)->length, 4.0);  // a must not be trimmed by a rename of b
  CHECK_EQ(f.st().clips.size(), 2u);
}

SQ_TEST("verify: time-map leaves clips placed after the tempo edit of the same transaction") {
  Fx f;
  const sq_guid t = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "T");
  CHECK_OK(setPropS(f.ed.get(), t, "timebase", "time"));
  const sq_guid old = addClip(f.ed.get(), t, SQ_CLIP_AUDIO, 4.0, 1.0, "old.wav");
  REQUIRE_OK(f.ed->beginTransaction("tempo + place"));
  CHECK_OK(setTempo(f.ed.get(), 0.0, 60.0));
  const sq_guid fresh = addClip(f.ed.get(), t, SQ_CLIP_AUDIO, 8.0, 1.0, "new.wav");
  REQUIRE_OK(f.ed->commitTransaction());
  CHECK_EQ(f.st().clip(fresh)->start, 8.0);               // placed under the new map: untouched
  CHECK(std::fabs(f.st().clip(old)->start - 2.0) < 1e-9);  // 2 s at 120 bpm -> 2 beats at 60 bpm
}

namespace {
struct DtorFlag final : public sq::Object<DtorFlag, sq::IObserver> {
  explicit DtorFlag(bool* f, sq::IDocument* d) : flag(f), doc(d) {}
  ~DtorFlag() { *flag = true; }
  void SQ_CALL onNotify(sq::IUnknown*, const char*, sq::IAttributes*) override {}
  bool* flag;
  Ptr<sq::IDocument> doc;  // typical UI observer: keeps the document it watches
};
}  // namespace

SQ_TEST("verify: closing a document releases its observers (no doc <-> observer leak)") {
  bool destroyed = false;
  {
    Fx f;
    Ptr<sq::ISubject> subj = f.doc.as<sq::ISubject>();
    REQUIRE(subj);
    {
      Ptr<DtorFlag> o = Ptr<DtorFlag>::adopt(new DtorFlag(&destroyed, f.doc.get()));
      REQUIRE_OK(subj->addObserver(o.get()));
    }
    REQUIRE_OK(f.mgr->closeDocument(f.doc.get()));
    CHECK(destroyed);
  }
  CHECK(destroyed);
}
