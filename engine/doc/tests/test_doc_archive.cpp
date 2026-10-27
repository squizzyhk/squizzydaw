// engine/doc: XmlArchive round trip (byte-stable), foreign parts, errors, crash-safe write, autosave.
#include "doc_test_util.hpp"

using namespace doctest;
using sq::Ptr;

namespace {

// A document that touches every persisted field.
void buildRich(Fx& f) {
  sq::IEditor* ed = f.ed.get();
  sq_guid docId = f.st().props.id;
  CHECK_OK(setPropS(ed, docId, "name", "Round \"Trip\" <&> \xC3\xA9t\xC3\xA9\nline2"));
  CHECK_OK(setProp(ed, docId, "sampleRate", 44100.0));
  CHECK_OK(setPropB(ed, docId, "removeOverlaps", true));
  CHECK_OK(setProp(ed, docId, "user.rating", 0.1));
  sq_guid folder = addTrack(ed, SQ_TRACK_FOLDER, "Strings");
  sq_guid vln = addTrack(ed, SQ_TRACK_AUDIO, "Violin", &folder);
  sq_guid bus = addTrack(ed, SQ_TRACK_BUS, "Verb");
  sq_guid syn = addTrack(ed, SQ_TRACK_INSTRUMENT, "Synth");
  CHECK_OK(setProp(ed, vln, "volumeDb", -3.3333333333333335));
  CHECK_OK(setProp(ed, vln, "pan", -0.25));
  CHECK_OK(setPropB(ed, vln, "mute", true));
  CHECK_OK(setPropS(ed, vln, "timebase", "time"));
  auto po = sq::makeAttributes();
  po->setGuid("node", &vln);
  po->setString("key", "output");
  po->setGuid("value", &bus);
  CHECK_OK(exec(ed, sq::doc::kSetProperty, po.get()));
  auto blob = sq::makeAttributes();
  const char raw[] = {0, 1, 2, '\xFF'};
  blob->setGuid("node", &syn);
  blob->setString("key", "user.state");
  blob->setBlob("value", raw, 4);
  CHECK_OK(exec(ed, sq::doc::kSetProperty, blob.get()));
  CHECK_OK(setAutomation(ed, vln, "volume", {0.0, 0.25, 3.5, 1.0 / 3.0}));
  sq_guid c = addClip(ed, vln, SQ_CLIP_AUDIO, 1.0 / 3.0, 7.25, "C:/audio/violin take.wav", 0.125);
  CHECK_OK(setProp(ed, c, "fadeIn", 0.01));
  CHECK_OK(setProp(ed, c, "gainDb", -6.0));
  sq_guid m = addClip(ed, syn, SQ_CLIP_MIDI, 0.0, 16.0);
  std::vector<sq_clip_event> ev = {note(0.0, 1.0, 60), note(1.5, 0.25, 72, 1)};
  sq_clip_event cc{};
  cc.struct_size = sizeof cc;
  cc.position = 2.0;
  cc.bytes[0] = 0xB0;
  cc.bytes[1] = 7;
  cc.bytes[2] = 100;
  cc.size = 3;
  ev.push_back(cc);
  CHECK_OK(setEvents(ed, m, ev));
  auto mk = sq::makeAttributes();
  mk->setDouble("position", 8.0);
  mk->setDouble("length", 8.0);
  mk->setString("name", "Chorus");
  mk->setInt("flags", SQ_DOCMARKER_LOOP);
  CHECK_OK(exec(ed, sq::doc::kAddMarker, mk.get()));
  mk->setDouble("position", 0.0);
  mk->setString("name", "Intro");
  mk->setInt("flags", 0);
  CHECK_OK(exec(ed, sq::doc::kAddMarker, mk.get()));
  CHECK_OK(setTempo(ed, 16.0, 97.5, true));
  CHECK_OK(setTempo(ed, 32.0, 140.0));
  auto sig = sq::makeAttributes();
  sig->setDouble("position", 16.0);
  sig->setInt("numerator", 7);
  sig->setInt("denominator", 8);
  CHECK_OK(exec(ed, sq::doc::kSetSignature, sig.get()));
  f.impl->setJournal("zoom", "1.5");
  f.impl->setJournal("selection", sq::edoc::guidStr(c));
}

}  // namespace

