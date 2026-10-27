// Squizzy Studio engine/edit - timeline actions: quantize, insert/delete time, fit tempo.
#include <algorithm>
#include <cstring>

#include "common.hpp"

namespace sq::edit::detail {
namespace {

// ---- quantize ----
struct QuantizeSpec {
  double grid = 1.0;
  double strength = 1.0;
  double swing = 0.0;
  double toGrid(double pos, bool useSwing) const {
    const double k = std::round(pos / grid);
    double t = k * grid;
    if (useSwing && swing > 0 && std::fmod(std::fabs(k), 2.0) == 1.0) t += swing * grid * 0.5;  // late off-beats
    return std::max(0.0, t);
  }
  double apply(double pos, bool useSwing = true) const { return pos + (toGrid(pos, useSwing) - pos) * strength; }
};

bool isNote(const edoc::EventRec& e) { return (e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0; }

sq_result doQuantize(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  QuantizeSpec q;
  bool lengths = false;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "grid", q.grid); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "strength", q.strength); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "swing", q.swing); r != SQ_OK) return r;
  if (sq_result r = optBool(p, "lengths", lengths); r != SQ_OK) return r;
  if (!(q.grid > 0) || q.strength < 0 || q.strength > 1 || q.swing < 0 || q.swing > 1) return SQ_E_INVALIDARG;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  for (const ClipRec& c : clips) {
    ClipRec n = c;
    if (c.kind == SQ_CLIP_MIDI) {
      for (edoc::EventRec& e : n.events) {
        if (!isNote(e)) continue;
        const double absStart = c.start + e.pos;
        const double absEnd = absStart + e.len;
        const double ns = q.apply(absStart);
        double ne = absEnd + (ns - absStart);  // keep the length...
        if (lengths) {
          ne = q.apply(absEnd, false);  // ...or quantize the end too
          if (ne <= ns + kEps) ne = ns + q.grid * q.strength + (1.0 - q.strength) * e.len;
        }
        e.pos = ns - c.start;
        e.len = std::max(0.0, ne - ns);
      }
      std::stable_sort(n.events.begin(), n.events.end(), [](const edoc::EventRec& a, const edoc::EventRec& b) { return a.pos < b.pos; });
      if (!(n == c)) ed.putClip(n);
    } else {
      // Audio: the anchor (user.anchor = seconds into the clip, e.g. a detected transient) or the
      // clip start lands on the grid; the clip moves.
      double anchorSec = 0.0;
      if (auto it = c.user.find("user.anchor"); it != c.user.end()) {
        if (it->second.type == SQ_VALUE_DOUBLE) anchorSec = it->second.d;
        else if (it->second.type == SQ_VALUE_INT) anchorSec = static_cast<double>(it->second.i);
      }
      if (!std::isfinite(anchorSec) || anchorSec < 0) anchorSec = 0.0;
      const double anchor = anchorSec > 0 ? ppqAt(s, secs(s, c.start) + anchorSec) : c.start;
      const double delta = q.apply(anchor) - anchor;
      n.start = std::max(0.0, c.start + delta);
      if (!(n == c)) {
        ed.setTag(edoc::kTagMove);
        ed.putClip(n);
        ed.setTag(edoc::kTagEdit);
      }
    }
  }
  return SQ_OK;
}

// ---- insert / delete time ----
sq_result readTracks(const Store& s, IAttributes* p, std::vector<sq_guid>& tracks) {
  if (sq_result r = readGuidList(p, "tracks", tracks); r != SQ_OK) return r;
  for (const sq_guid& t : tracks)
    if (!s.track(t)) return SQ_E_NOTFOUND;
  return SQ_OK;
}

