// engine/doc: randomized undo/redo stress. A seeded random editor performs N edits (every action
// kind, side effects switched on midway, grouped and cancelled transactions); undoing everything
// must return to the initial state hash, redoing everything to the final one, with per-step
// hashes checked on the way.
#include <random>

#include "doc_test_util.hpp"

using namespace doctest;
using sq::Ptr;

namespace {

class RandomEditor {
 public:
  RandomEditor(Fx& f, uint32_t seed) : f_(f), rng_(seed) {}

  // One random edit; returns the action's result (failures are legal and must not change state).
  sq_result step() {
    const sq::edoc::Store& s = f_.st();
    const int pick = uni(0, 99);
    sq::IEditor* ed = f_.ed.get();
    if (s.trackOrder.size() < 3 || pick < 6) return addTrackR();
    if (pick < 22 || s.clips.size() < 4) return addClipR();
    if (pick < 32) return moveClipR();
    if (pick < 38) return resizeClipR();
    if (pick < 44) return splitClipR();
    if (pick < 52 || s.clips.size() > 250) return removeClipR();
    if (pick < 62) return setPropR();
    if (pick < 66) {
      auto p = sq::makeAttributes();
      p->setDouble("position", uni(0, 64) * 0.5);
      p->setString("name", "M");
      return exec(ed, sq::doc::kAddMarker, p.get());
    }
    if (pick < 70) return setTempo(ed, uni(0, 16) * 2.0, 60.0 + uni(0, 120), uni(0, 1) == 1);
    if (pick < 72) {
      auto p = sq::makeAttributes();
      p->setInt("index", uni(0, 3));
      return exec(ed, sq::doc::kRemoveTempo, p.get());
    }
    if (pick < 74) {
      auto p = sq::makeAttributes();
      p->setDouble("position", uni(0, 8) * 4.0);
      p->setInt("numerator", uni(2, 7));
      p->setInt("denominator", 4);
      return exec(ed, sq::doc::kSetSignature, p.get());
    }
    if (pick < 79) return eventsR();
    if (pick < 83) {
      std::vector<double> pts;
      for (int i = uni(0, 4); i > 0; --i) {
        pts.push_back(uni(0, 64) * 0.5);
        pts.push_back(uni(0, 100) / 100.0);
      }
      return setAutomation(ed, randomTrack(), "volume", pts);
    }
    if (pick < 87) {
      auto p = sq::makeAttributes();
      sq_guid t = randomTrack();
      p->setGuid("node", &t);
      p->setInt("index", uni(0, static_cast<int>(s.trackOrder.size())));
      return exec(ed, sq::edoc::kMoveTrack, p.get());
    }
    if (pick < 90) {
      auto p = sq::makeAttributes();
      sq_guid c = randomClip();
      p->setGuid("clip", &c);
      p->setInt("semitones", uni(-5, 5));
      return exec(ed, sq::edoc::kTransposeClip, p.get());
    }
    if (pick < 93 && s.trackOrder.size() > 4) {
      auto p = sq::makeAttributes();
      sq_guid t = randomTrack();
      p->setGuid("node", &t);
      return exec(ed, sq::doc::kRemoveNode, p.get());
    }
    if (pick < 96) {  // the side-effect switches
      const char* keys[] = {"removeOverlaps", "automationFollowsEdits"};
      return setPropB(ed, s.props.id, keys[uni(0, 1)], uni(0, 1) == 1);
    }
    return addClipR();
  }

  int uni(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng_); }

