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

namespace {
void setStr(IEditor* ed, const sq_guid& node, const char* key, const std::string& v) {
  auto p = makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setString("value", v.c_str());
  exec(ed, doc::kSetProperty, p.get());
}
void setNum(IEditor* ed, const sq_guid& node, const char* key, double v) {
  auto p = makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setDouble("value", v);
  exec(ed, doc::kSetProperty, p.get());
}
void setLane(IEditor* ed, const sq_guid& track, const char* param, std::vector<double> pts) {
  static constexpr sq_guid kSetAutomation = sq::guid("{14D5E2BA-6EC7-4616-A188-F9A29E14D634}");
  auto p = makeAttributes();
  p->setGuid("track", &track);
  p->setString("param", param);
  p->setBlob("points", pts.data(), static_cast<uint32_t>(pts.size() * sizeof(double)));
  REQUIRE_OK(ed->execute(&kSetAutomation, p.get(), nullptr));
}
struct TrackView {
  sq_track_info info{};
  std::string name, instrument, inserts, sends, automation;
  sq_guid id{}, parent{};
};
std::vector<TrackView> view(IDocument* d) {
  std::vector<TrackView> out;
  sq_document_info di{};
  di.struct_size = sizeof di;
  REQUIRE_OK(d->getDocumentInfo(&di));
  for (uint32_t i = 0; i < di.track_count; ++i) {
    Ptr<ITrack> t;
    REQUIRE_OK(d->getTrack(i, t.put()));
    TrackView v;
    v.info.struct_size = sizeof v.info;
    REQUIRE_OK(t->getTrackInfo(&v.info));
    t->getId(&v.id);
    auto props = makeAttributes();
    REQUIRE_OK(t->getProperties(props.get()));
    v.name = getAttrString(props.get(), "name");
    v.instrument = getAttrString(props.get(), "user.instrument");
    v.inserts = getAttrString(props.get(), "user.inserts");
    v.sends = getAttrString(props.get(), "user.sends");
    v.automation = getAttrString(props.get(), "automation");
    props->getGuid("parent", &v.parent);
    out.push_back(v);
  }
  return out;
}
const TrackView* named(const std::vector<TrackView>& v, const std::string& n) {
  for (const auto& t : v)
    if (t.name == n) return &t;
  return nullptr;
}
const TrackView* ofKind(const std::vector<TrackView>& v, int32_t kind) {
  for (const auto& t : v)
    if (t.info.kind == kind) return &t;
  return nullptr;
}
}  // namespace