SQ_TEST("archive: create / save / reopen / save is byte-stable") {
  const std::string dir = tempDir("rt");
  Fx f;
  buildRich(f);
  const std::string p1 = sq::joinPath(dir, "a.sqsong");
  const std::string p2 = sq::joinPath(dir, "b.sqsong");
  CHECK(f.impl->modified());
  REQUIRE_OK(f.mgr->saveDocument(f.doc.get(), p1.c_str()));
  CHECK(!f.impl->modified());
  CHECK(!sq_fs_exists((p1 + ".tmp").c_str()));
  auto bytes1 = sq::readFile(p1);
  REQUIRE(bytes1.has_value());
  CHECK(bytes1->size() > 100);
  CHECK_EQ(bytes1->substr(0, 2), std::string("PK"));

  // the same model always serializes to the same bytes
  CHECK(sq::edoc::writeArchive(f.st()) == *bytes1);

  Ptr<sq::edoc::DocumentManagerImpl> mgr2 = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl());
  Ptr<sq::IDocument> d2;
  REQUIRE_OK(mgr2->openDocument(p1.c_str(), d2.put()));
  sq::edoc::DocumentImpl* i2 = sq::edoc::DocumentImpl::from(d2.get());
  REQUIRE(i2);
  CHECK(!i2->modified());
  CHECK_EQ(i2->stateHash(), f.hash());
  CHECK_EQ(i2->store().props.name, f.st().props.name);
  CHECK(i2->store().tempo == f.st().tempo);
  CHECK(i2->store().trackOrder == f.st().trackOrder);
  for (const auto& [id, c] : f.st().clips) CHECK(*i2->store().clip(id) == c);
  for (const auto& [id, t] : f.st().tracks) CHECK(*i2->store().track(id) == t);
  CHECK_EQ(i2->journal("zoom"), std::string("1.5"));
  char path[512];
  uint32_t need = 0;
  REQUIRE_OK(d2->getPath(path, sizeof path, &need));
  CHECK_EQ(std::string(path), p1);

  REQUIRE_OK(mgr2->saveDocument(d2.get(), p2.c_str()));
  auto bytes2 = sq::readFile(p2);
  REQUIRE(bytes2.has_value());
  CHECK(*bytes1 == *bytes2);

  // opening the same document twice hands out the open instance
  Ptr<sq::IDocument> again;
  CHECK_EQ(mgr2->openDocument(p1.c_str(), again.put()), SQ_S_FALSE);
  CHECK(again.get() == d2.get());

  // edit -> save (NULL path = current) -> modified flips back
  sq_guid docId = i2->store().props.id;
  Ptr<sq::IEditor> e2;
  REQUIRE_OK(d2->getEditor(e2.put()));
  CHECK_OK(setPropS(e2.get(), docId, "name", "Edited"));
  CHECK(i2->modified());
  REQUIRE_OK(mgr2->saveDocument(d2.get(), nullptr));
  CHECK(!i2->modified());
  REQUIRE_OK(e2->undo());
  CHECK(i2->modified());

  REQUIRE_OK(mgr2->closeDocument(d2.get()));
  CHECK_EQ(mgr2->getDocumentCount(), 0u);
  CHECK_EQ(e2->undo(), SQ_E_CLOSED);
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("archive: every kind round-trips; unknown parts are preserved") {
  for (int32_t kind : {SQ_DOC_SONG, SQ_DOC_PROJECT, SQ_DOC_SHOW}) {
    Fx f(kind);
    addTrack(f.ed.get(), SQ_TRACK_AUDIO, "A");
    sq::edoc::Store s = f.st();
    s.foreignParts["mixer/console.xml"] = "<?xml version=\"1.0\"?>\n<Console future=\"1\"/>\n";
    const std::string bytes = sq::edoc::writeArchive(s);
    REQUIRE(!bytes.empty());
    sq::edoc::Store back;
    std::string err;
    REQUIRE_OK(sq::edoc::readArchive(bytes, back, nullptr, &err));
    CHECK_EQ(back.props.kind, kind);
    CHECK_EQ(back.foreignParts.size(), size_t(1));
    CHECK(sq::edoc::writeArchive(back) == bytes);
  }
}

