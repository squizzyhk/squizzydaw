// Squizzy Studio engine/edit - clip actions: split, trim, slip, move, duplicate, delete (ripple), glue.
#include <algorithm>
#include <cstring>

#include "common.hpp"

namespace sq::edit::detail {
namespace {

bool byStart(const ClipRec& a, const ClipRec& b) {
  return a.start != b.start ? a.start < b.start : std::memcmp(&a.id, &b.id, sizeof a.id) < 0;
}

// Earliest content position (ppq) an audio clip can start at: where its source begins.
double audioOrigin(const Store& s, const ClipRec& c) { return ppqAt(s, secs(s, c.start) - c.sourceOffset); }

// ---- split ----
sq_result doSplit(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  SnapParams sp;
  double at = 0;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = num(p, "at", at); r != SQ_OK) return r;
  if (sq_result r = readSnap(p, sp); r != SQ_OK) return r;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  at = snapPosition(s, at, sp.mode, sp.grid, sp.tolerance);
  std::vector<sq_guid> rights;
  for (ClipRec left : clips) {
    if (at <= left.start + kEps || at >= left.end() - kEps) continue;
    ClipRec right = splitClip(left, at, s.tempo);
    ed.setTag(edoc::kTagResize);
    ed.putClip(left);
    ed.setTag(edoc::kTagEdit);
    ed.putClip(right);
    rights.push_back(right.id);
  }
  if (rights.empty()) return SQ_E_INVALIDARG;  // `at` is inside none of the clips
  putResultNodes(result, rights);
  return SQ_OK;
}

// ---- trim ----
sq_result doTrim(Editing& ed, IAttributes* p, IAttributes* result, bool start) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  SnapParams sp;
  double to = 0;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = num(p, "to", to); r != SQ_OK) return r;
  if (sq_result r = readSnap(p, sp); r != SQ_OK) return r;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  to = snapPosition(s, to, sp.mode, sp.grid, sp.tolerance);
  for (const ClipRec& c : clips) {
    double ns = c.start, ne = c.end();
    if (start) {
      ns = std::max(0.0, to);
      if (c.kind == SQ_CLIP_AUDIO) ns = std::max(ns, audioOrigin(s, c));
      if (ns >= ne - kEps) return SQ_E_INVALIDARG;
    } else {
      ne = to;
      if (ne <= ns + kEps) return SQ_E_INVALIDARG;
      MediaInfo mi;
      if (c.kind == SQ_CLIP_AUDIO && ne > c.end() && mediaInfo(c.source, mi) == SQ_OK && mi.seconds() > 0)
        ne = std::min(ne, ppqAt(s, secs(s, c.start) - c.sourceOffset + mi.seconds()));
      if (ne <= ns + kEps) return SQ_E_INVALIDARG;
    }
    if (std::fabs(ns - c.start) < 1e-12 && std::fabs(ne - c.end()) < 1e-12) continue;
    ClipRec n = c;
    trimClip(n, ns, ne, s.tempo);
    if (n.kind == SQ_CLIP_AUDIO && n.sourceOffset < 0) n.sourceOffset = 0.0;  // rounding at the origin
    ed.setTag(edoc::kTagResize);
    ed.putClip(n);
  }
  putResultNodes(result, ids);
  return SQ_OK;
}
sq_result doTrimStart(Editing& ed, IAttributes* p, IAttributes* r) { return doTrim(ed, p, r, true); }
sq_result doTrimEnd(Editing& ed, IAttributes* p, IAttributes* r) { return doTrim(ed, p, r, false); }

