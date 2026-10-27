// Squizzy Studio codec services - RIFF marker vocabulary (cue / LIST-adtl / smpl), shared by the
// WAV writer/reader and the FLAC foreign-chunk path. Little endian, RIFF sub-chunk padding rules.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sqcodec/common.hpp"

namespace sq::cx {

inline constexpr size_t kSmplHeaderSize = 36;
inline constexpr size_t kSmplLoopSize = 24;

// Collects cue / adtl / smpl payloads, then build() produces markers.
class RiffMarkerParser {
 public:
  void cue(const uint8_t* p, size_t n) {
    if (n < 4) return;
    uint32_t count = rd32le(p);
    size_t avail = (n - 4) / 24;
    if (count > avail) count = static_cast<uint32_t>(avail);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* e = p + 4 + i * 24;
      cues_.push_back(Cue{rd32le(e), rd32le(e + 20)});
    }
  }
  // payload of a LIST chunk (starts with the list type)
  void list(const uint8_t* p, size_t n) {
    if (n < 4 || std::memcmp(p, "adtl", 4) != 0) return;
    size_t off = 4;
    while (off + 8 <= n) {
      const uint8_t* h = p + off;
      uint32_t sz = rd32le(h + 4);
      if (sz > n - off - 8) break;
      const uint8_t* d = h + 8;
      if ((std::memcmp(h, "labl", 4) == 0 || std::memcmp(h, "note", 4) == 0) && sz >= 4) {
        uint32_t id = rd32le(d);
        std::string text = toUtf8Lenient(fieldText(d + 4, sz - 4));
        if (std::memcmp(h, "labl", 4) == 0) labels_[id] = text;
        else if (!notes_.count(id)) notes_[id] = text;
      } else if (std::memcmp(h, "ltxt", 4) == 0 && sz >= 20) {
        uint32_t id = rd32le(d);
        lengths_[id] = rd32le(d + 4);
        if (sz > 20 && !labels_.count(id)) notes_[id] = toUtf8Lenient(fieldText(d + 20, sz - 20));
      }
      off += 8 + sz + (sz & 1);
    }
  }
  void smpl(const uint8_t* p, size_t n) {
    if (n < kSmplHeaderSize) return;
    uint32_t count = rd32le(p + 28);
    size_t avail = (n - kSmplHeaderSize) / kSmplLoopSize;
    if (count > avail) count = static_cast<uint32_t>(avail);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* e = p + kSmplHeaderSize + i * kSmplLoopSize;
      loops_.push_back(Loop{rd32le(e), rd32le(e + 8), rd32le(e + 12)});
    }
  }
  // frameCount (>= 0) clamps positions into the file; -1 = unknown.
  std::vector<sq_audio_file_marker> build(sq_sample_time frameCount) const {
    std::vector<sq_audio_file_marker> out;
    for (const Cue& c : cues_) {
      sq_sample_time pos = c.offset;
      if (frameCount >= 0 && pos > frameCount) continue;
      sq_sample_time len = 0;
      auto l = lengths_.find(c.id);
      if (l != lengths_.end()) len = l->second;
      out.push_back(makeMarker(c.id, pos, len, 0, nameOf(c.id)));
    }
    for (const Loop& lp : loops_) {
      if (lp.end < lp.start) continue;
      if (frameCount >= 0 && lp.start > frameCount) continue;
      const sq_sample_time len = static_cast<sq_sample_time>(lp.end) - lp.start + 1;
      bool merged = false;
      for (auto& m : out) {
        if (m.id == lp.id && m.position == lp.start && !(m.flags & SQ_MARKER_LOOP)) {
          m.flags |= SQ_MARKER_LOOP | SQ_MARKER_REGION;
          m.length = len;
          merged = true;
          break;
        }
      }
      if (!merged) out.push_back(makeMarker(lp.id, lp.start, len, SQ_MARKER_LOOP, nameOf(lp.id)));
    }
    return out;
  }

 private:
  std::string nameOf(uint32_t id) const {
    auto l = labels_.find(id);
    if (l != labels_.end()) return l->second;
    auto n = notes_.find(id);
    return n != notes_.end() ? n->second : std::string();
  }
  struct Cue { uint32_t id; uint32_t offset; };
  struct Loop { uint32_t id; uint32_t start; uint32_t end; };
  std::vector<Cue> cues_;
  std::vector<Loop> loops_;
  std::map<uint32_t, std::string> labels_, notes_;
  std::map<uint32_t, uint32_t> lengths_;
};

inline uint32_t clampU32(sq_sample_time v) {
  if (v < 0) return 0;
  return v > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
}

