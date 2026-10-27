// engine/edit - every edit action: behaviour, failure atomicity, undo/redo exactness (state hash).
#include <cstring>

#include "edit_test_util.hpp"

using namespace edittest;
namespace ed = sq::edit;

namespace {
struct MediaScope {  // installs a registry with the fake codec for the media-reading actions
  sq::app::ClassRegistry reg;
  MediaScope() {
    registerFakeCodec(reg);
    ed::setMediaRegistry(&reg);
  }
  ~MediaScope() { ed::setMediaRegistry(nullptr); }
};
std::string userString(const ClipRec& c, const char* key) {
  auto it = c.user.find(key);
  return it == c.user.end() ? std::string() : it->second.s;
}
}  // namespace

// ---- split -----------------------------------------------------------------------------------------
SQ_TEST("split: audio keeps source alignment; MIDI events are divided; result = right parts") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO), m = addTrack(fx, SQ_TRACK_INSTRUMENT);
  const sq_guid ca = addClip(fx, a, SQ_CLIP_AUDIO, 0, 8, "a.wav", 1.0);
  const sq_guid cm = addClip(fx, m, SQ_CLIP_MIDI, 2, 6);
  CHECK_OK(setEvents(fx, cm, {note(0.5, 1.0, 60), note(1.5, 2.0, 62), note(3.0, 1.0, 64)}));
  auto p = nodes({ca, cm});
  p->setDouble("at", 4.0);
  auto r = sq::makeAttributes();
  CHECK_OK(execUndoRedo(fx, ed::kSplit, p.get(), r.get()));
  auto rights = resultNodes(r.get());
  REQUIRE_EQ(rights.size(), size_t(2));
  CHECK(near(fx.clip(ca).length, 4.0));
  CHECK(near(fx.clip(rights[0]).start, 4.0));
  CHECK(near(fx.clip(rights[0]).sourceOffset, 1.0 + 2.0));  // 4 ppq at 120 bpm = 2 s
  // MIDI: left keeps notes before the cut (the long one is shortened), right gets the rest
  CHECK_EQ(fx.clip(cm).events.size(), size_t(2));
  CHECK(near(fx.clip(cm).events[1].len, 0.5));
  CHECK_EQ(fx.clip(rights[1]).events.size(), size_t(1));
  CHECK(near(fx.clip(rights[1]).events[0].pos, 1.0));
}

SQ_TEST("split: snapping, positions outside every target, locked clips") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 1, 8, "a.wav");
  auto p = nodes({c});
  p->setDouble("at", 3.3);
  p->setString("snap", "grid");
  p->setDouble("grid", 1.0);
  CHECK_OK(execUndoRedo(fx, ed::kSplit, p.get()));
  CHECK(near(fx.clip(c).end(), 3.0));
  auto q = nodes({c});
  q->setDouble("at", 20.0);
  CHECK_EQ(execUndoRedo(fx, ed::kSplit, q.get()), SQ_E_INVALIDARG);
  CHECK_OK(setPropB(fx, c, "locked", true));
  auto l = nodes({c});
  l->setDouble("at", 2.0);
  CHECK_EQ(execUndoRedo(fx, ed::kSplit, l.get()), SQ_E_ACCESS);
  auto bad = nodes({c});
  bad->setString("snap", "sideways");
  bad->setDouble("at", 2.0);
  CHECK_EQ(exec(fx, ed::kSplit, bad.get()), SQ_E_INVALIDARG);
}

// ---- trim ------------------------------------------------------------------------------------------
SQ_TEST("trim start/end: content stays put, audio clamps to the source start and (known) end") {
  MediaScope ms;
  media()["t.fake"] = FakeMedia{1000.0, std::vector<float>(10000, 0.1f)};  // 10 s
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 4, 4, "t.fake", 1.0);  // source origin at ppq 2
  auto p = nodes({c});
  p->setDouble("to", 1.0);
  CHECK_OK(execUndoRedo(fx, ed::kTrimStart, p.get()));
  CHECK(near(fx.clip(c).start, 2.0));
  CHECK(near(fx.clip(c).sourceOffset, 0.0));
  CHECK(near(fx.clip(c).end(), 8.0));
  auto q = nodes({c});
  q->setDouble("to", 5.0);
  CHECK_OK(execUndoRedo(fx, ed::kTrimStart, q.get()));
  CHECK(near(fx.clip(c).sourceOffset, 1.5));
  auto e = nodes({c});
  e->setDouble("to", 100.0);  // the source ends 10 s after its origin (ppq 2 + 20)
  CHECK_OK(execUndoRedo(fx, ed::kTrimEnd, e.get()));
  CHECK(near(fx.clip(c).end(), 22.0));
  auto bad = nodes({c});
  bad->setDouble("to", 5.0);  // == start: nothing would remain
  CHECK_EQ(execUndoRedo(fx, ed::kTrimEnd, bad.get()), SQ_E_INVALIDARG);
}

