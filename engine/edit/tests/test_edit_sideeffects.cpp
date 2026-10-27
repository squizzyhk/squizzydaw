// engine/edit - side effects: tempo edits move time-based content and keep unstretched audio
// durations (audio-time-follow), fades stay inside clips (fade-clamp), and engine/doc's
// remove-overlaps / automation-follow fire (exactly once) for the edit actions.
#include "edit_test_util.hpp"
#include "sq/app/host_context.hpp"
#include "sq/app/settings.hpp"

using namespace edittest;
namespace ed = sq::edit;

namespace {
sq_result setTempo(Fx& fx, double pos, double bpm) {
  auto p = sq::makeAttributes();
  p->setDouble("position", pos);
  p->setDouble("bpm", bpm);
  return exec(fx, sq::doc::kSetTempo, p.get());
}
double secsOf(Fx& fx, double ppq) { return fx.st().tempo.ppqToSeconds(ppq); }
}  // namespace

SQ_TEST("tempo change: musical events keep bars, time-based clips keep seconds, audio keeps its duration") {
  Fx fx;
  const sq_guid m = addTrack(fx, SQ_TRACK_INSTRUMENT, "keys");
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO, "loop");
  const sq_guid t = addTrack(fx, SQ_TRACK_AUDIO, "dialog");
  CHECK_OK(setPropS(fx, t, "timebase", "time"));
  const sq_guid cm = addClip(fx, m, SQ_CLIP_MIDI, 8, 4);
  CHECK_OK(setEvents(fx, cm, {note(1.0, 1.0, 60)}));
  const sq_guid ca = addClip(fx, a, SQ_CLIP_AUDIO, 8, 4, "loop.wav");      // 2 s at 120
  const sq_guid cs = addClip(fx, a, SQ_CLIP_AUDIO, 16, 4, "stretch.wav");  // follows the tempo
  CHECK_OK(setPropB(fx, cs, "user.stretch", true));
  const sq_guid ct = addClip(fx, t, SQ_CLIP_AUDIO, 8, 4, "dialog.wav");  // at 4 s
  CHECK(near(secsOf(fx, 8), 4.0));
  const std::string before = fx.hash();

  CHECK_OK(setTempo(fx, 0, 60.0));  // half speed: one undo step including every side effect
  CHECK_EQ(fx.ed->getUndoCount(), 11u);
  // musical-time content stays on its bar, so it now plays later (event follows its clip)
  CHECK(near(fx.clip(cm).start, 8.0));
  CHECK(near(secsOf(fx, fx.clip(cm).start + fx.clip(cm).events[0].pos), 9.0));
  // unstretched audio on a musical track: same bar, same number of seconds of source
  CHECK(near(fx.clip(ca).start, 8.0));
  CHECK(near(secsOf(fx, fx.clip(ca).end()) - secsOf(fx, fx.clip(ca).start), 2.0));
  CHECK(near(fx.clip(ca).length, 2.0));
  // stretched audio follows the bars
  CHECK(near(fx.clip(cs).length, 4.0));
  // time-based track (engine/doc time-map): keeps its position in seconds
  CHECK(near(secsOf(fx, fx.clip(ct).start), 4.0));
  CHECK(near(fx.clip(ct).start, 4.0));

  const std::string after = fx.hash();
  CHECK_OK(fx.ed->undo());
  CHECK_EQ(fx.hash(), before);
  CHECK(near(fx.clip(ca).length, 4.0));
  CHECK_OK(fx.ed->redo());
  CHECK_EQ(fx.hash(), after);
}

SQ_TEST("tempo edits by edit actions (fit tempo, insert/delete time) fire the same side effects") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 8, 4, "loop.wav");  // 2 s
  auto p = sq::makeAttributes();
  p->setDouble("from", 0.0);
  p->setDouble("to", 8.0);
  p->setDouble("seconds", 2.0);  // 240 bpm before the clip, 120 after
  CHECK_OK(execUndoRedo(fx, ed::kFitTempo, p.get()));
  CHECK(near(fx.clip(c).start, 8.0));
  CHECK(near(secsOf(fx, 8.0), 2.0));
  CHECK(near(secsOf(fx, fx.clip(c).end()) - secsOf(fx, 8.0), 2.0));
  // a fit across the clip: still 2 s of source
  auto q = sq::makeAttributes();
  q->setDouble("from", 8.0);
  q->setDouble("to", 16.0);
  q->setDouble("seconds", 8.0);  // 60 bpm
  CHECK_OK(execUndoRedo(fx, ed::kFitTempo, q.get()));
  CHECK(near(fx.clip(c).length, 2.0));
  // insert time before the clip: the clip moves with the tempo, its duration is unchanged
  auto ins = sq::makeAttributes();
  ins->setDouble("at", 4.0);
  ins->setDouble("length", 4.0);
  CHECK_OK(execUndoRedo(fx, ed::kInsertTime, ins.get()));
  CHECK(near(fx.clip(c).start, 12.0));
  CHECK(near(secsOf(fx, fx.clip(c).end()) - secsOf(fx, fx.clip(c).start), 2.0));
}

