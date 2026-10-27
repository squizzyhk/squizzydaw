// codec-mp3: Squizzy Studio MP3 codec service.
//   decode: dr_mp3 (third_party/dr_mp3, public domain / MIT-0), gapless via the LAME/Xing info tag,
//           sample-accurate seeking through a seek table built at open.
//   encode: LAME (LGPL) loaded dynamically at runtime only if the user has it installed:
//           setting "Services.codec-mp3.LameLibrary" -> env SQ_LAME_LIBRARY -> default names.
//           Without it, supports(..., SQ_CODEC_WRITE) is false and createWriter is SQ_E_UNSUPPORTED.
//   tags:   ID3v2.2/2.3/2.4 + ID3v1 read ("tag:*", chapters -> markers, raw tag as "chunk:ID3 "),
//           ID3v2.4 written.
#include <memory>
#include <mutex>

#include "dr_mp3.h"
#include "id3.hpp"
#include "sqcodec/common.hpp"

namespace sq::mp3 {
namespace {

using namespace sq::cx;

// ---- LAME runtime binding ----------------------------------------------------------------------
struct Lame {
  using Init = void* (*)();
  using SetInt = int (*)(void*, int);
  using SetFloat = int (*)(void*, float);
  using Params = int (*)(void*);
  using EncodeF = int (*)(void*, const float*, const float*, int, unsigned char*, int);
  using Flush = int (*)(void*, unsigned char*, int);
  using TagFrame = size_t (*)(void*, unsigned char*, size_t);
  using Close = int (*)(void*);

  sq_dylib* lib = nullptr;
  Init init = nullptr;
  SetInt setInRate = nullptr, setChannels = nullptr, setBrate = nullptr, setVbr = nullptr, setQuality = nullptr,
         setMode = nullptr, setVbrTag = nullptr, setId3Auto = nullptr;
  SetFloat setVbrQuality = nullptr;
  Params initParams = nullptr;
  EncodeF encodeFloat = nullptr;
  Flush flush = nullptr;
  TagFrame tagFrame = nullptr;
  Close close = nullptr;
  bool tried = false;
  std::string path;