SQ_TEST("trim: MIDI events keep their timeline position") {
  Fx fx;
  const sq_guid m = addTrack(fx, SQ_TRACK_INSTRUMENT);
  const sq_guid c = addClip(fx, m, SQ_CLIP_MIDI, 4, 4);
  CHECK_OK(setEvents(fx, c, {note(1.0, 0.5, 60)}));
  auto p = nodes({c});
  p->setDouble("to", 2.0);
  CHECK_OK(execUndoRedo(fx, ed::kTrimStart, p.get()));
  CHECK(near(fx.clip(c).start, 2.0));
  CHECK(near(fx.clip(c).start + fx.clip(c).events[0].pos, 5.0));
}

// ---- slip ------------------------------------------------------------------------------------------
SQ_TEST("slip: audio shifts the source offset (clamped at 0), MIDI shifts events; bounds stay") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO), m = addTrack(fx, SQ_TRACK_INSTRUMENT);
  const sq_guid ca = addClip(fx, a, SQ_CLIP_AUDIO, 4, 4, "a.wav", 2.0);
  const sq_guid cm = addClip(fx, m, SQ_CLIP_MIDI, 0, 4);
  CHECK_OK(setEvents(fx, cm, {note(1.0, 0.5, 60)}));
  auto p = nodes({ca, cm});
  p->setDouble("delta", 1.0);
  CHECK_OK(execUndoRedo(fx, ed::kSlip, p.get()));
  CHECK(near(fx.clip(ca).sourceOffset, 1.5));
  CHECK(near(fx.clip(ca).start, 4.0));
  CHECK(near(fx.clip(cm).events[0].pos, 2.0));
  auto s = nodes({ca});
  s->setDouble("delta_seconds", 5.0);
  CHECK_OK(execUndoRedo(fx, ed::kSlip, s.get()));
  CHECK(near(fx.clip(ca).sourceOffset, 0.0));
  auto both = nodes({ca});
  both->setDouble("delta", 1.0);
  both->setDouble("delta_seconds", 1.0);
  CHECK_EQ(exec(fx, ed::kSlip, both.get()), SQ_E_INVALIDARG);
}

// ---- move ------------------------------------------------------------------------------------------
SQ_TEST("move: delta/start, grid/relative/bar/events snapping, clamping at 0") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 1.25, 1, "a.wav");
  const sq_guid c2 = addClip(fx, a, SQ_CLIP_AUDIO, 3.25, 1, "a.wav");
  const sq_guid other = addClip(fx, a, SQ_CLIP_AUDIO, 10.1, 1, "a.wav");
  auto p = nodes({c1, c2});
  p->setDouble("delta", 2.1);
  p->setString("snap", "grid");
  CHECK_OK(execUndoRedo(fx, ed::kMove, p.get()));
  CHECK(near(fx.clip(c1).start, 3.0));  // earliest clip lands on the grid
  CHECK(near(fx.clip(c2).start, 5.0));  // the group moves together
  auto r = nodes({c1});
  r->setDouble("delta", 0.9);
  r->setString("snap", "relative");
  r->setDouble("grid", 0.5);
  CHECK_OK(execUndoRedo(fx, ed::kMove, r.get()));
  CHECK(near(fx.clip(c1).start, 4.0));  // delta rounded to 1.0, offset from the grid kept
  {
    auto sig = sq::makeAttributes();
    sig->setDouble("position", 0);
    sig->setInt("numerator", 3);
    sig->setInt("denominator", 4);
    CHECK_OK(exec(fx, sq::doc::kSetSignature, sig.get()));
  }
  auto b = nodes({c1});
  b->setDouble("start", 7.2);
  b->setString("snap", "bar");
  CHECK_OK(execUndoRedo(fx, ed::kMove, b.get()));
  CHECK(near(fx.clip(c1).start, 6.0));  // 3/4 bars: 0, 3, 6, 9
  auto e = nodes({c1});
  e->setDouble("start", 9.8);
  e->setString("snap", "events");
  e->setDouble("tolerance", 0.5);
  CHECK_OK(execUndoRedo(fx, ed::kMove, e.get()));
  CHECK(near(fx.clip(c1).start, 10.1));  // the other clip's start
  CHECK(fx.exists(other));
  auto z = nodes({c1});
  z->setDouble("delta", -100.0);
  CHECK_OK(execUndoRedo(fx, ed::kMove, z.get()));
  CHECK(near(fx.clip(c1).start, 0.0));
}

