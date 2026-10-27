// Squizzy Studio engine/edit - shared helpers: params, guid lists, snapping, clip primitives, media.
#include "common.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>

#include "sq/interfaces/codec.h"
#include "sq/platform.hpp"

namespace sq::edit {

// ---- snapping --------------------------------------------------------------------------------------
bool parseSnapMode(const std::string& s, SnapMode& out) {
  if (s == "off" || s.empty()) out = SnapMode::Off;
  else if (s == "grid") out = SnapMode::Grid;
  else if (s == "relative") out = SnapMode::Relative;
  else if (s == "bar") out = SnapMode::Bar;
  else if (s == "events") out = SnapMode::Events;
  else return false;
  return true;
}

std::vector<double> barLines(const edoc::TempoMapRec& tempo, double from, double to) {
  std::vector<double> out;
  const auto& sigs = tempo.sigs;
  for (size_t i = 0; i < sigs.size(); ++i) {
    const double segStart = sigs[i].pos;
    const double segEnd = i + 1 < sigs.size() ? sigs[i + 1].pos : to + 1.0;
    const double bar = 4.0 * static_cast<double>(sigs[i].num) / static_cast<double>(sigs[i].den ? sigs[i].den : 4);
    if (bar <= 0 || segEnd < from) continue;
    double k0 = std::max(0.0, std::floor((from - segStart) / bar));
    for (double k = k0;; k += 1.0) {
      const double p = segStart + k * bar;
      if (p >= segEnd - detail::kEps || p > to + detail::kEps) break;
      if (p >= from - detail::kEps) out.push_back(p);
    }
    if (segStart > to) break;
  }
  return out;
}

double snapPosition(const edoc::Store& s, double pos, SnapMode mode, double grid, double tolerance,
                    const std::vector<sq_guid>& exclude) {
  if (!std::isfinite(pos)) return pos;
  if (!(grid > 0) || !std::isfinite(grid)) grid = 1.0;
  auto gridSnap = [&](double p) { return std::max(0.0, std::round(p / grid) * grid); };
  switch (mode) {
    case SnapMode::Off:
    case SnapMode::Relative: return pos;
    case SnapMode::Grid: return gridSnap(pos);
    case SnapMode::Bar: {
      double best = 0.0, bestD = -1.0;
      for (double b : barLines(s.tempo, std::max(0.0, pos - 256.0), pos + 256.0)) {
        const double d = std::fabs(b - pos);
        if (bestD < 0 || d < bestD) {
          bestD = d;
          best = b;
        }
      }
      return bestD < 0 ? gridSnap(pos) : best;
    }
    case SnapMode::Events: {
      const double tol = tolerance >= 0 ? tolerance : grid * 0.5;
      double best = 0.0, bestD = -1.0;
      auto consider = [&](double p) {
        const double d = std::fabs(p - pos);
        if (d <= tol + detail::kEps && (bestD < 0 || d < bestD)) {
          bestD = d;
          best = p;
        }
      };
      consider(0.0);
      for (const auto& [id, c] : s.clips) {
        if (std::find(exclude.begin(), exclude.end(), id) != exclude.end()) continue;
        consider(c.start);
        consider(c.end());
      }
      for (const auto& [id, m] : s.markers) {
        consider(m.position);
        if (m.length > 0) consider(m.position + m.length);
      }
      return bestD < 0 ? gridSnap(pos) : best;
    }
  }
  return pos;
}

namespace detail {

// ---- params --------------------------------------------------------------------------------------
namespace {
bool isNumber(int32_t t) { return t == SQ_VALUE_INT || t == SQ_VALUE_DOUBLE || t == SQ_VALUE_BOOL; }
}  // namespace

bool has(IAttributes* p, const char* key) { return p && p->getType(key) != SQ_VALUE_NONE; }

sq_result num(IAttributes* p, const char* key, double& out) {
  if (!p || !isNumber(p->getType(key))) return p && p->getType(key) == SQ_VALUE_NONE ? SQ_E_INVALIDARG : SQ_E_TYPE;
  const double v = p->getDouble(key, 0.0);
  if (!std::isfinite(v)) return SQ_E_INVALIDARG;
  out = v;
  return SQ_OK;
}
sq_result optNum(IAttributes* p, const char* key, double& out) { return has(p, key) ? num(p, key, out) : SQ_OK; }
sq_result optInt(IAttributes* p, const char* key, int64_t& out) {
  if (!has(p, key)) return SQ_OK;
  if (!isNumber(p->getType(key))) return SQ_E_TYPE;
  out = p->getInt(key, 0);
  return SQ_OK;
}
sq_result optBool(IAttributes* p, const char* key, bool& out) {
  if (!has(p, key)) return SQ_OK;
  if (!isNumber(p->getType(key))) return SQ_E_TYPE;
  out = p->getBool(key, SQ_FALSE) != SQ_FALSE;
  return SQ_OK;
}
sq_result optStr(IAttributes* p, const char* key, std::string& out) {
  if (!has(p, key)) return SQ_OK;
  if (p->getType(key) != SQ_VALUE_STRING) return SQ_E_TYPE;
  out = getAttrString(p, key);
  return SQ_OK;
}
sq_result guidParam(IAttributes* p, const char* key, sq_guid& out) {
  const int32_t t = p ? p->getType(key) : SQ_VALUE_NONE;
  if (t == SQ_VALUE_GUID) return p->getGuid(key, &out);
  if (t == SQ_VALUE_STRING) {
    const std::string v = getAttrString(p, key);
    if (v.empty()) {
      out = sq_guid{};
      return SQ_OK;
    }
    return parseGuid(v, out) ? SQ_OK : SQ_E_INVALIDARG;
  }
  return t == SQ_VALUE_NONE ? SQ_E_INVALIDARG : SQ_E_TYPE;
}

std::vector<sq_guid> parseGuidList(const std::string& s) {
  std::vector<sq_guid> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == ';' || s[i] == '\t' || s[i] == '\n')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != ',' && s[j] != ';' && s[j] != '\t' && s[j] != '\n') ++j;
    if (j > i) {
      sq_guid g{};
      if (parseGuid(s.substr(i, j - i), g)) out.push_back(g);
    }
    i = j;
  }
  return out;
}
std::string formatGuidList(const std::vector<sq_guid>& ids) {
  std::string out;
  for (const sq_guid& g : ids) {
    if (!out.empty()) out += ' ';
    out += toString(g);
  }
  return out;
}