SQ_TEST("archive: errors") {
  const std::string dir = tempDir("err");
  Ptr<sq::edoc::DocumentManagerImpl> mgr = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl());
  Ptr<sq::IDocument> d;
  CHECK_EQ(mgr->openDocument(sq::joinPath(dir, "missing.sqsong").c_str(), d.put()), SQ_E_NOTFOUND);
  const std::string junk = sq::joinPath(dir, "junk.sqsong");
  REQUIRE_OK(sq::writeFile(junk, "this is not a zip"));
  CHECK_EQ(mgr->openDocument(junk.c_str(), d.put()), SQ_E_PARSE);
  sq::edoc::Store s;
  CHECK_EQ(sq::edoc::readArchive(std::string(), s), SQ_E_PARSE);
  // unsaved document without a path
  REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, d.put()));
  CHECK_EQ(mgr->saveDocument(d.get(), nullptr), SQ_E_INVALIDARG);
  CHECK_EQ(mgr->createDocument(42, d.put()), SQ_E_INVALIDARG);
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("archive: safeWriteFile replaces atomically") {
  const std::string dir = tempDir("safe");
  const std::string p = sq::joinPath(dir, "sub/file.bin");
  REQUIRE_OK(sq::edoc::safeWriteFile(p, "first"));
  REQUIRE_OK(sq::edoc::safeWriteFile(p, std::string("second\0x", 8)));
  auto b = sq::readFile(p);
  REQUIRE(b.has_value());
  CHECK(*b == std::string("second\0x", 8));
  CHECK(!sq_fs_exists((p + ".tmp").c_str()));
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("autosave: interval tick, crash recovery, cleanup on save") {
  const std::string dir = tempDir("auto");
  const std::string asDir = sq::joinPath(dir, "autosave");
  Fx f;
  f.mgr->setAutosave(asDir, 1000);
  const std::string saved = sq::joinPath(dir, "song.sqsong");
  REQUIRE_OK(f.mgr->saveDocument(f.doc.get(), saved.c_str()));
  f.mgr->tick(0);
  addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Unsaved work");
  const std::string ap = f.mgr->autosavePath(*f.impl);
  f.mgr->tick(500);
  CHECK(!sq_fs_exists(ap.c_str()));
  f.mgr->tick(1000);
  REQUIRE(sq_fs_exists(ap.c_str()));
  auto list = f.mgr->listRecoverable();
  REQUIRE_EQ(list.size(), size_t(1));

  // "crash": a fresh manager recovers the autosave
  Ptr<sq::edoc::DocumentManagerImpl> mgr2 = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl());
  mgr2->setAutosave(asDir, 1000);
  Ptr<sq::IDocument> rec;
  REQUIRE_OK(mgr2->recover(list[0].c_str(), rec.put()));
  sq::edoc::DocumentImpl* ri = sq::edoc::DocumentImpl::from(rec.get());
  REQUIRE(ri);
  CHECK_EQ(ri->stateHash(), f.hash());
  CHECK(ri->modified());
  CHECK_EQ(ri->path(), saved);
  CHECK_EQ(mgr2->recover(list[0].c_str(), rec.put()), SQ_E_EXISTS);

  // unchanged since the last autosave: no rewrite; a real save removes the autosave
  f.mgr->tick(2000);
  REQUIRE_OK(f.mgr->saveDocument(f.doc.get(), nullptr));
  CHECK(!sq_fs_exists(ap.c_str()));
  CHECK_EQ(f.mgr->listRecoverable().size(), size_t(0));
  sq_fs_remove_all(dir.c_str());
}
