// Squizzy Studio engine/edit - keyboard-addressable commands (ICommandHandler, category "Command"),
// edit state journals, class registration and host wiring.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "common.hpp"
#include "sq/engine/doc/manager.hpp"

namespace sq::edit {

// ---- edit state ------------------------------------------------------------------------------------
namespace {
constexpr const char* kJSelection = "edit.selection";
constexpr const char* kJCursor = "edit.cursor";
constexpr const char* kJGrid = "edit.grid";
constexpr const char* kJSnap = "edit.snap";
constexpr const char* kJTrack = "edit.track";
constexpr const char* kJRange = "edit.range";

bool parseNumber(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (!end || end == s.c_str() || !std::isfinite(v)) return false;
  out = v;
  return true;
}
}  // namespace

EditState readEditState(const edoc::DocumentImpl& doc) {
  EditState st;
  const edoc::Store& s = doc.store();
  for (const sq_guid& g : detail::parseGuidList(doc.journal(kJSelection)))
    if (s.clip(g) && std::find(st.selection.begin(), st.selection.end(), g) == st.selection.end()) st.selection.push_back(g);
  double v = 0;
  if (parseNumber(doc.journal(kJCursor), v) && v >= 0) st.cursor = v;
  if (parseNumber(doc.journal(kJGrid), v) && v > 0) st.grid = v;
  SnapMode m;
  const std::string snap = doc.journal(kJSnap);
  if (!snap.empty() && parseSnapMode(snap, m)) st.snap = snap;
  sq_guid t{};
  if (parseGuid(doc.journal(kJTrack), t) && s.track(t)) st.track = t;
  const std::string range = doc.journal(kJRange);
  const size_t sp = range.find(' ');
  double a = 0, b = 0;
  if (sp != std::string::npos && parseNumber(range.substr(0, sp), a) && parseNumber(range.substr(sp + 1), b) && a >= 0 && b > a) {
    st.hasRange = true;
    st.rangeFrom = a;
    st.rangeTo = b;
  }
  return st;
}

namespace {
void putJournal(edoc::DocumentImpl& doc, const char* key, const std::string& value) {
  if (doc.journal(key) == value) return;
  if (value.empty()) doc.removeJournal(key);
  else doc.setJournal(key, value);
}
}  // namespace

void setSelection(edoc::DocumentImpl& doc, const std::vector<sq_guid>& clips) { putJournal(doc, kJSelection, detail::formatGuidList(clips)); }
void setCursor(edoc::DocumentImpl& doc, double ppq) { putJournal(doc, kJCursor, detail::fmt(std::max(0.0, ppq))); }
void setGrid(edoc::DocumentImpl& doc, double ppq) {
  if (ppq > 0 && std::isfinite(ppq)) putJournal(doc, kJGrid, detail::fmt(ppq));
}
void setSnap(edoc::DocumentImpl& doc, const std::string& mode) {
  SnapMode m;
  if (parseSnapMode(mode, m)) putJournal(doc, kJSnap, mode);
}
void setFocusTrack(edoc::DocumentImpl& doc, const sq_guid& track) {
  putJournal(doc, kJTrack, sq_guid_is_null(&track) ? std::string() : toString(track));
}
void setRange(edoc::DocumentImpl& doc, double from, double to) {
  putJournal(doc, kJRange, (from >= 0 && to > from) ? detail::fmt(from) + " " + detail::fmt(to) : std::string());
}

// ---- command context -------------------------------------------------------------------------------
namespace {

using detail::ClipRec;
using detail::kEps;

struct Ctx {
  edoc::DocumentImpl* impl = nullptr;
  IEditor* ed = nullptr;
  IAttributes* args = nullptr;
  EditState st;
  bool checkOnly = false;