// Gives every marker a unique non-zero id (keeps valid caller ids).
inline void assignMarkerIds(std::vector<sq_audio_file_marker>& ms) {
  std::vector<uint32_t> used;
  uint32_t next = 1;
  for (auto& m : ms) {
    bool dup = m.id == 0 || std::find(used.begin(), used.end(), m.id) != used.end();
    if (dup) {
      while (std::find(used.begin(), used.end(), next) != used.end() ||
             std::any_of(ms.begin(), ms.end(), [&](const sq_audio_file_marker& o) { return o.id == next; }))
        ++next;
      m.id = next++;
    }
    used.push_back(m.id);
  }
}

// RIFF sub-chunk append with pad byte.
inline void putSubChunk(Bytes& b, const char* id, const void* data, size_t n) {
  b.fourcc(id);
  b.u32le(static_cast<uint32_t>(n));
  b.raw(data, n);
  if (n & 1) b.u8(0);
}

struct RiffMarkerChunks {
  std::vector<uint8_t> cue, adtl, smpl;  // payloads; empty = none
};

// Markers -> cue / LIST-adtl / smpl payloads. `rawSmpl` (may be empty) supplies the sampler header
// and trailing sampler-specific data to keep. `sampleRate` fills a default header.
inline RiffMarkerChunks makeRiffMarkerChunks(std::vector<sq_audio_file_marker> ms, const std::vector<uint8_t>& rawSmpl,
                                             double sampleRate) {
  RiffMarkerChunks r;
  assignMarkerIds(ms);
  Bytes cue;
  cue.u32le(static_cast<uint32_t>(ms.size()));
  for (size_t i = 0; i < ms.size(); ++i) {
    cue.u32le(ms[i].id);
    cue.u32le(static_cast<uint32_t>(i));
    cue.fourcc("data");
    cue.u32le(0);
    cue.u32le(0);
    cue.u32le(clampU32(ms[i].position));
  }
  r.cue = std::move(cue.v);

  Bytes adtl;
  adtl.fourcc("adtl");
  for (const auto& m : ms) {
    size_t nl = strnlen(m.name, sizeof m.name);
    if (nl) {
      Bytes l;
      l.u32le(m.id);
      l.raw(m.name, nl);
      l.u8(0);
      putSubChunk(adtl, "labl", l.v.data(), l.size());
    }
    if (m.length > 0) {
      Bytes t;
      t.u32le(m.id);
      t.u32le(clampU32(m.length));
      t.fourcc("rgn ");
      t.u16le(0); t.u16le(0); t.u16le(0); t.u16le(0);
      putSubChunk(adtl, "ltxt", t.v.data(), t.size());
    }
  }
  if (adtl.size() > 4) r.adtl = std::move(adtl.v);

  std::vector<const sq_audio_file_marker*> loops;
  for (const auto& m : ms)
    if ((m.flags & SQ_MARKER_LOOP) && m.length > 0) loops.push_back(&m);
  if (!loops.empty() || !rawSmpl.empty()) {
    Bytes s;
    std::vector<uint8_t> tail;
    if (rawSmpl.size() >= kSmplHeaderSize) {
      s.raw(rawSmpl.data(), 28);
      uint32_t oldLoops = rd32le(rawSmpl.data() + 28);
      uint64_t tailAt = kSmplHeaderSize + static_cast<uint64_t>(oldLoops) * kSmplLoopSize;
      if (tailAt < rawSmpl.size()) tail.assign(rawSmpl.begin() + static_cast<ptrdiff_t>(tailAt), rawSmpl.end());
    } else {
      s.u32le(0);  // manufacturer
      s.u32le(0);  // product
      s.u32le(sampleRate > 0 ? static_cast<uint32_t>(1e9 / sampleRate + 0.5) : 0);  // sample period ns
      s.u32le(60);  // unity note
      s.u32le(0);   // pitch fraction
      s.u32le(0);   // smpte format
      s.u32le(0);   // smpte offset
    }
    s.u32le(static_cast<uint32_t>(loops.size()));
    s.u32le(static_cast<uint32_t>(tail.size()));
    for (const auto* m : loops) {
      s.u32le(m->id);
      s.u32le(0);  // forward
      s.u32le(clampU32(m->position));
      s.u32le(clampU32(m->position + m->length - 1));
      s.u32le(0);
      s.u32le(0);  // infinite
    }
    s.raw(tail.data(), tail.size());
    r.smpl = std::move(s.v);
  }
  return r;
}

}  // namespace sq::cx
