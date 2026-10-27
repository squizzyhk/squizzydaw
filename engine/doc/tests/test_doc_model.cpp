// engine/doc: document tree, actions, transactions, undo/redo/merge, side effects, journals, URLs.
#include <cmath>

#include "doc_test_util.hpp"

using namespace doctest;
using sq::Ptr;

namespace {

struct CountingObserver final : sq::Object<CountingObserver, sq::IObserver> {
  int changed = 0, modifiedOn = 0, modifiedOff = 0;
  std::string lastReason, lastDescription;
  uint32_t lastNodes = 0;
  void SQ_CALL onNotify(sq::IUnknown*, const char* topic, sq::IAttributes* args) override {
    std::string t = topic;
    if (t == "document.changed") {
      ++changed;
      lastReason = sq::getAttrString(args, "reason");
      lastDescription = sq::getAttrString(args, "description");
      uint32_t need = 0;
      args->getBlob("nodes", nullptr, 0, &need);
      lastNodes = need / static_cast<uint32_t>(sizeof(sq_guid));
    } else if (t == "document.modified") {
      (args->getBool("modified", SQ_FALSE) ? modifiedOn : modifiedOff)++;
    }
  }
};

std::string nodeName(sq::INode* n) {
  char buf[256];
  uint32_t need = 0;
  return n->getName(buf, sizeof buf, &need) == SQ_OK ? std::string(buf) : std::string();
}

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

}  // namespace

SQ_TEST("doc: new song has master, tempo map and defaults") {
  Fx f;
  sq_document_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(f.doc->getDocumentInfo(&info));
  CHECK_EQ(info.kind, SQ_DOC_SONG);
  CHECK_EQ(info.track_count, 1u);
  CHECK(!info.modified);
  CHECK_EQ(f.doc->getChildCount(), 2u);  // tempo map + master
  Ptr<sq::INode> c0;
  REQUIRE_OK(f.doc->getChild(0, c0.put()));
  CHECK_EQ(c0->getKind(), SQ_NODE_TEMPO_MAP);
  Ptr<sq::ITempoMap> tm;
  REQUIRE_OK(f.doc->getTempoMap(tm.put()));
  CHECK_EQ(tm->getTempoCount(), 1u);
  CHECK(near(tm->ppqToSeconds(8.0), 4.0));
  Ptr<sq::ITrack> master;
  REQUIRE_OK(f.doc->getTrack(0, master.put()));
  sq_track_info ti{};
  REQUIRE_OK(master->getTrackInfo(&ti));
  CHECK_EQ(ti.kind, SQ_TRACK_MASTER);
  sq_guid mid{};
  master->getId(&mid);
  auto p = sq::makeAttributes();
  p->setGuid("node", &mid);
  CHECK_EQ(exec(f.ed.get(), sq::doc::kRemoveNode, p.get()), SQ_E_ACCESS);
  CHECK_EQ(f.ed->getUndoCount(), 0u);

  Fx show(SQ_DOC_SHOW);
  CHECK_EQ(show.st().trackOrder.size(), 0u);
  Fx proj(SQ_DOC_PROJECT);
  auto q = sq::makeAttributes();
  q->setInt("kind", SQ_TRACK_INSTRUMENT);
  CHECK_EQ(exec(proj.ed.get(), sq::doc::kAddTrack, q.get()), SQ_E_UNSUPPORTED);
}