SQ_TEST("audio-time-follow: clips placed after the tempo edit in the same transaction stay put") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 4, "x.wav");
  CHECK_OK(fx.ed->beginTransaction("tempo + place"));
  CHECK_OK(setTempo(fx, 0, 60.0));
  auto p = nodes({c});
  p->setDouble("to", 6.0);
  CHECK_OK(exec(fx, ed::kTrimEnd, p.get()));
  CHECK_OK(fx.ed->commitTransaction());
  CHECK(near(fx.clip(c).length, 6.0));  // what the user asked for, not re-derived
}

SQ_TEST("installSideEffects is idempotent (a second install would apply audio-time-follow twice)") {
  Fx fx;
  ed::installSideEffects(*fx.impl);
  ed::installSideEffects(*fx.impl);
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 4, "x.wav");
  CHECK_OK(setTempo(fx, 0, 240.0));
  CHECK(near(fx.clip(c).length, 8.0));  // still 2 s
}

SQ_TEST("fade-clamp: fades are scaled into the clip after trims, tempo changes and explicit fades") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 8, "x.wav");  // 4 s
  auto f = nodes({c});
  f->setDouble("in", 1.0);
  f->setDouble("out", 3.0);
  CHECK_OK(execUndoRedo(fx, ed::kFade, f.get()));
  CHECK(near(fx.clip(c).fadeIn, 1.0));
  auto t = nodes({c});
  t->setDouble("to", 4.0);  // 2 s left: 1 + 3 -> 0.5 + 1.5
  CHECK_OK(execUndoRedo(fx, ed::kTrimEnd, t.get()));
  CHECK(near(fx.clip(c).fadeIn, 0.5));
  CHECK(near(fx.clip(c).fadeOut, 1.5));
  auto big = nodes({c});
  big->setDouble("in", 10.0);
  big->setDouble("out", 0.0);
  CHECK_OK(execUndoRedo(fx, ed::kFade, big.get()));
  CHECK(near(fx.clip(c).fadeIn, 2.0));
  // a stretched clip gets shorter in seconds when the tempo doubles
  CHECK_OK(setPropB(fx, c, "user.stretch", true));
  CHECK_OK(setTempo(fx, 0, 240.0));
  CHECK(near(fx.clip(c).fadeIn, 1.0));
  // engine/doc's SetProperty path is clamped too
  CHECK_OK(setProp(fx, c, "fadeOut", 5.0));
  CHECK(near(fx.clip(c).fadeIn + fx.clip(c).fadeOut, 1.0));
}

SQ_TEST("remove-overlaps fires for move and duplicate, not for an intended crossfade") {
  Fx fx;
  CHECK_OK(setPropB(fx, fx.st().props.id, "removeOverlaps", true));
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 0, 4, "x.wav");
  const sq_guid c2 = addClip(fx, a, SQ_CLIP_AUDIO, 8, 4, "x.wav");
  auto m = nodes({c1});
  m->setDouble("start", 6.0);  // covers 6..10: c2 is trimmed to 10..12
  CHECK_OK(execUndoRedo(fx, ed::kMove, m.get()));
  CHECK(near(fx.clip(c2).start, 10.0));
  CHECK(near(fx.clip(c2).sourceOffset, 1.0));
  auto d = nodes({c1});
  d->setDouble("offset", 5.0);  // copy at 11..15 covers the rest of c2
  CHECK_OK(execUndoRedo(fx, ed::kDuplicate, d.get()));
  CHECK(near(fx.clip(c2).end(), 11.0));
  auto x = nodes({c1, c2});
  x->setDouble("length", 0.5);
  CHECK_OK(execUndoRedo(fx, ed::kCrossfade, x.get()));
  CHECK(fx.clip(c1).end() > fx.clip(c2).start);  // overlap kept
}

