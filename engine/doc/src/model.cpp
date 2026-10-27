// Squizzy Studio engine/doc - store, change application, tempo math.
#include "sq/engine/doc/model.hpp"

#include <algorithm>
#include <cmath>

#include "sq/platform.h"

namespace sq::edoc {

sq_guid newGuid() {
  sq_guid g{};
  sq_guid_generate(&g);
  return g;
}

std::string guidStr(const sq_guid& g) { return sq::toString(g); }

// ---- tempo math -----------------------------------------------------------------------------------
// Between two points with SQ_TEMPO_RAMP the bpm changes linearly in ppq:
//   bpm(p) = b0 + k (p - p0),  k = (b1 - b0) / L
//   seconds(p) = 60/k * ln(bpm(p) / b0)            (k != 0)
//   p(seconds) = p0 + b0/k * (exp(seconds*k/60) - 1)
namespace {
struct Segment {
  double p0, b0, k;  // k = bpm slope per ppq (0 = constant)
};
Segment segmentAt(const TempoMapRec& m, size_t i) {
  const TempoPoint& a = m.tempos[i];
  Segment s{a.pos, a.bpm > 0 ? a.bpm : 120.0, 0.0};
  if ((a.flags & SQ_TEMPO_RAMP) && i + 1 < m.tempos.size()) {
    const TempoPoint& b = m.tempos[i + 1];
    const double len = b.pos - a.pos;
    if (len > 0) s.k = ((b.bpm > 0 ? b.bpm : 120.0) - s.b0) / len;
  }
  return s;
}
double segSeconds(const Segment& s, double dp) {
  if (dp <= 0) return 0.0;
  if (std::fabs(s.k) < 1e-12) return dp * 60.0 / s.b0;
  return 60.0 / s.k * std::log((s.b0 + s.k * dp) / s.b0);
}
double segPpq(const Segment& s, double secs) {
  if (secs <= 0) return 0.0;
  if (std::fabs(s.k) < 1e-12) return secs * s.b0 / 60.0;
  return s.b0 / s.k * (std::exp(secs * s.k / 60.0) - 1.0);
}
}  // namespace

double TempoMapRec::ppqToSeconds(double ppq) const {
  if (tempos.empty()) return ppq * 0.5;
  if (ppq <= 0) return ppq * 60.0 / (tempos[0].bpm > 0 ? tempos[0].bpm : 120.0);
  double t = 0.0;
  for (size_t i = 0; i < tempos.size(); ++i) {
    Segment s = segmentAt(*this, i);
    const bool last = i + 1 == tempos.size();
    const double segEnd = last ? ppq : std::min(ppq, tempos[i + 1].pos);
    t += segSeconds(s, segEnd - s.p0);
    if (last || ppq <= tempos[i + 1].pos) break;
  }
  return t;
}

double TempoMapRec::secondsToPpq(double seconds) const {
  if (tempos.empty()) return seconds * 2.0;
  if (seconds <= 0) return seconds * (tempos[0].bpm > 0 ? tempos[0].bpm : 120.0) / 60.0;
  double t = 0.0;
  for (size_t i = 0; i < tempos.size(); ++i) {
    Segment s = segmentAt(*this, i);
    const bool last = i + 1 == tempos.size();
    if (!last) {
      const double dur = segSeconds(s, tempos[i + 1].pos - s.p0);
      if (seconds > t + dur) {
        t += dur;
        continue;
      }
    }
    return s.p0 + segPpq(s, seconds - t);
  }
  return 0.0;
}

// ---- store ----------------------------------------------------------------------------------------
const TrackRec* Store::track(const sq_guid& id) const {
  auto it = tracks.find(id);
  return it == tracks.end() ? nullptr : &it->second;
}
const ClipRec* Store::clip(const sq_guid& id) const {
  auto it = clips.find(id);
  return it == clips.end() ? nullptr : &it->second;
}
const MarkerRec* Store::marker(const sq_guid& id) const {
  auto it = markers.find(id);
  return it == markers.end() ? nullptr : &it->second;
}
int32_t Store::trackIndex(const sq_guid& id) const {
  for (size_t i = 0; i < trackOrder.size(); ++i)
    if (trackOrder[i] == id) return static_cast<int32_t>(i);
  return -1;
}

const std::vector<sq_guid>& Store::clipsOf(const sq_guid& trackId) const {
  if (clipIndexDirty_) {
    clipsByTrack_.clear();
    for (const auto& [id, c] : clips) clipsByTrack_[c.track].push_back(id);
    for (auto& [tid, v] : clipsByTrack_) {
      std::sort(v.begin(), v.end(), [this](const sq_guid& a, const sq_guid& b) {
        const ClipRec& ca = clips.at(a);
        const ClipRec& cb = clips.at(b);
        if (ca.start != cb.start) return ca.start < cb.start;
        return a < b;
      });
    }
    clipIndexDirty_ = false;
  }
  static const std::vector<sq_guid> kEmpty;
  auto it = clipsByTrack_.find(trackId);
  return it == clipsByTrack_.end() ? kEmpty : it->second;
}

const std::vector<sq_guid>& Store::markerOrder() const {
  if (markerIndexDirty_) {
    markerOrder_.clear();
    for (const auto& [id, m] : markers) markerOrder_.push_back(id);
    std::sort(markerOrder_.begin(), markerOrder_.end(), [this](const sq_guid& a, const sq_guid& b) {
      const MarkerRec& ma = markers.at(a);
      const MarkerRec& mb = markers.at(b);
      if (ma.position != mb.position) return ma.position < mb.position;
      return a < b;
    });
    markerIndexDirty_ = false;
  }
  return markerOrder_;
}

double Store::contentLength() const {
  double len = 0.0;
  for (const auto& [id, c] : clips) len = std::max(len, c.end());
  for (const auto& [id, m] : markers) len = std::max(len, m.position + m.length);
  return len;
}

void Store::applyTrack(const sq_guid& id, const std::optional<TrackRec>& rec, int32_t index) {
  const int32_t cur = trackIndex(id);
  if (!rec) {
    tracks.erase(id);
    if (cur >= 0) trackOrder.erase(trackOrder.begin() + cur);
    return;
  }
  tracks[id] = *rec;
  if (cur >= 0) {
    if (index < 0 || index == cur) return;
    trackOrder.erase(trackOrder.begin() + cur);
  }
  const size_t at = (index < 0 || static_cast<size_t>(index) > trackOrder.size()) ? trackOrder.size() : static_cast<size_t>(index);
  trackOrder.insert(trackOrder.begin() + static_cast<std::ptrdiff_t>(at), id);
}

void Store::apply(const Change& c, bool forward) {
  switch (c.kind) {
    case kChangeDoc: {
      const auto& d = std::get<Delta<DocProps>>(c.data);
      props = forward ? *d.after : *d.before;
      break;
    }
    case kChangeTempo: {
      const auto& d = std::get<Delta<TempoMapRec>>(c.data);
      tempo = forward ? *d.after : *d.before;
      break;
    }
    case kChangeTrack: {
      const auto& d = std::get<TrackDelta>(c.data);
      applyTrack(c.id, forward ? d.after : d.before, forward ? d.indexAfter : d.indexBefore);
      break;
    }
    case kChangeClip: {
      const auto& d = std::get<Delta<ClipRec>>(c.data);
      const auto& r = forward ? d.after : d.before;
      if (r) clips[c.id] = *r;
      else clips.erase(c.id);
      clipIndexDirty_ = true;
      break;
    }
    case kChangeMarker: {
      const auto& d = std::get<Delta<MarkerRec>>(c.data);
      const auto& r = forward ? d.after : d.before;
      if (r) markers[c.id] = *r;
      else markers.erase(c.id);
      markerIndexDirty_ = true;
      break;
    }
    case kChangeJournal: {
      const auto& d = std::get<JournalDelta>(c.data);
      const auto& r = forward ? d.after : d.before;
      if (r) journal[d.key] = *r;
      else journal.erase(d.key);
      break;
    }
  }
}

}  // namespace sq::edoc