SQ_TEST("doc: tracks, clips, events and the read API") {
  Fx f;
  sq_guid a = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Vox");
  sq_guid inst = addTrack(f.ed.get(), SQ_TRACK_INSTRUMENT);
  sq_guid c2 = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 8.0, 4.0, "C:/media/take2.wav");
  sq_guid c1 = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 0.0, 4.0, "C:/media/take1.wav", 1.5);
  sq_guid m = addClip(f.ed.get(), inst, SQ_CLIP_MIDI, 0.0, 4.0);
  CHECK_OK(setEvents(f.ed.get(), m, {note(1.0, 0.5, 64), note(0.0, 1.0, 60)}));
  CHECK_EQ(f.ed->getUndoCount(), 6u);

  Ptr<sq::ITrack> t;
  REQUIRE_OK(f.doc->getTrack(1, t.put()));
  CHECK_EQ(nodeName(t.get()), std::string("Vox"));
  sq_track_info ti{};
  REQUIRE_OK(t->getTrackInfo(&ti));
  CHECK_EQ(ti.clip_count, 2u);
  Ptr<sq::IClip> first;
  REQUIRE_OK(t->getClip(0, first.put()));
  sq_guid fid{};
  first->getId(&fid);
  CHECK(fid == c1);  // sorted by start
  CHECK_EQ(nodeName(first.get()), std::string("take1"));
  sq_clip_info ci{};
  REQUIRE_OK(first->getClipInfo(&ci));
  CHECK(near(ci.source_offset_seconds, 1.5));
  char path[256];
  uint32_t need = 0;
  REQUIRE_OK(first->getSourcePath(path, sizeof path, &need));
  CHECK_EQ(std::string(path), std::string("C:/media/take1.wav"));
  CHECK_EQ(first->getSourcePath(path, 4, &need), SQ_E_BUFFER_TOO_SMALL);
  Ptr<sq::INode> parent;
  REQUIRE_OK(first->getParent(parent.put()));
  CHECK_EQ(parent->getKind(), SQ_NODE_TRACK);
  (void)c2;

  Ptr<sq::INode> mn;
  REQUIRE_OK(f.doc->findNode(&m, mn.put()));
  auto mc = mn.as<sq::IClip>();
  REQUIRE(mc);
  CHECK(!mn.as<sq::ITrack>());
  sq_clip_event ev{};
  REQUIRE_OK(mc->getEvent(0, &ev));
  CHECK_EQ(int(ev.bytes[1]), 60);  // events sorted by position
  CHECK_EQ(mc->getEvent(2, &ev), SQ_E_NOTFOUND);

  auto props = sq::makeAttributes();
  REQUIRE_OK(t->getProperties(props.get()));
  CHECK_EQ(sq::getAttrString(props.get(), "name"), std::string("Vox"));
  CHECK_EQ(sq::getAttrString(props.get(), "timebase"), std::string("musical"));

  // invalid: MIDI clip on an audio track leaves everything unchanged
  const std::string h = f.hash();
  auto bad = sq::makeAttributes();
  bad->setGuid("track", &a);
  bad->setInt("kind", SQ_CLIP_MIDI);
  bad->setDouble("start", 0.0);
  bad->setDouble("length", 1.0);
  CHECK_EQ(exec(f.ed.get(), sq::doc::kAddClip, bad.get()), SQ_E_INVALIDARG);
  CHECK_EQ(f.hash(), h);
  CHECK_EQ(f.ed->getUndoCount(), 6u);
  CHECK(!f.ed->inTransaction());
}

SQ_TEST("doc: object:// URLs") {
  Fx f;
  sq_guid a = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Gtr");
  sq_guid c = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 0.0, 2.0);
  Ptr<sq::INode> n;
  std::string docId = sq::edoc::guidStr(f.st().props.id);
  REQUIRE_OK(f.mgr->findNodeByUrl(("object://doc/" + docId).c_str(), n.put()));
  CHECK_EQ(n->getKind(), SQ_NODE_DOCUMENT);
  REQUIRE_OK(f.mgr->findNodeByUrl("object://doc/active/track/1", n.put()));
  CHECK_EQ(nodeName(n.get()), std::string("Gtr"));
  const std::string cs = sq::edoc::guidStr(c);
  REQUIRE_OK(f.mgr->findNodeByUrl(("object://doc/active/node/" + cs.substr(1, cs.size() - 2)).c_str(), n.put()));
  CHECK_EQ(n->getKind(), SQ_NODE_CLIP);
  char url[256];
  uint32_t need = 0;
  REQUIRE_OK(n->getUrl(url, sizeof url, &need));
  CHECK_EQ(std::string(url), "object://doc/" + docId + "/node/" + cs);
  Ptr<sq::INode> back;
  REQUIRE_OK(f.mgr->findNodeByUrl(url, back.put()));
  CHECK_EQ(back->getKind(), SQ_NODE_CLIP);
  REQUIRE_OK(f.mgr->findNodeByUrl("object://doc/active/tempomap", n.put()));
  CHECK_EQ(n->getKind(), SQ_NODE_TEMPO_MAP);
  CHECK_EQ(f.mgr->findNodeByUrl("object://doc/active/track/9", n.put()), SQ_E_NOTFOUND);
  CHECK_EQ(f.mgr->findNodeByUrl("object://app/settings/x", n.put()), SQ_E_INVALIDARG);
  // node views survive undo as "not found" and come back on redo
  Ptr<sq::INode> cn;
  REQUIRE_OK(f.doc->findNode(&c, cn.put()));
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(cn.as<sq::IClip>()->getClipInfo(nullptr), SQ_E_INVALIDARG);
  sq_clip_info ci{};
  CHECK_EQ(cn.as<sq::IClip>()->getClipInfo(&ci), SQ_E_NOTFOUND);
  REQUIRE_OK(f.ed->redo());
  CHECK_OK(cn.as<sq::IClip>()->getClipInfo(&ci));
}