// ---- slip ----
sq_result doSlip(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  const bool hasPpq = has(p, "delta"), hasSec = has(p, "delta_seconds");
  if (hasPpq == hasSec) return SQ_E_INVALIDARG;  // exactly one of them
  double delta = 0, deltaSec = 0;
  if (sq_result r = hasPpq ? num(p, "delta", delta) : num(p, "delta_seconds", deltaSec); r != SQ_OK) return r;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  for (const ClipRec& c : clips) {
    ClipRec n = c;
    if (c.kind == SQ_CLIP_AUDIO) {
      const double ds = hasSec ? deltaSec : secs(s, c.start + delta) - secs(s, c.start);
      double off = c.sourceOffset - ds;  // content later in the clip = earlier source position
      MediaInfo mi;
      if (mediaInfo(c.source, mi) == SQ_OK && mi.seconds() > 0) off = std::min(off, std::max(0.0, mi.seconds() - clipSeconds(s, c)));
      n.sourceOffset = std::max(0.0, off);
    } else {
      const double d = hasPpq ? delta : ppqAt(s, secs(s, c.start) + deltaSec) - c.start;
      for (edoc::EventRec& e : n.events) e.pos += d;
    }
    if (!(n == c)) ed.putClip(n);
  }
  putResultNodes(result, ids);
  return SQ_OK;
}

// ---- move ----
sq_result doMove(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  SnapParams sp;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = readSnap(p, sp); r != SQ_OK) return r;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  std::sort(clips.begin(), clips.end(), byStart);
  const double first = clips.front().start;
  const bool hasDelta = has(p, "delta"), hasStart = has(p, "start");
  if (hasDelta && hasStart) return SQ_E_INVALIDARG;
  double delta = 0;
  if (hasDelta) {
    if (sq_result r = num(p, "delta", delta); r != SQ_OK) return r;
  } else if (hasStart) {
    double st = 0;
    if (sq_result r = num(p, "start", st); r != SQ_OK) return r;
    if (st < 0) return SQ_E_INVALIDARG;
    delta = st - first;
  }
  if (sp.mode == SnapMode::Relative) {
    delta = std::round(delta / sp.grid) * sp.grid;
  } else if (sp.mode != SnapMode::Off) {
    delta = snapPosition(s, first + delta, sp.mode, sp.grid, sp.tolerance, ids) - first;
  }
  if (first + delta < 0) delta = -first;
  // destination tracks
  sq_guid track{};
  int64_t offset = 0;
  const bool hasTrack = has(p, "track");
  if (hasTrack && guidParam(p, "track", track) != SQ_OK) return SQ_E_INVALIDARG;
  if (sq_result r = optInt(p, "track_offset", offset); r != SQ_OK) return r;
  if (hasTrack && offset != 0) return SQ_E_INVALIDARG;
  std::vector<ClipRec> moved;
  for (const ClipRec& c : clips) {
    ClipRec n = c;
    n.start = c.start + delta;
    if (hasTrack) {
      n.track = track;
    } else if (offset != 0) {
      const int64_t idx = s.trackIndex(c.track) + offset;
      if (idx < 0 || idx >= static_cast<int64_t>(s.trackOrder.size())) return SQ_E_INVALIDARG;
      n.track = s.trackOrder[static_cast<size_t>(idx)];
    }
    const TrackRec* t = s.track(n.track);
    if (!t) return SQ_E_NOTFOUND;
    if (!fitsTrack(n.kind, t->kind)) return SQ_E_INVALIDARG;
    moved.push_back(n);
  }
  ed.setTag(edoc::kTagMove);
  for (size_t i = 0; i < moved.size(); ++i)
    if (!(moved[i] == clips[i])) ed.putClip(moved[i]);
  if (result) result->setDouble("delta", delta);
  putResultNodes(result, ids);
  return SQ_OK;
}

