// codec-ogg: Squizzy Studio Ogg codec service.
//   Vorbis decode: libvorbisfile (third_party/libvorbis, BSD-3-Clause), sample-accurate seek.
//                  (replaced stb_vorbis, whose output for 5.1 streams was wrong in this reader)
//   Opus decode:   seam. libopusfile (BSD) is loaded at runtime when present:
//                  setting "Services.codec-ogg.OpusfileLibrary" -> env SQ_OPUSFILE_LIBRARY ->
//                  default names. Without it, Opus files report SQ_E_UNSUPPORTED.
//   Vorbis encode: libvorbis + libvorbisenc + libogg (third_party/libvorbis, libogg; BSD-3-Clause).
//   Opus encode:   not provided (libopus/libopusenc are not vendored): createWriter("opus") and
//                  supports("opus", ..., SQ_CODEC_WRITE) report unsupported.
// The reader keeps the compressed stream in memory (decoders read from a memory image).
#include <memory>
#include <mutex>

#include "sqcodec/common.hpp"
#include "vorbis/codec.h"
#include "vorbis/vorbisenc.h"
#define OV_EXCLUDE_STATIC_CALLBACKS
#include "vorbis/vorbisfile.h"

namespace sq::ogg {
namespace {

using namespace sq::cx;

constexpr uint64_t kMaxFileBytes = 1ull << 31;  // memory image cap (2 GB)

// ---- opusfile runtime binding --------------------------------------------------------------------
struct OpusTagsView {  // layout of libopusfile's OpusTags
  char** user_comments;
  int* comment_lengths;
  int comments;
  char* vendor;
};
struct Opusfile {
  using Open = void* (*)(const unsigned char*, size_t, int*);
  using Free = void (*)(void*);
  using Count = int (*)(const void*, int);
  using Total = int64_t (*)(const void*, int);
  using Read = int (*)(void*, float*, int, int*);
  using Seek = int (*)(void*, int64_t);
  using Tags = const OpusTagsView* (*)(const void*, int);
  sq_dylib* lib = nullptr;
  Open open = nullptr;
  Free free = nullptr;
  Count channels = nullptr;
  Total total = nullptr;
  Read read = nullptr;
  Seek seek = nullptr;
  Tags tags = nullptr;
  bool tried = false;
  bool ok() const { return lib != nullptr; }
  void unload() {
    if (lib) sq_dylib_close(lib);
    *this = Opusfile{};
  }
};
std::mutex& opusMutex() {
  static std::mutex m;
  return m;
}
Opusfile& opusSlot() {
  static Opusfile o;
  return o;
}
Opusfile* opusfile() {
  std::lock_guard<std::mutex> g(opusMutex());
  Opusfile& o = opusSlot();
  if (o.ok()) return &o;
  if (o.tried) return nullptr;
  o.tried = true;
  std::vector<std::string> names;
  std::string s = hostSetting("Services.codec-ogg.OpusfileLibrary");
  if (!s.empty()) names.push_back(s);
  s = envVar("SQ_OPUSFILE_LIBRARY");
  if (!s.empty()) names.push_back(s);
#if defined(_WIN32)
  names.insert(names.end(), {"libopusfile-0.dll", "opusfile.dll", "libopusfile.dll"});
#elif defined(__APPLE__)
  names.insert(names.end(), {"libopusfile.0.dylib", "libopusfile.dylib"});
#else
  names.insert(names.end(), {"libopusfile.so.0", "libopusfile.so"});
#endif
  for (const auto& n : names) {
    sq_dylib* d = nullptr;
    char err[256] = {};
    if (SQ_FAILED(sq_dylib_open(n.c_str(), &d, err, sizeof err)) || !d) continue;
    Opusfile t;
    t.open = reinterpret_cast<Opusfile::Open>(sq_dylib_symbol(d, "op_open_memory"));
    t.free = reinterpret_cast<Opusfile::Free>(sq_dylib_symbol(d, "op_free"));
    t.channels = reinterpret_cast<Opusfile::Count>(sq_dylib_symbol(d, "op_channel_count"));
    t.total = reinterpret_cast<Opusfile::Total>(sq_dylib_symbol(d, "op_pcm_total"));
    t.read = reinterpret_cast<Opusfile::Read>(sq_dylib_symbol(d, "op_read_float"));
    t.seek = reinterpret_cast<Opusfile::Seek>(sq_dylib_symbol(d, "op_pcm_seek"));
    t.tags = reinterpret_cast<Opusfile::Tags>(sq_dylib_symbol(d, "op_tags"));
    if (t.open && t.free && t.channels && t.total && t.read && t.seek && t.tags) {
      t.lib = d;
      t.tried = true;
      o = t;
      sq::logf(SQ_LOG_INFO, "Opus decoding enabled via %s", n.c_str());
      return &o;
    }
    sq_dylib_close(d);
  }
  return nullptr;
}
sq_result onInit(IHostContext*) {
  std::lock_guard<std::mutex> g(opusMutex());
  opusSlot().tried = false;
  return SQ_OK;
}
void onShutdown() {
  std::lock_guard<std::mutex> g(opusMutex());
  opusSlot().unload();
}

enum class Kind { None, Vorbis, Opus };

// Ogg page CRC-32 (polynomial 0x04C11DB7, MSB first, init 0).
uint32_t oggCrc(const uint8_t* p, size_t n) {
  static const auto table = [] {
    std::vector<uint32_t> t(256);
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i << 24;
      for (int b = 0; b < 8; ++b) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
      t[i] = r;
    }
    return t;
  }();
  uint32_t crc = 0;
  for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ table[((crc >> 24) ^ p[i]) & 0xFF];
  return crc;
}

// Walks the pages and cuts the image at the first page that is incomplete, malformed, fails its
// CRC or belongs to another logical stream, so corrupted data (truncation, bit rot) never reaches
// the decoder; decoding then simply ends where the damage starts. Chained files decode their
// first link only.
// Returns the number of valid pages.
size_t sanitizeOgg(std::vector<uint8_t>& d) {
  size_t off = 0, pages = 0;
  uint32_t firstSerial = 0;
  std::vector<uint8_t> page;
  while (off + 27 <= d.size()) {
    const uint8_t* h = d.data() + off;
    if (std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) break;
    const size_t segs = h[26];
    if (off + 27 + segs > d.size()) break;
    size_t body = 0;
    for (size_t i = 0; i < segs; ++i) body += h[27 + i];
    const size_t len = 27 + segs + body;
    if (off + len > d.size()) break;
    page.assign(h, h + len);
    const uint32_t stored = rd32le(page.data() + 22);
    std::memset(page.data() + 22, 0, 4);
    if (oggCrc(page.data(), page.size()) != stored) break;
    // One logical stream only: chained / multiplexed streams end at the first foreign serial (or a
    // second BOS). Keeps the decoder away from link switches with inconsistent setups.
    const uint32_t serial = rd32le(h + 14);
    if (pages == 0) firstSerial = serial;
    else if (serial != firstSerial || (h[5] & 0x02)) break;
    off += len;
    ++pages;
    if (h[5] & 0x04) break;  // EOS: anything after it belongs to another stream
  }
  d.resize(off);
  return pages;
}

// First `want` packets of the first logical stream (pages already sanitized), each capped.
std::vector<std::vector<uint8_t>> firstPackets(const std::vector<uint8_t>& d, size_t want, size_t cap = 16u << 20) {
  std::vector<std::vector<uint8_t>> out;
  std::vector<uint8_t> cur;
  size_t off = 0;
  uint32_t serial = 0;
  bool first = true;
  while (off + 27 <= d.size() && out.size() < want) {
    const uint8_t* h = d.data() + off;
    const size_t segs = h[26];
    size_t body = 0;
    for (size_t i = 0; i < segs; ++i) body += h[27 + i];
    const uint8_t* p = h + 27 + segs;
    if (first) { serial = rd32le(h + 14); first = false; }
    if (rd32le(h + 14) == serial) {
      for (size_t i = 0; i < segs && out.size() < want; ++i) {
        const size_t n = h[27 + i];
        if (cur.size() + n > cap) return out;
        cur.insert(cur.end(), p, p + n);
        p += n;
        if (n < 255) { out.push_back(std::move(cur)); cur.clear(); }
      }
    }
    off += 27 + segs + body;
  }
  return out;
}

// Header packets are validated here before the decoder sees them (defence in depth: bounded
// comment counts and lengths, non-zero channels and rate).
bool vorbisHeadersSane(const std::vector<uint8_t>& d) {
  auto pk = firstPackets(d, 2);
  if (pk.size() < 2) return false;
  const auto& id = pk[0];
  if (id.size() < 30 || id[0] != 1 || std::memcmp(id.data() + 1, "vorbis", 6) != 0) return false;
  if (id[11] == 0 || rd32le(id.data() + 12) == 0) return false;  // channels, rate
  const auto& c = pk[1];
  if (c.size() < 7 + 8 || c[0] != 3 || std::memcmp(c.data() + 1, "vorbis", 6) != 0) return false;
  size_t off = 7;
  uint64_t vlen = rd32le(c.data() + off);
  off += 4;
  if (vlen > c.size() - off) return false;
  off += static_cast<size_t>(vlen);
  if (c.size() - off < 4) return false;
  uint64_t count = rd32le(c.data() + off);
  off += 4;
  if (count > (c.size() - off) / 4) return false;
  for (uint64_t i = 0; i < count; ++i) {
    if (c.size() - off < 4) return false;
    uint64_t len = rd32le(c.data() + off);
    off += 4;
    if (len > c.size() - off) return false;
    off += static_cast<size_t>(len);
  }
  return true;
}

Kind sniff(const uint8_t* p, size_t n) {
  if (n < 28 || std::memcmp(p, "OggS", 4) != 0) return Kind::None;
  // first packet of the first page
  const size_t segs = p[26];
  const size_t at = 27 + segs;
  if (at + 8 > n) return Kind::None;
  if (p[at] == 0x01 && !std::memcmp(p + at + 1, "vorbis", 6)) return Kind::Vorbis;
  if (!std::memcmp(p + at, "OpusHead", 8)) return Kind::Opus;
  return Kind::None;
}

// ---- reader ---------------------------------------------------------------------------------------
class Reader final : public ReaderBase<Reader> {
 public:
  ~Reader() override { close(); }