SQ_TEST("doc: nested transactions, cancel, descriptions, modified notifications") {
  Fx f;
  Ptr<CountingObserver> obs = Ptr<CountingObserver>::adopt(new CountingObserver());
  auto subj = f.doc.as<sq::ISubject>();
  REQUIRE(subj);
  REQUIRE_OK(subj->addObserver(obs.get()));
  CHECK_EQ(subj->addObserver(obs.get()), SQ_S_FALSE);

  const std::string h0 = f.hash();
  REQUIRE_OK(f.ed->beginTransaction("Build Band"));
  addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Bass");
  REQUIRE_OK(f.ed->beginTransaction("inner"));
  addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Drums");
  const std::string hInner = f.hash();
  addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Keys");
  REQUIRE_OK(f.ed->cancelTransaction());  // drops Drums + Keys only
  CHECK(f.ed->inTransaction());
  CHECK_EQ(f.st().trackOrder.size(), 2u);
  (void)hInner;
  REQUIRE_OK(f.ed->beginTransaction("inner2"));
  addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Gtr");
  REQUIRE_OK(f.ed->commitTransaction());
  CHECK_EQ(obs->changed, 0);  // nothing published before the outermost commit
  CHECK_EQ(f.ed->undo(), SQ_E_BUSY);
  REQUIRE_OK(f.ed->commitTransaction());
  CHECK_EQ(obs->changed, 1);
  CHECK_EQ(obs->lastReason, std::string("commit"));
  CHECK_EQ(obs->lastNodes, 2u);
  CHECK_EQ(obs->modifiedOn, 1);
  CHECK_EQ(f.ed->getUndoCount(), 1u);
  char d[64];
  uint32_t need = 0;
  REQUIRE_OK(f.ed->getUndoDescription(0, d, sizeof d, &need));
  CHECK_EQ(std::string(d), std::string("Build Band"));
  CHECK_EQ(f.st().trackOrder.size(), 3u);

  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.hash(), h0);
  CHECK_EQ(obs->lastReason, std::string("undo"));
  CHECK_EQ(obs->modifiedOff, 1);  // back at the saved (pristine) state
  REQUIRE_OK(f.ed->getRedoDescription(0, d, sizeof d, &need));
  CHECK_EQ(std::string(d), std::string("Build Band"));
  REQUIRE_OK(f.ed->redo());
  CHECK_EQ(f.st().trackOrder.size(), 3u);
  REQUIRE_OK(f.ed->undo());
  addTrack(f.ed.get(), SQ_TRACK_BUS);
  CHECK(!f.ed->canRedo());  // new edit clears redo

  // cancel at the outermost level restores everything; empty commit creates no step
  const std::string h1 = f.hash();
  REQUIRE_OK(f.ed->beginTransaction("x"));
  addTrack(f.ed.get(), SQ_TRACK_FX);
  REQUIRE_OK(f.ed->cancelTransaction());
  CHECK_EQ(f.hash(), h1);
  const uint32_t steps = f.ed->getUndoCount();
  REQUIRE_OK(f.ed->beginTransaction("nothing"));
  REQUIRE_OK(f.ed->commitTransaction());
  CHECK_EQ(f.ed->getUndoCount(), steps);
  CHECK_EQ(f.ed->commitTransaction(), SQ_E_FAIL);
  REQUIRE_OK(subj->removeObserver(obs.get()));
}