sq_result doInsertTime(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  double at = 0, length = 0;
  std::vector<sq_guid> tracks;
  if (sq_result r = num(p, "at", at); r != SQ_OK) return r;
  if (sq_result r = num(p, "length", length); r != SQ_OK) return r;
  if (at < 0 || !(length > 0)) return SQ_E_INVALIDARG;
  if (sq_result r = readTracks(s, p, tracks); r != SQ_OK) return r;
  const bool global = tracks.empty();
  const std::vector<sq_guid> scope = global ? s.trackOrder : tracks;
  // Refuse before touching anything: a locked clip is never cut.
  for (const sq_guid& t : scope)
    for (const sq_guid& cid : s.clipsOf(t)) {
      const ClipRec& c = *s.clip(cid);
      if ((c.flags & SQ_CLIP_LOCKED) && c.start < at - kEps && c.end() > at + kEps) return SQ_E_ACCESS;
    }
  if (global) shiftTempoMap(ed, at, length);  // first, see shiftTempoMap
  for (const sq_guid& t : scope) {
    const std::vector<sq_guid> clips = s.clipsOf(t);
    for (const sq_guid& cid : clips) {
      ClipRec left = *s.clip(cid);
      if (left.start < at - kEps && left.end() > at + kEps) {
        ClipRec right = splitClip(left, at, s.tempo);
        ed.setTag(edoc::kTagResize);
        ed.putClip(left);
        ed.setTag(edoc::kTagEdit);
        ed.putClip(right);
      }
    }
  }
  shiftAfter(ed, at, length, tracks, global);
  return SQ_OK;
}

// The tempo (or signature) point in effect at `pos` in a sorted point list.
template <class P>
const P* inEffect(const std::vector<P>& v, double pos) {
  const P* cur = v.empty() ? nullptr : &v.front();
  for (const P& x : v)
    if (x.pos <= pos + kEps) cur = &x;
  return cur;
}

// Cuts [from,to) out of a point list: points inside go, later ones move left, and the value in
// effect at `to` is re-established at `from` when a point inside the range changed it.
bool sameValue(const edoc::TempoPoint& a, const edoc::TempoPoint& b) { return a.bpm == b.bpm && a.flags == b.flags; }
bool sameValue(const edoc::SigPoint& a, const edoc::SigPoint& b) { return a.num == b.num && a.den == b.den; }

template <class P>
bool cutPoints(std::vector<P>& v, double from, double to) {
  if (v.empty()) return false;
  const double len = to - from;
  const P carry = *inEffect(v, to);
  std::vector<P> out;
  for (const P& x : v) {
    if (x.pos < from - kEps) {
      out.push_back(x);
    } else if (x.pos >= to - kEps) {
      P y = x;
      y.pos = std::max(0.0, x.pos - len);
      out.push_back(y);
    }
  }
  const bool hasAtFrom = std::any_of(out.begin(), out.end(), [&](const P& x) { return std::fabs(x.pos - from) < kEps; });
  if (!hasAtFrom) {
    const P* prev = nullptr;  // what is in effect just before `from` after the cut
    for (const P& x : out)
      if (x.pos < from - kEps) prev = &x;
    if (!prev || !sameValue(*prev, carry)) {
      P c = carry;
      c.pos = from;
      out.push_back(c);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const P& a, const P& b) { return a.pos < b.pos; });
  const bool changed = !(out == v);
  v = std::move(out);
  return changed;
}