  sq_result SQ_CALL open(const char* path) override {
    if (kind_ != Kind::None) return SQ_E_EXISTS;
    Ptr<IByteStream> s;
    sq_result r = FileStream::open(path, false, s);
    if (SQ_FAILED(r)) return r;
    return attach(s.get());
  }
  sq_result SQ_CALL openStream(IByteStream* stream) override {
    if (kind_ != Kind::None) return SQ_E_EXISTS;
    if (!stream) return SQ_E_INVALIDARG;
    return attach(stream);
  }
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
    if (kind_ == Kind::None) return SQ_E_CLOSED;
    out->sample_rate = rate_;
    out->channels = channels_;
    out->sample_format = SQ_SAMPLE_F32;
    out->bits_per_sample = 32;
    out->frame_count = frames_;
    out->bitrate_kbps = frames_ > 0 ? static_cast<uint32_t>(data_.size() * 8.0 / (frames_ / rate_) / 1000.0 + 0.5) : 0;
    out->flags = SQ_AUDIOFILE_SEEKABLE | SQ_AUDIOFILE_SAMPLE_EXACT | SQ_AUDIOFILE_VBR;
    std::snprintf(out->container, sizeof out->container, "%s", kind_ == Kind::Opus ? "opus" : "ogg");
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* framesRead) override {
    if (framesRead) *framesRead = 0;
    if (kind_ == Kind::None) return SQ_E_CLOSED;
    if (!blockOk(dst, channels_)) return SQ_E_INVALIDARG;
    uint32_t done = 0;
    if (!atEnd_) {
      if (kind_ == Kind::Vorbis) {
        int holes = 0;
        while (done < dst->frames) {
          int want = static_cast<int>(std::min<uint32_t>(dst->frames - done, 4096));
          if (frames_ >= 0) want = static_cast<int>(std::min<int64_t>(want, frames_ - (pos_ + done)));
          if (want <= 0) break;
          float** pcm = nullptr;
          int link = 0;
          long got = ov_read_float(&vf_, &pcm, want, &link);
          if (got == OV_HOLE && ++holes < 64) continue;  // corrupt/missing data: skip it
          if (got <= 0 || !pcm) break;
          const vorbis_info* li = ov_info(&vf_, link);
          const uint32_t lch = li && li->channels > 0 ? static_cast<uint32_t>(li->channels) : 0;
          got = std::min<long>(got, want);
          for (uint32_t c = 0; c < channels_; ++c) {
            float* o = dst->channels[c] + done;
            if (c < lch) std::memcpy(o, pcm[c], static_cast<size_t>(got) * sizeof(float));
            else std::memset(o, 0, static_cast<size_t>(got) * sizeof(float));
          }
          done += static_cast<uint32_t>(got);
        }
      } else {
        while (done < dst->frames) {
          int want = static_cast<int>(std::min<uint32_t>(dst->frames - done, 5760));
          tmp_.resize(static_cast<size_t>(want) * channels_);
          int li = 0;
          int got = opus_->read(opus_file_, tmp_.data(), static_cast<int>(tmp_.size()), &li);
          if (got <= 0) break;
          int ch = opus_->channels(opus_file_, li);
          got = std::min(got, want);
          for (uint32_t c = 0; c < channels_; ++c) {
            float* o = dst->channels[c] + done;
            for (int i = 0; i < got; ++i) o[i] = static_cast<int>(c) < ch ? tmp_[static_cast<size_t>(i) * ch + c] : 0.0f;
          }
          done += static_cast<uint32_t>(got);
        }
      }
    }
    pos_ += done;
    if (framesRead) *framesRead = done;
    return SQ_OK;
  }
  sq_result SQ_CALL seek(sq_sample_time frame) override {
    if (kind_ == Kind::None) return SQ_E_CLOSED;
    if (frame < 0 || (frames_ >= 0 && frame > frames_)) return SQ_E_INVALIDARG;
    atEnd_ = false;
    if (frames_ >= 0 && frame == frames_) {
      atEnd_ = true;
      pos_ = frame;
      return SQ_OK;
    }
    bool ok = kind_ == Kind::Vorbis ? ov_pcm_seek(&vf_, frame) == 0 : opus_->seek(opus_file_, frame) == 0;
    if (!ok) return SQ_E_IO;
    pos_ = frame;
    return SQ_OK;
  }
  sq_result SQ_CALL tell(sq_sample_time* frame) override {
    if (!frame) return SQ_E_INVALIDARG;
    if (kind_ == Kind::None) return SQ_E_CLOSED;
    *frame = pos_;
    return SQ_OK;
  }