SQ_TEST("doc: merge keys collapse consecutive property drags") {
  Fx f;
  sq_guid a = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  const uint32_t base = f.ed->getUndoCount();
  for (int i = 1; i <= 10; ++i) CHECK_OK(setProp(f.ed.get(), a, "volumeDb", -1.0 * i, true));
  CHECK_EQ(f.ed->getUndoCount(), base + 1);
  CHECK(near(f.st().track(a)->volumeDb, -10.0));
  CHECK_OK(setProp(f.ed.get(), a, "pan", 0.5, true));  // different key: new step
  CHECK_EQ(f.ed->getUndoCount(), base + 2);
  REQUIRE_OK(f.ed->undo());
  REQUIRE_OK(f.ed->undo());
  CHECK(near(f.st().track(a)->volumeDb, 0.0));
  CHECK_EQ(setProp(f.ed.get(), a, "pan", 3.0), SQ_E_INVALIDARG);
  CHECK_EQ(setProp(f.ed.get(), a, "kind", 1.0), SQ_E_ACCESS);
  CHECK_EQ(setProp(f.ed.get(), a, "nonsense", 1.0), SQ_E_NOTFOUND);
  CHECK_OK(setPropS(f.ed.get(), a, "user.tag", "lead"));
  auto props = sq::makeAttributes();
  Ptr<sq::INode> n;
  REQUIRE_OK(f.doc->findNode(&a, n.put()));
  n->getProperties(props.get());
  CHECK_EQ(sq::getAttrString(props.get(), "user.tag"), std::string("lead"));
}

SQ_TEST("doc: split/resize keep audio aligned and partition MIDI") {
  Fx f;
  sq_guid a = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  sq_guid c = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 4.0, 8.0, "x.wav", 1.0);
  auto p = sq::makeAttributes();
  p->setGuid("node", &c);
  p->setDouble("at", 6.0);
  sq_guid right{};
  REQUIRE_OK(exec(f.ed.get(), sq::doc::kSplitClip, p.get(), &right));
  const sq::edoc::ClipRec* l = f.st().clip(c);
  const sq::edoc::ClipRec* r = f.st().clip(right);
  REQUIRE(l && r);
  CHECK(near(l->length, 2.0));
  CHECK(near(r->start, 6.0));
  CHECK(near(r->sourceOffset, 2.0));  // 2 ppq @120 = 1 s further into the file
  p->setDouble("at", 20.0);
  CHECK_EQ(exec(f.ed.get(), sq::doc::kSplitClip, p.get()), SQ_E_INVALIDARG);
  // resize left edge earlier than the file start is rejected
  auto rz = sq::makeAttributes();
  rz->setGuid("node", &c);
  rz->setDouble("start", 0.0);
  rz->setDouble("length", 6.0);
  CHECK_EQ(exec(f.ed.get(), sq::doc::kResizeClip, rz.get()), SQ_E_INVALIDARG);
  rz->setDouble("start", 2.0);
  rz->setDouble("length", 4.0);
  CHECK_OK(exec(f.ed.get(), sq::doc::kResizeClip, rz.get()));
  CHECK(near(f.st().clip(c)->sourceOffset, 0.0));

  sq_guid inst = addTrack(f.ed.get(), SQ_TRACK_INSTRUMENT);
  sq_guid m = addClip(f.ed.get(), inst, SQ_CLIP_MIDI, 0.0, 4.0);
  CHECK_OK(setEvents(f.ed.get(), m, {note(0.0, 3.0, 60), note(2.5, 0.5, 62)}));
  auto sp = sq::makeAttributes();
  sp->setGuid("node", &m);
  sp->setDouble("at", 2.0);
  sq_guid mr{};
  REQUIRE_OK(exec(f.ed.get(), sq::doc::kSplitClip, sp.get(), &mr));
  REQUIRE_EQ(f.st().clip(m)->events.size(), size_t(1));
  CHECK(near(f.st().clip(m)->events[0].len, 2.0));  // truncated at the cut
  REQUIRE_EQ(f.st().clip(mr)->events.size(), size_t(1));
  CHECK(near(f.st().clip(mr)->events[0].pos, 0.5));

  // locked clips refuse edits
  CHECK_OK(setPropB(f.ed.get(), c, "locked", true));
  auto mv = sq::makeAttributes();
  mv->setGuid("node", &c);
  mv->setDouble("start", 10.0);
  CHECK_EQ(exec(f.ed.get(), sq::doc::kMoveClip, mv.get()), SQ_E_ACCESS);
}