sq_result doDeleteTime(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  double from = 0, to = 0;
  std::vector<sq_guid> tracks;
  if (sq_result r = num(p, "from", from); r != SQ_OK) return r;
  if (sq_result r = num(p, "to", to); r != SQ_OK) return r;
  if (from < 0 || to <= from + kEps) return SQ_E_INVALIDARG;
  if (sq_result r = readTracks(s, p, tracks); r != SQ_OK) return r;
  const bool global = tracks.empty();
  const std::vector<sq_guid> scope = global ? s.trackOrder : tracks;
  for (const sq_guid& t : scope)
    for (const sq_guid& cid : s.clipsOf(t)) {
      const ClipRec& c = *s.clip(cid);
      if ((c.flags & SQ_CLIP_LOCKED) && c.start < to - kEps && c.end() > from + kEps) return SQ_E_ACCESS;
    }
  // Cut the clips against the old map (source offsets of the material after `to`), then cut the
  // tempo map BEFORE shifting: engine/doc's time-map and audio-time-follow leave clips placed after
  // a tempo edit alone, and nothing before `from` changes its timing.
  for (const sq_guid& t : scope) {
    const std::vector<sq_guid> clips = s.clipsOf(t);
    for (const sq_guid& cid : clips) cutRange(ed, *s.clip(cid), from, to);
  }
  if (global) {
    TempoMapRec t = s.tempo;
    const bool a = cutPoints(t.tempos, from, to);
    const bool b = cutPoints(t.sigs, from, to);
    if (a || b) ed.setTempo(t);
  }
  // automation inside the range goes, markers too (global)
  for (const sq_guid& tid : scope) {
    const TrackRec* t = s.track(tid);
    if (!t || t->automation.empty()) continue;
    TrackRec n = *t;
    bool changed = false;
    for (auto& [param, pts] : n.automation) {
      const size_t before = pts.size();
      pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const edoc::AutoPoint& x) { return x.pos >= from - kEps && x.pos < to - kEps; }), pts.end());
      changed |= pts.size() != before;
    }
    if (changed) ed.putTrack(n);
  }
  if (global) {
    std::vector<sq_guid> doomed;
    for (const auto& [mid, m] : s.markers)
      if (m.position >= from - kEps && m.position < to - kEps) doomed.push_back(mid);
    std::sort(doomed.begin(), doomed.end(), [](const sq_guid& a, const sq_guid& b) { return std::memcmp(&a, &b, sizeof a) < 0; });
    for (const sq_guid& m : doomed) ed.removeMarker(m);
  }
  shiftAfter(ed, to, -(to - from), tracks, global);
  return SQ_OK;
}

// ---- fit tempo ----
sq_result doFitTempo(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  double from = 0, to = 0, seconds = 0;
  if (sq_result r = num(p, "from", from); r != SQ_OK) return r;
  if (sq_result r = num(p, "to", to); r != SQ_OK) return r;
  if (sq_result r = num(p, "seconds", seconds); r != SQ_OK) return r;
  if (from < 0 || to <= from + kEps || !(seconds > 0)) return SQ_E_INVALIDARG;
  const double bpm = 60.0 * (to - from) / seconds;
  if (bpm < 1.0 || bpm > 999.0) return SQ_E_INVALIDARG;
  TempoMapRec t = s.tempo;
  const edoc::TempoPoint* after = inEffect(s.tempo.tempos, to);
  const edoc::TempoPoint keep = after ? *after : edoc::TempoPoint{};
  std::vector<edoc::TempoPoint> out;
  bool pointAtTo = false;
  for (const edoc::TempoPoint& x : t.tempos) {
    if (x.pos < from - kEps) out.push_back(x);
    else if (x.pos >= to - kEps) {
      out.push_back(x);
      pointAtTo |= std::fabs(x.pos - to) < kEps;
    }
  }
  out.push_back(edoc::TempoPoint{from, bpm, 0u});
  if (!pointAtTo && std::fabs(keep.bpm - bpm) > 1e-9) out.push_back(edoc::TempoPoint{to, keep.bpm, keep.flags});
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.pos < b.pos; });
  // a ramp into `from` from the previous point would bend the fitted range: keep it as authored
  t.tempos = std::move(out);
  if (result) result->setDouble("bpm", bpm);
  if (t == s.tempo) return SQ_OK;
  ed.setTempo(t);
  return SQ_OK;
}

}  // namespace

const std::vector<ActionDef>& timeActionDefs() {
  static const std::vector<ActionDef> defs = {
    {kQuantize, "Quantize", &doQuantize},
    {kInsertTime, "Insert Time", &doInsertTime},
    {kDeleteTime, "Delete Time", &doDeleteTime},
    {kFitTempo, "Fit Tempo", &doFitTempo},
  };
  return defs;
}

}  // namespace sq::edit::detail