// ---- duplicate ----
sq_result doDuplicate(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  for (const sq_guid& id : ids) {  // duplicating a locked clip is fine: the original is untouched
    const ClipRec* c = s.clip(id);
    if (!c) return SQ_E_NOTFOUND;
    clips.push_back(*c);
  }
  double lo = clips.front().start, hi = clips.front().end();
  for (const ClipRec& c : clips) {
    lo = std::min(lo, c.start);
    hi = std::max(hi, c.end());
  }
  double offset = hi - lo;
  int64_t count = 1;
  if (sq_result r = optNum(p, "offset", offset); r != SQ_OK) return r;
  if (sq_result r = optInt(p, "count", count); r != SQ_OK) return r;
  if (count < 1 || count > 256 || offset == 0.0) return SQ_E_INVALIDARG;
  if (lo + offset * static_cast<double>(count) < 0 || lo + offset < 0) return SQ_E_INVALIDARG;
  std::vector<sq_guid> created;
  std::sort(clips.begin(), clips.end(), byStart);
  for (int64_t k = 1; k <= count; ++k) {
    for (const ClipRec& c : clips) {
      ClipRec n = c;
      n.id = edoc::newGuid();
      n.start = c.start + offset * static_cast<double>(k);
      n.flags &= ~SQ_CLIP_LOCKED;
      ed.putClip(n);
      created.push_back(n.id);
    }
  }
  putResultNodes(result, created);
  return SQ_OK;
}

// ---- delete (+ ripple) ----
struct Span {
  double from, to;
};
std::vector<Span> merge(std::vector<Span> v) {
  std::sort(v.begin(), v.end(), [](const Span& a, const Span& b) { return a.from < b.from; });
  std::vector<Span> out;
  for (const Span& x : v) {
    if (!out.empty() && x.from <= out.back().to + kEps) out.back().to = std::max(out.back().to, x.to);
    else out.push_back(x);
  }
  return out;
}
// How far content at `pos` moves left: the removed spans that end at or before it.
double rippleShift(const std::vector<Span>& spans, double pos) {
  double d = 0;
  for (const Span& x : spans)
    if (x.to <= pos + kEps) d += x.to - x.from;
  return d;
}

sq_result doDelete(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  std::string ripple = "none";
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = optStr(p, "ripple", ripple); r != SQ_OK) return r;
  if (ripple != "none" && ripple != "track" && ripple != "all") return SQ_E_INVALIDARG;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  for (const ClipRec& c : clips) ed.removeClip(c.id);
  if (ripple == "none") return SQ_OK;

  std::vector<std::pair<sq_guid, std::vector<Span>>> perTrack;  // track -> spans to close
  if (ripple == "track") {
    for (const ClipRec& c : clips) {
      auto it = std::find_if(perTrack.begin(), perTrack.end(), [&](const auto& e) { return e.first == c.track; });
      if (it == perTrack.end()) {
        perTrack.push_back({c.track, {}});
        it = perTrack.end() - 1;
      }
      it->second.push_back({c.start, c.end()});
    }
    for (auto& e : perTrack) e.second = merge(e.second);
  } else {
    std::vector<Span> all;
    for (const ClipRec& c : clips) all.push_back({c.start, c.end()});
    all = merge(all);
    for (const sq_guid& t : s.trackOrder) perTrack.push_back({t, all});
  }
  // Clips: each later clip on an affected track moves left by the removed spans before it.
  ed.setTag(edoc::kTagEdit);  // not a move: automation is shifted explicitly (ripple "all")
  for (const auto& [tid, spans] : perTrack) {
    const std::vector<sq_guid> later = s.clipsOf(tid);
    for (const sq_guid& cid : later) {
      ClipRec n = *s.clip(cid);
      const double d = rippleShift(spans, n.start);
      if (d <= 0) continue;
      n.start = std::max(0.0, n.start - d);
      ed.putClip(n);
    }
  }
  if (ripple == "all") {
    const std::vector<Span>& spans = perTrack.empty() ? std::vector<Span>{} : perTrack.front().second;
    for (const sq_guid& tid : s.trackOrder) {
      const TrackRec* t = s.track(tid);
      if (!t || t->automation.empty()) continue;
      TrackRec n = *t;
      bool changed = false;
      for (auto& [param, pts] : n.automation)
        for (auto& pt : pts)
          if (const double d = rippleShift(spans, pt.pos); d > 0) {
            pt.pos = std::max(0.0, pt.pos - d);
            changed = true;
          }
      if (changed) ed.putTrack(n);
    }
    std::vector<edoc::MarkerRec> ms;
    for (const auto& [mid, m] : s.markers)
      if (rippleShift(spans, m.position) > 0) ms.push_back(m);
    std::sort(ms.begin(), ms.end(), [](const auto& a, const auto& b) { return std::memcmp(&a.id, &b.id, sizeof a.id) < 0; });
    for (edoc::MarkerRec m : ms) {
      m.position = std::max(0.0, m.position - rippleShift(spans, m.position));
      ed.putMarker(m);
    }
  }
  return SQ_OK;
}