SQ_TEST("move: to another track, by track offset, kind mismatch, locked") {
  Fx fx;
  const sq_guid a1 = addTrack(fx, SQ_TRACK_AUDIO), a2 = addTrack(fx, SQ_TRACK_AUDIO), m = addTrack(fx, SQ_TRACK_INSTRUMENT);
  const sq_guid c = addClip(fx, a1, SQ_CLIP_AUDIO, 0, 2, "a.wav");
  auto p = nodes({c});
  p->setGuid("track", &a2);
  p->setDouble("delta", 1.0);
  CHECK_OK(execUndoRedo(fx, ed::kMove, p.get()));
  CHECK(fx.clip(c).track == a2);
  auto o = nodes({c});
  o->setInt("track_offset", -1);
  CHECK_OK(execUndoRedo(fx, ed::kMove, o.get()));
  CHECK(fx.clip(c).track == a1);
  auto bad = nodes({c});
  bad->setGuid("track", &m);
  CHECK_EQ(execUndoRedo(fx, ed::kMove, bad.get()), SQ_E_INVALIDARG);
  CHECK_OK(setPropB(fx, c, "locked", true));
  auto l = nodes({c});
  l->setDouble("delta", 1.0);
  CHECK_EQ(execUndoRedo(fx, ed::kMove, l.get()), SQ_E_ACCESS);
}

// ---- fades / crossfade -----------------------------------------------------------------------------
SQ_TEST("fade: lengths and curves; invalid curves and MIDI targets are refused") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO), m = addTrack(fx, SQ_TRACK_INSTRUMENT);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 8, "a.wav");  // 4 s
  const sq_guid cm = addClip(fx, m, SQ_CLIP_MIDI, 0, 8);
  auto p = nodes({c});
  p->setDouble("in", 0.5);
  p->setDouble("out", 1.0);
  p->setString("in_curve", "exponential");
  p->setString("out_curve", "scurve");
  CHECK_OK(execUndoRedo(fx, ed::kFade, p.get()));
  CHECK(near(fx.clip(c).fadeIn, 0.5));
  CHECK(near(fx.clip(c).fadeOut, 1.0));
  CHECK_EQ(userString(fx.clip(c), "user.fadeInCurve"), std::string("exponential"));
  CHECK_EQ(userString(fx.clip(c), "user.fadeOutCurve"), std::string("scurve"));
  auto bad = nodes({c});
  bad->setString("in_curve", "wobbly");
  CHECK_EQ(execUndoRedo(fx, ed::kFade, bad.get()), SQ_E_INVALIDARG);
  auto neg = nodes({c});
  neg->setDouble("in", -1.0);
  CHECK_EQ(execUndoRedo(fx, ed::kFade, neg.get()), SQ_E_INVALIDARG);
  auto midi = nodes({cm});
  midi->setDouble("in", 0.1);
  CHECK_EQ(execUndoRedo(fx, ed::kFade, midi.get()), SQ_E_UNSUPPORTED);
}

SQ_TEST("crossfade: touching clips overlap around the cut; overlaps fade over the overlap; gaps refused") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  CHECK_OK(setPropB(fx, fx.st().props.id, "removeOverlaps", true));  // the overlap must survive
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 0, 4, "a.wav");
  const sq_guid c2 = addClip(fx, a, SQ_CLIP_AUDIO, 4, 4, "a.wav", 2.0);
  auto p = nodes({c2, c1});
  p->setDouble("length", 0.2);
  auto r = sq::makeAttributes();
  CHECK_OK(execUndoRedo(fx, ed::kCrossfade, p.get(), r.get()));
  CHECK(near(r->getDouble("length", 0), 0.2));
  CHECK(near(fx.clip(c1).end(), 4.2));  // 0.1 s each way at 120 bpm = 0.2 ppq
  CHECK(near(fx.clip(c2).start, 3.8));
  CHECK(near(fx.clip(c2).sourceOffset, 1.9));
  CHECK(near(fx.clip(c1).fadeOut, 0.2));
  CHECK(near(fx.clip(c2).fadeIn, 0.2));
  CHECK_EQ(userString(fx.clip(c2), "user.fadeInCurve"), std::string("scurve"));
  // already overlapping: fade over the overlap, placement unchanged
  auto again = nodes({c1, c2});
  CHECK_OK(execUndoRedo(fx, ed::kCrossfade, again.get()));
  CHECK(near(fx.clip(c1).fadeOut, 0.2));
  // a gap
  const sq_guid c3 = addClip(fx, a, SQ_CLIP_AUDIO, 20, 2, "a.wav");
  auto gap = nodes({c2, c3});
  CHECK_EQ(execUndoRedo(fx, ed::kCrossfade, gap.get()), SQ_E_INVALIDARG);
  // touching but the right clip has no material before its start and the left source is
  // unknown: all handle comes from the left clip
  const sq_guid c4 = addClip(fx, a, SQ_CLIP_AUDIO, 22, 2, "b.wav");
  auto h = nodes({c3, c4});
  h->setDouble("length", 0.1);
  CHECK_OK(execUndoRedo(fx, ed::kCrossfade, h.get()));
  CHECK(near(fx.clip(c4).start, 22.0));
  CHECK(near(fx.clip(c3).end(), 22.2));
}