SQ_TEST("doc: removing a folder removes children, clips, and re-routes sends; undo restores order") {
  Fx f;
  sq_guid bus = addTrack(f.ed.get(), SQ_TRACK_BUS, "Bus");
  sq_guid folder = addTrack(f.ed.get(), SQ_TRACK_FOLDER, "Folder");
  sq_guid k1 = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "K1", &folder);
  sq_guid k2 = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "K2", &folder);
  sq_guid tail = addTrack(f.ed.get(), SQ_TRACK_AUDIO, "Tail");
  addClip(f.ed.get(), k1, SQ_CLIP_AUDIO, 0, 4);
  addClip(f.ed.get(), k2, SQ_CLIP_AUDIO, 0, 4);
  auto po = sq::makeAttributes();
  po->setGuid("node", &tail);
  po->setString("key", "output");
  po->setGuid("value", &bus);
  CHECK_OK(exec(f.ed.get(), sq::doc::kSetProperty, po.get()));
  Ptr<sq::INode> fn;
  REQUIRE_OK(f.doc->findNode(&folder, fn.put()));
  CHECK_EQ(fn->getChildCount(), 2u);
  CHECK_EQ(f.doc->getChildCount(), 1u + 4u);  // tempo + master, bus, folder, tail
  const std::string h = f.hash();

  auto rm = sq::makeAttributes();
  rm->setGuid("node", &bus);
  CHECK_OK(exec(f.ed.get(), sq::doc::kRemoveNode, rm.get()));
  CHECK(sq_guid_is_null(&f.st().track(tail)->output));
  rm->setGuid("node", &folder);
  CHECK_OK(exec(f.ed.get(), sq::doc::kRemoveNode, rm.get()));
  CHECK_EQ(f.st().trackOrder.size(), 2u);
  CHECK_EQ(f.st().clips.size(), 0u);
  REQUIRE_OK(f.ed->undo());
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.hash(), h);

  // move the folder block to the end
  auto mt = sq::makeAttributes();
  mt->setGuid("node", &folder);
  mt->setInt("index", 99);
  CHECK_OK(exec(f.ed.get(), sq::edoc::kMoveTrack, mt.get()));
  const auto& o = f.st().trackOrder;
  REQUIRE_EQ(o.size(), size_t(6));
  CHECK(o[3] == folder);
  CHECK(o[4] == k1);
  CHECK(o[5] == k2);
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.hash(), h);
}

SQ_TEST("doc: tempo map math with ramps") {
  sq::edoc::TempoMapRec m;
  m.tempos = {{0.0, 120.0, SQ_TEMPO_RAMP}, {4.0, 240.0, 0}, {8.0, 60.0, 0}};
  // ramp 120->240 over 4 ppq: 60/k ln(240/120), k = 30 bpm/ppq -> 2 ln 2
  CHECK(near(m.ppqToSeconds(4.0), 2.0 * std::log(2.0)));
  CHECK(near(m.ppqToSeconds(8.0), 2.0 * std::log(2.0) + 1.0));
  CHECK(near(m.ppqToSeconds(9.0), 2.0 * std::log(2.0) + 2.0));
  for (double p : {0.0, 0.5, 1.7, 3.99, 4.0, 6.0, 8.0, 12.5}) CHECK(near(m.secondsToPpq(m.ppqToSeconds(p)), p, 1e-9));
}

SQ_TEST("side effects: remove-overlaps splits, trims and removes; one undo step") {
  Fx f;
  sq_guid a = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  sq_guid big = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 0.0, 16.0, "big.wav");
  sq_guid small = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 20.0, 2.0);
  CHECK_EQ(f.st().clips.size(), 2u);  // overlaps allowed while the option is off
  CHECK_OK(setPropB(f.ed.get(), f.st().props.id, "removeOverlaps", true));
  const std::string h = f.hash();
  const uint32_t steps = f.ed->getUndoCount();
  sq_guid mid = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 4.0, 4.0);
  CHECK_EQ(f.ed->getUndoCount(), steps + 1);
  REQUIRE_EQ(f.st().clips.size(), size_t(4));  // big split around mid
  CHECK(near(f.st().clip(big)->length, 4.0));
  const auto& on = f.st().clipsOf(a);
  const sq::edoc::ClipRec& rightPart = f.st().clips.at(on[2]);
  CHECK(near(rightPart.start, 8.0));
  CHECK(near(rightPart.sourceOffset, 4.0));  // 8 ppq @120
  auto mv = sq::makeAttributes();
  mv->setGuid("node", &mid);
  mv->setDouble("start", 19.0);
  CHECK_OK(exec(f.ed.get(), sq::doc::kMoveClip, mv.get()));  // covers `small` completely
  CHECK(f.st().clip(small) == nullptr);
  REQUIRE_OK(f.ed->undo());
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.hash(), h);
}