namespace {
sq_result guidListValue(IAttributes* p, const char* key, std::vector<sq_guid>& out) {
  const int32_t t = p->getType(key);
  if (t == SQ_VALUE_BLOB) {
    uint32_t need = 0;
    p->getBlob(key, nullptr, 0, &need);
    if (need % sizeof(sq_guid)) return SQ_E_INVALIDARG;
    std::vector<sq_guid> raw(need / sizeof(sq_guid));
    if (need && p->getBlob(key, raw.data(), need, &need) != SQ_OK) return SQ_E_FAIL;
    out.insert(out.end(), raw.begin(), raw.end());
    return SQ_OK;
  }
  if (t == SQ_VALUE_STRING) {
    const std::string s = getAttrString(p, key);
    std::vector<sq_guid> v = parseGuidList(s);
    // every non-separator token must have parsed
    size_t tokens = 0;
    for (size_t i = 0; i < s.size();) {
      while (i < s.size() && std::strchr(" ,;\t\n", s[i])) ++i;
      if (i >= s.size()) break;
      ++tokens;
      while (i < s.size() && !std::strchr(" ,;\t\n", s[i])) ++i;
    }
    if (tokens != v.size()) return SQ_E_INVALIDARG;
    out.insert(out.end(), v.begin(), v.end());
    return SQ_OK;
  }
  if (t == SQ_VALUE_GUID) {
    sq_guid g{};
    if (p->getGuid(key, &g) != SQ_OK) return SQ_E_FAIL;
    out.push_back(g);
    return SQ_OK;
  }
  return SQ_E_TYPE;
}
void dedupe(std::vector<sq_guid>& v) {
  std::vector<sq_guid> out;
  for (const sq_guid& g : v)
    if (!sq_guid_is_null(&g) && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
  v = std::move(out);
}
}  // namespace

sq_result readNodes(IAttributes* p, std::vector<sq_guid>& out, const char* listKey, const char* oneKey) {
  out.clear();
  if (!p) return SQ_E_INVALIDARG;
  if (has(p, listKey)) {
    if (sq_result r = guidListValue(p, listKey, out); r != SQ_OK) return r;
  }
  if (oneKey && has(p, oneKey)) {
    sq_guid g{};
    if (sq_result r = guidParam(p, oneKey, g); r != SQ_OK) return r;
    out.insert(out.begin(), g);
  }
  dedupe(out);
  return out.empty() ? SQ_E_INVALIDARG : SQ_OK;
}

sq_result readGuidList(IAttributes* p, const char* key, std::vector<sq_guid>& out) {
  out.clear();
  if (!has(p, key)) return SQ_OK;
  if (sq_result r = guidListValue(p, key, out); r != SQ_OK) return r;
  dedupe(out);
  return SQ_OK;
}

sq_result targetClips(const Store& s, const std::vector<sq_guid>& ids, std::vector<ClipRec>& out) {
  out.clear();
  for (const sq_guid& id : ids) {
    const ClipRec* c = s.clip(id);
    if (!c) return SQ_E_NOTFOUND;
    if (c->flags & SQ_CLIP_LOCKED) return SQ_E_ACCESS;
    out.push_back(*c);
  }
  return SQ_OK;
}

void putResultNodes(IAttributes* result, const std::vector<sq_guid>& ids) {
  if (!result) return;
  result->setBlob("nodes", ids.data(), static_cast<uint32_t>(ids.size() * sizeof(sq_guid)));
  if (!ids.empty()) result->setGuid("node", &ids.front());
}

sq_result readSnap(IAttributes* p, SnapParams& out) {
  std::string mode = "off";
  if (sq_result r = optStr(p, "snap", mode); r != SQ_OK) return r;
  if (!parseSnapMode(mode, out.mode)) return SQ_E_INVALIDARG;
  if (sq_result r = optNum(p, "grid", out.grid); r != SQ_OK) return r;
  if (!(out.grid > 0)) return SQ_E_INVALIDARG;
  if (sq_result r = optNum(p, "tolerance", out.tolerance); r != SQ_OK) return r;
  return SQ_OK;
}

// ---- time / clips ----------------------------------------------------------------------------------
double clipSeconds(const Store& s, const ClipRec& c) { return secs(s, c.end()) - secs(s, c.start); }

bool fitsTrack(int32_t clipKind, int32_t trackKind) {
  return (clipKind == SQ_CLIP_AUDIO && trackKind == SQ_TRACK_AUDIO) || (clipKind == SQ_CLIP_MIDI && trackKind == SQ_TRACK_INSTRUMENT);
}

std::string fmt(double v) {
  if (v == 0.0) return "0";
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

void trimClip(ClipRec& c, double newStart, double newEnd, const TempoMapRec& tempo) {
  const double delta = newStart - c.start;
  if (delta != 0.0) {
    if (c.kind == SQ_CLIP_AUDIO) {
      c.sourceOffset += tempo.ppqToSeconds(newStart) - tempo.ppqToSeconds(c.start);
      if (c.sourceOffset < 0 && c.sourceOffset > -1e-9) c.sourceOffset = 0.0;
    } else {
      for (edoc::EventRec& e : c.events) e.pos -= delta;
    }
  }
  c.start = newStart;
  c.length = newEnd - newStart;
}

ClipRec splitClip(ClipRec& left, double at, const TempoMapRec& tempo) {
  ClipRec right = left;
  right.id = edoc::newGuid();
  const double end = left.end();
  const double cut = at - left.start;
  if (left.kind == SQ_CLIP_MIDI) {
    std::vector<edoc::EventRec> l, r;
    for (const edoc::EventRec& e : left.events) {
      if (e.pos < cut) {
        edoc::EventRec x = e;
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

std::vector<sq_guid> cutRange(Editing& ed, const ClipRec& c, double from, double to) {
  const TempoMapRec& tm = ed.store().tempo;
  if (to <= c.start + kEps || from >= c.end() - kEps) return {c.id};
  if (from <= c.start + kEps && to >= c.end() - kEps) {
    ed.removeClip(c.id);
    return {};
  }
  const edoc::ChangeTag saved = ed.tag();
  std::vector<sq_guid> out;
  if (from > c.start && to < c.end()) {
    ClipRec left = c;
    const double end = c.end();
    ClipRec right = splitClip(left, from, tm);
    trimClip(right, to, end, tm);
    ed.setTag(edoc::kTagResize);
    ed.putClip(left);
    ed.setTag(edoc::kTagEdit);
    ed.putClip(right);
    out = {left.id, right.id};
  } else {
    ClipRec n = c;
    if (from <= c.start) {
      trimClip(n, to, c.end(), tm);
    } else {
      trimClip(n, c.start, from, tm);
      n.fadeOut = 0.0;
    }
    if (from <= c.start) n.fadeIn = 0.0;
    ed.setTag(edoc::kTagResize);
    ed.putClip(n);
    out = {c.id};
  }
  ed.setTag(saved);
  return out;
}

void shiftTempoMap(Editing& ed, double at, double delta) {
  if (delta == 0.0) return;
  TempoMapRec t = ed.store().tempo;
  bool changed = false;
  for (auto& p : t.tempos)
    if (p.pos > 0 && p.pos >= at - kEps) {
      p.pos = std::max(0.0, p.pos + delta);
      changed = true;
    }
  for (auto& p : t.sigs)
    if (p.pos > 0 && p.pos >= at - kEps) {
      p.pos = std::max(0.0, p.pos + delta);
      changed = true;
    }
  if (changed) ed.setTempo(t);
}

void shiftAfter(Editing& ed, double at, double delta, const std::vector<sq_guid>& tracks, bool markers) {
  if (delta == 0.0) return;
  const edoc::ChangeTag saved = ed.tag();
  const Store& s = ed.store();
  std::vector<sq_guid> trackIds = tracks.empty() ? s.trackOrder : tracks;
  // Tagged as plain edits, not moves: the automation of these tracks is shifted explicitly below,
  // so engine/doc's automation-follow must not move it a second time.
  ed.setTag(edoc::kTagEdit);
  for (const sq_guid& tid : trackIds) {
    const std::vector<sq_guid> clips = s.clipsOf(tid);
    for (const sq_guid& cid : clips) {
      const ClipRec* c = s.clip(cid);
      if (!c || c->start < at - kEps) continue;
      ClipRec n = *c;
      n.start = std::max(0.0, n.start + delta);
      ed.putClip(n);
    }
  }
  ed.setTag(edoc::kTagEdit);
  for (const sq_guid& tid : trackIds) {
    const TrackRec* t = s.track(tid);
    if (!t || t->automation.empty()) continue;
    TrackRec n = *t;
    bool changed = false;
    for (auto& [param, pts] : n.automation)
      for (auto& p : pts)
        if (p.pos >= at - kEps) {
          p.pos = std::max(0.0, p.pos + delta);
          changed = true;
        }
    if (changed) ed.putTrack(n);
  }
  if (markers) {
    std::vector<edoc::MarkerRec> ms;
    for (const auto& [id, m] : s.markers)
      if (m.position >= at - kEps) ms.push_back(m);
    std::sort(ms.begin(), ms.end(), [](const edoc::MarkerRec& a, const edoc::MarkerRec& b) {
      return a.position != b.position ? a.position < b.position : std::memcmp(&a.id, &b.id, sizeof a.id) < 0;
    });
    for (edoc::MarkerRec m : ms) {
      m.position = std::max(0.0, m.position + delta);
      ed.putMarker(m);
    }
  }
  ed.setTag(saved);
}

// ---- media ---------------------------------------------------------------------------------------
namespace {
IClassRegistry*& mediaReg() {
  static IClassRegistry* r = nullptr;
  return r;
}
std::string extensionOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string e = path.substr(dot + 1);
  for (char& ch : e)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return e;
}
sq_result openReader(const std::string& path, Ptr<IAudioFileReader>& reader, sq_audio_file_format& fmt) {
  IClassRegistry* reg = mediaReg();
  if (!reg || path.empty()) return SQ_E_UNSUPPORTED;
  const std::string ext = extensionOf(path);
  Ptr<ICodecService> codec = Ptr<ICodecService>::adopt(codec::findCodec(reg, ext.c_str(), nullptr, SQ_CODEC_READ));
  if (!codec) return SQ_E_UNSUPPORTED;
  if (sq_result r = codec->createReader(reader.put()); SQ_FAILED(r) || !reader) return SQ_FAILED(r) ? r : SQ_E_FAIL;
  if (sq_result r = reader->open(path.c_str()); SQ_FAILED(r)) return r;
  fmt = sq_audio_file_format{};
  fmt.struct_size = sizeof fmt;
  if (sq_result r = reader->getFormat(&fmt); SQ_FAILED(r)) return r;
  if (!(fmt.sample_rate > 0) || fmt.channels == 0 || fmt.channels > 256) return SQ_E_PARSE;
  return SQ_OK;
}
}  // namespace

IClassRegistry* mediaRegistry() { return mediaReg(); }

sq_result mediaInfo(const std::string& path, MediaInfo& out) {
  Ptr<IAudioFileReader> reader;
  sq_audio_file_format f{};
  if (sq_result r = openReader(path, reader, f); r != SQ_OK) return r;
  out.sampleRate = f.sample_rate;
  out.channels = f.channels;
  out.frames = f.frame_count;
  return SQ_OK;
}

sq_result scanFrames(const std::string& path, double startSec, double durSec, double& sampleRate,
                     const std::function<void(int64_t, float)>& fn) {
  Ptr<IAudioFileReader> reader;
  sq_audio_file_format f{};
  if (sq_result r = openReader(path, reader, f); r != SQ_OK) return r;
  sampleRate = f.sample_rate;
  const int64_t first = static_cast<int64_t>(std::llround(std::max(0.0, startSec) * f.sample_rate));
  int64_t want = durSec < 0 ? INT64_MAX : static_cast<int64_t>(std::llround(durSec * f.sample_rate));
  if (f.frame_count >= 0) want = std::min<int64_t>(want, std::max<int64_t>(0, f.frame_count - first));
  if (want <= 0) return SQ_OK;
  if (first > 0) {
    if (sq_result r = reader->seek(first); SQ_FAILED(r)) return r;
  }
  constexpr uint32_t kBlock = 4096;
  std::vector<std::vector<float>> bufs(f.channels, std::vector<float>(kBlock));
  std::vector<float*> ptrs(f.channels);
  for (uint32_t c = 0; c < f.channels; ++c) ptrs[c] = bufs[c].data();
  int64_t done = 0;
  while (done < want) {
    sq_audio_block b{};
    b.channels = ptrs.data();
    b.channel_count = f.channels;
    b.frames = static_cast<uint32_t>(std::min<int64_t>(kBlock, want - done));
    uint32_t got = 0;
    if (sq_result r = reader->read(&b, &got); SQ_FAILED(r)) return r;
    if (got > b.frames) got = b.frames;
    for (uint32_t i = 0; i < got; ++i) {
      float pk = 0.0f;
      for (uint32_t c = 0; c < f.channels; ++c) {
        const float v = std::fabs(bufs[c][i]);
        if (v > pk) pk = v;  // NaN never wins
      }
      fn(done + i, pk);
    }
    done += got;
    if (got < b.frames) break;  // end of file
  }
  return SQ_OK;
}

}  // namespace detail

void setMediaRegistry(IClassRegistry* reg) { detail::mediaReg() = reg; }

}  // namespace sq::edit
