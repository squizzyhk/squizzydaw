// Standard MIDI File parser and writer (own implementation of the public SMF 1.0 layout).
#include "smf.hpp"

#include <cstring>

namespace sq::smf {
namespace {

constexpr size_t kMaxPayload = size_t(1) << 30;

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void putBe32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(uint8_t(v >> 24));
  o.push_back(uint8_t(v >> 16));
  o.push_back(uint8_t(v >> 8));
  o.push_back(uint8_t(v));
}
void putBe16(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(uint8_t(v >> 8));
  o.push_back(uint8_t(v));
}

void setErr(std::string* err, const char* msg, size_t at) {
  if (err) *err = std::string(msg) + " (offset " + std::to_string(at) + ")";
}

// Reads a VLQ (max 4 bytes). False if truncated or longer than 4 bytes.
bool readVlq(const uint8_t* d, size_t n, size_t& p, uint32_t& v) {
  v = 0;
  for (int i = 0; i < 4; ++i) {
    if (p >= n) return false;
    const uint8_t b = d[p++];
    v = (v << 7) | (b & 0x7Fu);
    if (!(b & 0x80u)) return true;
  }
  return false;
}

enum class TrackResult { Ok, Truncated, Error };

TrackResult parseTrack(const uint8_t* d, size_t n, size_t baseOffset, File& file, Track& track, std::string* err) {
  uint64_t tick = 0;
  uint8_t running = 0;
  size_t p = 0;
  bool haveName = false;
  bool ended = false;
  TrackResult result = TrackResult::Ok;
  while (p < n) {
    uint32_t delta = 0;
    if (!readVlq(d, n, p, delta)) {
      result = TrackResult::Truncated;
      break;
    }
    if (p >= n) {
      result = TrackResult::Truncated;
      break;
    }
    tick += delta;
    const uint8_t b = d[p];
    Event e;
    e.tick = tick;
    if (b == 0xFFu) {
      if (p + 2 > n) {
        result = TrackResult::Truncated;
        break;
      }
      const uint8_t type = d[p + 1];
      size_t q = p + 2;
      uint32_t len = 0;
      if (!readVlq(d, n, q, len) || len > n - q) {
        result = TrackResult::Truncated;
        break;
      }
      if (type >= 0x80u) {
        setErr(err, "invalid meta type", baseOffset + p);
        return TrackResult::Error;
      }
      e.status = 0xFFu;
      e.metaType = type;
      e.size = len;
      if (file.payload.size() + len > kMaxPayload) {
        setErr(err, "file too large", baseOffset + p);
        return TrackResult::Error;
      }
      e.offset = uint32_t(file.payload.size());
      file.payload.insert(file.payload.end(), d + q, d + q + len);
      p = q + len;
      if (type == 0x03u && !haveName) {
        track.name.assign(reinterpret_cast<const char*>(d + q), len);
        // Track names are free text; cut at an embedded NUL so the C string stays faithful.
        size_t z = track.name.find('\0');
        if (z != std::string::npos) track.name.resize(z);
        haveName = true;
      }
      track.events.push_back(e);
      if (type == 0x2Fu) {
        ended = true;
        break;
      }
      continue;
    }
    if (b == 0xF0u || b == 0xF7u) {
      size_t q = p + 1;
      uint32_t len = 0;
      if (!readVlq(d, n, q, len) || len > n - q) {
        result = TrackResult::Truncated;
        break;
      }
      e.status = b;
      e.size = len;
      if (file.payload.size() + len > kMaxPayload) {
        setErr(err, "file too large", baseOffset + p);
        return TrackResult::Error;
      }
      e.offset = uint32_t(file.payload.size());
      file.payload.insert(file.payload.end(), d + q, d + q + len);
      p = q + len;
      track.events.push_back(e);
      continue;
    }
    uint8_t status = 0;
    if (b & 0x80u) {
      if (b > 0xEFu) {
        setErr(err, "invalid status byte in track", baseOffset + p);
        return TrackResult::Error;
      }
      status = b;
      running = b;
      ++p;
    } else {
      if (!running) {
        setErr(err, "data byte without running status", baseOffset + p);
        return TrackResult::Error;
      }
      status = running;
    }
    const uint32_t nd = channelDataBytes(status);
    if (p + nd > n) {
      result = TrackResult::Truncated;
      break;
    }
    for (uint32_t i = 0; i < nd; ++i) {
      if (d[p + i] & 0x80u) {
        setErr(err, "status byte where a data byte was expected", baseOffset + p + i);
        return TrackResult::Error;
      }
    }
    e.status = status;
    e.data1 = d[p];
    e.data2 = nd == 2 ? d[p + 1] : 0;
    p += nd;
    track.events.push_back(e);
  }
  if (!ended) {
    Event eot;
    eot.tick = track.events.empty() ? tick : track.events.back().tick;
    if (tick > eot.tick) eot.tick = tick;
    eot.status = 0xFFu;
    eot.metaType = 0x2Fu;
    eot.offset = uint32_t(file.payload.size());
    track.events.push_back(eot);
  }
  return result;
}

}  // namespace

