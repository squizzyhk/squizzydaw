// formats/dawproject: export a document built with the real engine/doc, import it into a fresh
// document, compare tracks/clips/notes/markers/tempo; import is one undo step.
#include <cmath>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/engine/doc/manager.hpp"
#include "sq/formats/dawproject/dawproject.hpp"
#include "sq/platform.hpp"
#include "sqtest.hpp"

using namespace sq;

namespace {
sq_guid exec(IEditor* ed, const sq_guid& action, IAttributes* p) {
  auto res = makeAttributes();
  REQUIRE_OK(ed->execute(&action, p, res.get()));
  sq_guid g{};
  res->getGuid("node", &g);
  return g;
}
std::string tmpDir() {
  std::string d = joinPath(specialDir(SQ_DIR_TEMP), "sqtest-dawproject-" + std::to_string(sq_process_id()));
  sq_fs_create_dirs(d.c_str());
  return d;
}
}  // namespace

SQ_TEST("dawproject: export -> import round trip through engine/doc") {
  std::string dir = tmpDir();
  std::string media = joinPath(dir, "take.wav");
  REQUIRE_OK(writeFile(media, std::string("RIFF fake media bytes", 21)));
  auto mgr = Ptr<edoc::DocumentManagerImpl>::adopt(new edoc::DocumentManagerImpl());
  Ptr<IDocument> doc;
  REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, doc.put()));
  Ptr<IEditor> ed;
  REQUIRE_OK(doc->getEditor(ed.put()));
  {
    auto p = makeAttributes();
    p->setDouble("position", 0);
    p->setDouble("bpm", 96);
    exec(ed.get(), doc::kSetTempo, p.get());
  }
  auto p = makeAttributes();
  p->setInt("kind", SQ_TRACK_AUDIO);
  p->setString("name", "Vocals");
  p->setInt("channels", 1);
  sq_guid audio = exec(ed.get(), doc::kAddTrack, p.get());
  p = makeAttributes();
  p->setGuid("node", &audio);
  p->setString("key", "volumeDb");
  p->setDouble("value", -6.0);
  exec(ed.get(), doc::kSetProperty, p.get());
  p = makeAttributes();
  p->setGuid("track", &audio);
  p->setInt("kind", SQ_CLIP_AUDIO);
  p->setDouble("start", 4);
  p->setDouble("length", 8);
  p->setString("source", media.c_str());
  p->setDouble("source_offset", 0.25);
  exec(ed.get(), doc::kAddClip, p.get());
  p = makeAttributes();
  p->setInt("kind", SQ_TRACK_INSTRUMENT);
  p->setString("name", "Keys");
  sq_guid keys = exec(ed.get(), doc::kAddTrack, p.get());
  p = makeAttributes();
  p->setGuid("track", &keys);
  p->setInt("kind", SQ_CLIP_MIDI);
  p->setDouble("start", 0);
  p->setDouble("length", 4);
  sq_guid mclip = exec(ed.get(), doc::kAddClip, p.get());
  std::vector<sq_clip_event> ev;
  for (int i = 0; i < 4; ++i) {
    sq_clip_event e{};
    e.struct_size = sizeof e;
    e.position = i;
    e.length = 0.5;
    e.bytes[0] = 0x90;
    e.bytes[1] = static_cast<uint8_t>(60 + i);
    e.bytes[2] = 100;
    e.size = 3;
    ev.push_back(e);
  }
  p = makeAttributes();
  p->setGuid("clip", &mclip);
  p->setBlob("events", ev.data(), static_cast<uint32_t>(ev.size() * sizeof(sq_clip_event)));
  exec(ed.get(), doc::kSetClipEvents, p.get());
  p = makeAttributes();
  p->setDouble("position", 8);
  p->setString("name", "Chorus");
  exec(ed.get(), doc::kAddMarker, p.get());

  std::string file = joinPath(dir, "song.dawproject");
  formats::dawproject::ExportReport er;
  std::string err;
  REQUIRE_OK(formats::dawproject::exportProject(doc.get(), file, &er, &err));
  CHECK_EQ(er.tracks, 2);
  CHECK_EQ(er.clips, 2);
  CHECK_EQ(er.notes, 4);
  CHECK_EQ(er.media, 1);
  CHECK_EQ(er.markers, 1);

  Ptr<IDocument> back;
  REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, back.put()));
  sq_document_info fresh{};
  fresh.struct_size = sizeof fresh;
  REQUIRE_OK(back->getDocumentInfo(&fresh));  // a new song may already hold a master track
  formats::dawproject::ExportReport ir;
  REQUIRE_OK(formats::dawproject::importProject(file, back.get(), joinPath(dir, "imported-media"), &ir, &err));
  CHECK_EQ(ir.tracks, 2);
  CHECK_EQ(ir.clips, 2);
  CHECK_EQ(ir.notes, 4);
  sq_document_info di{};
  di.struct_size = sizeof di;
  REQUIRE_OK(back->getDocumentInfo(&di));
  REQUIRE_EQ(di.track_count, fresh.track_count + 2u);
  // Find the imported tracks by kind (display order may put a master first).
  auto byKind = [&](int32_t kind) {
    for (uint32_t i = 0; i < di.track_count; ++i) {
      Ptr<ITrack> t;
      sq_track_info info{};
      info.struct_size = sizeof info;
      if (back->getTrack(i, t.put()) == SQ_OK && t->getTrackInfo(&info) == SQ_OK && info.kind == kind) return t;
    }
    return Ptr<ITrack>();
  };
  Ptr<ITrack> t0 = byKind(SQ_TRACK_AUDIO);
  REQUIRE(t0);
  sq_track_info ti{};
  ti.struct_size = sizeof ti;
  REQUIRE_OK(t0->getTrackInfo(&ti));
  CHECK_EQ(ti.kind, SQ_TRACK_AUDIO);
  CHECK_EQ(ti.channels, 1u);
  CHECK(std::fabs(ti.volume_db + 6.0) < 1e-9);
  Ptr<IClip> c0;
  REQUIRE_OK(t0->getClip(0, c0.put()));
  sq_clip_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(c0->getClipInfo(&ci));
  CHECK_EQ(ci.start, 4.0);
  CHECK_EQ(ci.length, 8.0);
  CHECK_EQ(ci.source_offset_seconds, 0.25);
  char src[1024];
  uint32_t need = 0;
  REQUIRE_OK(c0->getSourcePath(src, sizeof src, &need));
  auto bytes = readFile(src);
  REQUIRE(bytes.has_value());
  CHECK_EQ(*bytes, std::string("RIFF fake media bytes", 21));
  Ptr<ITrack> t1 = byKind(SQ_TRACK_INSTRUMENT);
  REQUIRE(t1);
  Ptr<IClip> c1;
  REQUIRE_OK(t1->getClip(0, c1.put()));
  REQUIRE_OK(c1->getClipInfo(&ci));
  CHECK_EQ(ci.event_count, 4u);
  sq_clip_event e{};
  e.struct_size = sizeof e;
  REQUIRE_OK(c1->getEvent(2, &e));
  CHECK_EQ(int{e.bytes[1]}, 62);
  CHECK_EQ(e.position, 2.0);
  CHECK_EQ(back->getMarkerCount(), 1u);
  Ptr<ITempoMap> tm;
  REQUIRE_OK(back->getTempoMap(tm.put()));
  sq_tempo_point tp{};
  tp.struct_size = sizeof tp;
  REQUIRE_OK(tm->getTempo(0, &tp));
  CHECK_EQ(tp.bpm, 96.0);
  // The whole import is one undo step.
  Ptr<IEditor> bed;
  REQUIRE_OK(back->getEditor(bed.put()));
  CHECK_EQ(bed->getUndoCount(), 1u);
  REQUIRE_OK(bed->undo());
  REQUIRE_OK(back->getDocumentInfo(&di));
  CHECK_EQ(di.track_count, fresh.track_count);

  CHECK(formats::dawproject::importProject(joinPath(dir, "missing.dawproject"), back.get(), dir, nullptr, &err) < 0);
  REQUIRE_OK(writeFile(joinPath(dir, "bad.dawproject"), "not a zip"));
  CHECK(formats::dawproject::importProject(joinPath(dir, "bad.dawproject"), back.get(), dir, nullptr, &err) < 0);
  mgr->closeDocument(doc.get());
  mgr->closeDocument(back.get());
  sq_fs_remove_all(dir.c_str());
}