 private:
  sq_result attach(IByteStream* s) {
    try {
      if (!readAll(s, data_, kMaxFileBytes)) return SQ_E_IO;
      if (sanitizeOgg(data_) < 2) { data_.clear(); return SQ_E_PARSE; }  // no usable header pages
      Kind k = sniff(data_.data(), data_.size());
      if (k == Kind::Vorbis) return openVorbis();
      if (k == Kind::Opus) return openOpus();
      data_.clear();
      return SQ_E_PARSE;
    } catch (const std::bad_alloc&) {
      data_.clear();
      return SQ_E_OUTOFMEMORY;
    }
  }
  // vorbisfile callbacks over the memory image.
  static size_t memRead(void* ptr, size_t size, size_t n, void* self) {
    auto* r = static_cast<Reader*>(self);
    const size_t want = size * n, left = r->data_.size() - r->memPos_;
    const size_t got = std::min(want, left);
    if (got) std::memcpy(ptr, r->data_.data() + r->memPos_, got);
    r->memPos_ += got;
    return size ? got / size : 0;
  }
  static int memSeek(void* self, ogg_int64_t off, int whence) {
    auto* r = static_cast<Reader*>(self);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<int64_t>(r->memPos_) : static_cast<int64_t>(r->data_.size());
    int64_t to = base + off;
    if (to < 0 || to > static_cast<int64_t>(r->data_.size())) return -1;
    r->memPos_ = static_cast<size_t>(to);
    return 0;
  }
  static long memTell(void* self) { return static_cast<long>(static_cast<Reader*>(self)->memPos_); }