void writeVlq(std::vector<uint8_t>& out, uint32_t value) {
  uint8_t tmp[4];
  int n = 0;
  tmp[n++] = uint8_t(value & 0x7Fu);
  while ((value >>= 7) != 0 && n < 4) tmp[n++] = uint8_t((value & 0x7Fu) | 0x80u);
  while (n > 0) out.push_back(tmp[--n]);
}

sq_result parse(const uint8_t* data, size_t size, File& out, std::string* err) {
  out = File{};
  if (!data && size) return SQ_E_INVALIDARG;
  // RIFF "RMID" wrapper: find the "data" sub-chunk holding the SMF.
  if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "RMID", 4) == 0) {
    size_t p = 12;
    bool found = false;
    while (p + 8 <= size) {
      const uint32_t len = le32(data + p + 4);
      if (std::memcmp(data + p, "data", 4) == 0) {
        const size_t avail = size - (p + 8);
        data = data + p + 8;
        size = len < avail ? len : avail;
        found = true;
        break;
      }
      const size_t next = p + 8 + size_t(len) + (len & 1u);
      if (next <= p) break;
      p = next;
    }
    if (!found) {
      setErr(err, "RMID container without a data chunk", 0);
      return SQ_E_PARSE;
    }
  }
  if (size < 14 || std::memcmp(data, "MThd", 4) != 0) {
    setErr(err, "not a Standard MIDI File", 0);
    return SQ_E_PARSE;
  }
  const uint32_t hlen = be32(data + 4);
  if (hlen < 6) {
    setErr(err, "MThd chunk too short", 4);
    return SQ_E_PARSE;
  }
  out.format = be16(data + 8);
  out.declaredTracks = be16(data + 10);
  out.division = be16(data + 12);
  if (out.format > 2) {
    setErr(err, "unsupported SMF format", 8);
    return SQ_E_PARSE;
  }
  if (out.division == 0) {
    setErr(err, "zero time division", 12);
    return SQ_E_PARSE;
  }
  if (out.division & 0x8000u) {
    const int fps = -int(int8_t(out.division >> 8));
    if ((fps != 24 && fps != 25 && fps != 29 && fps != 30) || (out.division & 0xFFu) == 0) {
      setErr(err, "invalid SMPTE time division", 12);
      return SQ_E_PARSE;
    }
  }
  size_t p = 8 + size_t(hlen);
  while (p + 8 <= size) {
    const uint32_t len = be32(data + p + 4);
    const size_t body = p + 8;
    const size_t avail = size - body;
    const size_t n = len < avail ? len : avail;
    if (std::memcmp(data + p, "MTrk", 4) == 0) {
      out.tracks.emplace_back();
      TrackResult r = parseTrack(data + body, n, body, out, out.tracks.back(), err);
      if (r == TrackResult::Error) return SQ_E_PARSE;
      if (r == TrackResult::Truncated && err && err->empty()) setErr(err, "track truncated (recovered)", body);
    }
    if (len > avail) break;  // truncated file: nothing follows
    p = body + len;
  }
  if (out.tracks.empty() && out.declaredTracks > 0) {
    setErr(err, "no MTrk chunk found", 14);
    return SQ_E_PARSE;
  }
  return SQ_OK;
}

// ---- writer --------------------------------------------------------------------------------

sq_result Writer::begin(uint32_t format, uint32_t division) {
  if (format > 1) return SQ_E_UNSUPPORTED;
  if (division == 0 || division > 0xFFFFu) return SQ_E_INVALIDARG;
  if (division & 0x8000u) {
    const int fps = -int(int8_t(division >> 8));
    if ((fps != 24 && fps != 25 && fps != 29 && fps != 30) || (division & 0xFFu) == 0) return SQ_E_INVALIDARG;
  }
  *this = Writer{};
  started_ = true;
  format_ = format;
  division_ = uint16_t(division);
  return SQ_OK;
}