 private:
  sq_guid randomTrack() {
    const auto& o = f_.st().trackOrder;
    return o[static_cast<size_t>(uni(0, static_cast<int>(o.size()) - 1))];
  }
  sq_guid randomClip() {
    const auto& cs = f_.st().clips;
    if (cs.empty()) return sq_guid{};
    auto it = cs.begin();
    std::advance(it, uni(0, static_cast<int>(cs.size()) - 1));
    return it->first;
  }
  sq_result addTrackR() {
    const int32_t kinds[] = {SQ_TRACK_AUDIO, SQ_TRACK_INSTRUMENT, SQ_TRACK_AUDIO, SQ_TRACK_BUS, SQ_TRACK_FOLDER};
    auto p = sq::makeAttributes();
    p->setInt("kind", kinds[uni(0, 4)]);
    sq_result r = exec(f_.ed.get(), sq::doc::kAddTrack, p.get());
    return r;
  }
  sq_result addClipR() {
    sq_guid t = randomTrack();
    const sq::edoc::TrackRec* tr = f_.st().track(t);
    auto p = sq::makeAttributes();
    p->setGuid("track", &t);
    p->setInt("kind", tr->kind == SQ_TRACK_INSTRUMENT ? SQ_CLIP_MIDI : SQ_CLIP_AUDIO);
    p->setDouble("start", uni(0, 128) * 0.25);
    p->setDouble("length", uni(1, 32) * 0.25);
    p->setString("source", "take.wav");
    p->setDouble("source_offset", uni(0, 10) * 0.5);
    return exec(f_.ed.get(), sq::doc::kAddClip, p.get());  // fails on bus/folder/master: fine
  }
  sq_result moveClipR() {
    sq_guid c = randomClip();
    auto p = sq::makeAttributes();
    p->setGuid("node", &c);
    p->setDouble("start", uni(0, 128) * 0.25);
    if (uni(0, 2) == 0) {
      sq_guid t = randomTrack();
      p->setGuid("track", &t);
    }
    return exec(f_.ed.get(), sq::doc::kMoveClip, p.get());
  }
  sq_result resizeClipR() {
    sq_guid c = randomClip();
    const sq::edoc::ClipRec* cr = f_.st().clip(c);
    if (!cr) return SQ_E_NOTFOUND;
    auto p = sq::makeAttributes();
    p->setGuid("node", &c);
    p->setDouble("start", cr->start + uni(-4, 4) * 0.25);
    p->setDouble("length", uni(1, 32) * 0.25);
    return exec(f_.ed.get(), sq::doc::kResizeClip, p.get());
  }
  sq_result splitClipR() {
    sq_guid c = randomClip();
    const sq::edoc::ClipRec* cr = f_.st().clip(c);
    if (!cr) return SQ_E_NOTFOUND;
    auto p = sq::makeAttributes();
    p->setGuid("node", &c);
    p->setDouble("at", cr->start + cr->length * uni(1, 9) / 10.0);
    return exec(f_.ed.get(), sq::doc::kSplitClip, p.get());
  }
  sq_result removeClipR() {
    sq_guid c = randomClip();
    auto p = sq::makeAttributes();
    p->setGuid("node", &c);
    return exec(f_.ed.get(), sq::doc::kRemoveNode, p.get());
  }
  sq_result setPropR() {
    sq::IEditor* ed = f_.ed.get();
    switch (uni(0, 7)) {
      case 0: return setProp(ed, randomTrack(), "volumeDb", -uni(0, 60) * 0.5, uni(0, 1) == 1);
      case 1: return setProp(ed, randomTrack(), "pan", uni(-10, 10) / 10.0);
      case 2: return setPropB(ed, randomTrack(), "mute", uni(0, 1) == 1);
      case 3: return setPropS(ed, randomTrack(), "timebase", uni(0, 1) ? "time" : "musical");
      case 4: return setPropB(ed, randomTrack(), uni(0, 1) ? "chordTrack" : "followChords", uni(0, 1) == 1);
      case 5: return setProp(ed, randomClip(), "gainDb", -uni(0, 12) * 1.0);
      case 6: return setPropB(ed, randomClip(), "locked", uni(0, 5) == 0);
      default: return setPropS(ed, randomTrack(), "name", uni(0, 1) ? "Alpha" : "Beta");
    }
  }
  sq_result eventsR() {
    sq_guid c = randomClip();
    std::vector<sq_clip_event> ev;
    for (int i = uni(0, 6); i > 0; --i) ev.push_back(note(uni(0, 16) * 0.25, uni(1, 8) * 0.25, static_cast<uint8_t>(uni(36, 84))));
    return setEvents(f_.ed.get(), c, ev);  // fails on audio clips: fine
  }

  Fx& f_;
  std::mt19937 rng_;
};

