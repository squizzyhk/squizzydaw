// Squizzy Studio engine/doc - declarative SideEffect propagators.
//
//   remove-overlaps     (doc.removeOverlaps)          a clip added/moved/resized by the user trims,
//                                                      splits or removes the clips it covers on its track
//   automation-follow   (doc.automationFollowsEdits)  moved clips carry the automation points under
//                                                      their old range to the new place/track
//   time-map            (always)                      after a tempo-map edit, clips on tracks with
//                                                      timebase "time" keep their position in seconds
//   chord-follow        (always)                      when a MIDI clip on a chord track changes its
//                                                      root note, MIDI clips on followChords tracks in
//                                                      that range are transposed by the same interval
// All their edits go into the triggering transaction (one undo step).
#include <algorithm>
#include <cmath>
#include <set>

#include "internal.hpp"

namespace sq::edoc {

namespace {

const ClipRec* afterClip(const Change& c) {
  if (c.kind != kChangeClip) return nullptr;
  const auto& d = std::get<Delta<ClipRec>>(c.data);
  return d.after ? &*d.after : nullptr;
}

// Added, or start/length/track changed.
bool placementChanged(const Change& c) {
  if (c.kind != kChangeClip) return false;
  const auto& d = std::get<Delta<ClipRec>>(c.data);
  if (!d.after) return false;
  if (!d.before) return true;
  return d.before->start != d.after->start || d.before->length != d.after->length || d.before->track != d.after->track;
}

class RemoveOverlaps final : public SideEffect {
 public:
  const char* name() const override { return "remove-overlaps"; }
  uint32_t triggers() const override { return kChangeClip; }
  bool enabled(const Store& s) const override { return s.props.removeOverlaps; }
  void propagate(Editing& ed, const std::vector<Change>& fresh) override {
    std::vector<sq_guid> edited;  // clips the user added, moved or resized (not renamed, recoloured...)
    for (const Change& c : fresh)
      if (c.tag != kTagSideEffect && afterClip(c) && placementChanged(c)) edited.push_back(c.id);
    for (const sq_guid& xid : edited) {
      const ClipRec* xp = ed.store().clip(xid);
      if (!xp) continue;
      const ClipRec x = *xp;
      const std::vector<sq_guid> others = ed.store().clipsOf(x.track);
      for (const sq_guid& yid : others) {
        if (yid == xid) continue;
        const ClipRec* yp = ed.store().clip(yid);
        if (!yp || (yp->flags & SQ_CLIP_LOCKED)) continue;
        ClipRec y = *yp;
        if (!(y.start < x.end() && y.end() > x.start)) continue;
        const TempoMapRec& tm = ed.store().tempo;
        if (x.start <= y.start && x.end() >= y.end()) {
          ed.removeClip(yid);
        } else if (y.start < x.start && y.end() > x.end()) {
          const double yEnd = y.end();
          ClipRec right = detail::splitClip(y, x.start, tm);
          detail::trimClip(right, x.end(), yEnd, tm);
          ed.putClip(y);
          ed.putClip(right);
        } else if (y.start < x.start) {
          detail::trimClip(y, y.start, x.start, tm);
          ed.putClip(y);
        } else {
          detail::trimClip(y, x.end(), y.end(), tm);
          ed.putClip(y);
        }
      }
    }
  }
};

class AutomationFollow final : public SideEffect {
 public:
  const char* name() const override { return "automation-follow"; }
  uint32_t triggers() const override { return kChangeClip; }
  bool enabled(const Store& s) const override { return s.props.automationFollowsEdits; }
  void propagate(Editing& ed, const std::vector<Change>& fresh) override {
    for (const Change& c : fresh) {
      if (c.kind != kChangeClip || c.tag != kTagMove) continue;
      const auto& d = std::get<Delta<ClipRec>>(c.data);
      if (!d.before || !d.after) continue;
      const ClipRec& b = *d.before;
      const ClipRec& a = *d.after;
      const double delta = a.start - b.start;
      const TrackRec* src = ed.store().track(b.track);
      const TrackRec* dst = ed.store().track(a.track);
      if (!src || !dst) continue;
      TrackRec s = *src;
      TrackRec t = (a.track == b.track) ? TrackRec{} : *dst;
      TrackRec& target = (a.track == b.track) ? s : t;
      bool changed = false;
      for (auto& [param, pts] : s.automation) {
        std::vector<AutoPoint> moving, keep;
        for (const AutoPoint& p : pts) (p.pos >= b.start && p.pos < b.end() ? moving : keep).push_back(p);
        if (moving.empty()) continue;
        pts = std::move(keep);
        changed = true;
        auto& lane = target.automation[param];
        lane.erase(std::remove_if(lane.begin(), lane.end(), [&](const AutoPoint& p) { return p.pos >= a.start && p.pos < a.end(); }), lane.end());
        for (AutoPoint p : moving) {
          p.pos += delta;
          lane.push_back(p);
        }
        std::stable_sort(lane.begin(), lane.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
      }
      if (!changed) continue;
      ed.putTrack(s);
      if (a.track != b.track) ed.putTrack(t);
    }
  }
};

class TimeMap final : public SideEffect {
 public:
  const char* name() const override { return "time-map"; }
  uint32_t triggers() const override { return kChangeTempo; }
  void propagate(Editing& ed, const std::vector<Change>& fresh) override {
    const TempoMapRec* oldMap = nullptr;
    size_t firstTempo = 0;
    for (size_t i = 0; i < fresh.size(); ++i) {
      if (fresh[i].kind == kChangeTempo) {
        oldMap = &*std::get<Delta<TempoMapRec>>(fresh[i].data).before;
        firstTempo = i;
        break;
      }
    }
    if (!oldMap) return;
    const TempoMapRec newMap = ed.store().tempo;
    if (*oldMap == newMap) return;
    // Clips placed after the tempo edit in the same transaction were positioned against the new
    // map already: leave them where the user put them.
    std::set<sq_guid> placedAfter;
    for (size_t i = firstTempo + 1; i < fresh.size(); ++i)
      if (fresh[i].tag != kTagSideEffect && placementChanged(fresh[i])) placedAfter.insert(fresh[i].id);
    for (const sq_guid& tid : std::vector<sq_guid>(ed.store().trackOrder)) {
      if (ed.store().tracks.at(tid).timebase != "time") continue;
      const std::vector<sq_guid> clips = ed.store().clipsOf(tid);
      for (const sq_guid& cid : clips) {
        if (placedAfter.count(cid)) continue;
        ClipRec c = *ed.store().clip(cid);
        const double s = newMap.secondsToPpq(oldMap->ppqToSeconds(c.start));
        const double e = newMap.secondsToPpq(oldMap->ppqToSeconds(c.end()));
        if (std::fabs(s - c.start) < 1e-12 && std::fabs(e - c.end()) < 1e-12) continue;
        c.start = s;
        c.length = std::max(e - s, 1e-9);
        ed.putClip(c);
      }
    }
  }
};

int lowestNote(const ClipRec& c) {
  int low = -1;
  for (const EventRec& e : c.events)
    if ((e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0 && (low < 0 || e.bytes[1] < low)) low = e.bytes[1];
  return low;
}

class ChordFollow final : public SideEffect {
 public:
  const char* name() const override { return "chord-follow"; }
  uint32_t triggers() const override { return kChangeClip; }
  void propagate(Editing& ed, const std::vector<Change>& fresh) override {
    for (const Change& c : fresh) {
      if (c.kind != kChangeClip || c.tag == kTagSideEffect) continue;
      const auto& d = std::get<Delta<ClipRec>>(c.data);
      if (!d.before || !d.after || d.after->kind != SQ_CLIP_MIDI) continue;
      const TrackRec* ct = ed.store().track(d.after->track);
      if (!ct || !ct->chordTrack) continue;
      const int oldRoot = lowestNote(*d.before), newRoot = lowestNote(*d.after);
      if (oldRoot < 0 || newRoot < 0 || oldRoot == newRoot) continue;
      int delta = ((newRoot - oldRoot) % 12 + 12) % 12;
      if (delta > 6) delta -= 12;
      if (delta == 0) continue;
      const double r0 = d.after->start, r1 = d.after->end();
      for (const sq_guid& tid : std::vector<sq_guid>(ed.store().trackOrder)) {
        const TrackRec& t = ed.store().tracks.at(tid);
        if (!t.followChords || t.chordTrack) continue;
        const std::vector<sq_guid> clips = ed.store().clipsOf(tid);
        for (const sq_guid& cid : clips) {
          ClipRec m = *ed.store().clip(cid);
          if (m.kind != SQ_CLIP_MIDI || m.start >= r1 || m.end() <= r0) continue;
          bool changed = false;
          for (EventRec& e : m.events) {
            const double at = m.start + e.pos;
            const uint8_t st = e.bytes[0] & 0xF0;
            if (at < r0 || at >= r1 || (st != 0x80 && st != 0x90 && st != 0xA0)) continue;
            const int n = std::clamp(static_cast<int>(e.bytes[1]) + delta, 0, 127);
            if (n != e.bytes[1]) {
              e.bytes[1] = static_cast<uint8_t>(n);
              changed = true;
            }
          }
          if (changed) ed.putClip(m);
        }
      }
    }
  }
};

}  // namespace

std::shared_ptr<SideEffect> makeRemoveOverlaps() { return std::make_shared<RemoveOverlaps>(); }
std::shared_ptr<SideEffect> makeAutomationFollow() { return std::make_shared<AutomationFollow>(); }
std::shared_ptr<SideEffect> makeTimeMap() { return std::make_shared<TimeMap>(); }
std::shared_ptr<SideEffect> makeChordFollow() { return std::make_shared<ChordFollow>(); }

std::vector<std::shared_ptr<SideEffect>> defaultSideEffects() {
  return {makeRemoveOverlaps(), makeAutomationFollow(), makeTimeMap(), makeChordFollow()};
}

}  // namespace sq::edoc
