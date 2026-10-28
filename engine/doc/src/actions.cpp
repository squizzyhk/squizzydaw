// Squizzy Studio engine/doc - property views/setters and the built-in Action classes.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "internal.hpp"
#include "sq/engine/doc/actions.hpp"

namespace sq::edoc::detail {

// ---- value helpers ----------------------------------------------------------------------------------
std::string fmtDouble(double v) {
  if (v == 0.0) return "0";  // also folds -0
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

void putUser(const PropertyBag& bag, IAttributes* dst) {
  for (const auto& [k, v] : bag) {
    switch (v.type) {
      case SQ_VALUE_INT: dst->setInt(k.c_str(), v.i); break;
      case SQ_VALUE_DOUBLE: dst->setDouble(k.c_str(), v.d); break;
      case SQ_VALUE_BOOL: dst->setBool(k.c_str(), v.i ? SQ_TRUE : SQ_FALSE); break;
      case SQ_VALUE_STRING: dst->setString(k.c_str(), v.s.c_str()); break;
      case SQ_VALUE_GUID: dst->setGuid(k.c_str(), &v.g); break;
      case SQ_VALUE_BLOB: dst->setBlob(k.c_str(), v.s.data(), static_cast<uint32_t>(v.s.size())); break;
      default: break;
    }
  }
}

sq_result readValue(IAttributes* src, const char* key, Value& out) {
  out = Value{};
  const int32_t t = src->getType(key);
  switch (t) {
    case SQ_VALUE_INT: out.type = t; out.i = src->getInt(key, 0); return SQ_OK;
    case SQ_VALUE_DOUBLE: out.type = t; out.d = src->getDouble(key, 0.0); return SQ_OK;
    case SQ_VALUE_BOOL: out.type = t; out.i = src->getBool(key, SQ_FALSE) ? 1 : 0; return SQ_OK;
    case SQ_VALUE_STRING: out.type = t; out.s = getAttrString(src, key); return SQ_OK;
    case SQ_VALUE_GUID: out.type = t; return src->getGuid(key, &out.g);
    case SQ_VALUE_BLOB: {
      uint32_t need = 0;
      src->getBlob(key, nullptr, 0, &need);
      out.type = t;
      out.s.resize(need);
      if (need && src->getBlob(key, out.s.data(), need, &need) != SQ_OK) return SQ_E_FAIL;
      return SQ_OK;
    }
    case SQ_VALUE_NONE: return SQ_E_INVALIDARG;
    default: return SQ_E_TYPE;
  }
}

namespace {
bool isNumber(int32_t t) { return t == SQ_VALUE_INT || t == SQ_VALUE_DOUBLE || t == SQ_VALUE_BOOL; }
sq_result num(IAttributes* s, const char* k, double& out) {
  if (!isNumber(s->getType(k))) return SQ_E_TYPE;
  out = s->getDouble(k, 0.0);
  return std::isfinite(out) ? SQ_OK : SQ_E_INVALIDARG;
}
sq_result integer(IAttributes* s, const char* k, int64_t& out) {
  if (!isNumber(s->getType(k))) return SQ_E_TYPE;
  out = s->getInt(k, 0);
  return SQ_OK;
}
sq_result boolean(IAttributes* s, const char* k, bool& out) {
  if (!isNumber(s->getType(k))) return SQ_E_TYPE;
  out = s->getBool(k, SQ_FALSE) != SQ_FALSE;
  return SQ_OK;
}
sq_result str(IAttributes* s, const char* k, std::string& out) {
  if (s->getType(k) != SQ_VALUE_STRING) return SQ_E_TYPE;
  out = getAttrString(s, k);
  return SQ_OK;
}
sq_result guidv(IAttributes* s, const char* k, sq_guid& out) {
  const int32_t t = s->getType(k);
  if (t == SQ_VALUE_GUID) return s->getGuid(k, &out);
  if (t == SQ_VALUE_STRING) {
    std::string v = getAttrString(s, k);
    if (v.empty()) {
      out = sq_guid{};
      return SQ_OK;
    }
    return parseGuid(v, out) ? SQ_OK : SQ_E_INVALIDARG;
  }
  return SQ_E_TYPE;
}
sq_result flag(IAttributes* s, const char* k, uint32_t& flags, uint32_t bit) {
  bool b = false;
  sq_result r = boolean(s, k, b);
  if (r != SQ_OK) return r;
  flags = b ? (flags | bit) : (flags & ~bit);
  return SQ_OK;
}
sq_result u32(IAttributes* s, const char* k, uint32_t& out) {
  int64_t v = 0;
  sq_result r = integer(s, k, v);
  if (r != SQ_OK) return r;
  if (v < 0 || v > 0xFFFFFFFFll) return SQ_E_INVALIDARG;
  out = static_cast<uint32_t>(v);
  return SQ_OK;
}
bool isUserKey(const std::string& k) { return k.size() > 5 && k.compare(0, 5, "user.") == 0; }
sq_result setUser(PropertyBag& bag, const std::string& key, IAttributes* src, const char* vk) {
  if (src->getType(vk) == SQ_VALUE_NONE) {  // no value: remove the user property
    bag.erase(key);
    return SQ_OK;
  }
  Value v;
  sq_result r = readValue(src, vk, v);
  if (r != SQ_OK) return r;
  bag[key] = std::move(v);
  return SQ_OK;
}
}  // namespace

// ---- property views ----------------------------------------------------------------------------------
void docProps(const DocProps& p, IAttributes* d) {
  d->setInt("kind", p.kind);
  d->setString("name", p.name.c_str());
  d->setDouble("sampleRate", p.sampleRate);
  d->setBool("removeOverlaps", p.removeOverlaps ? SQ_TRUE : SQ_FALSE);
  d->setBool("automationFollowsEdits", p.automationFollowsEdits ? SQ_TRUE : SQ_FALSE);
  putUser(p.user, d);
}
void trackProps(const TrackRec& t, IAttributes* d) {
  d->setInt("kind", t.kind);
  d->setString("name", t.name.c_str());
  d->setGuid("parent", &t.parent);
  d->setInt("channels", t.channels);
  d->setBool("mute", (t.flags & SQ_TRACK_MUTE) ? SQ_TRUE : SQ_FALSE);
  d->setBool("solo", (t.flags & SQ_TRACK_SOLO) ? SQ_TRUE : SQ_FALSE);
  d->setBool("armed", (t.flags & SQ_TRACK_ARMED) ? SQ_TRUE : SQ_FALSE);
  d->setBool("monitor", (t.flags & SQ_TRACK_MONITOR) ? SQ_TRUE : SQ_FALSE);
  d->setBool("hidden", (t.flags & SQ_TRACK_HIDDEN) ? SQ_TRUE : SQ_FALSE);
  d->setInt("color", t.color);
  d->setDouble("volumeDb", t.volumeDb);
  d->setDouble("pan", t.pan);
  d->setGuid("output", &t.output);
  d->setString("timebase", t.timebase.c_str());
  d->setBool("chordTrack", t.chordTrack ? SQ_TRUE : SQ_FALSE);
  d->setBool("followChords", t.followChords ? SQ_TRUE : SQ_FALSE);
  // Read-only text view of the automation lanes for consumers of IDocument (engine/audio parses
  // it with parseAutomation): one lane per line "<param> <ppq>:<value> ..." (automation.hpp).
  {
    std::string lanes;
    char buf[64];
    for (const auto& [param, pts] : t.automation) {
      if (param.empty()) continue;
      lanes += param;
      for (const AutoPoint& pt : pts) {
        std::snprintf(buf, sizeof buf, " %.10g:%.10g", pt.pos, pt.value);
        lanes += buf;
      }
      lanes += '\n';
    }
    d->setString("automation", lanes.c_str());
  }
  putUser(t.user, d);
}
void clipProps(const ClipRec& c, IAttributes* d) {
  d->setInt("kind", c.kind);
  d->setString("name", c.name.c_str());
  d->setGuid("track", &c.track);
  d->setDouble("start", c.start);
  d->setDouble("length", c.length);
  d->setDouble("sourceOffset", c.sourceOffset);
  d->setDouble("gainDb", c.gainDb);
  d->setDouble("fadeIn", c.fadeIn);
  d->setDouble("fadeOut", c.fadeOut);
  d->setBool("muted", (c.flags & SQ_CLIP_MUTED) ? SQ_TRUE : SQ_FALSE);
  d->setBool("locked", (c.flags & SQ_CLIP_LOCKED) ? SQ_TRUE : SQ_FALSE);
  d->setInt("color", c.color);
  d->setString("source", c.source.c_str());
  d->setInt("eventCount", static_cast<int64_t>(c.events.size()));
  putUser(c.user, d);
}
void markerProps(const MarkerRec& m, IAttributes* d) {
  d->setString("name", m.name.c_str());
  d->setDouble("position", m.position);
  d->setDouble("length", m.length);
  d->setInt("flags", m.flags);
  d->setInt("color", m.color);
  putUser(m.user, d);
}

// ---- property setters --------------------------------------------------------------------------------
sq_result setDocProp(DocProps& p, const std::string& k, IAttributes* s, const char* vk) {
  if (k == "name") return str(s, vk, p.name);
  if (k == "sampleRate") {
    double v = 0;
    sq_result r = num(s, vk, v);
    if (r != SQ_OK) return r;
    if (v < 8000.0 || v > 768000.0) return SQ_E_INVALIDARG;
    p.sampleRate = v;
    return SQ_OK;
  }
  if (k == "removeOverlaps") return boolean(s, vk, p.removeOverlaps);
  if (k == "automationFollowsEdits") return boolean(s, vk, p.automationFollowsEdits);
  if (k == "kind") return SQ_E_ACCESS;
  if (isUserKey(k)) return setUser(p.user, k, s, vk);
  return SQ_E_NOTFOUND;
}

sq_result setTrackProp(TrackRec& t, const std::string& k, IAttributes* s, const char* vk) {
  if (k == "name") return str(s, vk, t.name);
  if (k == "channels") {
    uint32_t v = 0;
    sq_result r = u32(s, vk, v);
    if (r != SQ_OK) return r;
    if (v < 1 || v > 64) return SQ_E_INVALIDARG;
    t.channels = v;
    return SQ_OK;
  }
  if (k == "mute") return flag(s, vk, t.flags, SQ_TRACK_MUTE);
  if (k == "solo") return flag(s, vk, t.flags, SQ_TRACK_SOLO);
  if (k == "armed") return flag(s, vk, t.flags, SQ_TRACK_ARMED);
  if (k == "monitor") return flag(s, vk, t.flags, SQ_TRACK_MONITOR);
  if (k == "hidden") return flag(s, vk, t.flags, SQ_TRACK_HIDDEN);
  if (k == "color") return u32(s, vk, t.color);
  if (k == "volumeDb") {
    double v = 0;
    sq_result r = num(s, vk, v);
    if (r != SQ_OK) return r;
    if (v > 24.0) return SQ_E_INVALIDARG;
    t.volumeDb = v;
    return SQ_OK;
  }
  if (k == "pan") {
    double v = 0;
    sq_result r = num(s, vk, v);
    if (r != SQ_OK) return r;
    if (v < -1.0 || v > 1.0) return SQ_E_INVALIDARG;
    t.pan = v;
    return SQ_OK;
  }
  if (k == "output") return guidv(s, vk, t.output);
  if (k == "timebase") {
    std::string v;
    sq_result r = str(s, vk, v);
    if (r != SQ_OK) return r;
    if (v != "musical" && v != "time") return SQ_E_INVALIDARG;
    t.timebase = v;
    return SQ_OK;
  }
  if (k == "chordTrack") return boolean(s, vk, t.chordTrack);
  if (k == "followChords") return boolean(s, vk, t.followChords);
  if (k == "kind" || k == "parent" || k == "automation") return SQ_E_ACCESS;  // automation: edoc::kSetAutomation
  if (isUserKey(k)) return setUser(t.user, k, s, vk);
  return SQ_E_NOTFOUND;
}

sq_result setClipProp(ClipRec& c, const std::string& k, IAttributes* s, const char* vk) {
  auto nonneg = [&](double& field, bool positive) {
    double v = 0;
    sq_result r = num(s, vk, v);
    if (r != SQ_OK) return r;
    if (positive ? v <= 0.0 : v < 0.0) return SQ_E_INVALIDARG;
    field = v;
    return SQ_OK;
  };
  if (k == "name") return str(s, vk, c.name);
  if (k == "start") return nonneg(c.start, false);
  if (k == "length") return nonneg(c.length, true);
  if (k == "sourceOffset") return nonneg(c.sourceOffset, false);
  if (k == "gainDb") return num(s, vk, c.gainDb);
  if (k == "fadeIn") return nonneg(c.fadeIn, false);
  if (k == "fadeOut") return nonneg(c.fadeOut, false);
  if (k == "muted") return flag(s, vk, c.flags, SQ_CLIP_MUTED);
  if (k == "locked") return flag(s, vk, c.flags, SQ_CLIP_LOCKED);
  if (k == "color") return u32(s, vk, c.color);
  if (k == "source") return str(s, vk, c.source);
  if (k == "kind" || k == "track" || k == "eventCount") return SQ_E_ACCESS;
  if (isUserKey(k)) return setUser(c.user, k, s, vk);
  return SQ_E_NOTFOUND;
}

sq_result setMarkerProp(MarkerRec& m, const std::string& k, IAttributes* s, const char* vk) {
  if (k == "name") return str(s, vk, m.name);
  if (k == "position" || k == "length") {
    double v = 0;
    sq_result r = num(s, vk, v);
    if (r != SQ_OK) return r;
    if (v < 0) return SQ_E_INVALIDARG;
    (k == "position" ? m.position : m.length) = v;
    return SQ_OK;
  }
  if (k == "flags") return u32(s, vk, m.flags);
  if (k == "color") return u32(s, vk, m.color);
  if (isUserKey(k)) return setUser(m.user, k, s, vk);
  return SQ_E_NOTFOUND;
}

// ---- structure checks ------------------------------------------------------------------------------------
bool routesTo(const Store& s, const sq_guid& from, const sq_guid& target) {
  sq_guid cur = from;
  for (size_t guard = 0; !sq_guid_is_null(&cur) && guard <= s.tracks.size(); ++guard) {
    if (cur == target) return true;
    const TrackRec* t = s.track(cur);
    if (!t) return false;
    cur = t->output;
  }
  return !sq_guid_is_null(&cur);  // ran past the guard: a cycle somewhere on the path
}

// ---- clip primitives -----------------------------------------------------------------------------------
void trimClip(ClipRec& c, double newStart, double newEnd, const TempoMapRec& tempo) {
  const double delta = newStart - c.start;
  if (delta != 0.0) {
    if (c.kind == SQ_CLIP_AUDIO) {
      c.sourceOffset += tempo.ppqToSeconds(newStart) - tempo.ppqToSeconds(c.start);
      if (c.sourceOffset < 0 && c.sourceOffset > -1e-9) c.sourceOffset = 0.0;
    } else {
      for (EventRec& e : c.events) e.pos -= delta;
    }
  }
  c.start = newStart;
  c.length = newEnd - newStart;
}

ClipRec splitClip(ClipRec& left, double at, const TempoMapRec& tempo) {
  ClipRec right = left;
  right.id = newGuid();
  const double end = left.end();
  const double cut = at - left.start;
  if (left.kind == SQ_CLIP_MIDI) {
    std::vector<EventRec> l, r;
    for (const EventRec& e : left.events) {
      if (e.pos < cut) {
        EventRec x = e;
        if (x.len > 0 && x.pos + x.len > cut) x.len = cut - x.pos;
        l.push_back(x);
      } else {
        r.push_back(e);
      }
    }
    left.events = std::move(l);
    right.events = std::move(r);
  }
  trimClip(right, at, end, tempo);
  trimClip(left, left.start, at, tempo);
  left.fadeOut = 0.0;
  right.fadeIn = 0.0;
  return right;
}

// ---- built-in actions ------------------------------------------------------------------------------------
namespace {

using PerformFn = std::function<sq_result(Editing&, IAttributes*, IAttributes*)>;
class FnAction final : public Action {
 public:
  FnAction(const char* name, PerformFn fn) : name_(name), fn_(std::move(fn)) {}
  const char* name() const override { return name_; }
  sq_result perform(Editing& ed, IAttributes* p, IAttributes* r) override {
    if (!p) return SQ_E_INVALIDARG;
    return fn_(ed, p, r);
  }

 private:
  const char* name_;
  PerformFn fn_;
};

void putResultNode(IAttributes* result, const sq_guid& id) {
  if (result) result->setGuid("node", &id);
}

std::vector<sq_guid> descendants(const Store& s, const sq_guid& id) {
  std::vector<sq_guid> out;  // display order
  for (const sq_guid& t : s.trackOrder) {
    sq_guid p = s.tracks.at(t).parent;
    // bounded walk: the archive reader rejects parent cycles, but never spin on a corrupt store
    for (size_t guard = 0; !sq_guid_is_null(&p) && guard <= s.tracks.size(); ++guard) {
      if (p == id) {
        out.push_back(t);
        break;
      }
      const TrackRec* pr = s.track(p);
      if (!pr) break;
      p = pr->parent;
    }
  }
  return out;
}

const char* defaultTrackName(int32_t kind) {
  switch (kind) {
    case SQ_TRACK_AUDIO: return "Audio";
    case SQ_TRACK_INSTRUMENT: return "Instrument";
    case SQ_TRACK_BUS: return "Bus";
    case SQ_TRACK_FX: return "FX";
    case SQ_TRACK_FOLDER: return "Folder";
    case SQ_TRACK_MASTER: return "Master";
    default: return "Track";
  }
}

bool clipFitsTrack(int32_t clipKind, int32_t trackKind) {
  return (clipKind == SQ_CLIP_AUDIO && trackKind == SQ_TRACK_AUDIO) || (clipKind == SQ_CLIP_MIDI && trackKind == SQ_TRACK_INSTRUMENT);
}

sq_result doSetProperty(Editing& ed, IAttributes* p, IAttributes*) {
  sq_guid node{};
  if (guidv(p, "node", node) != SQ_OK) return SQ_E_INVALIDARG;
  const std::string key = getAttrString(p, "key");
  if (key.empty()) return SQ_E_INVALIDARG;
  if (p->getType("value") == SQ_VALUE_NONE && key.compare(0, 5, "user.") != 0) return SQ_E_INVALIDARG;
  const Store& s = ed.store();
  sq_result r = SQ_E_NOTFOUND;
  if (node == s.props.id) {
    DocProps d = s.props;
    r = setDocProp(d, key, p, "value");
    if (r == SQ_OK && !(d == s.props)) ed.setDocProps(d);
  } else if (node == s.tempo.id) {
    return SQ_E_ACCESS;
  } else if (const TrackRec* t = s.track(node)) {
    TrackRec n = *t;
    r = setTrackProp(n, key, p, "value");
    if (r == SQ_OK && key == "output" && !sq_guid_is_null(&n.output)) {
      const TrackRec* o = s.track(n.output);
      if (!o || o->id == n.id || (o->kind != SQ_TRACK_BUS && o->kind != SQ_TRACK_FX && o->kind != SQ_TRACK_MASTER)) return SQ_E_INVALIDARG;
      if (detail::routesTo(s, n.output, n.id)) return SQ_E_INVALIDARG;  // would close a feedback loop
    }
    if (r == SQ_OK && !(n == *t)) ed.putTrack(n);
  } else if (const ClipRec* c = s.clip(node)) {
    if ((c->flags & SQ_CLIP_LOCKED) && (key == "start" || key == "length" || key == "sourceOffset")) return SQ_E_ACCESS;
    ClipRec n = *c;
    r = setClipProp(n, key, p, "value");
    if (r == SQ_OK && !(n == *c)) ed.putClip(n);
  } else if (const MarkerRec* m = s.marker(node)) {
    MarkerRec n = *m;
    r = setMarkerProp(n, key, p, "value");
    if (r == SQ_OK && !(n == *m)) ed.putMarker(n);
  }
  if (r == SQ_OK && p->getBool("merge", SQ_FALSE)) ed.setMergeKey("prop:" + guidStr(node) + ":" + key);
  return r;
}

sq_result doAddTrack(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  int64_t kind = 0;
  if (integer(p, "kind", kind) != SQ_OK || kind < SQ_TRACK_AUDIO || kind > SQ_TRACK_MASTER) return SQ_E_INVALIDARG;
  if (kind == SQ_TRACK_INSTRUMENT && s.props.kind == SQ_DOC_PROJECT) return SQ_E_UNSUPPORTED;
  if (kind == SQ_TRACK_MASTER) {
    for (const auto& [id, t] : s.tracks)
      if (t.kind == SQ_TRACK_MASTER) return SQ_E_EXISTS;
  }
  TrackRec t;
  t.id = newGuid();
  t.kind = static_cast<int32_t>(kind);
  if (p->getType("parent") != SQ_VALUE_NONE) {
    if (guidv(p, "parent", t.parent) != SQ_OK) return SQ_E_INVALIDARG;
    if (!sq_guid_is_null(&t.parent)) {
      const TrackRec* par = s.track(t.parent);
      if (!par || par->kind != SQ_TRACK_FOLDER) return SQ_E_INVALIDARG;
    }
  }
  if (p->getType("channels") != SQ_VALUE_NONE) {
    if (u32(p, "channels", t.channels) != SQ_OK || t.channels < 1 || t.channels > 64) return SQ_E_INVALIDARG;
  }
  if (p->getType("name") != SQ_VALUE_NONE) {
    if (str(p, "name", t.name) != SQ_OK) return SQ_E_INVALIDARG;
  } else {
    int n = 1;
    for (const auto& [id, o] : s.tracks)
      if (o.kind == t.kind) ++n;
    t.name = std::string(defaultTrackName(t.kind)) + " " + std::to_string(n);
  }
  int32_t index = -1;
  if (p->getType("index") != SQ_VALUE_NONE) {
    int64_t i = 0;
    if (integer(p, "index", i) != SQ_OK || i < 0) return SQ_E_INVALIDARG;
    index = static_cast<int32_t>(std::min<int64_t>(i, static_cast<int64_t>(s.trackOrder.size())));
  } else if (!sq_guid_is_null(&t.parent)) {
    auto d = descendants(s, t.parent);
    index = (d.empty() ? s.trackIndex(t.parent) : s.trackIndex(d.back())) + 1;
  }
  ed.putTrack(t, index);
  putResultNode(result, t.id);
  return SQ_OK;
}

sq_result doRemoveNode(Editing& ed, IAttributes* p, IAttributes*) {
  sq_guid node{};
  if (guidv(p, "node", node) != SQ_OK) return SQ_E_INVALIDARG;
  const Store& s = ed.store();
  if (const ClipRec* c = s.clip(node)) {
    if (c->flags & SQ_CLIP_LOCKED) return SQ_E_ACCESS;
    ed.removeClip(node);
    return SQ_OK;
  }
  if (s.marker(node)) {
    ed.removeMarker(node);
    return SQ_OK;
  }
  if (const TrackRec* t = s.track(node)) {
    if (t->kind == SQ_TRACK_MASTER) return SQ_E_ACCESS;
    std::vector<sq_guid> doomed = descendants(s, node);
    doomed.push_back(node);
    // clips first
    for (const sq_guid& tid : doomed) {
      std::vector<sq_guid> clips = s.clipsOf(tid);
      for (const sq_guid& cid : clips) ed.removeClip(cid);
    }
    // re-route tracks that sent into a removed track
    std::vector<TrackRec> reroute;
    for (const sq_guid& tid : s.trackOrder) {
      const TrackRec& o = s.tracks.at(tid);
      if (std::find(doomed.begin(), doomed.end(), tid) != doomed.end()) continue;
      if (std::find(doomed.begin(), doomed.end(), o.output) != doomed.end()) {
        TrackRec n = o;
        n.output = sq_guid{};
        reroute.push_back(n);
      }
    }
    for (const TrackRec& n : reroute) ed.putTrack(n);
    // tracks: highest display index first so undo restores positions exactly
    std::sort(doomed.begin(), doomed.end(), [&](const sq_guid& a, const sq_guid& b) { return s.trackIndex(a) > s.trackIndex(b); });
    for (const sq_guid& tid : doomed) ed.removeTrack(tid);
    return SQ_OK;
  }
  if (node == s.props.id || node == s.tempo.id) return SQ_E_ACCESS;
  return SQ_E_NOTFOUND;
}

sq_result doAddClip(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  ClipRec c;
  int64_t kind = 0;
  if (guidv(p, "track", c.track) != SQ_OK) return SQ_E_INVALIDARG;
  if (integer(p, "kind", kind) != SQ_OK || (kind != SQ_CLIP_AUDIO && kind != SQ_CLIP_MIDI)) return SQ_E_INVALIDARG;
  c.kind = static_cast<int32_t>(kind);
  const TrackRec* t = s.track(c.track);
  if (!t) return SQ_E_NOTFOUND;
  if (!clipFitsTrack(c.kind, t->kind)) return SQ_E_INVALIDARG;
  if (num(p, "start", c.start) != SQ_OK || c.start < 0) return SQ_E_INVALIDARG;
  if (num(p, "length", c.length) != SQ_OK || c.length <= 0) return SQ_E_INVALIDARG;
  if (p->getType("source") != SQ_VALUE_NONE && str(p, "source", c.source) != SQ_OK) return SQ_E_INVALIDARG;
  if (p->getType("source_offset") != SQ_VALUE_NONE && (num(p, "source_offset", c.sourceOffset) != SQ_OK || c.sourceOffset < 0))
    return SQ_E_INVALIDARG;
  if (p->getType("name") != SQ_VALUE_NONE) {
    if (str(p, "name", c.name) != SQ_OK) return SQ_E_INVALIDARG;
  } else if (!c.source.empty()) {
    size_t slash = c.source.find_last_of("/\\");
    std::string stem = slash == std::string::npos ? c.source : c.source.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    c.name = dot == std::string::npos || dot == 0 ? stem : stem.substr(0, dot);
  } else {
    c.name = c.kind == SQ_CLIP_MIDI ? "MIDI" : "Audio";
  }
  c.id = newGuid();
  ed.putClip(c);
  putResultNode(result, c.id);
  return SQ_OK;
}

sq_result doMoveClip(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  sq_guid node{};
  if (guidv(p, "node", node) != SQ_OK) return SQ_E_INVALIDARG;
  const ClipRec* c = s.clip(node);
  if (!c) return SQ_E_NOTFOUND;
  if (c->flags & SQ_CLIP_LOCKED) return SQ_E_ACCESS;
  ClipRec n = *c;
  if (num(p, "start", n.start) != SQ_OK || n.start < 0) return SQ_E_INVALIDARG;
  if (p->getType("track") != SQ_VALUE_NONE) {
    if (guidv(p, "track", n.track) != SQ_OK) return SQ_E_INVALIDARG;
    const TrackRec* t = s.track(n.track);
    if (!t) return SQ_E_NOTFOUND;
    if (!clipFitsTrack(n.kind, t->kind)) return SQ_E_INVALIDARG;
  }
  if (n == *c) return SQ_OK;
  ed.setTag(kTagMove);
  ed.putClip(n);
  return SQ_OK;
}

sq_result doResizeClip(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  sq_guid node{};
  if (guidv(p, "node", node) != SQ_OK) return SQ_E_INVALIDARG;
  const ClipRec* c = s.clip(node);
  if (!c) return SQ_E_NOTFOUND;
  if (c->flags & SQ_CLIP_LOCKED) return SQ_E_ACCESS;
  double start = 0, length = 0;
  if (num(p, "start", start) != SQ_OK || start < 0) return SQ_E_INVALIDARG;
  if (num(p, "length", length) != SQ_OK || length <= 0) return SQ_E_INVALIDARG;
  ClipRec n = *c;
  trimClip(n, start, start + length, s.tempo);
  if (n.kind == SQ_CLIP_AUDIO && n.sourceOffset < 0) return SQ_E_INVALIDARG;  // before the start of the source
  if (n == *c) return SQ_OK;
  ed.setTag(kTagResize);
  ed.putClip(n);
  return SQ_OK;
}

sq_result doSplitClip(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  sq_guid node{};
  if (guidv(p, "node", node) != SQ_OK) return SQ_E_INVALIDARG;
  const ClipRec* c = s.clip(node);
  if (!c) return SQ_E_NOTFOUND;
  if (c->flags & SQ_CLIP_LOCKED) return SQ_E_ACCESS;
  double at = 0;
  if (num(p, "at", at) != SQ_OK) return SQ_E_INVALIDARG;
  if (at <= c->start || at >= c->end()) return SQ_E_INVALIDARG;
  ClipRec left = *c;
  ClipRec right = splitClip(left, at, s.tempo);
  ed.setTag(kTagResize);
  ed.putClip(left);
  ed.setTag(kTagEdit);
  ed.putClip(right);
  putResultNode(result, right.id);
  return SQ_OK;
}

sq_result doSetClipEvents(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  sq_guid node{};
  if (guidv(p, "clip", node) != SQ_OK) return SQ_E_INVALIDARG;
  const ClipRec* c = s.clip(node);
  if (!c) return SQ_E_NOTFOUND;
  if (c->kind != SQ_CLIP_MIDI) return SQ_E_UNSUPPORTED;
  if (p->getType("events") != SQ_VALUE_BLOB && p->getType("events") != SQ_VALUE_STRING) return SQ_E_INVALIDARG;
  uint32_t need = 0;
  p->getBlob("events", nullptr, 0, &need);
  if (need % sizeof(sq_clip_event)) return SQ_E_INVALIDARG;
  std::vector<sq_clip_event> raw(need / sizeof(sq_clip_event));
  if (need && p->getBlob("events", raw.data(), need, &need) != SQ_OK) return SQ_E_FAIL;
  ClipRec n = *c;
  n.events.clear();
  for (const sq_clip_event& e : raw) {
    if (e.size > 4 || !std::isfinite(e.position) || !std::isfinite(e.length) || e.length < 0) return SQ_E_INVALIDARG;
    EventRec r;
    r.pos = e.position;
    r.len = e.length;
    std::memcpy(r.bytes, e.bytes, 4);
    r.size = e.size;
    n.events.push_back(r);
  }
  std::stable_sort(n.events.begin(), n.events.end(), [](const EventRec& a, const EventRec& b) { return a.pos < b.pos; });
  if (n == *c) return SQ_OK;
  ed.putClip(n);
  return SQ_OK;
}

sq_result doAddMarker(Editing& ed, IAttributes* p, IAttributes* result) {
  MarkerRec m;
  if (num(p, "position", m.position) != SQ_OK || m.position < 0) return SQ_E_INVALIDARG;
  if (p->getType("length") != SQ_VALUE_NONE && (num(p, "length", m.length) != SQ_OK || m.length < 0)) return SQ_E_INVALIDARG;
  if (p->getType("name") != SQ_VALUE_NONE && str(p, "name", m.name) != SQ_OK) return SQ_E_INVALIDARG;
  if (p->getType("flags") != SQ_VALUE_NONE && u32(p, "flags", m.flags) != SQ_OK) return SQ_E_INVALIDARG;
  if (p->getType("color") != SQ_VALUE_NONE && u32(p, "color", m.color) != SQ_OK) return SQ_E_INVALIDARG;
  if (m.name.empty()) m.name = "Marker " + std::to_string(ed.store().markers.size() + 1);
  m.id = newGuid();
  ed.putMarker(m);
  putResultNode(result, m.id);
  return SQ_OK;
}

sq_result doSetTempo(Editing& ed, IAttributes* p, IAttributes*) {
  TempoMapRec t = ed.store().tempo;
  TempoPoint pt;
  if (num(p, "position", pt.pos) != SQ_OK || pt.pos < 0) return SQ_E_INVALIDARG;
  if (num(p, "bpm", pt.bpm) != SQ_OK || pt.bpm < 1.0 || pt.bpm > 999.0) return SQ_E_INVALIDARG;
  bool ramp = false;
  if (p->getType("ramp") != SQ_VALUE_NONE && boolean(p, "ramp", ramp) != SQ_OK) return SQ_E_INVALIDARG;
  pt.flags = ramp ? SQ_TEMPO_RAMP : 0u;
  auto it = std::find_if(t.tempos.begin(), t.tempos.end(), [&](const TempoPoint& x) { return std::fabs(x.pos - pt.pos) < 1e-9; });
  if (it != t.tempos.end()) {
    pt.pos = it->pos;
    *it = pt;
  } else {
    t.tempos.insert(std::upper_bound(t.tempos.begin(), t.tempos.end(), pt.pos, [](double v, const TempoPoint& x) { return v < x.pos; }), pt);
  }
  if (t == ed.store().tempo) return SQ_OK;
  ed.setTempo(t);
  return SQ_OK;
}

sq_result doRemoveTempo(Editing& ed, IAttributes* p, IAttributes*) {
  TempoMapRec t = ed.store().tempo;
  int64_t i = 0;
  if (integer(p, "index", i) != SQ_OK || i < 0) return SQ_E_INVALIDARG;
  if (static_cast<size_t>(i) >= t.tempos.size()) return SQ_E_NOTFOUND;
  if (i == 0) return SQ_E_ACCESS;  // the point at 0 always exists
  t.tempos.erase(t.tempos.begin() + i);
  ed.setTempo(t);
  return SQ_OK;
}

sq_result doSetSignature(Editing& ed, IAttributes* p, IAttributes*) {
  TempoMapRec t = ed.store().tempo;
  SigPoint sp;
  if (num(p, "position", sp.pos) != SQ_OK || sp.pos < 0) return SQ_E_INVALIDARG;
  if (u32(p, "numerator", sp.num) != SQ_OK || sp.num < 1 || sp.num > 64) return SQ_E_INVALIDARG;
  if (u32(p, "denominator", sp.den) != SQ_OK || sp.den < 1 || sp.den > 64 || (sp.den & (sp.den - 1))) return SQ_E_INVALIDARG;
  auto it = std::find_if(t.sigs.begin(), t.sigs.end(), [&](const SigPoint& x) { return std::fabs(x.pos - sp.pos) < 1e-9; });
  if (it != t.sigs.end()) {
    sp.pos = it->pos;
    *it = sp;
  } else {
    t.sigs.insert(std::upper_bound(t.sigs.begin(), t.sigs.end(), sp.pos, [](double v, const SigPoint& x) { return v < x.pos; }), sp);
  }
  if (t == ed.store().tempo) return SQ_OK;
  ed.setTempo(t);
  return SQ_OK;
}

sq_result doSetAutomation(Editing& ed, IAttributes* p, IAttributes*) {
  sq_guid tid{};
  if (guidv(p, "track", tid) != SQ_OK) return SQ_E_INVALIDARG;
  const TrackRec* t = ed.store().track(tid);
  if (!t) return SQ_E_NOTFOUND;
  std::string param;
  if (str(p, "param", param) != SQ_OK || param.empty()) return SQ_E_INVALIDARG;
  uint32_t need = 0;
  if (p->getType("points") != SQ_VALUE_BLOB) return SQ_E_INVALIDARG;
  p->getBlob("points", nullptr, 0, &need);
  if (need % (2 * sizeof(double))) return SQ_E_INVALIDARG;
  std::vector<double> raw(need / sizeof(double));
  if (need && p->getBlob("points", raw.data(), need, &need) != SQ_OK) return SQ_E_FAIL;
  TrackRec n = *t;
  if (raw.empty()) {
    n.automation.erase(param);
  } else {
    std::vector<AutoPoint> pts;
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
      if (!std::isfinite(raw[i]) || !std::isfinite(raw[i + 1]) || raw[i] < 0) return SQ_E_INVALIDARG;
      pts.push_back(AutoPoint{raw[i], raw[i + 1]});
    }
    std::stable_sort(pts.begin(), pts.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.pos < b.pos; });
    n.automation[param] = std::move(pts);
  }
  if (n == *t) return SQ_OK;
  ed.putTrack(n);
  return SQ_OK;
}

sq_result doMoveTrack(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  sq_guid tid{};
  if (guidv(p, "node", tid) != SQ_OK) return SQ_E_INVALIDARG;
  const TrackRec* t = s.track(tid);
  if (!t) return SQ_E_NOTFOUND;
  int64_t index = 0;
  if (integer(p, "index", index) != SQ_OK || index < 0) return SQ_E_INVALIDARG;
  std::vector<sq_guid> block{tid};
  for (const sq_guid& d : descendants(s, tid)) block.push_back(d);
  TrackRec head = *t;
  if (p->getType("parent") != SQ_VALUE_NONE) {
    if (guidv(p, "parent", head.parent) != SQ_OK) return SQ_E_INVALIDARG;
    if (!sq_guid_is_null(&head.parent)) {
      const TrackRec* par = s.track(head.parent);
      if (!par || par->kind != SQ_TRACK_FOLDER) return SQ_E_INVALIDARG;
      if (std::find(block.begin(), block.end(), head.parent) != block.end()) return SQ_E_INVALIDARG;
    }
  }
  std::vector<sq_guid> rest;
  for (const sq_guid& id : s.trackOrder)
    if (std::find(block.begin(), block.end(), id) == block.end()) rest.push_back(id);
  const size_t at = std::min<size_t>(static_cast<size_t>(index), rest.size());
  std::vector<sq_guid> final(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(at));
  final.insert(final.end(), block.begin(), block.end());
  final.insert(final.end(), rest.begin() + static_cast<std::ptrdiff_t>(at), rest.end());
  if (!(head == *t)) ed.putTrack(head);
  for (size_t i = 0; i < final.size(); ++i) {
    if (ed.store().trackOrder[i] != final[i]) ed.putTrack(ed.store().tracks.at(final[i]), static_cast<int32_t>(i));
  }
  return SQ_OK;
}

sq_result doTransposeClip(Editing& ed, IAttributes* p, IAttributes*) {
  sq_guid cid{};
  if (guidv(p, "clip", cid) != SQ_OK) return SQ_E_INVALIDARG;
  const ClipRec* c = ed.store().clip(cid);
  if (!c) return SQ_E_NOTFOUND;
  if (c->kind != SQ_CLIP_MIDI) return SQ_E_UNSUPPORTED;
  int64_t semis = 0;
  if (integer(p, "semitones", semis) != SQ_OK || semis < -127 || semis > 127) return SQ_E_INVALIDARG;
  ClipRec n = *c;
  for (EventRec& e : n.events) {
    const uint8_t st = e.bytes[0] & 0xF0;
    if (st == 0x80 || st == 0x90 || st == 0xA0) e.bytes[1] = static_cast<uint8_t>(std::clamp<int64_t>(e.bytes[1] + semis, 0, 127));
  }
  if (n == *c) return SQ_OK;
  ed.putClip(n);
  return SQ_OK;
}

}  // namespace