  bool ok() const { return lib != nullptr; }
  void unload() {
    if (lib) sq_dylib_close(lib);
    *this = Lame{};
  }
  bool bind(sq_dylib* l) {
    auto sym = [&](const char* n) { return sq_dylib_symbol(l, n); };
    init = reinterpret_cast<Init>(sym("lame_init"));
    setInRate = reinterpret_cast<SetInt>(sym("lame_set_in_samplerate"));
    setChannels = reinterpret_cast<SetInt>(sym("lame_set_num_channels"));
    setBrate = reinterpret_cast<SetInt>(sym("lame_set_brate"));
    setVbr = reinterpret_cast<SetInt>(sym("lame_set_VBR"));
    setQuality = reinterpret_cast<SetInt>(sym("lame_set_quality"));
    setMode = reinterpret_cast<SetInt>(sym("lame_set_mode"));
    setVbrTag = reinterpret_cast<SetInt>(sym("lame_set_bWriteVbrTag"));
    setId3Auto = reinterpret_cast<SetInt>(sym("lame_set_write_id3tag_automatic"));  // optional
    setVbrQuality = reinterpret_cast<SetFloat>(sym("lame_set_VBR_quality"));
    initParams = reinterpret_cast<Params>(sym("lame_init_params"));
    encodeFloat = reinterpret_cast<EncodeF>(sym("lame_encode_buffer_ieee_float"));
    flush = reinterpret_cast<Flush>(sym("lame_encode_flush"));
    tagFrame = reinterpret_cast<TagFrame>(sym("lame_get_lametag_frame"));
    close = reinterpret_cast<Close>(sym("lame_close"));
    return init && setInRate && setChannels && setBrate && setVbr && setQuality && setMode && setVbrTag && setVbrQuality &&
           initParams && encodeFloat && flush && tagFrame && close;
  }
};

std::mutex& lameMutex() {
  static std::mutex m;
  return m;
}
Lame& lameSlot() {
  static Lame l;
  return l;
}

// Returns the bound library or nullptr. Thread-safe; probes once per module init.
Lame* lame() {
  std::lock_guard<std::mutex> g(lameMutex());
  Lame& l = lameSlot();
  if (l.ok()) return &l;
  if (l.tried) return nullptr;
  l.tried = true;
  std::vector<std::string> candidates;
  std::string s = hostSetting("Services.codec-mp3.LameLibrary");
  if (!s.empty()) candidates.push_back(s);
  s = envVar("SQ_LAME_LIBRARY");
  if (!s.empty()) candidates.push_back(s);
#if defined(_WIN32)
  candidates.insert(candidates.end(), {"libmp3lame.dll", "libmp3lame-0.dll", "lame_enc.dll"});
#elif defined(__APPLE__)
  candidates.insert(candidates.end(), {"libmp3lame.0.dylib", "libmp3lame.dylib"});
#else
  candidates.insert(candidates.end(), {"libmp3lame.so.0", "libmp3lame.so"});
#endif
  for (const auto& c : candidates) {
    sq_dylib* d = nullptr;
    char err[256] = {};
    if (SQ_FAILED(sq_dylib_open(c.c_str(), &d, err, sizeof err)) || !d) continue;
    if (l.bind(d)) {
      l.lib = d;
      l.path = c;
      sq::logf(SQ_LOG_INFO, "MP3 encoding enabled via %s", c.c_str());
      return &l;
    }
    sq_dylib_close(d);
  }
  return nullptr;
}

sq_result onInit(IHostContext*) {
  std::lock_guard<std::mutex> g(lameMutex());
  lameSlot().tried = false;
  return SQ_OK;
}
void onShutdown() {
  std::lock_guard<std::mutex> g(lameMutex());
  lameSlot().unload();
}

// ---- reader -------------------------------------------------------------------------------------
class Reader final : public ReaderBase<Reader> {
 public:
  ~Reader() override { close(); }

  sq_result SQ_CALL open(const char* path) override {
    if (mp3_) return SQ_E_EXISTS;
    Ptr<IByteStream> s;
    sq_result r = FileStream::open(path, false, s);
    if (SQ_FAILED(r)) return r;
    return attach(std::move(s));
  }
  sq_result SQ_CALL openStream(IByteStream* stream) override {
    if (mp3_) return SQ_E_EXISTS;
    if (!stream) return SQ_E_INVALIDARG;
    stream->addRef();
    return attach(Ptr<IByteStream>::adopt(stream));
  }
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
    if (!mp3_) return SQ_E_CLOSED;
    out->sample_rate = mp3_->sampleRate;
    out->channels = mp3_->channels;
    out->sample_format = SQ_SAMPLE_F32;
    out->bits_per_sample = 32;
    out->frame_count = frames_;
    out->bitrate_kbps = 0;
    if (frames_ > 0 && size_ > 0)
      out->bitrate_kbps = static_cast<uint32_t>(static_cast<double>(size_) * 8.0 / (static_cast<double>(frames_) / mp3_->sampleRate) / 1000.0 + 0.5);
    out->flags = SQ_AUDIOFILE_SEEKABLE | SQ_AUDIOFILE_SAMPLE_EXACT | (mp3_->isVBR ? SQ_AUDIOFILE_VBR : 0u);
    std::snprintf(out->container, sizeof out->container, "mp3");
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* framesRead) override {
    if (framesRead) *framesRead = 0;
    if (!mp3_) return SQ_E_CLOSED;
    const uint32_t ch = mp3_->channels;
    if (!blockOk(dst, ch)) return SQ_E_INVALIDARG;
    uint32_t done = 0;
    while (done < dst->frames) {
      uint32_t want = std::min<uint32_t>(dst->frames - done, 4096);
      if (frames_ >= 0) want = static_cast<uint32_t>(std::min<int64_t>(want, frames_ - pos_));
      if (want == 0) break;
      tmp_.resize(static_cast<size_t>(want) * ch);
      uint32_t got = static_cast<uint32_t>(drmp3_read_pcm_frames_f32(mp3_.get(), want, tmp_.data()));
      for (uint32_t c = 0; c < ch; ++c) {
        float* o = dst->channels[c] + done;
        for (uint32_t i = 0; i < got; ++i) o[i] = tmp_[static_cast<size_t>(i) * ch + c];
      }
      done += got;
      pos_ += got;
      if (got < want) break;
    }
    if (framesRead) *framesRead = done;
    return SQ_OK;
  }
  sq_result SQ_CALL seek(sq_sample_time frame) override {
    if (!mp3_) return SQ_E_CLOSED;
    if (frame < 0 || (frames_ >= 0 && frame > frames_)) return SQ_E_INVALIDARG;
    if (!drmp3_seek_to_pcm_frame(mp3_.get(), static_cast<drmp3_uint64>(frame))) return SQ_E_IO;
    pos_ = frame;
    return SQ_OK;
  }
  sq_result SQ_CALL tell(sq_sample_time* frame) override {
    if (!frame) return SQ_E_INVALIDARG;
    if (!mp3_) return SQ_E_CLOSED;
    *frame = pos_;
    return SQ_OK;
  }

