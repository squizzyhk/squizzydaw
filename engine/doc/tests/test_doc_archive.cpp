// engine/doc: XmlArchive round trip (byte-stable), foreign parts, errors, crash-safe write, autosave.
#include <algorithm>
#include <map>

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

SQ_TEST("archive: media inside the song folder is stored relative and follows a moved project") {
  const std::string dir = tempDir("media");
  const std::string proj = sq::joinPath(dir, "proj");
  const std::string mediaDir = sq::joinPath(proj, "Media");
  REQUIRE_OK(sq_fs_create_dirs(mediaDir.c_str()));
  const std::string media = sq::joinPath(mediaDir, "take.wav");
  REQUIRE_OK(sq::writeFile(media, "RIFF"));
  const std::string outside = sq::joinPath(dir, "elsewhere.wav");
  REQUIRE_OK(sq::writeFile(outside, "RIFF"));
  CHECK(sq::edoc::relativeMediaPath(media, proj) == "Media/take.wav");
  CHECK(sq::edoc::relativeMediaPath(outside, proj) == outside);
  const std::string sibling = sq::joinPath(dir, "proj2/x.wav");
  CHECK(sq::edoc::relativeMediaPath(sibling, proj) == sibling);  // "proj2" is not inside "proj"
  CHECK(sq::edoc::isAbsoluteMediaPath("C:\\a.wav"));
  CHECK(sq::edoc::isAbsoluteMediaPath("/a.wav"));
  CHECK(!sq::edoc::isAbsoluteMediaPath("Media/a.wav"));
  {
    Fx f;
    sq_guid t = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Vox");
    addClip(f.ed.get(), t, SQ_CLIP_AUDIO, 0.0, 4.0, media.c_str());
    sq_guid t2 = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Ext");
    addClip(f.ed.get(), t2, SQ_CLIP_AUDIO, 0.0, 4.0, outside.c_str());
    const std::string song = sq::joinPath(proj, "song.sqsong");
    REQUIRE_OK(f.mgr->saveDocument(f.doc.get(), song.c_str()));
    for (const auto& [id, c] : f.st().clips) CHECK(sq::edoc::isAbsoluteMediaPath(c.source));  // in memory: absolute
    // The archive holds the relative path for the media in the song folder only.
    sq::edoc::Store raw;
    auto bytes = sq::readFile(song);
    REQUIRE(bytes.has_value());
    REQUIRE_OK(sq::edoc::readArchive(*bytes, raw));
    std::vector<std::string> stored;
    for (const auto& [id, c] : raw.clips) stored.push_back(c.source);
    REQUIRE_EQ(stored.size(), size_t(2));
    CHECK(std::find(stored.begin(), stored.end(), "Media/take.wav") != stored.end());
    CHECK(std::find(stored.begin(), stored.end(), outside) != stored.end());
  }
  // Move the whole project folder: the relative media resolves at the new place.
  const std::string moved = sq::joinPath(dir, "moved");
  REQUIRE_OK(sq_fs_rename(proj.c_str(), moved.c_str()));
  Ptr<sq::edoc::DocumentManagerImpl> mgr = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl());
  Ptr<sq::IDocument> d;
  REQUIRE_OK(mgr->openDocument(sq::joinPath(moved, "song.sqsong").c_str(), d.put()));
  const sq::edoc::DocumentImpl* impl = sq::edoc::DocumentImpl::from(d.get());
  bool found = false;
  for (const auto& [id, c] : impl->store().clips)
    if (c.source.find("take.wav") != std::string::npos) {
      found = true;
      CHECK(sq_fs_is_file(c.source.c_str()));
      CHECK(c.source.find("moved") != std::string::npos);
    }
  CHECK(found);
  // An absolute path that is gone is found next to the song (songs saved before relative paths).
  CHECK(sq::edoc::resolveMediaPath(sq::joinPath(dir, "gone/Media/take.wav"), moved).find("moved") != std::string::npos);
  CHECK(sq::edoc::resolveMediaPath("/nowhere/none.wav", moved) == "/nowhere/none.wav");
  mgr->closeDocument(d.get());
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("document: untracked properties and media relinks stay out of the undo history") {
  const std::string dir = tempDir("untracked");
  Fx f;
  sq_guid t = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Vox");
  sq_guid c = addClip(f.ed.get(), t, SQ_CLIP_AUDIO, 0.0, 4.0, "/tmp/SquizzyStudio/Media/take.wav");
  CHECK_OK(setPropB(f.ed.get(), f.st().props.id, "removeOverlaps", true));  // a DocProps step in the history
  const uint32_t undos = f.ed->getUndoCount();
  const std::string p = sq::joinPath(dir, "s.sqsong");
  REQUIRE_OK(f.mgr->saveDocument(f.doc.get(), p.c_str()));
  CHECK(!f.impl->modified());
  sq::edoc::Value v;
  v.type = SQ_VALUE_DOUBLE;
  v.d = 8.0;
  CHECK_EQ(f.impl->setUntrackedDocProperty("user.loop.end", v), SQ_OK);
  CHECK_EQ(f.impl->setUntrackedDocProperty("user.loop.end", v), SQ_S_FALSE);
  CHECK_EQ(f.impl->setUntrackedDocProperty("loop.end", v), SQ_E_INVALIDARG);
  CHECK(f.impl->modified());
  CHECK_EQ(f.ed->getUndoCount(), undos);
  // Undoing the earlier DocProps step keeps the loop value.
  REQUIRE_OK(f.ed->undo());
  REQUIRE(f.st().props.user.count("user.loop.end"));
  CHECK_EQ(f.st().props.user.at("user.loop.end").d, 8.0);
  REQUIRE_OK(f.ed->redo());
  CHECK_EQ(f.st().props.user.at("user.loop.end").d, 8.0);
  // Relink: store and history snapshots point at the new file; no undo step.
  const std::map<std::string, std::string> m{{"/tmp/SquizzyStudio/Media/take.wav", "/songs/a Media/take.wav"}};
  CHECK_EQ(f.impl->relinkClipSources(m), size_t(1));
  CHECK_EQ(f.st().clip(c)->source, std::string("/songs/a Media/take.wav"));
  CHECK_EQ(f.ed->getUndoCount(), undos);
  REQUIRE_OK(f.ed->undo());  // removeOverlaps
  REQUIRE_OK(f.ed->undo());  // add clip
  CHECK(f.st().clip(c) == nullptr);
  REQUIRE_OK(f.ed->redo());
  REQUIRE(f.st().clip(c) != nullptr);
  CHECK_EQ(f.st().clip(c)->source, std::string("/songs/a Media/take.wav"));
  // Saved and reopened: the loop property persists.
  REQUIRE_OK(f.mgr->saveDocument(f.doc.get(), p.c_str()));
  Ptr<sq::edoc::DocumentManagerImpl> mgr = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl());
  Ptr<sq::IDocument> d;
  REQUIRE_OK(mgr->openDocument(p.c_str(), d.put()));
  const auto& user = sq::edoc::DocumentImpl::from(d.get())->store().props.user;
  REQUIRE(user.count("user.loop.end"));
  CHECK_EQ(user.at("user.loop.end").d, 8.0);
  mgr->closeDocument(d.get());
  sq_fs_remove_all(dir.c_str());
}