void registerBuiltinActions() {
  struct Def {
    sq_guid id;
    const char* name;
    sq_result (*fn)(Editing&, IAttributes*, IAttributes*);
  };
  const Def defs[] = {
    {sq::doc::kSetProperty, "Set Property", &doSetProperty},
    {sq::doc::kAddTrack, "Add Track", &doAddTrack},
    {sq::doc::kRemoveNode, "Remove", &doRemoveNode},
    {sq::doc::kAddClip, "Add Clip", &doAddClip},
    {sq::doc::kMoveClip, "Move Clip", &doMoveClip},
    {sq::doc::kResizeClip, "Resize Clip", &doResizeClip},
    {sq::doc::kSplitClip, "Split Clip", &doSplitClip},
    {sq::doc::kSetClipEvents, "Edit Events", &doSetClipEvents},
    {sq::doc::kAddMarker, "Add Marker", &doAddMarker},
    {sq::doc::kSetTempo, "Set Tempo", &doSetTempo},
    {sq::doc::kRemoveTempo, "Remove Tempo", &doRemoveTempo},
    {sq::doc::kSetSignature, "Set Time Signature", &doSetSignature},
    {kSetAutomation, "Edit Automation", &doSetAutomation},
    {kMoveTrack, "Move Track", &doMoveTrack},
    {kTransposeClip, "Transpose", &doTransposeClip},
  };
  for (const Def& d : defs) registerAction(d.id, std::make_shared<FnAction>(d.name, d.fn));
}

}  // namespace sq::edoc::detail