 private:
  sq_result attach(Ptr<IByteStream> s) {
    stream_ = std::move(s);
    size_ = streamSize(stream_.get());
    mp3_.reset(new (std::nothrow) drmp3{});
    if (!mp3_) return SQ_E_OUTOFMEMORY;
    inited_ = drmp3_init(mp3_.get(), &Reader::onRead, &Reader::onSeek, &Reader::onTell, &Reader::onMeta, this, nullptr) != DRMP3_FALSE;
    if (!inited_ || mp3_->channels == 0 || mp3_->channels > 2 || mp3_->sampleRate == 0) {
      close();
      meta_.clear();
      return SQ_E_PARSE;
    }
    inited_ = true;
    try {
      // exact length (honours encoder delay / padding), then a seek table for fast exact seeks
      drmp3_uint64 total = drmp3_get_pcm_frame_count(mp3_.get());
      frames_ = static_cast<sq_sample_time>(total);
      uint32_t cap = static_cast<uint32_t>(std::clamp<drmp3_uint64>(total / mp3_->sampleRate + 2, 16, 8192));
      seekPoints_.resize(cap);
      if (drmp3_calculate_seek_points(mp3_.get(), &cap, seekPoints_.data()) && cap > 0) {
        seekPoints_.resize(cap);
        drmp3_bind_seek_table(mp3_.get(), cap, seekPoints_.data());
      } else {
        seekPoints_.clear();
      }
      drmp3_seek_to_pcm_frame(mp3_.get(), 0);
      if (!id3v2_.empty()) {
        meta_.addChunk("ID3 ", id3v2_.data(), id3v2_.size());
        std::vector<Chapter> chapters;
        Id3Reader().parse(id3v2_.data(), id3v2_.size(), meta_, chapters);
        uint32_t idx = 1;
        for (const auto& c : chapters) {
          auto f = [&](uint32_t ms) { return static_cast<sq_sample_time>(std::llround(ms * static_cast<double>(mp3_->sampleRate) / 1000.0)); };
          sq_sample_time p = f(c.startMs), e = c.endMs > c.startMs ? f(c.endMs) : p;
          if (frames_ >= 0 && p > frames_) continue;
          markers_.push_back(makeMarker(idx++, p, e - p, 0, c.name));
        }
      }
      if (id3v1_.size() == 128) parseId3v1(id3v1_.data(), meta_);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  void close() {
    if (mp3_ && inited_) drmp3_uninit(mp3_.get());
    mp3_.reset();
    inited_ = false;
    stream_ = nullptr;
  }
  static size_t onRead(void* user, void* buf, size_t n) {
    auto* self = static_cast<Reader*>(user);
    size_t total = 0;
    while (total < n) {
      uint32_t want = static_cast<uint32_t>(std::min<size_t>(n - total, 1u << 30));
      uint32_t got = 0;
      if (SQ_FAILED(self->stream_->read(static_cast<uint8_t*>(buf) + total, want, &got)) || got == 0) break;
      total += got;
    }
    return total;
  }
  static drmp3_bool32 onSeek(void* user, int offset, drmp3_seek_origin origin) {
    auto* self = static_cast<Reader*>(user);
    int32_t o = origin == DRMP3_SEEK_SET ? SQ_SEEK_SET : origin == DRMP3_SEEK_CUR ? SQ_SEEK_CUR : SQ_SEEK_END;
    int64_t np = 0;
    if (SQ_FAILED(self->stream_->seek(offset, o, &np))) return DRMP3_FALSE;
    if (self->size_ >= 0 && np > self->size_) return DRMP3_FALSE;
    return DRMP3_TRUE;
  }
  static drmp3_bool32 onTell(void* user, drmp3_int64* cursor) {
    int64_t p = streamTell(static_cast<Reader*>(user)->stream_.get());
    if (p < 0) return DRMP3_FALSE;
    *cursor = p;
    return DRMP3_TRUE;
  }
  static void onMeta(void* user, const drmp3_metadata* m) {
    auto* self = static_cast<Reader*>(user);
    if (!m || !m->pRawData) return;
    try {
      const uint8_t* p = static_cast<const uint8_t*>(m->pRawData);
      if (m->type == DRMP3_METADATA_TYPE_ID3V2 && self->id3v2_.empty() && m->rawDataSize <= (64u << 20))
        self->id3v2_.assign(p, p + m->rawDataSize);
      else if (m->type == DRMP3_METADATA_TYPE_ID3V1 && m->rawDataSize == 128)
        self->id3v1_.assign(p, p + 128);
    } catch (...) {
    }
  }

  Ptr<IByteStream> stream_;
  std::unique_ptr<drmp3> mp3_;
  bool inited_ = false;
  int64_t size_ = -1;
  sq_sample_time frames_ = -1, pos_ = 0;
  std::vector<float> tmp_;
  std::vector<drmp3_seek_point> seekPoints_;
  std::vector<uint8_t> id3v2_, id3v1_;
};

// ---- writer (LAME) ------------------------------------------------------------------------------
constexpr size_t kTagPadding = 4096;

class Writer final : public Object<Writer, IAudioFileWriter> {
 public:
  explicit Writer(Lame* l) : lame_(l) {}
  ~Writer() override {
    if (gf_) lame_->close(gf_);
  }
  sq_result SQ_CALL open(const char* path, const sq_audio_file_format* format, IAttributes* options) override {
    if (gf_) return SQ_E_EXISTS;
    sq_result r = setup(format, options);
    if (SQ_FAILED(r)) return r;
    r = out_.openPath(path);
    if (SQ_FAILED(r)) { lame_->close(gf_); gf_ = nullptr; }
    return r;
  }
  sq_result SQ_CALL openStream(IByteStream* stream, const sq_audio_file_format* format, IAttributes* options) override {
    if (gf_) return SQ_E_EXISTS;
    sq_result r = setup(format, options);
    if (SQ_FAILED(r)) return r;
    r = out_.openStream(stream);
    if (SQ_FAILED(r)) { lame_->close(gf_); gf_ = nullptr; }
    return r;
  }
  sq_result SQ_CALL setMetadata(const char* key, const void* data, uint32_t size) override {
    if (!key) return SQ_E_INVALIDARG;
    if (finalized_) return SQ_E_CLOSED;
    std::string k(key);
    if (k == "chunk:ID3 ") {
      if (data && size && (size < 10 || std::memcmp(data, "ID3", 3) != 0)) return SQ_E_INVALIDARG;
    } else if (!isTagKey(k) || k.find('=') != std::string::npos) {
      return SQ_E_UNSUPPORTED;
    }
    try {
      meta_.set(k, data, data ? size : 0);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL addMarker(const sq_audio_file_marker* marker) override {
    if (!marker || marker->struct_size < sizeof(sq_audio_file_marker) || marker->position < 0 || marker->length < 0)
      return SQ_E_INVALIDARG;
    if (finalized_) return SQ_E_CLOSED;
    sq_audio_file_marker m = *marker;
    m.name[sizeof m.name - 1] = '\0';
    markers_.push_back(m);
    return SQ_OK;
  }
  sq_result SQ_CALL write(const sq_audio_block* src) override {
    if (!gf_ || !out_ || finalized_ || failed_) return SQ_E_CLOSED;
    if (!blockOk(src, channels_)) return SQ_E_INVALIDARG;
    try {
      if (!started_ && SQ_FAILED(begin())) { failed_ = true; return SQ_E_IO; }
      for (uint32_t at = 0; at < src->frames;) {
        int n = static_cast<int>(std::min<uint32_t>(src->frames - at, 8192));
        enc_.resize(static_cast<size_t>(n) * 5 / 4 + 7200);
        const float* l = src->channels[0] + at;
        const float* r = channels_ > 1 ? src->channels[1] + at : l;
        // LAME's float API expects +-1.0 full scale
        int got = lame_->encodeFloat(gf_, l, r, n, enc_.data(), static_cast<int>(enc_.size()));
        if (got < 0) { failed_ = true; return SQ_E_FAIL; }
        if (!writeAt(out_.get(), pos_, enc_.data(), static_cast<size_t>(got))) { failed_ = true; return SQ_E_IO; }
        pos_ += got;
        at += static_cast<uint32_t>(n);
      }
    } catch (...) {
      failed_ = true;
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL finalize() override {
    if (finalized_) return SQ_S_FALSE;
    if (!gf_ || !out_) return SQ_E_CLOSED;
    if (failed_) return SQ_E_IO;
    try {
      if (!started_ && SQ_FAILED(begin())) { failed_ = true; return SQ_E_IO; }
      IByteStream* s = out_.get();
      enc_.resize(16384);
      int got = lame_->flush(gf_, enc_.data(), static_cast<int>(enc_.size()));
      if (got < 0 || !writeAt(s, pos_, enc_.data(), static_cast<size_t>(got))) { failed_ = true; return SQ_E_IO; }
      pos_ += got;
      // final tag: grow the reserved region if needed
      // final ID3 tag fills the region reserved before the audio (grown when it does not fit)
      std::vector<uint8_t> tag = finalTag(0);
      if (tag.empty()) tag = buildId3v24(MetaStore{}, {}, 0, 0);
      if (static_cast<int64_t>(tag.size()) > audioStart_) {
        int64_t grow = static_cast<int64_t>(tag.size()) - audioStart_ + static_cast<int64_t>(kTagPadding);
        if (!shiftTail(s, audioStart_, grow)) { failed_ = true; return SQ_E_IO; }
        audioStart_ += grow;
      }
      const size_t pad = static_cast<size_t>(audioStart_) - tag.size();
      std::vector<uint8_t> full = finalTag(pad);
      if (full.size() != static_cast<size_t>(audioStart_)) full = buildId3v24(MetaStore{}, {}, 0, static_cast<size_t>(audioStart_) - 10);
      if (!writeAt(s, 0, full.data(), full.size())) { failed_ = true; return SQ_E_IO; }
      // LAME/Info frame (gapless delay/padding, length) over the placeholder first frame
      unsigned char info[8192];
      size_t n = lame_->tagFrame(gf_, info, sizeof info);
      if (n > 0 && n <= sizeof info && !writeAt(s, audioStart_, info, n)) { failed_ = true; return SQ_E_IO; }
      finalized_ = true;
      return out_.commit();
    } catch (...) {
      failed_ = true;
      return SQ_E_FAIL;
    }
  }

 private:
  sq_result setup(const sq_audio_file_format* f, IAttributes* options) {
    if (!f || f->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
    if (f->channels < 1 || f->channels > 2) return SQ_E_UNSUPPORTED;
    if (!(f->sample_rate >= 1000.0 && f->sample_rate <= 1.0e6)) return SQ_E_UNSUPPORTED;  // also NaN
    channels_ = f->channels;
    sampleRate_ = f->sample_rate;
    gf_ = lame_->init();
    if (!gf_) return SQ_E_OUTOFMEMORY;
    lame_->setInRate(gf_, static_cast<int>(f->sample_rate));
    lame_->setChannels(gf_, static_cast<int>(channels_));
    lame_->setMode(gf_, channels_ == 1 ? 3 : 1);
    lame_->setVbrTag(gf_, 1);
    if (lame_->setId3Auto) lame_->setId3Auto(gf_, 0);
    lame_->setQuality(gf_, 2);
    int64_t kbps = options ? options->getInt("bitrate_kbps", f->bitrate_kbps) : f->bitrate_kbps;
    if (kbps > 0) {
      lame_->setVbr(gf_, 0);
      lame_->setBrate(gf_, static_cast<int>(kbps));
    } else {
      double q = options ? options->getDouble("quality", 0.8) : 0.8;
      q = std::clamp(q, 0.0, 1.0);
      lame_->setVbr(gf_, 4);  // vbr_default
      lame_->setVbrQuality(gf_, static_cast<float>((1.0 - q) * 9.0));
    }
    if (lame_->initParams(gf_) < 0) {
      lame_->close(gf_);
      gf_ = nullptr;
      return SQ_E_UNSUPPORTED;
    }
    return SQ_OK;
  }
  std::vector<uint8_t> finalTag(size_t padding) {
    bool tags = !markers_.empty();
    for (const auto& e : meta_.items()) tags = tags || isTagKey(e.key);
    const MetaEntry* raw = meta_.find("chunk:ID3 ");
    if (tags) return buildId3v24(meta_, markers_, sampleRate_, padding, raw ? id3KeptFrames(raw->value, meta_, !markers_.empty()) : std::vector<uint8_t>{});
    if (raw) {
      std::vector<uint8_t> t = raw->value;
      if (padding) {  // extend the declared size with zero padding
        uint32_t sz = syncsafe(t.data() + 6) + static_cast<uint32_t>(padding);
        if (t[3] < 4 && (t[5] & 0x10)) return buildId3v24(meta_, markers_, sampleRate_, padding);
        t[6] = static_cast<uint8_t>((sz >> 21) & 0x7F); t[7] = static_cast<uint8_t>((sz >> 14) & 0x7F);
        t[8] = static_cast<uint8_t>((sz >> 7) & 0x7F); t[9] = static_cast<uint8_t>(sz & 0x7F);
        t.insert(t.end(), padding, 0);
      }
      return t;
    }
    return {};
  }
  sq_result begin() {
    std::vector<uint8_t> tag = finalTag(kTagPadding);
    if (tag.empty()) tag = buildId3v24(MetaStore{}, {}, 0, kTagPadding);
    if (!writeAll(out_.get(), tag.data(), tag.size())) return SQ_E_IO;
    audioStart_ = static_cast<int64_t>(tag.size());
    pos_ = audioStart_;
    started_ = true;
    return SQ_OK;
  }

  Lame* lame_;
  void* gf_ = nullptr;
  WriterTarget out_;
  uint32_t channels_ = 0;
  double sampleRate_ = 0;
  bool started_ = false, finalized_ = false, failed_ = false;
  MetaStore meta_;
  std::vector<sq_audio_file_marker> markers_;
  std::vector<unsigned char> enc_;
  int64_t audioStart_ = 0, pos_ = 0;
};

// ---- service ------------------------------------------------------------------------------------
constexpr uint32_t kFlagsAll = SQ_CODEC_READ | SQ_CODEC_WRITE | SQ_CODEC_LOSSY | SQ_CODEC_METADATA | SQ_CODEC_MARKERS;
constexpr FormatDesc kFormats[] = {
    {"mp3", "audio/mpeg", "MPEG-1/2 Layer III (encoding needs LAME installed)", kFlagsAll},
};
constexpr size_t kFormatCount = sizeof kFormats / sizeof kFormats[0];

class Service final : public Object<Service, ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_info)) return SQ_E_INVALIDARG;
    std::snprintf(out->name, sizeof out->name, "MP3");
    out->flags = kFlagsAll & mask();
    return SQ_OK;
  }
  uint32_t SQ_CALL getFormatCount() override { return 1; }
  sq_result SQ_CALL getFormatInfo(uint32_t index, sq_codec_format_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_format_info)) return SQ_E_INVALIDARG;
    if (index >= 1) return SQ_E_NOTFOUND;
    fillFormat(kFormats[index], mask(), out);
    return SQ_OK;
  }
  sq_bool SQ_CALL supports(const char* extension, const char* mime, uint32_t flags) override {
    return formatSupports(kFormats, 1, (flags & SQ_CODEC_WRITE) ? mask() : ~SQ_CODEC_WRITE, extension, mime, flags) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void* header, uint32_t size) override {
    const uint8_t* p = static_cast<const uint8_t*>(header);
    if (!p || size < 4) return 0;
    if (!std::memcmp(p, "ID3", 3)) {
      // skip the tag when the caller handed us enough bytes; otherwise "probably"
      if (size >= 10) {
        size_t skip = 10 + syncsafe(p + 6) + ((p[5] & 0x10) ? 10 : 0);
        if (skip + 4 <= size) return frameHeader(p + skip) ? 90 : (std::memcmp(p + skip, "fLaC", 4) ? 30 : 0);
      }
      return 50;
    }
    if (frameHeader(p)) {
      uint32_t len = frameLength(p);
      if (len && len + 4 <= size) return frameHeader(p + len) ? 80 : 20;
      return 40;
    }
    return 0;
  }
  sq_result SQ_CALL createReader(IAudioFileReader** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = new (std::nothrow) Reader();
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }
  sq_result SQ_CALL createWriter(const char* extension, IAudioFileWriter** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (extension && !ieq(*extension == '.' ? extension + 1 : extension, "mp3")) return SQ_E_UNSUPPORTED;
    Lame* l = lame();
    if (!l) return SQ_E_UNSUPPORTED;  // LAME not installed: encoding unavailable
    *out = new (std::nothrow) Writer(l);
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }

 private:
  static uint32_t mask() { return lame() ? ~0u : ~SQ_CODEC_WRITE; }
  static bool frameHeader(const uint8_t* p) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    const uint32_t version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3, br = p[2] >> 4, sr = (p[2] >> 2) & 3;
    return version != 1 && layer != 0 && br != 15 && br != 0 && sr != 3;
  }
  static uint32_t frameLength(const uint8_t* p) {
    static const uint16_t kBr1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};  // MPEG1 L3
    static const uint16_t kBr2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};     // MPEG2/2.5 L3
    static const uint32_t kSr[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    const uint32_t version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
    if (layer != 1) return 0;  // only layer III lengths are computed here
    const uint32_t br = (version == 3 ? kBr1 : kBr2)[p[2] >> 4] * 1000u, sr = kSr[version][(p[2] >> 2) & 3];
    if (!br || !sr) return 0;
    const uint32_t pad = (p[2] >> 1) & 1;
    return (version == 3 ? 144 * br / sr : 72 * br / sr) + pad;
  }
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Service>(sq::codec::kMp3CodecClassId, sq::codec::kCategory, "MP3 Codec"),
};
constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'M', 'P', '3'),
    .name = "codec-mp3",
    .description = "MP3 decoder (dr_mp3); encoder via a user-installed LAME library",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace
}  // namespace sq::mp3

SQ_MODULE_EXPORTS(sq::mp3::kModule)