// ---- quantize --------------------------------------------------------------------------------------
SQ_TEST("quantize: MIDI note starts (strength, swing, lengths) and audio anchors") {
  Fx fx;
  const sq_guid m = addTrack(fx, SQ_TRACK_INSTRUMENT), a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, m, SQ_CLIP_MIDI, 0, 8);
  CHECK_OK(setEvents(fx, c, {note(0.1, 0.5, 60), note(1.9, 0.5, 62), note(3.2, 0.7, 64)}));
  const std::string h0 = fx.hash();
  auto p = nodes({c});
  p->setDouble("grid", 1.0);
  CHECK_OK(execUndoRedo(fx, ed::kQuantize, p.get()));
  CHECK(near(fx.clip(c).events[0].pos, 0.0));
  CHECK(near(fx.clip(c).events[1].pos, 2.0));
  CHECK(near(fx.clip(c).events[2].pos, 3.0));
  CHECK(near(fx.clip(c).events[2].len, 0.7));
  CHECK_OK(fx.ed->undo());
  CHECK_EQ(fx.hash(), h0);
  auto half = nodes({c});
  half->setDouble("grid", 1.0);
  half->setDouble("strength", 0.5);
  CHECK_OK(execUndoRedo(fx, ed::kQuantize, half.get()));
  CHECK(near(fx.clip(c).events[0].pos, 0.05));
  CHECK_OK(fx.ed->undo());
  auto sw = nodes({c});
  sw->setDouble("grid", 1.0);
  sw->setDouble("swing", 0.5);
  sw->setBool("lengths", SQ_TRUE);
  CHECK_OK(execUndoRedo(fx, ed::kQuantize, sw.get()));
  CHECK(near(fx.clip(c).events[1].pos, 2.0));   // even grid line
  CHECK(near(fx.clip(c).events[2].pos, 3.25));  // odd line: late by swing * grid / 2
  CHECK(near(fx.clip(c).events[2].len, 0.75));  // end 3.9 -> 4
  // audio: the anchor (0.25 s = 0.5 ppq into the clip) lands on the grid
  const sq_guid ca = addClip(fx, a, SQ_CLIP_AUDIO, 1.7, 2, "a.wav");
  auto an = sq::makeAttributes();
  an->setGuid("node", &ca);
  an->setString("key", "user.anchor");
  an->setDouble("value", 0.25);
  CHECK_OK(exec(fx, sq::doc::kSetProperty, an.get()));
  auto q = nodes({ca});
  q->setDouble("grid", 1.0);
  CHECK_OK(execUndoRedo(fx, ed::kQuantize, q.get()));
  CHECK(near(fx.clip(ca).start, 1.5));  // anchor 2.2 -> 2.0
  auto bad = nodes({c});
  bad->setDouble("strength", 2.0);
  CHECK_EQ(exec(fx, ed::kQuantize, bad.get()), SQ_E_INVALIDARG);
}

// ---- strip silence ---------------------------------------------------------------------------------
SQ_TEST("strip silence: one clip per audible region (through a codec), silent clips vanish") {
  MediaScope ms;
  FakeMedia fm;
  fm.rate = 1000.0;
  fm.samples.assign(8000, 0.0f);  // 8 s
  for (int i = 1000; i < 2000; ++i) fm.samples[i] = 0.5f;  // 1..2 s
  for (int i = 5000; i < 5500; ++i) fm.samples[i] = 0.5f;  // 5..5.5 s
  media()["s.fake"] = fm;
  media()["silent.fake"] = FakeMedia{1000.0, std::vector<float>(4000, 0.0f)};
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 16, "s.fake");  // 8 s
  const sq_guid z = addClip(fx, a, SQ_CLIP_AUDIO, 20, 4, "silent.fake");
  auto p = nodes({c, z});
  p->setDouble("pad_before", 0.0);
  p->setDouble("pad_after", 0.0);
  auto r = sq::makeAttributes();
  CHECK_OK(execUndoRedo(fx, ed::kStripSilence, p.get(), r.get()));
  auto kept = resultNodes(r.get());
  REQUIRE_EQ(kept.size(), size_t(2));
  CHECK(kept[0] == c);
  CHECK(near(fx.clip(kept[0]).start, 2.0, 1e-3));  // 1 s
  CHECK(near(fx.clip(kept[0]).end(), 4.0, 1e-3));
  CHECK(near(fx.clip(kept[0]).sourceOffset, 1.0, 1e-3));
  CHECK(near(fx.clip(kept[1]).start, 10.0, 1e-3));
  CHECK(near(fx.clip(kept[1]).end(), 11.0, 1e-3));
  CHECK(!fx.exists(z));
  // no codec for the file: refused, document unchanged
  const sq_guid u = addClip(fx, a, SQ_CLIP_AUDIO, 30, 2, "unknown.xyz");
  auto q = nodes({u});
  CHECK_EQ(execUndoRedo(fx, ed::kStripSilence, q.get()), SQ_E_UNSUPPORTED);
}