SQ_TEST("side effects: automation follows moved clips") {
  Fx f;
  sq_guid a = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  sq_guid b = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  sq_guid c = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 0.0, 4.0);
  CHECK_OK(setAutomation(f.ed.get(), a, "volume", {0.0, 0.1, 2.0, 0.5, 6.0, 0.9}));
  CHECK_OK(setPropB(f.ed.get(), f.st().props.id, "automationFollowsEdits", true));
  const std::string h = f.hash();
  auto mv = sq::makeAttributes();
  mv->setGuid("node", &c);
  mv->setDouble("start", 8.0);
  mv->setGuid("track", &b);
  CHECK_OK(exec(f.ed.get(), sq::doc::kMoveClip, mv.get()));
  const auto& src = f.st().track(a)->automation.at("volume");
  const auto& dst = f.st().track(b)->automation.at("volume");
  REQUIRE_EQ(src.size(), size_t(1));
  CHECK(near(src[0].pos, 6.0));
  REQUIRE_EQ(dst.size(), size_t(2));
  CHECK(near(dst[0].pos, 8.0));
  CHECK(near(dst[1].pos, 10.0));
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.hash(), h);
}

SQ_TEST("side effects: time-map keeps time-based clips at the same seconds") {
  Fx f;
  sq_guid a = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  sq_guid musical = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  CHECK_OK(setPropS(f.ed.get(), a, "timebase", "time"));
  sq_guid c = addClip(f.ed.get(), a, SQ_CLIP_AUDIO, 4.0, 4.0);  // 2 s .. 4 s @120
  sq_guid m = addClip(f.ed.get(), musical, SQ_CLIP_AUDIO, 4.0, 4.0);
  const std::string h = f.hash();
  CHECK_OK(setTempo(f.ed.get(), 0.0, 60.0));
  CHECK(near(f.st().clip(c)->start, 2.0));
  CHECK(near(f.st().clip(c)->length, 2.0));
  CHECK(near(f.st().clip(m)->start, 4.0));
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.hash(), h);
}

SQ_TEST("side effects: chord-follow transposes follower clips") {
  Fx f;
  sq_guid chords = addTrack(f.ed.get(), SQ_TRACK_INSTRUMENT, "Chords");
  sq_guid lead = addTrack(f.ed.get(), SQ_TRACK_INSTRUMENT, "Lead");
  CHECK_OK(setPropB(f.ed.get(), chords, "chordTrack", true));
  CHECK_OK(setPropB(f.ed.get(), lead, "followChords", true));
  sq_guid cc = addClip(f.ed.get(), chords, SQ_CLIP_MIDI, 0.0, 4.0);
  sq_guid lc = addClip(f.ed.get(), lead, SQ_CLIP_MIDI, 0.0, 8.0);
  CHECK_OK(setEvents(f.ed.get(), cc, {note(0, 4, 48), note(0, 4, 52), note(0, 4, 55)}));  // C major
  CHECK_OK(setEvents(f.ed.get(), lc, {note(1, 1, 64), note(5, 1, 67)}));
  const std::string h = f.hash();
  CHECK_OK(setEvents(f.ed.get(), cc, {note(0, 4, 50), note(0, 4, 53), note(0, 4, 57)}));  // D minor
  const auto& ev = f.st().clip(lc)->events;
  CHECK_EQ(int(ev[0].bytes[1]), 66);  // inside the chord range: +2
  CHECK_EQ(int(ev[1].bytes[1]), 67);  // outside: untouched
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.hash(), h);
}

SQ_TEST("journals: UI state rides along with transactions but never makes undo steps") {
  Fx f;
  f.impl->setJournal("selection", "none");
  CHECK_EQ(f.ed->getUndoCount(), 0u);
  CHECK(!f.impl->modified());
  REQUIRE_OK(f.ed->beginTransaction("select only"));
  f.impl->setJournal("selection", "track1");
  REQUIRE_OK(f.ed->commitTransaction());
  CHECK_EQ(f.ed->getUndoCount(), 0u);
  CHECK(!f.impl->modified());
  CHECK_EQ(f.impl->journal("selection"), std::string("track1"));

  REQUIRE_OK(f.ed->beginTransaction("add + select"));
  sq_guid t = addTrack(f.ed.get(), SQ_TRACK_AUDIO);
  f.impl->setJournal("selection", sq::edoc::guidStr(t));
  REQUIRE_OK(f.ed->commitTransaction());
  REQUIRE_OK(f.ed->undo());
  CHECK_EQ(f.impl->journal("selection"), std::string("track1"));
  REQUIRE_OK(f.ed->redo());
  CHECK_EQ(f.impl->journal("selection"), sq::edoc::guidStr(t));
}
