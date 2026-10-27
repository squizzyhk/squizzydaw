// Squizzy Studio engine/edit - audio clip actions: fades, crossfade, gain, gain envelope, normalize,
// strip silence, silence range.
#include <algorithm>
#include <cstring>

#include "common.hpp"

namespace sq::edit::detail {
namespace {

constexpr double kMinGainDb = -144.0;
constexpr double kMaxGainDb = 48.0;

bool validCurve(const std::string& c) { return c == "linear" || c == "exponential" || c == "logarithmic" || c == "scurve"; }

void setUserString(ClipRec& c, const char* key, const std::string& v) {
  edoc::Value val;
  val.type = SQ_VALUE_STRING;
  val.s = v;
  c.user[key] = val;
}

sq_result audioTargets(const Store& s, IAttributes* p, std::vector<ClipRec>& clips) {
  std::vector<sq_guid> ids;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  for (const ClipRec& c : clips)
    if (c.kind != SQ_CLIP_AUDIO) return SQ_E_UNSUPPORTED;
  return SQ_OK;
}

// ---- fades ----
sq_result doFade(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  std::vector<ClipRec> clips;
  if (sq_result r = audioTargets(s, p, clips); r != SQ_OK) return r;
  double in = -1, out = -1;
  std::string inCurve, outCurve;
  if (sq_result r = optNum(p, "in", in); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "out", out); r != SQ_OK) return r;
  if (sq_result r = optStr(p, "in_curve", inCurve); r != SQ_OK) return r;
  if (sq_result r = optStr(p, "out_curve", outCurve); r != SQ_OK) return r;
  if ((has(p, "in") && in < 0) || (has(p, "out") && out < 0)) return SQ_E_INVALIDARG;
  if ((!inCurve.empty() && !validCurve(inCurve)) || (!outCurve.empty() && !validCurve(outCurve))) return SQ_E_INVALIDARG;
  if (!has(p, "in") && !has(p, "out") && inCurve.empty() && outCurve.empty()) return SQ_E_INVALIDARG;
  for (const ClipRec& c : clips) {
    ClipRec n = c;
    if (has(p, "in")) n.fadeIn = in;  // fade-clamp keeps in + out within the clip
    if (has(p, "out")) n.fadeOut = out;
    if (!inCurve.empty()) setUserString(n, "user.fadeInCurve", inCurve);
    if (!outCurve.empty()) setUserString(n, "user.fadeOutCurve", outCurve);
    if (!(n == c)) ed.putClip(n);
  }
  return SQ_OK;
}

sq_result doCrossfade(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<ClipRec> clips;
  if (sq_result r = audioTargets(s, p, clips); r != SQ_OK) return r;
  if (clips.size() != 2 || clips[0].track != clips[1].track) return SQ_E_INVALIDARG;
  if (clips[1].start < clips[0].start) std::swap(clips[0], clips[1]);
  ClipRec a = clips[0], b = clips[1];
  double len = 0.05;
  std::string curve = "scurve";
  if (sq_result r = optNum(p, "length", len); r != SQ_OK) return r;
  if (sq_result r = optStr(p, "curve", curve); r != SQ_OK) return r;
  if (!(len > 0) || !validCurve(curve)) return SQ_E_INVALIDARG;
  if (b.end() <= a.end() + kEps) return SQ_E_INVALIDARG;  // b inside a: nothing to cross into
  const double overlap = a.end() - b.start;
  double fade = 0;
  if (overlap > kEps) {
    fade = secs(s, a.end()) - secs(s, b.start);
  } else if (overlap >= -kEps) {
    // Touching: grow both into each other around the cut, as far as the sources allow.
    const double cut = secs(s, b.start);
    double leftExt = std::min({len * 0.5, b.sourceOffset, cut});
    double rightExt = len - leftExt;
    MediaInfo mi;
    if (mediaInfo(a.source, mi) == SQ_OK && mi.seconds() > 0) {
      const double avail = std::max(0.0, mi.seconds() - (a.sourceOffset + clipSeconds(s, a)));
      if (rightExt > avail) {
        rightExt = avail;
        leftExt = std::min({len - rightExt, b.sourceOffset, cut});
      }
    }
    fade = leftExt + rightExt;
    if (fade <= 1e-6) return SQ_E_UNSUPPORTED;  // no handle material on either side
    const double bEnd = b.end();
    trimClip(b, ppqAt(s, cut - leftExt), bEnd, s.tempo);
    if (b.sourceOffset < 0) b.sourceOffset = 0.0;
    a.length = ppqAt(s, cut + rightExt) - a.start;
  } else {
    return SQ_E_INVALIDARG;  // a gap between them
  }
  a.fadeOut = fade;
  b.fadeIn = fade;
  setUserString(a, "user.fadeOutCurve", curve);
  setUserString(b, "user.fadeInCurve", curve);
  // The overlap is intended: tag the edits so remove-overlaps does not trim it away again.
  ed.setTag(edoc::kTagSideEffect);
  if (!(a == clips[0])) ed.putClip(a);
  if (!(b == clips[1])) ed.putClip(b);
  ed.setTag(edoc::kTagEdit);
  if (result) result->setDouble("length", fade);
  putResultNodes(result, {a.id, b.id});
  return SQ_OK;
}

// ---- gain ----
sq_result doGain(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  std::vector<sq_guid> ids;
  std::vector<ClipRec> clips;
  if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
  if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  const bool abs = has(p, "gain_db"), rel = has(p, "delta_db");
  if (abs == rel) return SQ_E_INVALIDARG;
  double v = 0;
  if (sq_result r = num(p, abs ? "gain_db" : "delta_db", v); r != SQ_OK) return r;
  for (const ClipRec& c : clips) {
    ClipRec n = c;
    n.gainDb = abs ? v : c.gainDb + v;
    if (n.gainDb < kMinGainDb || n.gainDb > kMaxGainDb) return SQ_E_INVALIDARG;
    if (!(n == c)) ed.putClip(n);
  }
  return SQ_OK;
}

sq_result doGainEnvelope(Editing& ed, IAttributes* p, IAttributes*) {
  const Store& s = ed.store();
  std::vector<ClipRec> clips;
  if (sq_result r = audioTargets(s, p, clips); r != SQ_OK) return r;
  if (p->getType("points") != SQ_VALUE_BLOB) return SQ_E_INVALIDARG;
  uint32_t need = 0;
  p->getBlob("points", nullptr, 0, &need);
  if (need % (2 * sizeof(double))) return SQ_E_INVALIDARG;
  std::vector<double> raw(need / sizeof(double));
  if (need && p->getBlob("points", raw.data(), need, &need) != SQ_OK) return SQ_E_FAIL;
  std::vector<std::pair<double, double>> pts;
  for (size_t i = 0; i + 1 < raw.size(); i += 2) {
    if (!std::isfinite(raw[i]) || !std::isfinite(raw[i + 1]) || raw[i] < 0) return SQ_E_INVALIDARG;
    if (raw[i + 1] < kMinGainDb || raw[i + 1] > kMaxGainDb) return SQ_E_INVALIDARG;
    pts.emplace_back(raw[i], raw[i + 1]);
  }
  std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string bytes(pts.size() * 2 * sizeof(double), '\0');
  for (size_t i = 0; i < pts.size(); ++i) {
    std::memcpy(&bytes[i * 2 * sizeof(double)], &pts[i].first, sizeof(double));
    std::memcpy(&bytes[(i * 2 + 1) * sizeof(double)], &pts[i].second, sizeof(double));
  }
  for (const ClipRec& c : clips) {
    ClipRec n = c;
    if (pts.empty()) {
      n.user.erase("user.gainEnvelope");
    } else {
      edoc::Value v;
      v.type = SQ_VALUE_BLOB;
      v.s = bytes;
      n.user["user.gainEnvelope"] = v;
    }
    if (!(n == c)) ed.putClip(n);
  }
  return SQ_OK;
}

// ---- normalize ----
sq_result measurePeak(const Store& s, const ClipRec& c, double& peakDb) {
  double sr = 0;
  float peak = 0.0f;
  sq_result r = scanFrames(c.source, c.sourceOffset, clipSeconds(s, c), sr, [&](int64_t, float v) {
    if (v > peak) peak = v;
  });
  if (r != SQ_OK) return r;
  peakDb = peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -HUGE_VAL;
  return SQ_OK;
}

sq_result doNormalize(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<ClipRec> clips;
  if (sq_result r = audioTargets(s, p, clips); r != SQ_OK) return r;
  double target = 0.0;
  std::string mode = "independent";
  if (sq_result r = optNum(p, "target_db", target); r != SQ_OK) return r;
  if (sq_result r = optStr(p, "mode", mode); r != SQ_OK) return r;
  if (target > kMaxGainDb || target < kMinGainDb || (mode != "independent" && mode != "common")) return SQ_E_INVALIDARG;
  std::vector<double> peaks(clips.size(), 0.0);
  if (has(p, "peak_db")) {
    double pk = 0;
    if (sq_result r = num(p, "peak_db", pk); r != SQ_OK) return r;
    std::fill(peaks.begin(), peaks.end(), pk);
  } else {
    for (size_t i = 0; i < clips.size(); ++i)
      if (sq_result r = measurePeak(s, clips[i], peaks[i]); r != SQ_OK) return r;
  }
  double common = -HUGE_VAL;
  for (double pk : peaks) common = std::max(common, pk);
  for (size_t i = 0; i < clips.size(); ++i) {
    const double pk = mode == "common" ? common : peaks[i];
    if (!std::isfinite(pk)) continue;  // silent: nothing to normalize
    ClipRec n = clips[i];
    n.gainDb = std::clamp(target - pk, kMinGainDb, kMaxGainDb);
    if (!(n == clips[i])) ed.putClip(n);
  }
  if (result) result->setDouble("peak_db", std::isfinite(common) ? common : -1000.0);
  return SQ_OK;
}

// ---- strip silence ----
sq_result doStripSilence(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  std::vector<ClipRec> clips;
  if (sq_result r = audioTargets(s, p, clips); r != SQ_OK) return r;
  double thrDb = -48, minSilence = 0.1, minRegion = 0.01, padBefore = 0.005, padAfter = 0.02;
  if (sq_result r = optNum(p, "threshold_db", thrDb); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "min_silence", minSilence); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "min_region", minRegion); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "pad_before", padBefore); r != SQ_OK) return r;
  if (sq_result r = optNum(p, "pad_after", padAfter); r != SQ_OK) return r;
  if (thrDb > 0 || minSilence < 0 || minRegion < 0 || padBefore < 0 || padAfter < 0) return SQ_E_INVALIDARG;
  const float thr = static_cast<float>(std::pow(10.0, thrDb / 20.0));
  std::vector<sq_guid> kept;
  for (const ClipRec& c : clips) {
    const double dur = clipSeconds(s, c);
    double sr = 0;
    std::vector<std::pair<int64_t, int64_t>> regions;  // frames relative to the clip start
    int64_t runStart = -1, lastLoud = -1, total = 0;
    sq_result r = scanFrames(c.source, c.sourceOffset, dur, sr, [&](int64_t i, float v) {
      total = i + 1;
      if (v >= thr) {
        if (runStart < 0) runStart = i;
        lastLoud = i;
      } else if (runStart >= 0 && static_cast<double>(i - lastLoud) >= minSilence * sr) {
        regions.emplace_back(runStart, lastLoud + 1);
        runStart = -1;
      }
    });
    if (r != SQ_OK) return r;
    if (runStart >= 0) regions.emplace_back(runStart, lastLoud + 1);
    // drop blips, pad, merge what touches after padding
    std::vector<std::pair<double, double>> secsRegions;  // seconds from the clip start
    for (const auto& [a, b] : regions) {
      if (static_cast<double>(b - a) < minRegion * sr) continue;
      double x = std::max(0.0, static_cast<double>(a) / sr - padBefore);
      double y = std::min(std::min(dur, static_cast<double>(total) / sr), static_cast<double>(b) / sr + padAfter);
      if (!secsRegions.empty() && x <= secsRegions.back().second) secsRegions.back().second = std::max(secsRegions.back().second, y);
      else if (y > x) secsRegions.emplace_back(x, y);
    }
    if (secsRegions.empty()) {
      ed.removeClip(c.id);
      continue;
    }
    const double t0 = secs(s, c.start);
    bool usedOriginal = false;  // the first kept piece keeps the clip's id
    for (size_t i = 0; i < secsRegions.size(); ++i) {
      ClipRec piece = c;
      const double ps = secsRegions[i].first <= 1e-9 ? c.start : ppqAt(s, t0 + secsRegions[i].first);
      const bool toEnd = secsRegions[i].second >= dur - 1e-9;
      const double pe = toEnd ? c.end() : ppqAt(s, t0 + secsRegions[i].second);
      if (pe <= ps + kEps) continue;
      if (usedOriginal) piece.id = edoc::newGuid();
      trimClip(piece, ps, pe, s.tempo);
      if (piece.sourceOffset < 0) piece.sourceOffset = 0.0;
      if (ps > c.start + kEps) piece.fadeIn = 0.0;
      if (!toEnd) piece.fadeOut = 0.0;
      ed.setTag(usedOriginal ? edoc::kTagEdit : edoc::kTagResize);
      if (!(piece == c)) ed.putClip(piece);
      usedOriginal = true;
      kept.push_back(piece.id);
    }
    ed.setTag(edoc::kTagEdit);
    if (!usedOriginal) ed.removeClip(c.id);
  }
  putResultNodes(result, kept);
  return SQ_OK;
}