// ---- silence / duplicate / delete --------------------------------------------------------------------
SQ_TEST("silence: removes material inside the range without ripple; locked clips are skipped") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO), b = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 0, 8, "a.wav");
  const sq_guid c2 = addClip(fx, b, SQ_CLIP_AUDIO, 3, 2, "a.wav");
  const sq_guid c3 = addClip(fx, b, SQ_CLIP_AUDIO, 5.5, 4, "a.wav");
  const sq_guid lk = addClip(fx, b, SQ_CLIP_AUDIO, 10, 2, "a.wav");
  CHECK_OK(setPropB(fx, lk, "locked", true));
  auto p = sq::makeAttributes();
  p->setDouble("from", 2.0);
  p->setDouble("to", 6.0);
  CHECK_OK(execUndoRedo(fx, ed::kSilence, p.get()));
  CHECK(near(fx.clip(c1).end(), 2.0));
  CHECK(!fx.exists(c2));
  CHECK(near(fx.clip(c3).start, 6.0));
  CHECK(near(fx.clip(c3).sourceOffset, 0.25));
  CHECK_EQ(fx.st().clipsOf(a).size(), size_t(2));  // c1 was split: [0,2) and [6,8)
  auto t = sq::makeAttributes();
  t->setDouble("from", 10.0);
  t->setDouble("to", 11.0);
  CHECK_OK(execUndoRedo(fx, ed::kSilence, t.get()));
  CHECK(near(fx.clip(lk).length, 2.0));
}

SQ_TEST("duplicate: right after the span by default, count, explicit offset") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 1, 2, "a.wav");
  const sq_guid c2 = addClip(fx, a, SQ_CLIP_AUDIO, 3, 1, "a.wav");
  auto p = nodes({c1, c2});
  p->setInt("count", 2);
  auto r = sq::makeAttributes();
  CHECK_OK(execUndoRedo(fx, ed::kDuplicate, p.get(), r.get()));
  auto made = resultNodes(r.get());
  REQUIRE_EQ(made.size(), size_t(4));
  CHECK(near(fx.clip(made[0]).start, 4.0));
  CHECK(near(fx.clip(made[1]).start, 6.0));
  CHECK(near(fx.clip(made[2]).start, 7.0));
  CHECK(near(fx.clip(made[3]).start, 9.0));
  auto o = nodes({c1});
  o->setDouble("offset", 16.0);
  CHECK_OK(execUndoRedo(fx, ed::kDuplicate, o.get()));
  CHECK_EQ(fx.st().clipsOf(a).size(), size_t(7));
  auto bad = nodes({c1});
  bad->setInt("count", 0);
  CHECK_EQ(exec(fx, ed::kDuplicate, bad.get()), SQ_E_INVALIDARG);
}

SQ_TEST("delete: plain, ripple on the track, ripple on every track with markers and automation") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO), b = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid a1 = addClip(fx, a, SQ_CLIP_AUDIO, 0, 2, "a.wav");
  const sq_guid a2 = addClip(fx, a, SQ_CLIP_AUDIO, 2, 2, "a.wav");
  const sq_guid a3 = addClip(fx, a, SQ_CLIP_AUDIO, 6, 2, "a.wav");
  const sq_guid b1 = addClip(fx, b, SQ_CLIP_AUDIO, 5, 1, "a.wav");
  sq_guid mk{};
  {
    auto mp = sq::makeAttributes();
    mp->setDouble("position", 8.0);
    auto mr = sq::makeAttributes();
    CHECK_OK(exec(fx, sq::doc::kAddMarker, mp.get(), mr.get()));
    mr->getGuid("node", &mk);
    std::vector<double> pts{1.0, 0.5, 7.0, 1.0};
    auto ap = sq::makeAttributes();
    ap->setGuid("track", &b);
    ap->setString("param", "volume");
    ap->setBlob("points", pts.data(), static_cast<uint32_t>(pts.size() * sizeof(double)));
    CHECK_OK(exec(fx, sq::edoc::kSetAutomation, ap.get()));
  }
  const std::string h0 = fx.hash();
  auto t = nodes({a2});
  t->setString("ripple", "track");
  CHECK_OK(execUndoRedo(fx, ed::kDelete, t.get()));
  CHECK(!fx.exists(a2));
  CHECK(near(fx.clip(a3).start, 4.0));
  CHECK(near(fx.clip(b1).start, 5.0));  // other track untouched
  CHECK(near(fx.clip(a1).start, 0.0));
  CHECK_OK(fx.ed->undo());
  CHECK_EQ(fx.hash(), h0);
  auto all = nodes({a2});
  all->setString("ripple", "all");
  CHECK_OK(execUndoRedo(fx, ed::kDelete, all.get()));
  CHECK(near(fx.clip(a3).start, 4.0));
  CHECK(near(fx.clip(b1).start, 3.0));
  CHECK(near(fx.st().marker(mk)->position, 6.0));
  const auto& lane = fx.st().track(b)->automation.at("volume");
  CHECK(near(lane[0].pos, 1.0));  // before the gap
  CHECK(near(lane[1].pos, 5.0));
  CHECK_OK(fx.ed->undo());
  auto plain = nodes({a1, b1});
  CHECK_OK(execUndoRedo(fx, ed::kDelete, plain.get()));
  CHECK(near(fx.clip(a3).start, 6.0));
  auto bad = nodes({a3});
  bad->setString("ripple", "sideways");
  CHECK_EQ(exec(fx, ed::kDelete, bad.get()), SQ_E_INVALIDARG);
}