// Runs `actions` successful steps; hashes every `hashEvery` undo steps.
void stress(uint32_t seed, int actions, int hashEvery) {
  Fx f;
  RandomEditor re(f, seed);
  const std::string h0 = f.hash();
  std::vector<std::string> hashes;  // hashes[i] = state after undo step i (+1), sampled
  int done = 0, failed = 0, grouped = 0, cancelled = 0;
  while (done < actions) {
    const int mode = re.uni(0, 19);
    if (mode == 0) {  // grouped transaction: several edits, one step
      REQUIRE_OK(f.ed->beginTransaction("group"));
      for (int i = 0; i < 3; ++i) re.step();
      REQUIRE_OK(f.ed->commitTransaction());
      ++grouped;
    } else if (mode == 1) {  // cancelled transaction: no trace at all
      const std::string before = f.hash();
      const uint32_t steps = f.ed->getUndoCount();
      REQUIRE_OK(f.ed->beginTransaction("cancel"));
      for (int i = 0; i < 3; ++i) re.step();
      REQUIRE_OK(f.ed->cancelTransaction());
      REQUIRE_EQ(f.hash(), before);
      REQUIRE_EQ(f.ed->getUndoCount(), steps);
      ++cancelled;
      continue;
    } else {
      const std::string before = (done % hashEvery == 0) ? f.hash() : std::string();
      const uint32_t steps = f.ed->getUndoCount();
      if (SQ_FAILED(re.step())) {
        ++failed;
        REQUIRE_EQ(f.ed->getUndoCount(), steps);
        if (!before.empty()) REQUIRE_EQ(f.hash(), before);
        continue;
      }
    }
    ++done;
    REQUIRE(!f.ed->inTransaction());
  }
  const uint32_t steps = f.ed->getUndoCount();
  CHECK(steps > 0);
  const std::string hEnd = f.hash();
  CHECK(hEnd != h0);
  size_t sideEffectChanges = 0, totalChanges = 0;
  for (size_t i = 0; i < f.impl->undoKeeper().undoCount(); ++i)
    for (const auto& c : f.impl->undoKeeper().undoAt(i).changes) {
      ++totalChanges;
      if (c.tag == sq::edoc::kTagSideEffect) ++sideEffectChanges;
    }
  const size_t peakClips = f.st().clips.size(), peakTracks = f.st().trackOrder.size(), peakMarkers = f.st().markers.size();

  // undo everything, sampling intermediate hashes
  std::vector<std::string> sampled(steps + 1);
  sampled[steps] = hEnd;
  for (uint32_t i = steps; i > 0; --i) {
    REQUIRE_OK(f.ed->undo());
    if ((i - 1) % static_cast<uint32_t>(hashEvery) == 0) sampled[i - 1] = f.hash();
  }
  CHECK(!f.ed->canUndo());
  REQUIRE_EQ(f.hash(), h0);
  CHECK(!f.impl->modified());
  // redo everything, comparing against the samples
  for (uint32_t i = 1; i <= steps; ++i) {
    REQUIRE_OK(f.ed->redo());
    if (!sampled[i].empty()) REQUIRE_EQ(f.hash(), sampled[i]);
  }
  REQUIRE_EQ(f.hash(), hEnd);
  // and back once more to be sure redo re-recorded correctly
  for (uint32_t i = 0; i < steps; ++i) REQUIRE_OK(f.ed->undo());
  REQUIRE_EQ(f.hash(), h0);
  std::printf("  stress seed=%u actions=%d steps=%u failed=%d grouped=%d cancelled=%d | at end: clips=%zu tracks=%zu markers=%zu"
              " | changes=%zu (side effects %zu)\n",
              seed, actions, steps, failed, grouped, cancelled, peakClips, peakTracks, peakMarkers, totalChanges, sideEffectChanges);
  if (actions >= 1000) CHECK(sideEffectChanges > 0);
  (void)hashes;
}

}  // namespace

SQ_TEST("undo stress: 100 actions return to the identical state hash (every step checked)") {
  for (uint32_t seed : {1u, 2u, 3u, 42u, 1234u}) stress(seed, 100, 1);
}

SQ_TEST("undo stress: 10k actions return to the identical state hash") { stress(20261027u, 10000, 250); }