  sq_result openVorbis() {
    if (!vorbisHeadersSane(data_)) { data_.clear(); return SQ_E_PARSE; }
    if (data_.size() > 0x7FFFFFFF) return SQ_E_UNSUPPORTED;  // vorbisfile tells positions as long
    ov_callbacks cb{&Reader::memRead, &Reader::memSeek, nullptr, &Reader::memTell};
    memPos_ = 0;
    if (ov_open_callbacks(this, &vf_, nullptr, 0, cb) != 0) { data_.clear(); return SQ_E_PARSE; }
    vfOpen_ = true;
    const vorbis_info* info = ov_info(&vf_, 0);
    if (!info || info->channels < 1 || info->channels > 255 || info->rate <= 0) { close(); return SQ_E_PARSE; }
    channels_ = static_cast<uint32_t>(info->channels);
    rate_ = static_cast<double>(info->rate);
    const ogg_int64_t len = ov_pcm_total(&vf_, -1);
    frames_ = len >= 0 ? static_cast<sq_sample_time>(len) : -1;
    if (const vorbis_comment* c = ov_comment(&vf_, 0)) {
      for (int i = 0; i < c->comments && i < 10000; ++i)
        if (c->user_comments[i] && c->comment_lengths[i] >= 0)
          addVorbisComment(meta_, c->user_comments[i], static_cast<size_t>(c->comment_lengths[i]));
    }
    kind_ = Kind::Vorbis;
    return SQ_OK;
  }
  sq_result openOpus() {
    opus_ = opusfile();
    if (!opus_) { data_.clear(); return SQ_E_UNSUPPORTED; }  // seam inactive
    int err = 0;
    opus_file_ = opus_->open(data_.data(), data_.size(), &err);
    if (!opus_file_) { data_.clear(); return SQ_E_PARSE; }
    int ch = opus_->channels(opus_file_, -1);
    if (ch < 1 || ch > 8) { close(); return SQ_E_PARSE; }
    channels_ = static_cast<uint32_t>(ch);
    rate_ = 48000.0;  // Opus always decodes at 48 kHz
    int64_t total = opus_->total(opus_file_, -1);
    frames_ = total >= 0 ? total : -1;
    if (const OpusTagsView* t = opus_->tags(opus_file_, -1)) {
      for (int i = 0; i < t->comments && i < 10000; ++i)
        if (t->user_comments[i]) addVorbisComment(meta_, t->user_comments[i], static_cast<size_t>(std::max(0, t->comment_lengths[i])));
    }
    kind_ = Kind::Opus;
    return SQ_OK;
  }
  void close() {
    if (vfOpen_) ov_clear(&vf_);
    vfOpen_ = false;
    if (opus_file_ && opus_) opus_->free(opus_file_);
    opus_file_ = nullptr;
    kind_ = Kind::None;
    data_.clear();
  }