// ---- insert / delete time, fit tempo ------------------------------------------------------------------
SQ_TEST("insert time: splits clips at the point, shifts later content, markers, automation, tempo") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO), b = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 0, 8, "a.wav");
  const sq_guid c2 = addClip(fx, b, SQ_CLIP_AUDIO, 6, 2, "a.wav");
  auto tp = sq::makeAttributes();
  tp->setDouble("position", 8.0);
  tp->setDouble("bpm", 90.0);
  CHECK_OK(exec(fx, sq::doc::kSetTempo, tp.get()));
  auto mp = sq::makeAttributes();
  mp->setDouble("position", 5.0);
  CHECK_OK(exec(fx, sq::doc::kAddMarker, mp.get()));
  auto p = sq::makeAttributes();
  p->setDouble("at", 4.0);
  p->setDouble("length", 4.0);
  CHECK_OK(execUndoRedo(fx, ed::kInsertTime, p.get()));
  CHECK(near(fx.clip(c1).end(), 4.0));
  CHECK_EQ(fx.st().clipsOf(a).size(), size_t(2));
  const ClipRec& right = fx.clip(fx.st().clipsOf(a)[1]);
  CHECK(near(right.start, 8.0));
  CHECK(near(right.sourceOffset, 2.0));
  CHECK(near(fx.clip(c2).start, 10.0));
  CHECK(near(fx.st().tempo.tempos[1].pos, 12.0));
  CHECK(near(fx.st().marker(fx.st().markerOrder()[0])->position, 9.0));
  // only one track: tempo and markers stay
  auto q = sq::makeAttributes();
  q->setDouble("at", 0.0);
  q->setDouble("length", 1.0);
  std::vector<sq_guid> tr{b};
  q->setBlob("tracks", tr.data(), sizeof(sq_guid));
  CHECK_OK(execUndoRedo(fx, ed::kInsertTime, q.get()));
  CHECK(near(fx.clip(c2).start, 11.0));
  CHECK(near(fx.clip(c1).start, 0.0));
  CHECK(near(fx.st().tempo.tempos[1].pos, 12.0));
}

SQ_TEST("delete time: removes the range everywhere and closes the gap, cutting the tempo map") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 0, 6, "a.wav");
  const sq_guid c2 = addClip(fx, a, SQ_CLIP_AUDIO, 10, 2, "a.wav");
  auto tp = sq::makeAttributes();
  tp->setDouble("position", 6.0);
  tp->setDouble("bpm", 60.0);
  CHECK_OK(exec(fx, sq::doc::kSetTempo, tp.get()));
  tp->setDouble("position", 12.0);
  tp->setDouble("bpm", 100.0);
  CHECK_OK(exec(fx, sq::doc::kSetTempo, tp.get()));
  auto p = sq::makeAttributes();
  p->setDouble("from", 4.0);
  p->setDouble("to", 8.0);
  CHECK_OK(execUndoRedo(fx, ed::kDeleteTime, p.get()));
  CHECK(near(fx.clip(c1).end(), 4.0));
  CHECK(near(fx.clip(c2).start, 6.0));
  const auto& t = fx.st().tempo.tempos;
  REQUIRE_EQ(t.size(), size_t(3));
  CHECK(near(t[1].pos, 4.0));  // the 60 bpm in effect at 8 now starts at 4
  CHECK(near(t[1].bpm, 60.0));
  CHECK(near(t[2].pos, 8.0));
  auto bad = sq::makeAttributes();
  bad->setDouble("from", 4.0);
  bad->setDouble("to", 4.0);
  CHECK_EQ(exec(fx, ed::kDeleteTime, bad.get()), SQ_E_INVALIDARG);
}

SQ_TEST("fit tempo: the range lasts the requested seconds, later tempo kept") {
  Fx fx;
  auto p = sq::makeAttributes();
  p->setDouble("from", 4.0);
  p->setDouble("to", 8.0);
  p->setDouble("seconds", 1.0);
  auto r = sq::makeAttributes();
  CHECK_OK(execUndoRedo(fx, ed::kFitTempo, p.get(), r.get()));
  CHECK(near(r->getDouble("bpm", 0), 240.0));
  const auto& tm = fx.st().tempo;
  CHECK(near(tm.ppqToSeconds(8.0) - tm.ppqToSeconds(4.0), 1.0));
  CHECK(near(tm.ppqToSeconds(12.0) - tm.ppqToSeconds(8.0), 2.0));  // 120 bpm again after `to`
  auto bad = sq::makeAttributes();
  bad->setDouble("from", 0.0);
  bad->setDouble("to", 4.0);
  bad->setDouble("seconds", 1e-6);
  CHECK_EQ(exec(fx, ed::kFitTempo, bad.get()), SQ_E_INVALIDARG);
}