  const edoc::Store& store() const { return impl->store(); }
  double argNum(const char* key, double def) const {
    double v = def;
    return detail::has(args, key) && detail::num(args, key, v) == SQ_OK ? v : def;
  }
  bool argBool(const char* key, bool def) const {
    bool v = def;
    return detail::optBool(args, key, v) == SQ_OK ? v : def;
  }
  double cursor() const {
    if (detail::has(args, "at")) return std::max(0.0, argNum("at", st.cursor));
    return std::max(0.0, argNum("position", st.cursor));
  }
  double grid() const {
    const double g = argNum("grid", st.grid);
    return g > 0 ? g : st.grid;
  }
  bool range(double& from, double& to) const {
    from = argNum("from", st.hasRange ? st.rangeFrom : -1);
    to = argNum("to", st.hasRange ? st.rangeTo : -1);
    return from >= 0 && to > from + kEps;
  }
  // Explicit nodes/node args, else the selection. Missing clips are dropped; locked ones too unless
  // allowLocked. kind 0 = any. Sorted by start.
  std::vector<ClipRec> selected(int32_t kind = 0, bool allowLocked = false) const {
    std::vector<sq_guid> ids;
    if (detail::has(args, "nodes") || detail::has(args, "node")) detail::readNodes(args, ids);
    else ids = st.selection;
    std::vector<ClipRec> out;
    for (const sq_guid& id : ids) {
      const ClipRec* c = store().clip(id);
      if (!c || (!allowLocked && (c->flags & SQ_CLIP_LOCKED)) || (kind && c->kind != kind)) continue;
      out.push_back(*c);
    }
    std::sort(out.begin(), out.end(), [](const ClipRec& a, const ClipRec& b) { return a.start < b.start; });
    return out;
  }
  sq_result exec(const sq_guid& action, IAttributes* p, IAttributes* r = nullptr) const { return ed->execute(&action, p, r); }
};

Ptr<IAttributes> nodesParam(const std::vector<ClipRec>& clips) {
  std::vector<sq_guid> ids;
  for (const ClipRec& c : clips) ids.push_back(c.id);
  auto p = makeAttributes();
  p->setBlob("nodes", ids.data(), static_cast<uint32_t>(ids.size() * sizeof(sq_guid)));
  return p;
}
std::vector<sq_guid> resultNodes(IAttributes* r) {
  std::vector<sq_guid> out;
  uint32_t need = 0;
  if (r->getType("nodes") != SQ_VALUE_BLOB) return out;
  r->getBlob("nodes", nullptr, 0, &need);
  out.resize(need / sizeof(sq_guid));
  if (need) r->getBlob("nodes", out.data(), need, &need);
  return out;
}
void copyArgs(IAttributes* args, IAttributes* p, std::initializer_list<const char*> keys) {
  if (!args) return;
  for (const char* k : keys) {
    switch (args->getType(k)) {
      case SQ_VALUE_INT: p->setInt(k, args->getInt(k, 0)); break;
      case SQ_VALUE_DOUBLE: p->setDouble(k, args->getDouble(k, 0)); break;
      case SQ_VALUE_BOOL: p->setBool(k, args->getBool(k, SQ_FALSE)); break;
      case SQ_VALUE_STRING: p->setString(k, getAttrString(args, k).c_str()); break;
      case SQ_VALUE_BLOB: {
        uint32_t need = 0;
        args->getBlob(k, nullptr, 0, &need);
        std::string b(need, '\0');
        if (need) args->getBlob(k, b.data(), need, &need);
        p->setBlob(k, b.data(), need);
        break;
      }
      default: break;
    }
  }
}

constexpr sq_result kDisabled = SQ_COMMAND_DISABLED;

// ---- editing commands ----
sq_result cmdSplit(Ctx& x) {
  const double at = x.cursor();
  std::vector<ClipRec> clips;
  for (const ClipRec& c : x.selected())
    if (at > c.start + kEps && at < c.end() - kEps) clips.push_back(c);
  if (clips.empty() && x.st.selection.empty() && !detail::has(x.args, "nodes") && !detail::has(x.args, "node")) {
    // nothing selected: every unlocked clip under the cursor (on the focused track, if any)
    const std::vector<sq_guid> tracks = sq_guid_is_null(&x.st.track) ? x.store().trackOrder : std::vector<sq_guid>{x.st.track};
    for (const sq_guid& t : tracks)
      for (const sq_guid& cid : x.store().clipsOf(t)) {
        const ClipRec& c = *x.store().clip(cid);
        if (!(c.flags & SQ_CLIP_LOCKED) && at > c.start + kEps && at < c.end() - kEps) clips.push_back(c);
      }
  }
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setDouble("at", at);
  auto r = makeAttributes();
  if (sq_result e = x.exec(kSplit, p.get(), r.get()); SQ_FAILED(e)) return e;
  setSelection(*x.impl, resultNodes(r.get()));
  return SQ_OK;
}

sq_result trimTo(Ctx& x, bool start) {
  const double at = x.cursor();
  std::vector<ClipRec> clips;
  for (const ClipRec& c : x.selected())
    if (start ? (at < c.end() - kEps && std::fabs(at - c.start) > kEps) : (at > c.start + kEps && std::fabs(at - c.end()) > kEps))
      clips.push_back(c);
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setDouble("to", at);
  return x.exec(start ? kTrimStart : kTrimEnd, p.get());
}
sq_result cmdTrimStart(Ctx& x) { return trimTo(x, true); }
sq_result cmdTrimEnd(Ctx& x) { return trimTo(x, false); }

sq_result slip(Ctx& x, double sign) {
  auto clips = x.selected();
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setDouble("delta", sign * x.grid());
  return x.exec(kSlip, p.get());
}
sq_result cmdSlipLeft(Ctx& x) { return slip(x, -1.0); }
sq_result cmdSlipRight(Ctx& x) { return slip(x, 1.0); }

sq_result nudge(Ctx& x, double sign) {
  auto clips = x.selected();
  if (clips.empty() || (sign < 0 && clips.front().start <= kEps)) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setDouble("delta", sign * x.grid());
  return x.exec(kMove, p.get());
}
sq_result cmdNudgeLeft(Ctx& x) { return nudge(x, -1.0); }
sq_result cmdNudgeRight(Ctx& x) { return nudge(x, 1.0); }

sq_result cmdMoveToCursor(Ctx& x) {
  auto clips = x.selected();
  if (clips.empty() || std::fabs(clips.front().start - x.cursor()) < kEps) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setDouble("start", x.cursor());
  return x.exec(kMove, p.get());
}

sq_result moveTrack(Ctx& x, int dir) {
  auto clips = x.selected();
  if (clips.empty()) return kDisabled;
  for (const ClipRec& c : clips)
    if (c.track != clips.front().track || c.kind != clips.front().kind) return kDisabled;
  const auto& order = x.store().trackOrder;
  sq_guid dest{};
  for (int64_t i = x.store().trackIndex(clips.front().track) + dir; i >= 0 && i < static_cast<int64_t>(order.size()); i += dir) {
    const edoc::TrackRec* t = x.store().track(order[static_cast<size_t>(i)]);
    if (t && detail::fitsTrack(clips.front().kind, t->kind)) {
      dest = t->id;
      break;
    }
  }
  if (sq_guid_is_null(&dest)) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setGuid("track", &dest);
  if (sq_result e = x.exec(kMove, p.get()); SQ_FAILED(e)) return e;
  setFocusTrack(*x.impl, dest);
  return SQ_OK;
}
sq_result cmdMoveTrackUp(Ctx& x) { return moveTrack(x, -1); }
sq_result cmdMoveTrackDown(Ctx& x) { return moveTrack(x, 1); }

sq_result fadeToCursor(Ctx& x, bool in) {
  const double at = x.cursor();
  std::vector<ClipRec> clips;
  for (const ClipRec& c : x.selected(SQ_CLIP_AUDIO))
    if (at > c.start + kEps && at < c.end() - kEps) clips.push_back(c);
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  const auto& s = x.store();
  for (const ClipRec& c : clips) {
    auto p = makeAttributes();
    p->setGuid("node", &c.id);
    p->setDouble(in ? "in" : "out", in ? detail::secs(s, at) - detail::secs(s, c.start) : detail::secs(s, c.end()) - detail::secs(s, at));
    copyArgs(x.args, p.get(), {"in_curve", "out_curve"});
    if (sq_result e = x.exec(kFade, p.get()); SQ_FAILED(e)) return e;
  }
  return SQ_OK;
}
sq_result cmdFadeIn(Ctx& x) { return fadeToCursor(x, true); }
sq_result cmdFadeOut(Ctx& x) { return fadeToCursor(x, false); }

sq_result cmdCrossfade(Ctx& x) {
  auto clips = x.selected(SQ_CLIP_AUDIO);
  if (clips.size() != 2 || clips[0].track != clips[1].track || clips[1].start > clips[0].end() + kEps) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  copyArgs(x.args, p.get(), {"length", "curve"});
  return x.exec(kCrossfade, p.get());
}

sq_result cmdQuantize(Ctx& x) {
  auto clips = x.selected();
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setDouble("grid", x.grid());
  copyArgs(x.args, p.get(), {"strength", "swing", "lengths"});
  return x.exec(kQuantize, p.get());
}

sq_result cmdStripSilence(Ctx& x) {
  auto clips = x.selected(SQ_CLIP_AUDIO);
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  copyArgs(x.args, p.get(), {"threshold_db", "min_silence", "min_region", "pad_before", "pad_after"});
  auto r = makeAttributes();
  if (sq_result e = x.exec(kStripSilence, p.get(), r.get()); SQ_FAILED(e)) return e;
  setSelection(*x.impl, resultNodes(r.get()));
  return SQ_OK;
}

sq_result cmdSilence(Ctx& x) {
  double from = 0, to = 0;
  if (!x.range(from, to)) return kDisabled;
  auto clips = x.selected();
  if (x.checkOnly) return SQ_OK;
  auto p = clips.empty() ? makeAttributes() : nodesParam(clips);
  p->setDouble("from", from);
  p->setDouble("to", to);
  copyArgs(x.args, p.get(), {"tracks"});
  return x.exec(kSilence, p.get());
}

sq_result cmdInsertTime(Ctx& x) {
  double from = 0, to = 0;
  const bool r = x.range(from, to);
  const double at = r ? from : x.cursor();
  const double len = x.argNum("length", r ? to - from : x.grid());
  if (!(len > 0)) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = makeAttributes();
  p->setDouble("at", at);
  p->setDouble("length", len);
  copyArgs(x.args, p.get(), {"tracks"});
  return x.exec(kInsertTime, p.get());
}

sq_result cmdDeleteTime(Ctx& x) {
  double from = 0, to = 0;
  if (!x.range(from, to)) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = makeAttributes();
  p->setDouble("from", from);
  p->setDouble("to", to);
  copyArgs(x.args, p.get(), {"tracks"});
  if (sq_result e = x.exec(kDeleteTime, p.get()); SQ_FAILED(e)) return e;
  setRange(*x.impl, 0, 0);
  setCursor(*x.impl, from);
  return SQ_OK;
}

sq_result cmdDuplicate(Ctx& x) {
  auto clips = x.selected(0, true);
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  copyArgs(x.args, p.get(), {"offset", "count"});
  auto r = makeAttributes();
  if (sq_result e = x.exec(kDuplicate, p.get(), r.get()); SQ_FAILED(e)) return e;
  setSelection(*x.impl, resultNodes(r.get()));
  return SQ_OK;
}

sq_result deleteClips(Ctx& x, const char* ripple) {
  auto clips = x.selected();
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setString("ripple", ripple);
  if (sq_result e = x.exec(kDelete, p.get()); SQ_FAILED(e)) return e;
  setSelection(*x.impl, {});
  return SQ_OK;
}
sq_result cmdDelete(Ctx& x) { return deleteClips(x, "none"); }
sq_result cmdDeleteRipple(Ctx& x) { return deleteClips(x, "track"); }
sq_result cmdDeleteRippleAll(Ctx& x) { return deleteClips(x, "all"); }

sq_result cmdGlue(Ctx& x) {
  auto clips = x.selected();
  if (clips.size() < 2) return kDisabled;
  for (const ClipRec& c : clips)
    if (c.track != clips.front().track || c.kind != clips.front().kind) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  auto r = makeAttributes();
  if (sq_result e = x.exec(kGlue, p.get(), r.get()); SQ_FAILED(e)) return e;
  setSelection(*x.impl, resultNodes(r.get()));
  return SQ_OK;
}

sq_result cmdNormalize(Ctx& x) {
  auto clips = x.selected(SQ_CLIP_AUDIO);
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  copyArgs(x.args, p.get(), {"target_db", "mode", "peak_db"});
  return x.exec(kNormalize, p.get());
}

sq_result gainStep(Ctx& x, double sign) {
  auto clips = x.selected();
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = nodesParam(clips);
  p->setDouble("delta_db", sign * std::fabs(x.argNum("step", 1.0)));
  return x.exec(kGain, p.get());
}
sq_result cmdGainUp(Ctx& x) { return gainStep(x, 1.0); }
sq_result cmdGainDown(Ctx& x) { return gainStep(x, -1.0); }

sq_result cmdFitTempo(Ctx& x) {
  double from = 0, to = 0;
  const double seconds = x.argNum("seconds", -1.0);
  if (!x.range(from, to) || !(seconds > 0)) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  auto p = makeAttributes();
  p->setDouble("from", from);
  p->setDouble("to", to);
  p->setDouble("seconds", seconds);
  return x.exec(kFitTempo, p.get());
}

// ---- navigation commands (journals only) ----
sq_guid navTrack(const Ctx& x) {
  if (!sq_guid_is_null(&x.st.track)) return x.st.track;
  if (!x.st.selection.empty()) return x.store().clip(x.st.selection.front())->track;
  for (const sq_guid& t : x.store().trackOrder)
    if (!x.store().clipsOf(t).empty()) return t;
  return sq_guid{};
}

sq_result selectStep(Ctx& x, int dir) {
  const sq_guid track = navTrack(x);
  if (sq_guid_is_null(&track)) return kDisabled;
  const std::vector<sq_guid>& clips = x.store().clipsOf(track);  // sorted by (start, id)
  int64_t ref = -1;
  for (size_t i = 0; i < clips.size(); ++i)
    if (std::find(x.st.selection.begin(), x.st.selection.end(), clips[i]) != x.st.selection.end()) {
      ref = static_cast<int64_t>(i);
      if (dir < 0) break;  // previous: relative to the first selected, next: to the last
    }
  int64_t pick = -1;
  if (ref >= 0) {
    pick = ref + dir;
  } else {
    const double c = x.st.cursor;
    for (size_t i = 0; i < clips.size(); ++i) {
      const double st = x.store().clip(clips[i])->start;
      if (dir > 0 && st >= c - kEps) {
        pick = static_cast<int64_t>(i);
        break;
      }
      if (dir < 0 && st < c - kEps) pick = static_cast<int64_t>(i);
    }
  }
  if (pick < 0 || pick >= static_cast<int64_t>(clips.size())) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  const sq_guid id = clips[static_cast<size_t>(pick)];
  setSelection(*x.impl, {id});
  setCursor(*x.impl, x.store().clip(id)->start);
  setFocusTrack(*x.impl, track);
  return SQ_OK;
}
sq_result cmdSelectNext(Ctx& x) { return selectStep(x, 1); }
sq_result cmdSelectPrev(Ctx& x) { return selectStep(x, -1); }

sq_result cmdSelectAll(Ctx& x) {
  std::vector<sq_guid> ids;
  std::string scope = "track";
  detail::optStr(x.args, "scope", scope);
  const bool all = scope == "all" || sq_guid_is_null(&x.st.track);
  for (const sq_guid& t : all ? x.store().trackOrder : std::vector<sq_guid>{x.st.track})
    for (const sq_guid& c : x.store().clipsOf(t)) ids.push_back(c);
  if (ids.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  setSelection(*x.impl, ids);
  return SQ_OK;
}
sq_result cmdSelectNone(Ctx& x) {
  if (x.st.selection.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  setSelection(*x.impl, {});
  return SQ_OK;
}

sq_result cursorGrid(Ctx& x, int dir) {
  const double g = x.grid(), c = x.st.cursor;
  double n = dir > 0 ? (std::floor(c / g + 1e-9) + 1.0) * g : (std::ceil(c / g - 1e-9) - 1.0) * g;
  if (dir < 0 && c <= kEps) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  setCursor(*x.impl, std::max(0.0, n));
  return SQ_OK;
}
sq_result cmdCursorNext(Ctx& x) { return cursorGrid(x, 1); }
sq_result cmdCursorPrev(Ctx& x) { return cursorGrid(x, -1); }

sq_result cmdCursorToSelection(Ctx& x) {
  auto clips = x.selected(0, true);
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  double v = clips.front().start;
  if (x.argBool("end", false)) {
    v = 0;
    for (const ClipRec& c : clips) v = std::max(v, c.end());
  }
  setCursor(*x.impl, v);
  return SQ_OK;
}

sq_result trackStep(Ctx& x, int dir) {
  std::vector<sq_guid> order;
  for (const sq_guid& t : x.store().trackOrder)
    if (x.store().track(t)->kind != SQ_TRACK_MASTER) order.push_back(t);
  if (order.empty()) return kDisabled;
  int64_t i = -1;
  for (size_t k = 0; k < order.size(); ++k)
    if (order[k] == x.st.track) i = static_cast<int64_t>(k);
  const int64_t n = i < 0 ? (dir > 0 ? 0 : static_cast<int64_t>(order.size()) - 1) : i + dir;
  if (n < 0 || n >= static_cast<int64_t>(order.size())) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  setFocusTrack(*x.impl, order[static_cast<size_t>(n)]);
  return SQ_OK;
}
sq_result cmdTrackNext(Ctx& x) { return trackStep(x, 1); }
sq_result cmdTrackPrev(Ctx& x) { return trackStep(x, -1); }

sq_result cmdRangeFromSelection(Ctx& x) {
  auto clips = x.selected(0, true);
  if (clips.empty()) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  double lo = clips.front().start, hi = 0;
  for (const ClipRec& c : clips) hi = std::max(hi, c.end());
  setRange(*x.impl, lo, hi);
  return SQ_OK;
}

sq_result cmdSetEditState(Ctx& x) {
  IAttributes* a = x.args;
  if (!a) return kDisabled;
  // validate everything first, then apply
  double cursor = -1, grid = -1, rf = -1, rt = -1;
  std::string snap;
  sq_guid track{};
  std::vector<sq_guid> sel;
  const bool hasCursor = detail::has(a, "cursor"), hasGrid = detail::has(a, "grid"), hasSnap = detail::has(a, "snap");
  const bool hasTrack = detail::has(a, "track"), hasSel = detail::has(a, "nodes") || detail::has(a, "node");
  const bool hasRange = detail::has(a, "range_from") || detail::has(a, "range_to");
  if (hasCursor && (detail::num(a, "cursor", cursor) != SQ_OK || cursor < 0)) return SQ_E_INVALIDARG;
  if (hasGrid && (detail::num(a, "grid", grid) != SQ_OK || !(grid > 0))) return SQ_E_INVALIDARG;
  SnapMode m;
  if (hasSnap && (detail::optStr(a, "snap", snap) != SQ_OK || !parseSnapMode(snap, m))) return SQ_E_INVALIDARG;
  if (hasTrack && (detail::guidParam(a, "track", track) != SQ_OK || (!sq_guid_is_null(&track) && !x.store().track(track))))
    return SQ_E_INVALIDARG;
  if (hasSel) {
    if (detail::readNodes(a, sel) != SQ_OK && !(a->getType("nodes") == SQ_VALUE_BLOB || a->getType("nodes") == SQ_VALUE_STRING))
      return SQ_E_INVALIDARG;
    for (const sq_guid& g : sel)
      if (!x.store().clip(g)) return SQ_E_NOTFOUND;
  }
  if (hasRange && (detail::num(a, "range_from", rf) != SQ_OK || detail::num(a, "range_to", rt) != SQ_OK)) return SQ_E_INVALIDARG;
  if (!(hasCursor || hasGrid || hasSnap || hasTrack || hasSel || hasRange)) return kDisabled;
  if (x.checkOnly) return SQ_OK;
  if (hasCursor) setCursor(*x.impl, cursor);
  if (hasGrid) setGrid(*x.impl, grid);
  if (hasSnap) setSnap(*x.impl, snap);
  if (hasTrack) setFocusTrack(*x.impl, track);
  if (hasSel) setSelection(*x.impl, sel);
  if (hasRange) setRange(*x.impl, rf, rt);
  return SQ_OK;
}

// ---- table ----
using CmdFn = sq_result (*)(Ctx&);
struct CommandEntry {
  CommandInfo info;
  CmdFn fn;
};
const std::vector<CommandEntry>& entries() {
  static const std::vector<CommandEntry> e = {
    {{cmd::kSplit, "Edit", "Split", "Split", true}, &cmdSplit},
    {{cmd::kTrimStart, "Edit", "TrimStartToCursor", "Trim Start", true}, &cmdTrimStart},
    {{cmd::kTrimEnd, "Edit", "TrimEndToCursor", "Trim End", true}, &cmdTrimEnd},
    {{cmd::kSlipLeft, "Edit", "SlipLeft", "Slip Left", true}, &cmdSlipLeft},
    {{cmd::kSlipRight, "Edit", "SlipRight", "Slip Right", true}, &cmdSlipRight},
    {{cmd::kNudgeLeft, "Edit", "NudgeLeft", "Nudge Left", true}, &cmdNudgeLeft},
    {{cmd::kNudgeRight, "Edit", "NudgeRight", "Nudge Right", true}, &cmdNudgeRight},
    {{cmd::kMoveToCursor, "Edit", "MoveToCursor", "Move to Cursor", true}, &cmdMoveToCursor},
    {{cmd::kMoveTrackUp, "Edit", "MoveToTrackAbove", "Move to Track Above", true}, &cmdMoveTrackUp},
    {{cmd::kMoveTrackDown, "Edit", "MoveToTrackBelow", "Move to Track Below", true}, &cmdMoveTrackDown},
    {{cmd::kFadeInToCursor, "Edit", "FadeInToCursor", "Fade In", true}, &cmdFadeIn},
    {{cmd::kFadeOutToCursor, "Edit", "FadeOutFromCursor", "Fade Out", true}, &cmdFadeOut},
    {{cmd::kCrossfade, "Edit", "Crossfade", "Crossfade", true}, &cmdCrossfade},
    {{cmd::kQuantize, "Edit", "Quantize", "Quantize", true}, &cmdQuantize},
    {{cmd::kStripSilence, "Edit", "StripSilence", "Strip Silence", true}, &cmdStripSilence},
    {{cmd::kSilence, "Edit", "SilenceRange", "Silence", true}, &cmdSilence},
    {{cmd::kInsertTime, "Edit", "InsertTime", "Insert Time", true}, &cmdInsertTime},
    {{cmd::kDeleteTime, "Edit", "DeleteTime", "Delete Time", true}, &cmdDeleteTime},
    {{cmd::kDuplicate, "Edit", "Duplicate", "Duplicate", true}, &cmdDuplicate},
    {{cmd::kDelete, "Edit", "Delete", "Delete", true}, &cmdDelete},
    {{cmd::kDeleteRipple, "Edit", "DeleteRipple", "Ripple Delete", true}, &cmdDeleteRipple},
    {{cmd::kDeleteRippleAll, "Edit", "DeleteRippleAll", "Ripple Delete (All Tracks)", true}, &cmdDeleteRippleAll},
    {{cmd::kGlue, "Edit", "Glue", "Glue", true}, &cmdGlue},
    {{cmd::kNormalize, "Edit", "Normalize", "Normalize", true}, &cmdNormalize},
    {{cmd::kGainUp, "Edit", "GainUp", "Clip Gain Up", true}, &cmdGainUp},
    {{cmd::kGainDown, "Edit", "GainDown", "Clip Gain Down", true}, &cmdGainDown},
    {{cmd::kFitTempo, "Edit", "FitTempoToRange", "Fit Tempo", true}, &cmdFitTempo},
    {{cmd::kSelectNext, "Navigate", "SelectNextClip", "Select Next Clip", false}, &cmdSelectNext},
    {{cmd::kSelectPrev, "Navigate", "SelectPreviousClip", "Select Previous Clip", false}, &cmdSelectPrev},
    {{cmd::kSelectAll, "Navigate", "SelectAll", "Select All", false}, &cmdSelectAll},
    {{cmd::kSelectNone, "Navigate", "SelectNone", "Select None", false}, &cmdSelectNone},
    {{cmd::kCursorNextGrid, "Navigate", "CursorNextGrid", "Cursor to Next Grid Line", false}, &cmdCursorNext},
    {{cmd::kCursorPrevGrid, "Navigate", "CursorPreviousGrid", "Cursor to Previous Grid Line", false}, &cmdCursorPrev},
    {{cmd::kCursorToSelection, "Navigate", "CursorToSelection", "Cursor to Selection", false}, &cmdCursorToSelection},
    {{cmd::kTrackNext, "Navigate", "NextTrack", "Next Track", false}, &cmdTrackNext},
    {{cmd::kTrackPrev, "Navigate", "PreviousTrack", "Previous Track", false}, &cmdTrackPrev},
    {{cmd::kRangeFromSelection, "Navigate", "RangeFromSelection", "Range from Selection", false}, &cmdRangeFromSelection},
    {{cmd::kSetEditState, "Navigate", "SetEditState", "Set Edit State", false}, &cmdSetEditState},
  };
  return e;
}
const CommandEntry* entryFor(const sq_guid& id) {
  for (const CommandEntry& e : entries())
    if (e.info.id == id) return &e;
  return nullptr;
}

}  // namespace

const std::vector<CommandInfo>& commandTable() {
  static const std::vector<CommandInfo> t = [] {
    std::vector<CommandInfo> v;
    for (const CommandEntry& e : entries()) v.push_back(e.info);
    return v;
  }();
  return t;
}
const CommandInfo* findCommand(const sq_guid& id) {
  for (const CommandInfo& c : commandTable())
    if (c.id == id) return &c;
  return nullptr;
}
const CommandInfo* findCommand(const char* category, const char* name) {
  if (!name) return nullptr;
  for (const CommandInfo& c : commandTable())
    if (std::strcmp(c.name, name) == 0 && (!category || !*category || std::strcmp(c.category, category) == 0)) return &c;
  return nullptr;
}

sq_result runCommand(IDocument* doc, const sq_guid& command, IAttributes* args, bool checkOnly) {
  const CommandEntry* e = entryFor(command);
  if (!e) return SQ_E_NOTFOUND;
  if (!doc) return SQ_E_INVALIDARG;
  edoc::DocumentImpl* impl = edoc::DocumentImpl::from(doc);
  if (!impl) return SQ_E_UNSUPPORTED;  // not an engine/doc document
  if (impl->closed()) return SQ_E_CLOSED;
  try {
    registerEditActions();
    installSideEffects(*impl);
    Ctx x;
    x.impl = impl;
    x.ed = static_cast<IEditor*>(impl);
    x.args = args;
    x.st = readEditState(*impl);
    x.checkOnly = checkOnly;
    if (checkOnly || !e->info.editsDocument) return e->fn(x);
    if (sq_result r = x.ed->beginTransaction(e->info.title); SQ_FAILED(r)) return r;
    const sq_result r = e->fn(x);
    if (SQ_FAILED(r)) {
      x.ed->cancelTransaction();
      return r;
    }
    if (sq_result c = x.ed->commitTransaction(); SQ_FAILED(c)) return c;
    return r;
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

// ---- class registration + host wiring ------------------------------------------------------------
namespace {

struct Env {
  Ptr<IDocumentManager> mgr;  // explicit manager (registerEditClasses)
  IHostContext* host = nullptr;  // borrowed (registerEditServices)
  Ptr<ISubject> observed;
  Ptr<IObserver> observer;
  bool services = false;
};
Env& env() {
  static Env* e = new Env();  // intentionally leaked (no foreign release from static destructors)
  return *e;
}

Ptr<IDocumentManager> manager() {
  if (env().mgr) return env().mgr;
  Ptr<IDocumentManager> m;
  if (env().host) env().host->queryService(&IDocumentManager::iid, m.putVoid());
  return m;
}

sq_result resolveDocument(IAttributes* args, Ptr<IDocument>& out) {
  if (args && args->getType("document") == SQ_VALUE_OBJECT) {
    if (args->getObject("document", &IDocument::iid, out.putVoid()) != SQ_OK || !out) return SQ_E_INVALIDARG;
    return SQ_OK;
  }
  Ptr<IDocumentManager> mgr = manager();
  if (!mgr) return SQ_E_NOTFOUND;
  if (detail::has(args, "doc")) {
    sq_guid want{};
    if (detail::guidParam(args, "doc", want) != SQ_OK) return SQ_E_INVALIDARG;
    for (uint32_t i = 0; i < mgr->getDocumentCount(); ++i) {
      Ptr<IDocument> d;
      sq_guid id{};
      if (mgr->getDocument(i, d.put()) == SQ_OK && d && d->getId(&id) == SQ_OK && id == want) {
        out = d;
        return SQ_OK;
      }
    }
    return SQ_E_NOTFOUND;
  }
  return mgr->getActiveDocument(out.put());
}

class CommandHandler final : public Object<CommandHandler, ICommandHandler> {
 public:
  explicit CommandHandler(const CommandInfo* info) : info_(info) {}  // null = dispatcher
  sq_result SQ_CALL interpretCommand(const sq_command* command, IAttributes* args, sq_bool checkOnly) override {
    try {
      const CommandInfo* info = info_;
      if (!info) {
        if (!command || command->struct_size < sizeof(sq_command)) return SQ_E_NOTFOUND;
        info = findCommand(command->category, command->name);
        if (!info) return SQ_E_NOTFOUND;
      }
      Ptr<IDocument> doc;
      if (sq_result r = resolveDocument(args, doc); r != SQ_OK) return checkOnly ? kDisabled : r;
      return runCommand(doc.get(), info->id, args, checkOnly != SQ_FALSE);
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

 private:
  const CommandInfo* info_;
};

const std::vector<std::string>& classNames() {  // "Edit.Split", ... (registry copies them anyway)
  static const std::vector<std::string> n = [] {
    std::vector<std::string> v;
    for (const CommandInfo& c : commandTable()) v.push_back(std::string(c.category) + "." + c.name);
    return v;
  }();
  return n;
}
constexpr const char* kDispatcherName = "Edit.CommandDispatcher";

class CommandFactory final : public Object<CommandFactory, IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info* out) override {
    if (!out || out->struct_size < sizeof(sq_module_info)) return SQ_E_INVALIDARG;
    out->fourcc = SQ_FOURCC('S', 'Q', 'E', 'D');
    out->name = "engine-edit";
    out->version = SQ_VERSION;
    out->abi_min = SQ_ABI_DEFAULT_MIN;
    out->abi_max = SQ_ABI_DEFAULT_MAX;
    out->sdk_abi = SQ_PLATFORM_ABI;
    out->build_stamp = SQ_BUILD_STAMP;
    out->description = "arrange edit commands";
    return SQ_OK;
  }
  uint32_t SQ_CALL getClassCount() override { return static_cast<uint32_t>(commandTable().size() + 1); }
  sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) override {
    if (!out || out->struct_size < sizeof(sq_class_info)) return SQ_E_INVALIDARG;
    const auto& t = commandTable();
    if (index > t.size()) return SQ_E_NOTFOUND;
    out->class_id = index == t.size() ? cmd::kDispatcher : t[index].id;
    out->category = scripting::kCommandCategory;
    out->name = index == t.size() ? kDispatcherName : classNames()[index].c_str();
    out->flags = 0;
    out->module_fourcc = 0;
    return SQ_OK;
  }
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!classId || !riid) return SQ_E_INVALIDARG;
    const CommandInfo* info = nullptr;
    if (*classId != cmd::kDispatcher) {
      info = findCommand(*classId);
      if (!info) return SQ_E_NOTFOUND;
    }
    try {
      Ptr<CommandHandler> h = Ptr<CommandHandler>::adopt(new CommandHandler(info));
      return h->queryInterface(riid, out);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

class OpenObserver final : public Object<OpenObserver, IObserver> {
 public:
  void SQ_CALL onNotify(IUnknown* subject, const char* topic, IAttributes* args) override {
    if (!topic || std::strcmp(topic, "document.opened") != 0 || !args) return;
    sq_guid id{};
    if (args->getGuid("docId", &id) != SQ_OK) return;
    Ptr<IDocumentManager> mgr = Ptr<IUnknown>(subject).as<IDocumentManager>();
    if (!mgr) return;
    for (uint32_t i = 0; i < mgr->getDocumentCount(); ++i) {
      Ptr<IDocument> d;
      sq_guid did{};
      if (mgr->getDocument(i, d.put()) != SQ_OK || !d || d->getId(&did) != SQ_OK || !(did == id)) continue;
      if (edoc::DocumentImpl* impl = edoc::DocumentImpl::from(d.get())) installSideEffects(*impl);
    }
  }
};

}  // namespace

sq_result registerEditClasses(IClassRegistry* reg, IDocumentManager* mgr) {
  if (!reg) return SQ_E_INVALIDARG;
  registerEditActions();
  if (mgr) env().mgr = Ptr<IDocumentManager>(mgr);
  sq_result result = SQ_OK;
  Ptr<IClassFactory> actions = edoc::makeActionClassFactory();
  for (const detail::ActionDef& d : allActionDefs()) {
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    ci.class_id = d.id;
    ci.category = doc::kActionCategory;
    ci.name = d.name;
    const sq_result r = reg->registerClass(&ci, actions.get());
    if (SQ_FAILED(r) && r != SQ_E_EXISTS) result = r;
  }
  Ptr<CommandFactory> commands = Ptr<CommandFactory>::adopt(new CommandFactory());
  for (uint32_t i = 0; i < commands->getClassCount(); ++i) {
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    commands->getClassInfo(i, &ci);
    const sq_result r = reg->registerClass(&ci, commands.get());
    if (SQ_FAILED(r) && r != SQ_E_EXISTS) result = r;
  }
  return result;
}

void unregisterEditClasses(IClassRegistry* reg) {
  if (reg) {
    for (const detail::ActionDef& d : allActionDefs()) reg->unregisterClass(&d.id);
    for (const CommandInfo& c : commandTable()) reg->unregisterClass(&c.id);
    reg->unregisterClass(&cmd::kDispatcher);
  }
  env().mgr = nullptr;
}

sq_result registerEditServices(IHostContext* host) {
  if (!host) return SQ_E_INVALIDARG;
  Env& e = env();
  if (e.services) return SQ_E_EXISTS;
  IClassRegistry* reg = host->getClassRegistry();
  e.host = host;
  setMediaRegistry(reg);
  if (sq_result r = registerEditClasses(reg, nullptr); SQ_FAILED(r)) {
    unregisterEditClasses(reg);
    setMediaRegistry(nullptr);
    e.host = nullptr;
    return r;
  }
  Ptr<IDocumentManager> mgr;
  if (host->queryService(&IDocumentManager::iid, mgr.putVoid()) == SQ_OK && mgr) {
    Ptr<ISubject> subject = mgr.as<ISubject>();
    Ptr<IObserver> obs = Ptr<IObserver>::adopt(new OpenObserver());
    if (subject && subject->addObserver(obs.get()) == SQ_OK) {
      e.observed = subject;
      e.observer = obs;
    }
    for (uint32_t i = 0; i < mgr->getDocumentCount(); ++i) {
      Ptr<IDocument> d;
      if (mgr->getDocument(i, d.put()) == SQ_OK && d)
        if (edoc::DocumentImpl* impl = edoc::DocumentImpl::from(d.get())) installSideEffects(*impl);
    }
  }
  e.services = true;
  return SQ_OK;
}

void unregisterEditServices(IHostContext* host) {
  Env& e = env();
  if (!e.services) return;
  if (e.observed && e.observer) e.observed->removeObserver(e.observer.get());
  e.observed = nullptr;
  e.observer = nullptr;
  unregisterEditClasses(host ? host->getClassRegistry() : nullptr);
  setMediaRegistry(nullptr);
  e.host = nullptr;
  e.services = false;
}

}  // namespace sq::edit