  std::vector<uint8_t> data_;
  Kind kind_ = Kind::None;
  OggVorbis_File vf_{};
  bool vfOpen_ = false;
  size_t memPos_ = 0;
  Opusfile* opus_ = nullptr;
  void* opus_file_ = nullptr;
  uint32_t channels_ = 0;
  double rate_ = 0;
  sq_sample_time frames_ = -1, pos_ = 0;
  bool atEnd_ = false;
  std::vector<float> tmp_;
};

// ---- writer ---------------------------------------------------------------------------------------
// Ogg Vorbis encoder over libvorbis/libvorbisenc + libogg (BSD, vendored).
//   options: "quality" double 0..1 (default 0.5; maps to Vorbis VBR quality -0.1..1.0),
//            "bitrate_kbps" int (> 0 selects managed average bitrate instead of VBR quality).
//   metadata: "tag:<name>" -> Vorbis comments (any time before finalize). "chunk:*" and markers
//            are SQ_E_UNSUPPORTED (Ogg has no chunks; no standard marker form).
// Deterministic: fixed stream serial, so identical input and options give identical bytes.
// Encoded audio pages are held in memory (compressed size) until finalize so tags set after the
// first write still land in the comment header; above kSpillBytes the headers are written and the
// rest streams out, after which new tags are refused (SQ_E_UNSUPPORTED).
constexpr uint32_t kStreamSerial = 0x53517A31u;  // fixed: deterministic output
constexpr size_t kSpillBytes = 32u << 20;
constexpr uint32_t kMaxTagBytes = 1u << 20;

// Rewrites the page sequence numbers of a run of complete pages by `delta` and fixes their CRCs.
void renumberPages(std::vector<uint8_t>& d, int64_t delta) {
  if (delta == 0) return;
  size_t off = 0;
  while (off + 27 <= d.size()) {
    uint8_t* h = d.data() + off;
    const size_t segs = h[26];
    size_t body = 0;
    for (size_t i = 0; i < segs; ++i) body += h[27 + i];
    const size_t len = 27 + segs + body;
    if (off + len > d.size()) break;
    wr32le(h + 18, static_cast<uint32_t>(static_cast<int64_t>(rd32le(h + 18)) + delta));
    std::memset(h + 22, 0, 4);
    wr32le(h + 22, oggCrc(h, len));
    off += len;
  }
}

class Writer final : public Object<Writer, IAudioFileWriter> {
 public:
  ~Writer() override { teardown(); }