// ---- glue ----
sq_result doGlue(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  if (clips.size() < 2) return SQ_E_INVALIDARG;
  std::sort(clips.begin(), clips.end(), byStart);
  const ClipRec& first = clips.front();
  double end = first.end();
  const ClipRec* last = &first;  // the one that ends last
  for (const ClipRec& c : clips) {
    if (c.track != first.track || c.kind != first.kind) return SQ_E_INVALIDARG;
    if (c.end() > end) {
      end = c.end();
      last = &c;
    }
  }
  ClipRec base = first;
  if (first.kind == SQ_CLIP_MIDI) {
    std::vector<edoc::EventRec> evs;
    for (const ClipRec& c : clips) {
      for (const edoc::EventRec& e : c.events) {
        if (e.pos < -kEps || e.pos >= c.length - kEps) continue;  // hidden by the clip bounds
        edoc::EventRec x = e;
        if (x.len > 0 && x.pos + x.len > c.length) x.len = c.length - x.pos;
        x.pos = c.start + e.pos - first.start;
        evs.push_back(x);
      }
    }
    std::stable_sort(evs.begin(), evs.end(), [](const edoc::EventRec& a, const edoc::EventRec& b) { return a.pos < b.pos; });
    base.events = std::move(evs);
  } else {
    // Contiguous parts of one recording only: same source, same alignment, no gaps, same gain.
    const double origin = secs(s, first.start) - first.sourceOffset;
    double covered = first.end();
    for (const ClipRec& c : clips) {
      if (c.source != first.source || std::fabs(c.gainDb - first.gainDb) > 1e-9) return SQ_E_UNSUPPORTED;
      if (std::fabs(secs(s, c.start) - c.sourceOffset - origin) > 1e-6) return SQ_E_UNSUPPORTED;
      if (c.start > covered + kEps) return SQ_E_UNSUPPORTED;  // a gap would need rendering
      covered = std::max(covered, c.end());
    }
  }
  base.length = end - base.start;
  base.fadeOut = last->fadeOut;
  {
    auto it = last->user.find("user.fadeOutCurve");
    if (it != last->user.end()) base.user["user.fadeOutCurve"] = it->second;
    else base.user.erase("user.fadeOutCurve");
  }
  for (size_t i = 1; i < clips.size(); ++i) ed.removeClip(clips[i].id);
  ed.setTag(edoc::kTagResize);
  ed.putClip(base);
  putResultNodes(result, {base.id});
  return SQ_OK;
}

}  // namespace

const std::vector<ActionDef>& clipActionDefs() {
  static const std::vector<ActionDef> defs = {
    {kSplit, "Split", &doSplit},
    {kTrimStart, "Trim Start", &doTrimStart},
    {kTrimEnd, "Trim End", &doTrimEnd},
    {kSlip, "Slip", &doSlip},
    {kMove, "Move", &doMove},
    {kDuplicate, "Duplicate", &doDuplicate},
    {kDelete, "Delete", &doDelete},
    {kGlue, "Glue", &doGlue},
  };
  return defs;
}

}  // namespace sq::edit::detail