// ---- glue --------------------------------------------------------------------------------------------
SQ_TEST("glue: MIDI merges visible events; audio only for contiguous parts of one source") {
  Fx fx;
  const sq_guid m = addTrack(fx, SQ_TRACK_INSTRUMENT), a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid m1 = addClip(fx, m, SQ_CLIP_MIDI, 0, 2);
  const sq_guid m2 = addClip(fx, m, SQ_CLIP_MIDI, 4, 2);
  CHECK_OK(setEvents(fx, m1, {note(0.5, 0.5, 60), note(2.5, 0.5, 61)}));  // second one is hidden
  CHECK_OK(setEvents(fx, m2, {note(1.0, 2.0, 62)}));                     // truncated at the clip end
  auto p = nodes({m2, m1});
  auto r = sq::makeAttributes();
  CHECK_OK(execUndoRedo(fx, ed::kGlue, p.get(), r.get()));
  CHECK(!fx.exists(m2));
  const ClipRec& g = fx.clip(m1);
  CHECK(near(g.length, 6.0));
  REQUIRE_EQ(g.events.size(), size_t(2));
  CHECK(near(g.events[1].pos, 5.0));
  CHECK(near(g.events[1].len, 1.0));
  // audio: split parts of one recording glue back together
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 8, "rec.wav", 1.0);
  auto sp = nodes({c});
  sp->setDouble("at", 4.0);
  auto sr = sq::makeAttributes();
  CHECK_OK(exec(fx, ed::kSplit, sp.get(), sr.get()));
  const sq_guid right = resultNodes(sr.get())[0];
  auto gp = nodes({c, right});
  CHECK_OK(execUndoRedo(fx, ed::kGlue, gp.get()));
  CHECK(near(fx.clip(c).length, 8.0));
  CHECK(near(fx.clip(c).sourceOffset, 1.0));
  CHECK(!fx.exists(right));
  const sq_guid other = addClip(fx, a, SQ_CLIP_AUDIO, 8, 2, "other.wav");
  auto un = nodes({c, other});
  CHECK_EQ(execUndoRedo(fx, ed::kGlue, un.get()), SQ_E_UNSUPPORTED);
  auto mixed = nodes({c, m1});
  CHECK_EQ(execUndoRedo(fx, ed::kGlue, mixed.get()), SQ_E_INVALIDARG);
}

// ---- gain / envelope / normalize ---------------------------------------------------------------------
SQ_TEST("gain: absolute, relative, range; gain envelope stored in source seconds") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 4, "a.wav");
  auto p = nodes({c});
  p->setDouble("gain_db", -3.0);
  CHECK_OK(execUndoRedo(fx, ed::kGain, p.get()));
  auto d = nodes({c});
  d->setDouble("delta_db", 1.5);
  CHECK_OK(execUndoRedo(fx, ed::kGain, d.get()));
  CHECK(near(fx.clip(c).gainDb, -1.5));
  auto big = nodes({c});
  big->setDouble("delta_db", 100.0);
  CHECK_EQ(execUndoRedo(fx, ed::kGain, big.get()), SQ_E_INVALIDARG);
  std::vector<double> pts{1.0, -6.0, 0.0, 0.0};
  auto e = nodes({c});
  e->setBlob("points", pts.data(), static_cast<uint32_t>(pts.size() * sizeof(double)));
  CHECK_OK(execUndoRedo(fx, ed::kGainEnvelope, e.get()));
  const auto& v = fx.clip(c).user.at("user.gainEnvelope");
  REQUIRE_EQ(v.s.size(), 4 * sizeof(double));
  double first = -1;
  std::memcpy(&first, v.s.data(), sizeof(double));
  CHECK(near(first, 0.0));  // sorted
  auto clear = nodes({c});
  clear->setBlob("points", nullptr, 0);
  CHECK_OK(execUndoRedo(fx, ed::kGainEnvelope, clear.get()));
  CHECK(fx.clip(c).user.count("user.gainEnvelope") == 0);
  std::vector<double> nan{std::nan(""), 0.0};
  auto bad = nodes({c});
  bad->setBlob("points", nan.data(), 2 * sizeof(double));
  CHECK_EQ(exec(fx, ed::kGainEnvelope, bad.get()), SQ_E_INVALIDARG);
}