SQ_TEST("automation-follow moves points with a moved clip, and exactly once for insert time") {
  Fx fx;
  CHECK_OK(setPropB(fx, fx.st().props.id, "automationFollowsEdits", true));
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 4, 4, "x.wav");
  std::vector<double> pts{5.0, 0.25, 10.0, 0.75};
  auto ap = sq::makeAttributes();
  ap->setGuid("track", &a);
  ap->setString("param", "volume");
  ap->setBlob("points", pts.data(), static_cast<uint32_t>(pts.size() * sizeof(double)));
  CHECK_OK(exec(fx, sq::edoc::kSetAutomation, ap.get()));
  auto m = nodes({c});
  m->setDouble("delta", 8.0);
  CHECK_OK(execUndoRedo(fx, ed::kMove, m.get()));
  auto lane = fx.st().track(a)->automation.at("volume");
  REQUIRE_EQ(lane.size(), size_t(2));
  CHECK(near(lane[0].pos, 10.0));  // the untouched point
  CHECK(near(lane[1].pos, 13.0));  // moved with the clip (5 -> 13)
  CHECK_OK(fx.ed->undo());
  auto ins = sq::makeAttributes();
  ins->setDouble("at", 0.0);
  ins->setDouble("length", 2.0);
  CHECK_OK(execUndoRedo(fx, ed::kInsertTime, ins.get()));
  lane = fx.st().track(a)->automation.at("volume");
  CHECK(near(lane[0].pos, 7.0));
  CHECK(near(lane[1].pos, 12.0));
}

SQ_TEST("host wiring: documents opened after registerEditServices get the side effects; unregister cleans up") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::edoc::DocumentManagerImpl* mgr = nullptr;
  REQUIRE_OK(sq::edoc::registerDocumentServices(&host, &mgr));
  REQUIRE_OK(ed::registerEditServices(&host));
  CHECK_EQ(ed::registerEditServices(&host), SQ_E_EXISTS);
  {
    Ptr<sq::IDocument> doc;
    REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, doc.put()));
    Ptr<sq::IEditor> e;
    REQUIRE_OK(doc->getEditor(e.put()));
    sq::edoc::DocumentImpl* impl = sq::edoc::DocumentImpl::from(doc.get());
    // only engine/doc actions: the side effect must already be installed by the observer
    auto tp = sq::makeAttributes();
    tp->setInt("kind", SQ_TRACK_AUDIO);
    auto tr = sq::makeAttributes();
    REQUIRE_OK(e->execute(&sq::doc::kAddTrack, tp.get(), tr.get()));
    sq_guid track{};
    tr->getGuid("node", &track);
    auto cp = sq::makeAttributes();
    cp->setGuid("track", &track);
    cp->setInt("kind", SQ_CLIP_AUDIO);
    cp->setDouble("start", 0.0);
    cp->setDouble("length", 4.0);
    auto cr = sq::makeAttributes();
    REQUIRE_OK(e->execute(&sq::doc::kAddClip, cp.get(), cr.get()));
    sq_guid clip{};
    cr->getGuid("node", &clip);
    auto t = sq::makeAttributes();
    t->setDouble("position", 0.0);
    t->setDouble("bpm", 60.0);
    REQUIRE_OK(e->execute(&sq::doc::kSetTempo, t.get(), nullptr));
    CHECK(near(impl->store().clip(clip)->length, 2.0));
    // commands are in the registry and act on the active document
    Ptr<sq::ICommandHandler> h;
    REQUIRE_OK(reg.createInstance(&ed::cmd::kSelectAll, &sq::ICommandHandler::iid, h.putVoid()));
    CHECK_OK(h->interpretCommand(nullptr, nullptr, SQ_TRUE));
    CHECK_OK(h->interpretCommand(nullptr, nullptr, SQ_FALSE));
    CHECK_EQ(ed::readEditState(*impl).selection.size(), size_t(1));
    mgr->closeDocument(doc.get());
  }
  ed::unregisterEditServices(&host);
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  CHECK_EQ(reg.findClass(&ed::cmd::kSelectAll, &ci), SQ_E_NOTFOUND);
  CHECK_EQ(reg.findClass(&ed::kSplit, &ci), SQ_E_NOTFOUND);
  REQUIRE_OK(ed::registerEditServices(&host));  // re-registrable
  ed::unregisterEditServices(&host);
  sq::edoc::unregisterDocumentServices(&host);
}