  sq_result SQ_CALL open(const char* path, const sq_audio_file_format* format, IAttributes* options) override {
    if (configured_) return SQ_E_EXISTS;
    sq_result r = setup(format, options);
    if (SQ_FAILED(r)) return r;
    r = out_.openPath(path);
    if (SQ_FAILED(r)) teardown();
    return r;
  }
  sq_result SQ_CALL openStream(IByteStream* stream, const sq_audio_file_format* format, IAttributes* options) override {
    if (configured_) return SQ_E_EXISTS;
    sq_result r = setup(format, options);
    if (SQ_FAILED(r)) return r;
    r = out_.openStream(stream);
    if (SQ_FAILED(r)) teardown();
    return r;
  }
  sq_result SQ_CALL setMetadata(const char* key, const void* data, uint32_t size) override {
    if (!key) return SQ_E_INVALIDARG;
    if (finalized_) return SQ_E_CLOSED;
    std::string k(key);
    if (!isTagKey(k)) return SQ_E_UNSUPPORTED;  // Ogg has no chunks
    if (k.size() < 5 || k.find('=') != std::string::npos) return SQ_E_INVALIDARG;
    for (char c : k.substr(4))
      if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7D) return SQ_E_INVALIDARG;
    if (data && size) {
      if (size > kMaxTagBytes || std::memchr(data, 0, size)) return SQ_E_INVALIDARG;
      if (!sq_utf8_validate(static_cast<const char*>(data), size)) return SQ_E_INVALIDARG;
    }
    if (headersOut_) return SQ_E_UNSUPPORTED;  // spilled: the comment header is already on disk
    try {
      meta_.set(k, data, data ? size : 0);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL addMarker(const sq_audio_file_marker* marker) override {
    if (!marker || marker->struct_size < sizeof(sq_audio_file_marker)) return SQ_E_INVALIDARG;
    return SQ_E_UNSUPPORTED;
  }
  sq_result SQ_CALL write(const sq_audio_block* src) override {
    if (!configured_ || !out_ || finalized_ || failed_) return SQ_E_CLOSED;
    if (!blockOk(src, channels_)) return SQ_E_INVALIDARG;
    try {
      uint32_t done = 0;
      while (done < src->frames) {
        const uint32_t n = std::min<uint32_t>(src->frames - done, 4096);
        float** buf = vorbis_analysis_buffer(&vd_, static_cast<int>(n));
        if (!buf) { failed_ = true; return SQ_E_OUTOFMEMORY; }
        for (uint32_t c = 0; c < channels_; ++c) {
          const float* in = src->channels[c] + done;
          float* o = buf[c];
          for (uint32_t i = 0; i < n; ++i) {
            float x = in[i];
            o[i] = (x == x) ? std::clamp(x, -8.0f, 8.0f) : 0.0f;  // NaN -> 0, keep the psychoacoustics sane
          }
        }
        if (vorbis_analysis_wrote(&vd_, static_cast<int>(n)) != 0) { failed_ = true; return SQ_E_FAIL; }
        sq_result r = drain(false);
        if (SQ_FAILED(r)) { failed_ = true; return r; }
        done += n;
      }
    } catch (const std::bad_alloc&) {
      failed_ = true;
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL finalize() override {
    if (finalized_) return SQ_S_FALSE;
    if (!configured_ || !out_) return SQ_E_CLOSED;
    if (failed_) return SQ_E_IO;
    try {
      if (vorbis_analysis_wrote(&vd_, 0) != 0) { failed_ = true; return SQ_E_FAIL; }
      sq_result r = drain(true);
      if (SQ_SUCCEEDED(r) && !headersOut_) r = spill();
      if (SQ_SUCCEEDED(r) && !pending_.empty()) r = flushPending();
      if (SQ_FAILED(r)) { failed_ = true; return r; }
      finalized_ = true;
      return out_.commit();
    } catch (const std::bad_alloc&) {
      failed_ = true;
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      failed_ = true;
      return SQ_E_FAIL;
    }
  }

 private:
  sq_result setup(const sq_audio_file_format* f, IAttributes* options) {
    if (!f || f->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
    if (f->channels < 1 || f->channels > 8) return SQ_E_UNSUPPORTED;
    if (!(f->sample_rate >= 1000) || f->sample_rate > 384000 || f->sample_rate != std::floor(f->sample_rate)) return SQ_E_UNSUPPORTED;
    channels_ = f->channels;
    const long rate = static_cast<long>(f->sample_rate);
    double q = 0.5;
    int64_t kbps = 0;
    if (options) {
      q = options->getDouble("quality", 0.5);
      kbps = options->getInt("bitrate_kbps", 0);
      // Private (tests): page memory threshold before the headers are committed.
      int64_t spill = options->getInt("ogg.spill_bytes", 0);
      if (spill > 0) spillBytes_ = static_cast<size_t>(spill);
    }
    if (!(q >= 0.0)) q = 0.0;
    if (q > 1.0) q = 1.0;
    vorbis_info_init(&vi_);
    viInit_ = true;
    int rc = 0;
    if (kbps > 0) {
      if (kbps > 2000) {
        teardown();
        return SQ_E_UNSUPPORTED;
      }
      rc = vorbis_encode_init(&vi_, static_cast<long>(channels_), rate, -1, static_cast<long>(kbps * 1000), -1);
    } else {
      rc = vorbis_encode_init_vbr(&vi_, static_cast<long>(channels_), rate, static_cast<float>(-0.1 + 1.1 * q));
    }
    if (rc != 0) {
      teardown();
      return SQ_E_UNSUPPORTED;  // rate/channel/quality combination the encoder has no mode for
    }
    if (vorbis_analysis_init(&vd_, &vi_) != 0) { teardown(); return SQ_E_FAIL; }
    vdInit_ = true;
    if (vorbis_block_init(&vd_, &vb_) != 0) { teardown(); return SQ_E_FAIL; }
    vbInit_ = true;
    if (ogg_stream_init(&os_, static_cast<int>(kStreamSerial)) != 0) { teardown(); return SQ_E_FAIL; }
    osInit_ = true;
    // Push placeholder headers through the audio stream so its packet numbers / page sequence
    // start where real audio starts; the real headers are built at spill time and the audio
    // pages renumbered if the header page count differs.
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    ogg_packet h0, h1, h2;
    rc = vorbis_analysis_headerout(&vd_, &vc, &h0, &h1, &h2);
    if (rc == 0) {
      ogg_stream_packetin(&os_, &h0);
      ogg_stream_packetin(&os_, &h1);
      ogg_stream_packetin(&os_, &h2);
      ogg_page pg;
      while (ogg_stream_flush(&os_, &pg) != 0) ++placeholderPages_;
    }
    vorbis_comment_clear(&vc);
    if (rc != 0) { teardown(); return SQ_E_FAIL; }
    configured_ = true;
    return SQ_OK;
  }

  // Runs the encoder on everything submitted; pages go to pending_ (or straight out once spilled).
  sq_result drain(bool eos) {
    ogg_packet op;
    ogg_page pg;
    while (vorbis_analysis_blockout(&vd_, &vb_) == 1) {
      if (vorbis_analysis(&vb_, nullptr) != 0) return SQ_E_FAIL;
      if (vorbis_bitrate_addblock(&vb_) != 0) return SQ_E_FAIL;
      while (vorbis_bitrate_flushpacket(&vd_, &op) == 1) {
        if (ogg_stream_packetin(&os_, &op) != 0) return SQ_E_FAIL;
        while (ogg_stream_pageout(&os_, &pg) != 0) appendPage(pg);
      }
    }
    if (eos)
      while (ogg_stream_flush(&os_, &pg) != 0) appendPage(pg);
    if (!headersOut_ && pending_.size() > spillBytes_) return spill();
    if (headersOut_ && !pending_.empty()) return flushPending();
    return SQ_OK;
  }
  void appendPage(const ogg_page& pg) {
    pending_.insert(pending_.end(), pg.header, pg.header + pg.header_len);
    pending_.insert(pending_.end(), pg.body, pg.body + pg.body_len);
  }
  // Writes the real header pages (with the current tags), then everything pending.
  sq_result spill() {
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    for (const auto& e : meta_.items()) {
      if (!isTagKey(e.key)) continue;
      std::string field = tagToVorbisField(e.key.substr(4));
      std::string value(e.value.begin(), e.value.end());
      vorbis_comment_add_tag(&vc, field.c_str(), value.c_str());
    }
    ogg_packet h0, h1, h2;
    int rc = vorbis_analysis_headerout(&vd_, &vc, &h0, &h1, &h2);
    std::vector<uint8_t> head;
    int64_t pages = 0;
    if (rc == 0) {
      ogg_stream_state hs;
      ogg_stream_init(&hs, static_cast<int>(kStreamSerial));
      ogg_stream_packetin(&hs, &h0);
      ogg_stream_packetin(&hs, &h1);
      ogg_stream_packetin(&hs, &h2);
      ogg_page pg;
      while (ogg_stream_flush(&hs, &pg) != 0) {
        head.insert(head.end(), pg.header, pg.header + pg.header_len);
        head.insert(head.end(), pg.body, pg.body + pg.body_len);
        ++pages;
      }
      ogg_stream_clear(&hs);
    }
    vorbis_comment_clear(&vc);
    if (rc != 0) return SQ_E_FAIL;
    seqDelta_ = pages - placeholderPages_;
    if (!sq::cx::writeAll(out_.get(), head.data(), head.size())) return SQ_E_IO;
    headersOut_ = true;
    return flushPending();
  }
  sq_result flushPending() {
    renumberPages(pending_, seqDelta_);
    bool ok = sq::cx::writeAll(out_.get(), pending_.data(), pending_.size());
    pending_.clear();
    return ok ? SQ_OK : SQ_E_IO;
  }
  void teardown() {
    if (osInit_) ogg_stream_clear(&os_);
    if (vbInit_) vorbis_block_clear(&vb_);
    if (vdInit_) vorbis_dsp_clear(&vd_);
    if (viInit_) vorbis_info_clear(&vi_);
    osInit_ = vbInit_ = vdInit_ = viInit_ = false;
    configured_ = false;
  }

  WriterTarget out_;
  MetaStore meta_;
  vorbis_info vi_{};
  vorbis_dsp_state vd_{};
  vorbis_block vb_{};
  ogg_stream_state os_{};
  bool viInit_ = false, vdInit_ = false, vbInit_ = false, osInit_ = false;
  bool configured_ = false, finalized_ = false, failed_ = false, headersOut_ = false;
  uint32_t channels_ = 0;
  int64_t placeholderPages_ = 0, seqDelta_ = 0;
  size_t spillBytes_ = kSpillBytes;
  std::vector<uint8_t> pending_;
};

// ---- service --------------------------------------------------------------------------------------
constexpr uint32_t kRead = SQ_CODEC_READ | SQ_CODEC_LOSSY | SQ_CODEC_METADATA;
constexpr uint32_t kReadWrite = kRead | SQ_CODEC_WRITE;
constexpr FormatDesc kFormats[] = {
    {"ogg", "audio/ogg", "Ogg Vorbis", kReadWrite},
    {"oga", "audio/x-vorbis+ogg", "Ogg Vorbis", kReadWrite},
    {"opus", "audio/opus", "Ogg Opus (decode, needs libopusfile installed)", kRead},
};
constexpr size_t kFormatCount = sizeof kFormats / sizeof kFormats[0];

class Service final : public Object<Service, ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_info)) return SQ_E_INVALIDARG;
    std::snprintf(out->name, sizeof out->name, "Ogg Vorbis/Opus");
    out->flags = kReadWrite;
    return SQ_OK;
  }
  uint32_t SQ_CALL getFormatCount() override { return opusfile() ? 3u : 2u; }
  sq_result SQ_CALL getFormatInfo(uint32_t index, sq_codec_format_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_format_info)) return SQ_E_INVALIDARG;
    if (index >= getFormatCount()) return SQ_E_NOTFOUND;
    fillFormat(kFormats[index], ~0u, out);
    return SQ_OK;
  }
  sq_bool SQ_CALL supports(const char* extension, const char* mime, uint32_t flags) override {
    const size_t n = (ieq(extension, "opus") || ieq(extension, ".opus") || ieq(mime, "audio/opus")) && !opusfile() ? 2 : kFormatCount;
    return formatSupports(kFormats, n, ~0u, extension, mime, flags) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void* header, uint32_t size) override {
    const uint8_t* p = static_cast<const uint8_t*>(header);
    if (!p || size < 4 || std::memcmp(p, "OggS", 4) != 0) return 0;
    Kind k = sniff(p, size);
    if (k == Kind::Vorbis) return 100;
    if (k == Kind::Opus) return opusfile() ? 100 : 10;
    return size >= 64 ? 5 : 40;  // Ogg, but not a codec we know (or header too short to tell)
  }
  sq_result SQ_CALL createReader(IAudioFileReader** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = new (std::nothrow) Reader();
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }
  sq_result SQ_CALL createWriter(const char* extension, IAudioFileWriter** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    static const char* const kWritable[] = {"ogg", "oga"};
    if (extension && *extension && !extIs(extension, kWritable, 2)) return SQ_E_UNSUPPORTED;  // Opus: no encoder
    *out = new (std::nothrow) Writer();
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Service>(sq::codec::kOggCodecClassId, sq::codec::kCategory, "Ogg Codec"),
};
constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'O', 'G', 'G'),
    .name = "codec-ogg",
    .description = "Ogg Vorbis decoder and encoder (libvorbis); Opus decode via a user-installed libopusfile",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace
}  // namespace sq::ogg

SQ_MODULE_EXPORTS(sq::ogg::kModule)