SQ_TEST("normalize: measured peak through a codec, common mode, explicit peak, no codec") {
  MediaScope ms;
  FakeMedia loud{1000.0, std::vector<float>(2000, 0.0f)};
  loud.samples[500] = 0.5f;
  loud.samples[1500] = -1.0f;  // outside the clip's played range below
  media()["n1.fake"] = loud;
  FakeMedia quiet{1000.0, std::vector<float>(2000, 0.0f)};
  quiet.samples[10] = 0.25f;
  media()["n2.fake"] = quiet;
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c1 = addClip(fx, a, SQ_CLIP_AUDIO, 0, 2, "n1.fake");  // plays 0..1 s
  const sq_guid c2 = addClip(fx, a, SQ_CLIP_AUDIO, 4, 2, "n2.fake");
  auto p = nodes({c1, c2});
  CHECK_OK(execUndoRedo(fx, ed::kNormalize, p.get()));
  CHECK(near(fx.clip(c1).gainDb, 6.0206, 1e-3));
  CHECK(near(fx.clip(c2).gainDb, 12.0412, 1e-3));
  auto cm = nodes({c1, c2});
  cm->setString("mode", "common");
  cm->setDouble("target_db", -1.0);
  CHECK_OK(execUndoRedo(fx, ed::kNormalize, cm.get()));
  CHECK(near(fx.clip(c1).gainDb, 5.0206, 1e-3));
  CHECK(near(fx.clip(c2).gainDb, 5.0206, 1e-3));
  auto ex = nodes({c1});
  ex->setDouble("peak_db", -10.0);
  CHECK_OK(execUndoRedo(fx, ed::kNormalize, ex.get()));
  CHECK(near(fx.clip(c1).gainDb, 10.0));
  ed::setMediaRegistry(nullptr);
  auto none = nodes({c2});
  CHECK_EQ(execUndoRedo(fx, ed::kNormalize, none.get()), SQ_E_UNSUPPORTED);
}

// ---- robustness ------------------------------------------------------------------------------------
SQ_TEST("robustness: every action refuses missing, wrong-typed and non-finite params atomically") {
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 4, "a.wav");
  const sq_guid ids[] = {ed::kSplit, ed::kTrimStart, ed::kTrimEnd, ed::kSlip, ed::kMove, ed::kFade, ed::kCrossfade,
                         ed::kQuantize, ed::kStripSilence, ed::kSilence, ed::kDuplicate, ed::kDelete, ed::kInsertTime,
                         ed::kDeleteTime, ed::kFitTempo, ed::kGlue, ed::kNormalize, ed::kGain, ed::kGainEnvelope};
  const std::string h = fx.hash();
  const sq_guid ghost = sq::edoc::newGuid();
  for (const sq_guid& id : ids) {
    auto empty = sq::makeAttributes();
    CHECK(SQ_FAILED(exec(fx, id, empty.get())));
    auto missing = nodes({ghost});
    for (const char* k : {"at", "to", "delta", "gain_db"}) missing->setDouble(k, 1.0);
    CHECK(SQ_FAILED(exec(fx, id, missing.get())));
    auto nan = nodes({c});
    for (const char* k : {"at", "to", "delta", "from", "length", "seconds", "gain_db", "in", "grid", "offset", "ripple"})
      nan->setDouble(k, std::nan(""));
    CHECK(SQ_FAILED(exec(fx, id, nan.get())));
    auto typed = sq::makeAttributes();
    typed->setDouble("node", 3.0);
    typed->setString("at", "soon");
    CHECK(SQ_FAILED(exec(fx, id, typed.get())));
    CHECK(SQ_FAILED(fx.ed->execute(&id, nullptr, nullptr)));
  }
  CHECK_EQ(fx.hash(), h);
  CHECK_EQ(fx.ed->getUndoCount(), 2u);  // AddTrack + AddClip only
}

SQ_TEST("registry: edit actions are classes in category Action; IAction::perform runs them") {
  sq::app::ClassRegistry reg;
  REQUIRE_OK(ed::registerEditClasses(&reg, nullptr));
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(reg.findClass(&ed::kSplit, &ci));
  CHECK_EQ(std::string(ci.category), std::string("Action"));
  REQUIRE_OK(reg.findClass(&ed::cmd::kSplit, &ci));
  CHECK_EQ(std::string(ci.category), std::string("Command"));
  Fx fx;
  const sq_guid a = addTrack(fx, SQ_TRACK_AUDIO);
  const sq_guid c = addClip(fx, a, SQ_CLIP_AUDIO, 0, 4, "a.wav");
  Ptr<sq::IAction> act;
  REQUIRE_OK(reg.createInstance(&ed::kSplit, &sq::IAction::iid, act.putVoid()));
  auto p = nodes({c});
  p->setDouble("at", 1.0);
  CHECK_OK(fx.ed->beginTransaction("macro"));
  CHECK_OK(act->perform(fx.doc.get(), fx.ed.get(), p.get(), nullptr));
  CHECK_OK(fx.ed->commitTransaction());
  CHECK_EQ(fx.st().clipsOf(a).size(), size_t(2));
  ed::unregisterEditClasses(&reg);
  CHECK_EQ(reg.findClass(&ed::kSplit, &ci), SQ_E_NOTFOUND);
}