sq_result Writer::beginTrack() {
  if (!started_ || inTrack_) return SQ_E_FAIL;
  if (format_ == 0 && trackCount_ >= 1) return SQ_E_UNSUPPORTED;
  if (trackCount_ >= 0xFFFFu) return SQ_E_UNSUPPORTED;
  inTrack_ = true;
  trackEnded_ = false;
  lastTick_ = 0;
  endTick_ = 0;
  running_ = 0;
  track_.clear();
  return SQ_OK;
}

sq_result Writer::addEvent(uint64_t tick, uint8_t status, uint8_t data1, uint8_t data2, uint8_t metaType,
                           const uint8_t* data, uint32_t size) {
  if (!inTrack_) return SQ_E_FAIL;
  if (trackEnded_) return SQ_E_FAIL;
  if (tick < lastTick_) return SQ_E_INVALIDARG;
  if (tick - lastTick_ > 0x0FFFFFFFull) return SQ_E_INVALIDARG;
  if (size && !data) return SQ_E_INVALIDARG;
  if (size > 0x0FFFFFFFu) return SQ_E_INVALIDARG;
  const uint32_t delta = uint32_t(tick - lastTick_);
  if (status >= 0x80u && status <= 0xEFu) {
    const uint32_t nd = channelDataBytes(status);
    if ((data1 & 0x80u) || (nd == 2 && (data2 & 0x80u))) return SQ_E_INVALIDARG;
    writeVlq(track_, delta);
    if (status != running_) track_.push_back(status);
    running_ = status;
    track_.push_back(data1);
    if (nd == 2) track_.push_back(data2);
  } else if (status == 0xFFu) {
    if (metaType & 0x80u) return SQ_E_INVALIDARG;
    if (metaType == 0x2Fu) {
      endTick_ = tick;
      trackEnded_ = true;
      return SQ_OK;  // lastTick_ unchanged: the delta is computed in endTrack()
    }
    writeVlq(track_, delta);
    track_.push_back(0xFFu);
    track_.push_back(metaType);
    writeVlq(track_, size);
    if (size) track_.insert(track_.end(), data, data + size);
    running_ = 0;
  } else if (status == 0xF0u || status == 0xF7u) {
    writeVlq(track_, delta);
    track_.push_back(status);
    writeVlq(track_, size);
    if (size) track_.insert(track_.end(), data, data + size);
    running_ = 0;
  } else {
    return SQ_E_INVALIDARG;
  }
  lastTick_ = tick;
  return SQ_OK;
}

sq_result Writer::endTrack() {
  if (!inTrack_) return SQ_E_FAIL;
  const uint64_t end = endTick_ > lastTick_ ? endTick_ : lastTick_;
  if (end - lastTick_ > 0x0FFFFFFFull) return SQ_E_INVALIDARG;
  writeVlq(track_, uint32_t(end - lastTick_));
  track_.push_back(0xFFu);
  track_.push_back(0x2Fu);
  track_.push_back(0x00u);
  if (track_.size() > 0xFFFFFFFFull) return SQ_E_UNSUPPORTED;
  chunks_.insert(chunks_.end(), {'M', 'T', 'r', 'k'});
  putBe32(chunks_, uint32_t(track_.size()));
  chunks_.insert(chunks_.end(), track_.begin(), track_.end());
  track_.clear();
  inTrack_ = false;
  ++trackCount_;
  return SQ_OK;
}

sq_result Writer::finish(std::vector<uint8_t>& out) {
  if (!started_) return SQ_E_FAIL;
  if (inTrack_) {
    sq_result r = endTrack();
    if (SQ_FAILED(r)) return r;
  }
  if (trackCount_ == 0) return SQ_E_FAIL;
  out.clear();
  out.reserve(14 + chunks_.size());
  out.insert(out.end(), {'M', 'T', 'h', 'd'});
  putBe32(out, 6);
  putBe16(out, format_);
  putBe16(out, trackCount_);
  putBe16(out, division_);
  out.insert(out.end(), chunks_.begin(), chunks_.end());
  started_ = false;
  return SQ_OK;
}

}  // namespace sq::smf
