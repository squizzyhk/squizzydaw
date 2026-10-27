// Squizzy Studio codec services - shared header-only helpers (owner: codecs unit).
// Included by services/codec-{wav,flac,mp3,ogg,aac}. Header-only on purpose: services never link
// each other (CONTRACTS 0.6), they only share this source.
//
//   FileStream       IByteStream over the platform sq_file API (UTF-8 paths)
//   MetaStore        ordered "chunk:<id>" / "tag:<name>" key -> bytes store with contract copy-out
//   ReaderBase<D>    IAudioFileReader base implementing the metadata / marker getters
//   WriterTarget     open-by-path / open-by-stream destination with abort-deletes-file semantics
//   byte order helpers, float <-> int sample conversion, TPDF dither, stream shifting
//
// Not realtime safe (allocates, does I/O); codec objects run on disk worker threads.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <cstring>
#include <string>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/cpp/ptr.hpp"
#include "sq/interfaces/codec.h"
#include "sq/platform.h"

namespace sq::cx {

// ---- byte order --------------------------------------------------------------------------------
inline uint16_t rd16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rd64le(const uint8_t* p) { return rd32le(p) | (static_cast<uint64_t>(rd32le(p + 4)) << 32); }
inline uint16_t rd16be(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t rd32be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}
inline void wr16le(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32le(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
inline void wr64le(uint8_t* p, uint64_t v) { wr32le(p, uint32_t(v)); wr32le(p + 4, uint32_t(v >> 32)); }
inline void wr16be(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void wr32be(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// Growable little/big endian byte builder.
struct Bytes {
  std::vector<uint8_t> v;
  void u8(uint32_t x) { v.push_back(uint8_t(x)); }
  void u16le(uint32_t x) { uint8_t b[2]; wr16le(b, x); v.insert(v.end(), b, b + 2); }
  void u32le(uint32_t x) { uint8_t b[4]; wr32le(b, x); v.insert(v.end(), b, b + 4); }
  void u64le(uint64_t x) { uint8_t b[8]; wr64le(b, x); v.insert(v.end(), b, b + 8); }
  void u16be(uint32_t x) { uint8_t b[2]; wr16be(b, x); v.insert(v.end(), b, b + 2); }
  void u32be(uint32_t x) { uint8_t b[4]; wr32be(b, x); v.insert(v.end(), b, b + 4); }
  void fourcc(const char* id) { v.insert(v.end(), id, id + 4); }
  void raw(const void* p, size_t n) { if (n) v.insert(v.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
  void zeros(size_t n) { v.insert(v.end(), n, 0); }
  size_t size() const { return v.size(); }
};

// ---- strings -----------------------------------------------------------------------------------
// Contract 4.3 copy-out. nul=true: text (needed includes NUL, truncated text is NUL-terminated).
inline sq_result copyOut(const void* data, size_t size, bool nul, void* buf, uint32_t bufSize, uint32_t* needed) {
  const size_t need = size + (nul ? 1 : 0);
  if (needed) *needed = static_cast<uint32_t>(need);
  if (!buf || bufSize < need) {
    if (buf && bufSize && nul) {
      std::memcpy(buf, data, bufSize - 1);
      static_cast<char*>(buf)[bufSize - 1] = '\0';
    }
    return SQ_E_BUFFER_TOO_SMALL;
  }
  if (size) std::memcpy(buf, data, size);
  if (nul) static_cast<char*>(buf)[size] = '\0';
  return SQ_OK;
}

inline void copyName(char* dst, size_t cap, const std::string& s) {
  if (!cap) return;
  size_t n = std::min(cap - 1, s.size());
  // do not cut a UTF-8 sequence in half
  while (n > 0 && n < s.size() && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

inline std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
inline std::string upper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return s;
}
inline bool ieq(const char* a, const char* b) {
  if (!a || !b) return false;
  for (; *a && *b; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return *a == *b;
}
// Text from a fixed-size, possibly NUL padded field; trailing NULs/spaces dropped.
inline std::string fieldText(const uint8_t* p, size_t n) {
  size_t e = 0;
  while (e < n && p[e]) ++e;
  while (e > 0 && (p[e - 1] == ' ')) --e;
  return std::string(reinterpret_cast<const char*>(p), e);
}
// Latin-1 -> UTF-8 when the bytes are not valid UTF-8 already.
inline std::string toUtf8Lenient(const std::string& s) {
  if (sq_utf8_validate(s.data(), s.size())) return s;
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x80) o += static_cast<char>(c);
    else { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
  }
  return o;
}

// ---- FileStream ----------------------------------------------------------------------------------
class FileStream final : public Object<FileStream, IByteStream> {
 public:
  static sq_result open(const char* path, bool write, Ptr<IByteStream>& out) {
    if (!path || !*path) return SQ_E_INVALIDARG;
    sq_file* f = nullptr;
    uint32_t flags = write ? (SQ_FILE_READ | SQ_FILE_WRITE | SQ_FILE_CREATE | SQ_FILE_TRUNCATE) : SQ_FILE_READ;
    sq_result r = sq_file_open(path, flags, &f);
    if (SQ_FAILED(r)) return r;
    auto* s = new (std::nothrow) FileStream(f, write);
    if (!s) { sq_file_close(f); return SQ_E_OUTOFMEMORY; }
    out = Ptr<IByteStream>::adopt(s);
    return SQ_OK;
  }
  sq_result SQ_CALL read(void* buf, uint32_t size, uint32_t* got) override {
    size_t g = 0;
    sq_result r = size ? sq_file_read(f_, buf, size, &g) : SQ_OK;
    if (got) *got = static_cast<uint32_t>(g);
    return SQ_FAILED(r) ? r : SQ_OK;
  }
  sq_result SQ_CALL write(const void* data, uint32_t size, uint32_t* written) override {
    if (written) *written = 0;
    if (!writable_) return SQ_E_ACCESS;
    sq_result r = size ? sq_file_write(f_, data, size) : SQ_OK;
    if (SQ_SUCCEEDED(r) && written) *written = size;
    return r;
  }
  sq_result SQ_CALL seek(int64_t offset, int32_t origin, int64_t* newPos) override {
    uint64_t np = 0;
    sq_result r = sq_file_seek(f_, offset, origin, &np);
    if (SQ_SUCCEEDED(r) && newPos) *newPos = static_cast<int64_t>(np);
    return r;
  }
  sq_result SQ_CALL tell(int64_t* pos) override { return seek(0, SQ_SEEK_CUR, pos); }
  sq_result SQ_CALL getSize(int64_t* size) override {
    uint64_t s = 0;
    sq_result r = sq_file_size(f_, &s);
    if (SQ_SUCCEEDED(r) && size) *size = static_cast<int64_t>(s);
    return r;
  }
  sq_result SQ_CALL flush() override { return sq_file_flush(f_); }
  void close() {
    if (f_) { sq_file_close(f_); f_ = nullptr; }
  }

 private:
  FileStream(sq_file* f, bool w) : f_(f), writable_(w) {}

 public:
  ~FileStream() override { close(); }

 private:
  sq_file* f_;
  bool writable_;
};

// ---- stream helpers -------------------------------------------------------------------------------
inline bool readExact(IByteStream* s, void* buf, uint32_t n) {
  uint32_t got = 0;
  return n == 0 || (SQ_SUCCEEDED(s->read(buf, n, &got)) && got == n);
}
inline bool readAt(IByteStream* s, int64_t pos, void* buf, uint32_t n) {
  return SQ_SUCCEEDED(s->seek(pos, SQ_SEEK_SET, nullptr)) && readExact(s, buf, n);
}
inline bool writeAll(IByteStream* s, const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (n) {
    uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(n, 1u << 30));
    uint32_t w = 0;
    if (SQ_FAILED(s->write(p, chunk, &w)) || w != chunk) return false;
    p += chunk;
    n -= chunk;
  }
  return true;
}
inline bool writeAt(IByteStream* s, int64_t pos, const void* data, size_t n) {
  return SQ_SUCCEEDED(s->seek(pos, SQ_SEEK_SET, nullptr)) && writeAll(s, data, n);
}
inline int64_t streamSize(IByteStream* s) {
  int64_t n = -1;
  if (SQ_FAILED(s->getSize(&n))) return -1;
  return n;
}
inline int64_t streamTell(IByteStream* s) {
  int64_t p = -1;
  if (SQ_FAILED(s->tell(&p))) return -1;
  return p;
}
// Whole stream into memory (max `cap` bytes). Used by codecs whose decoder wants a memory image.
inline bool readAll(IByteStream* s, std::vector<uint8_t>& out, uint64_t cap = (1ull << 32)) {
  out.clear();
  if (SQ_FAILED(s->seek(0, SQ_SEEK_SET, nullptr))) return false;
  int64_t sz = streamSize(s);
  if (sz >= 0 && static_cast<uint64_t>(sz) > cap) return false;
  if (sz > 0) out.reserve(static_cast<size_t>(sz));
  uint8_t tmp[65536];
  for (;;) {
    uint32_t got = 0;
    if (SQ_FAILED(s->read(tmp, sizeof tmp, &got))) return false;
    out.insert(out.end(), tmp, tmp + got);
    if (out.size() > cap) return false;
    if (got < sizeof tmp) return true;
  }
}
// Moves bytes [from, end) of a seekable read/write stream forward by `delta` bytes (opens a gap
// at `from`, used to grow a header after audio was written). Copies back to front.
inline bool shiftTail(IByteStream* s, int64_t from, int64_t delta) {
  int64_t end = streamSize(s);
  if (end < from || delta < 0) return false;
  std::vector<uint8_t> buf(1 << 20);
  int64_t pos = end;
  while (pos > from) {
    uint32_t n = static_cast<uint32_t>(std::min<int64_t>(pos - from, static_cast<int64_t>(buf.size())));
    pos -= n;
    if (!readAt(s, pos, buf.data(), n) || !writeAt(s, pos + delta, buf.data(), n)) return false;
  }
  return true;
}

// ---- metadata store --------------------------------------------------------------------------------
struct MetaEntry {
  std::string key;
  std::vector<uint8_t> value;
};
class MetaStore {
 public:
  // size 0 (or data null) removes the key.
  void set(const std::string& key, const void* data, size_t size) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (it->key == key) {
        if (!data || !size) { items_.erase(it); return; }
        it->value.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        return;
      }
    }
    if (!data || !size) return;
    items_.push_back(MetaEntry{key, std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size)});
  }
  void setText(const std::string& key, const std::string& text) { set(key, text.data(), text.size()); }
  // Adds a chunk key; a repeated id gets "chunk:<id>#2", "#3", ... (all are preserved).
  void addChunk(const char id[4], const uint8_t* data, size_t size) {
    std::string base = std::string("chunk:") + std::string(id, 4);
    std::string key = base;
    for (int n = 2; find(key); ++n) key = base + "#" + std::to_string(n);
    items_.push_back(MetaEntry{key, std::vector<uint8_t>(data, data + size)});
  }
  const MetaEntry* find(const std::string& key) const {
    for (const auto& e : items_)
      if (e.key == key) return &e;
    return nullptr;
  }
  std::string text(const std::string& key) const {
    const MetaEntry* e = find(key);
    return e ? std::string(e->value.begin(), e->value.end()) : std::string();
  }
  bool has(const std::string& key) const { return find(key) != nullptr; }
  const std::vector<MetaEntry>& items() const { return items_; }
  std::vector<MetaEntry>& items() { return items_; }
  void clear() { items_.clear(); }

  uint32_t count() const { return static_cast<uint32_t>(items_.size()); }
  sq_result keyAt(uint32_t i, char* buf, uint32_t bufSize, uint32_t* needed) const {
    if (i >= items_.size()) return SQ_E_NOTFOUND;
    return copyOut(items_[i].key.data(), items_[i].key.size(), true, buf, bufSize, needed);
  }
  sq_result get(const char* key, void* buf, uint32_t bufSize, uint32_t* needed) const {
    if (!key) return SQ_E_INVALIDARG;
    const MetaEntry* e = find(key);
    if (!e) return SQ_E_NOTFOUND;
    return copyOut(e->value.data(), e->value.size(), false, buf, bufSize, needed);
  }

 private:
  std::vector<MetaEntry> items_;
};

// Parses "chunk:<id>" / "chunk:<id>#n" keys. Returns false for other keys or a bad id.
inline bool chunkKeyId(const std::string& key, char id[4]) {
  if (key.size() < 10 || key.compare(0, 6, "chunk:") != 0) return false;
  if (key.size() > 10 && key[10] != '#') return false;
  for (int i = 0; i < 4; ++i) {
    char c = key[6 + i];
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
    id[i] = c;
  }
  return true;
}
inline bool isTagKey(const std::string& key) { return key.size() > 4 && key.compare(0, 4, "tag:") == 0; }

// Normalized tag names from codec.h.
inline constexpr const char* kTagNames[] = {"title", "artist", "album", "genre", "date", "comment", "track",
                                            "copyright", "encoder", "bpm", "key", "isrc", "originator",
                                            "origination_date"};

// Vorbis comment field <-> normalized tag (FLAC, Ogg Vorbis, Opus). Unknown fields map to
// tag:<lowercase field name> so they round-trip.
inline std::string vorbisFieldToTag(const std::string& field) {
  std::string f = upper(field);
  if (f == "TITLE") return "title";
  if (f == "ARTIST") return "artist";
  if (f == "ALBUM") return "album";
  if (f == "GENRE") return "genre";
  if (f == "DATE") return "date";
  if (f == "COMMENT" || f == "DESCRIPTION") return "comment";
  if (f == "TRACKNUMBER") return "track";
  if (f == "COPYRIGHT") return "copyright";
  if (f == "ENCODER" || f == "ENCODED-BY") return "encoder";
  if (f == "BPM") return "bpm";
  if (f == "KEY" || f == "INITIALKEY") return "key";
  if (f == "ISRC") return "isrc";
  return lower(field);
}
inline std::string tagToVorbisField(const std::string& tag) {
  if (tag == "comment") return "COMMENT";
  if (tag == "track") return "TRACKNUMBER";
  if (tag == "key") return "INITIALKEY";
  return upper(tag);
}
// Adds one "FIELD=value" comment to the store (first occurrence wins; repeats append "; ").
inline void addVorbisComment(MetaStore& m, const char* s, size_t n) {
  std::string c(s, n);
  size_t eq = c.find('=');
  if (eq == std::string::npos || eq == 0) return;
  std::string key = "tag:" + vorbisFieldToTag(c.substr(0, eq));
  std::string val = toUtf8Lenient(c.substr(eq + 1));
  if (const MetaEntry* e = m.find(key)) {
    std::string prev(e->value.begin(), e->value.end());
    m.setText(key, prev + "; " + val);
  } else {
    m.setText(key, val);
  }
}

// ---- reader base -------------------------------------------------------------------------------------
template <class D>
class ReaderBase : public Object<D, IAudioFileReader> {
 public:
  uint32_t SQ_CALL getMetadataCount() override { return meta_.count(); }
  sq_result SQ_CALL getMetadataKey(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override {
    return meta_.keyAt(index, buf, bufSize, needed);
  }
  sq_result SQ_CALL getMetadata(const char* key, void* buf, uint32_t bufSize, uint32_t* needed) override {
    return meta_.get(key, buf, bufSize, needed);
  }
  uint32_t SQ_CALL getMarkerCount() override { return static_cast<uint32_t>(markers_.size()); }
  sq_result SQ_CALL getMarker(uint32_t index, sq_audio_file_marker* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_file_marker)) return SQ_E_INVALIDARG;
    if (index >= markers_.size()) return SQ_E_NOTFOUND;
    *out = markers_[index];
    return SQ_OK;
  }

 protected:
  MetaStore meta_;
  std::vector<sq_audio_file_marker> markers_;
};

inline sq_audio_file_marker makeMarker(uint32_t id, sq_sample_time pos, sq_sample_time len, uint32_t flags, const std::string& name) {
  sq_audio_file_marker m{};
  m.struct_size = sizeof m;
  m.id = id;
  m.position = pos;
  m.length = len;
  m.flags = flags | (len > 0 ? SQ_MARKER_REGION : 0u);
  copyName(m.name, sizeof m.name, name);
  return m;
}

// ---- writer destination --------------------------------------------------------------------------
// Owns the output stream. A path-opened target deletes its file if finalize never completed.
class WriterTarget {
 public:
  ~WriterTarget() { abort(); }
  sq_result openPath(const char* path) {
    if (stream_) return SQ_E_EXISTS;
    sq_result r = FileStream::open(path, true, stream_);
    if (SQ_SUCCEEDED(r)) path_ = path;
    return r;
  }
  sq_result openStream(IByteStream* s) {
    if (stream_) return SQ_E_EXISTS;
    if (!s) return SQ_E_INVALIDARG;
    s->addRef();
    stream_ = Ptr<IByteStream>::adopt(s);
    return SQ_OK;
  }
  IByteStream* get() const { return stream_.get(); }
  explicit operator bool() const { return static_cast<bool>(stream_); }
  // Success: flush + close, keep the file.
  sq_result commit() {
    if (!stream_) return SQ_E_CLOSED;
    sq_result r = stream_->flush();
    stream_ = nullptr;
    path_.clear();
    return r;
  }
  void abort() {
    if (!stream_) return;
    stream_ = nullptr;  // closes a FileStream (only owner)
    if (!path_.empty()) sq_fs_remove(path_.c_str());
    path_.clear();
  }

 private:
  Ptr<IByteStream> stream_;
  std::string path_;
};

// ---- sample conversion ---------------------------------------------------------------------------
inline int32_t floatToInt(float x, uint32_t bits) {
  const double scale = static_cast<double>(1u << (bits - 1));
  double v = std::nearbyint(static_cast<double>(x) * scale);
  const double hi = scale - 1.0, lo = -scale;
  if (!(v <= hi)) v = (v != v) ? 0.0 : hi;  // also maps NaN to 0
  if (v < lo) v = lo;
  return static_cast<int32_t>(v);
}
// TPDF dither generator (+-1 LSB triangular), deterministic xorshift.
class Dither {
 public:
  explicit Dither(bool on = false) : on_(on) {}
  float apply(float x, uint32_t bits) {
    if (!on_ || bits >= 24) return x;
    const float lsb = 1.0f / static_cast<float>(1u << (bits - 1));
    return x + (uniform() - uniform()) * lsb;
  }
  bool enabled() const { return on_; }

 private:
  float uniform() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return static_cast<float>(state_ & 0xFFFFFF) / 16777216.0f;
  }
  bool on_;
  uint32_t state_ = 0x9E3779B9u;
};

// Validates an sq_audio_block against a channel count.
inline bool blockOk(const sq_audio_block* b, uint32_t channels) {
  if (!b || b->channel_count != channels || (!b->channels && b->frames)) return false;
  if (b->frames)
    for (uint32_t c = 0; c < channels; ++c)
      if (!b->channels[c]) return false;
  return true;
}

inline bool extIs(const char* ext, const char* const* list, size_t n) {
  if (!ext) return false;
  if (*ext == '.') ++ext;
  for (size_t i = 0; i < n; ++i)
    if (ieq(ext, list[i])) return true;
  return false;
}

// Formats table helper for ICodecService implementations.
struct FormatDesc {
  const char* ext;
  const char* mime;
  const char* desc;
  uint32_t flags;
};
inline void fillFormat(const FormatDesc& d, uint32_t flagsMask, sq_codec_format_info* out) {
  std::snprintf(out->extension, sizeof out->extension, "%s", d.ext);
  std::snprintf(out->mime, sizeof out->mime, "%s", d.mime);
  std::snprintf(out->description, sizeof out->description, "%s", d.desc);
  out->flags = d.flags & flagsMask;
}
// supports(): extension and/or mime must match a format whose flags contain `flags`.
inline bool formatSupports(const FormatDesc* f, size_t n, uint32_t mask, const char* ext, const char* mime, uint32_t flags) {
  if (!ext && !mime) return false;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t have = f[i].flags & mask;
    if ((have & flags) != (flags & (SQ_CODEC_READ | SQ_CODEC_WRITE | SQ_CODEC_METADATA | SQ_CODEC_MARKERS | SQ_CODEC_LOSSY))) continue;
    const char* e = ext && *ext == '.' ? ext + 1 : ext;
    if (e && !ieq(e, f[i].ext)) continue;
    if (mime && !ieq(mime, f[i].mime)) continue;
    return true;
  }
  return false;
}

// Reads a string setting from the host (module context), "" when absent.
inline std::string hostSetting(const char* key) {
  IHostContext* h = nullptr;
  h = sq::host();
  if (!h || !h->getSettings()) return {};
  ISettings* s = h->getSettings();
  char small[512];
  uint32_t need = 0;
  if (s->getString(key, small, sizeof small, &need) == SQ_OK) return small;
  if (need > sizeof small) {
    std::string big(need, '\0');
    if (s->getString(key, big.data(), need, &need) == SQ_OK) { big.resize(need - 1); return big; }
  }
  return {};
}
inline std::string envVar(const char* name) {
  sq_buffer b{};
  if (SQ_FAILED(sq_env_get(name, &b)) || !b.data) return {};
  std::string s(static_cast<const char*>(b.data), b.size);
  sq_buffer_free(&b);
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

}  // namespace sq::cx