SQ_TEST("dawproject: buses, routing, sends, devices, master and automation round-trip") {
  std::string dir = tmpDir();
  auto mgr = Ptr<edoc::DocumentManagerImpl>::adopt(new edoc::DocumentManagerImpl());
  Ptr<IDocument> doc;
  REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, doc.put()));
  Ptr<IEditor> ed;
  REQUIRE_OK(doc->getEditor(ed.put()));
  const auto before = view(doc.get());
  const TrackView* m0 = ofKind(before, SQ_TRACK_MASTER);
  REQUIRE(m0);
  const sq_guid master = m0->id;
  auto add = [&](int32_t kind, const char* name, const sq_guid* parent = nullptr) {
    auto p = makeAttributes();
    p->setInt("kind", kind);
    p->setString("name", name);
    if (parent) p->setGuid("parent", parent);
    return exec(ed.get(), doc::kAddTrack, p.get());
  };
  const sq_guid bus = add(SQ_TRACK_BUS, "VerbBus");
  const sq_guid fx = add(SQ_TRACK_FX, "Delays");
  const sq_guid folder = add(SQ_TRACK_FOLDER, "Band");
  const sq_guid synth = add(SQ_TRACK_INSTRUMENT, "Synth", &folder);
  const sq_guid drums = add(SQ_TRACK_INSTRUMENT, "Drums", &folder);
  setStr(ed.get(), synth, "user.instrument", "builtin|synth||0=0.5");
  setStr(ed.get(), synth, "user.inserts", "builtin|reverb||0=0.7\n!builtin|eq||0=-3|4=1.5\n");
  setStr(ed.get(), synth, "user.sends", toString(bus) + " -6\n" + toString(fx) + " -12 pre\n");
  setNum(ed.get(), synth, "pan", -0.5);
  setLane(ed.get(), synth, "volume", {0, 0, 4, -12});
  setLane(ed.get(), synth, "pan", {0, -1, 8, 1});
  setLane(ed.get(), synth, "send.0", {0, -6, 2, -20});
  setLane(ed.get(), synth, "insert.0.0", {0, 0.7, 4, 0.2});
  setLane(ed.get(), synth, "instrument.3", {0, 0.1, 4, 0.9});
  setLane(ed.get(), synth, "mute", {0, 0, 2, 1});
  setStr(ed.get(), drums, "user.instrument", "builtin|drums|");
  {
    auto p = makeAttributes();
    p->setGuid("node", &drums);
    p->setString("key", "output");
    p->setGuid("value", &bus);
    exec(ed.get(), doc::kSetProperty, p.get());
  }
  setStr(ed.get(), bus, "user.inserts", "builtin|reverb||0=0.7\n");
  setNum(ed.get(), bus, "volumeDb", -3);
  setStr(ed.get(), master, "user.inserts", "builtin|limiter|\n");
  setNum(ed.get(), master, "volumeDb", -1.5);
  setLane(ed.get(), master, "volume", {0, -1.5, 16, -6});

  const std::string file = joinPath(dir, "mix.dawproject");
  formats::dawproject::ExportReport er;
  std::string err;
  REQUIRE_OK(formats::dawproject::exportProject(doc.get(), file, &er, &err));
  CHECK_EQ(er.tracks, 5);  // bus, fx, folder, 2 instruments
  CHECK(er.master);
  CHECK_EQ(er.buses, 2);
  CHECK_EQ(er.devices, 6);  // 2 instruments + 2 synth inserts + bus insert + master insert
  CHECK_EQ(er.sends, 2);
  CHECK_EQ(er.automation, 7);
  CHECK_EQ(er.warnings, std::string());

  Ptr<IDocument> back;
  REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, back.put()));
  formats::dawproject::ExportReport ir;
  REQUIRE_OK(formats::dawproject::importProject(file, back.get(), joinPath(dir, "media2"), &ir, &err));
  CHECK_EQ(ir.warnings, std::string());
  CHECK_EQ(ir.tracks, 5);
  CHECK_EQ(ir.devices, 6);
  CHECK_EQ(ir.sends, 2);
  CHECK_EQ(ir.automation, 7);
  const auto v = view(back.get());
  CHECK_EQ(v.size(), before.size() + 5);  // the existing master is reused
  const TrackView* b = named(v, "VerbBus");
  const TrackView* x = named(v, "Delays");
  const TrackView* f = named(v, "Band");
  const TrackView* s = named(v, "Synth");
  const TrackView* d = named(v, "Drums");
  const TrackView* m = ofKind(v, SQ_TRACK_MASTER);
  REQUIRE(b);
  REQUIRE(x);
  REQUIRE(f);
  REQUIRE(s);
  REQUIRE(d);
  REQUIRE(m);
  CHECK_EQ(b->info.kind, SQ_TRACK_BUS);
  CHECK_EQ(x->info.kind, SQ_TRACK_FX);
  CHECK_EQ(f->info.kind, SQ_TRACK_FOLDER);
  CHECK(s->parent == f->id);
  CHECK(d->parent == f->id);
  CHECK_EQ(s->instrument, std::string("builtin|synth||0=0.5"));
  CHECK_EQ(d->instrument, std::string("builtin|drums|"));
  CHECK_EQ(s->inserts, std::string("builtin|reverb||0=0.7\n!builtin|eq||0=-3|4=1.5\n"));
  CHECK_EQ(s->sends, toString(b->id) + " -6\n" + toString(x->id) + " -12 pre\n");
  CHECK(std::fabs(s->info.pan + 0.5) < 1e-9);
  CHECK(d->info.output == b->id);
  CHECK(sq_guid_is_null(&s->info.output));
  CHECK_EQ(b->inserts, std::string("builtin|reverb||0=0.7\n"));
  CHECK(std::fabs(b->info.volume_db + 3.0) < 1e-9);
  CHECK_EQ(m->inserts, std::string("builtin|limiter|\n"));
  CHECK(std::fabs(m->info.volume_db + 1.5) < 1e-9);
  // Same lanes, same points (the text view lists them sorted by target).
  const auto orig = view(doc.get());
  REQUIRE(named(orig, "Synth"));
  CHECK_EQ(s->automation, named(orig, "Synth")->automation);
  CHECK_EQ(m->automation, ofKind(orig, SQ_TRACK_MASTER)->automation);
  CHECK(s->automation.find("instrument.3 ") != std::string::npos);
  CHECK(s->automation.find("pan 0:-1 8:1") != std::string::npos);
  // Still one undo step.
  Ptr<IEditor> bed;
  REQUIRE_OK(back->getEditor(bed.put()));
  CHECK_EQ(bed->getUndoCount(), 1u);
  mgr->closeDocument(doc.get());
  mgr->closeDocument(back.get());
  sq_fs_remove_all(dir.c_str());
}