// ---- silence a range (no ripple) ----
sq_result doSilence(Editing& ed, IAttributes* p, IAttributes* result) {
  const Store& s = ed.store();
  double from = 0, to = 0;
  if (sq_result r = num(p, "from", from); r != SQ_OK) return r;
  if (sq_result r = num(p, "to", to); r != SQ_OK) return r;
  if (from < 0 || to <= from + kEps) return SQ_E_INVALIDARG;
  std::vector<ClipRec> clips;
  if (has(p, "nodes") || has(p, "node")) {
    std::vector<sq_guid> ids;
    if (sq_result r = readNodes(p, ids); r != SQ_OK) return r;
    if (sq_result r = targetClips(s, ids, clips); r != SQ_OK) return r;
  } else {
    std::vector<sq_guid> tracks;
    if (sq_result r = readGuidList(p, "tracks", tracks); r != SQ_OK) return r;
    if (tracks.empty()) tracks = s.trackOrder;
    for (const sq_guid& t : tracks) {
      if (!s.track(t)) return SQ_E_NOTFOUND;
      for (const sq_guid& cid : s.clipsOf(t)) {
        const ClipRec& c = *s.clip(cid);
        if (!(c.flags & SQ_CLIP_LOCKED)) clips.push_back(c);
      }
    }
  }
  std::vector<sq_guid> remaining;
  for (const ClipRec& c : clips)
    for (const sq_guid& id : cutRange(ed, c, from, to)) remaining.push_back(id);
  putResultNodes(result, remaining);
  return SQ_OK;
}

}  // namespace

const std::vector<ActionDef>& audioActionDefs() {
  static const std::vector<ActionDef> defs = {
    {kFade, "Fade", &doFade},
    {kCrossfade, "Crossfade", &doCrossfade},
    {kGain, "Clip Gain", &doGain},
    {kGainEnvelope, "Gain Envelope", &doGainEnvelope},
    {kNormalize, "Normalize", &doNormalize},
    {kStripSilence, "Strip Silence", &doStripSilence},
    {kSilence, "Silence", &doSilence},
  };
  return defs;
}

}  // namespace sq::edit::detail
