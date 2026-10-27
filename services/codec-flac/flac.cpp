// codec-flac: Squizzy Studio FLAC codec service.
//   decode: dr_flac (third_party/dr_flac, public domain / MIT-0)
//   encode: own encoder (flac_encoder.cpp) + MD5 + SEEKTABLE + VORBIS_COMMENT + PADDING
// Metadata: "tag:<name>" <-> Vorbis comments; "chunk:<id>" <-> APPLICATION 'riff' blocks, each
// holding one RIFF chunk (id, LE size, payload). Markers travel as RIFF cue/LIST-adtl/smpl chunks
// inside those blocks, the same vocabulary as codec-wav.
#include <memory>

#include "dr_flac.h"
#include "flac_encoder.hpp"
#include "md5.hpp"
#include "sqcodec/common.hpp"
#include "sqcodec/riff_markers.hpp"

namespace sq::flac {
namespace {

using namespace sq::cx;

constexpr uint32_t kAppRiff = 0x72696666u;  // 'riff'
constexpr uint32_t kBlockSize = 4096;
constexpr uint32_t kInitialPadding = 8192;
constexpr uint64_t kMaxBlockBytes = (1u << 24) - 1;
constexpr char kVendor[] = "Squizzy Studio FLAC encoder";

// ---- reader ------------------------------------------------------------------------------------
class Reader final : public ReaderBase<Reader> {
 public:
  ~Reader() override { close(); }

  sq_result SQ_CALL open(const char* path) override {
    if (flac_) return SQ_E_EXISTS;
    Ptr<IByteStream> s;
    sq_result r = FileStream::open(path, false, s);
    if (SQ_FAILED(r)) return r;
    return attach(std::move(s));
  }
  sq_result SQ_CALL openStream(IByteStream* stream) override {
    if (flac_) return SQ_E_EXISTS;
    if (!stream) return SQ_E_INVALIDARG;
    stream->addRef();
    return attach(Ptr<IByteStream>::adopt(stream));
  }
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
    if (!flac_) return SQ_E_CLOSED;
    out->sample_rate = flac_->sampleRate;
    out->channels = flac_->channels;
    const uint32_t bps = flac_->bitsPerSample;
    out->sample_format = bps <= 16 ? SQ_SAMPLE_I16 : bps <= 24 ? SQ_SAMPLE_I24 : SQ_SAMPLE_I32;
    out->bits_per_sample = bps;
    out->frame_count = frames_;
    out->bitrate_kbps = 0;
    if (frames_ > 0 && size_ > 0)
      out->bitrate_kbps = static_cast<uint32_t>(static_cast<double>(size_) * 8.0 / (static_cast<double>(frames_) / flac_->sampleRate) / 1000.0);
    out->flags = SQ_AUDIOFILE_SEEKABLE | SQ_AUDIOFILE_SAMPLE_EXACT | SQ_AUDIOFILE_VBR;
    std::snprintf(out->container, sizeof out->container, "flac");
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* framesRead) override {
    if (framesRead) *framesRead = 0;
    if (!flac_) return SQ_E_CLOSED;
    const uint32_t ch = flac_->channels;
    if (!blockOk(dst, ch)) return SQ_E_INVALIDARG;
    uint32_t done = 0;
    while (done < dst->frames) {
      uint32_t want = std::min<uint32_t>(dst->frames - done, 4096);
      tmp_.resize(static_cast<size_t>(want) * ch);
      uint32_t got = static_cast<uint32_t>(drflac_read_pcm_frames_f32(flac_, want, tmp_.data()));
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
    if (!flac_) return SQ_E_CLOSED;
    if (frame < 0 || (frames_ >= 0 && frame > frames_)) return SQ_E_INVALIDARG;
    if (frames_ >= 0 && frame == frames_) {  // end of stream: nothing left to decode
      if (!drflac_seek_to_pcm_frame(flac_, static_cast<drflac_uint64>(frame > 0 ? frame - 1 : 0))) return SQ_E_IO;
      if (frame > 0) {
        float scratch[8];
        if (flac_->channels <= 8) drflac_read_pcm_frames_f32(flac_, 1, scratch);
      }
      pos_ = frame;
      return SQ_OK;
    }
    if (!drflac_seek_to_pcm_frame(flac_, static_cast<drflac_uint64>(frame))) return SQ_E_IO;
    pos_ = frame;
    return SQ_OK;
  }
  sq_result SQ_CALL tell(sq_sample_time* frame) override {
    if (!frame) return SQ_E_INVALIDARG;
    if (!flac_) return SQ_E_CLOSED;
    *frame = pos_;
    return SQ_OK;
  }

 private:
  sq_result attach(Ptr<IByteStream> s) {
    stream_ = std::move(s);
    size_ = streamSize(stream_.get());
    try {
      flac_ = drflac_open_with_metadata(&Reader::onRead, &Reader::onSeek, &Reader::onTell, &Reader::onMeta, this, nullptr);
    } catch (...) {
      flac_ = nullptr;
    }
    if (!flac_ || flac_->channels == 0 || flac_->sampleRate == 0) {
      close();
      meta_.clear();
      return SQ_E_PARSE;
    }
    // A STREAMINFO total that the file size cannot possibly hold is corrupt: keep the upstream
    // binary-search seek (which extrapolates from it) out of the way; brute force still works.
    const uint64_t tot = flac_->totalPCMFrameCount;
    if (tot && (tot > (1ull << 40) || (size_ > 0 && tot / 65536 > static_cast<uint64_t>(size_)))) flac_->_noBinarySearchSeek = DRFLAC_TRUE;
    frames_ = flac_->totalPCMFrameCount ? static_cast<sq_sample_time>(flac_->totalPCMFrameCount) : -1;
    markers_ = mk_.build(frames_);
    return SQ_OK;
  }
  void close() {
    if (flac_) drflac_close(flac_);
    flac_ = nullptr;
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
  static drflac_bool32 onSeek(void* user, int offset, drflac_seek_origin origin) {
    auto* self = static_cast<Reader*>(user);
    int32_t o = origin == DRFLAC_SEEK_SET ? SQ_SEEK_SET : origin == DRFLAC_SEEK_CUR ? SQ_SEEK_CUR : SQ_SEEK_END;
    int64_t np = 0;
    if (SQ_FAILED(self->stream_->seek(offset, o, &np))) return DRFLAC_FALSE;
    if (self->size_ >= 0 && np > self->size_) return DRFLAC_FALSE;
    return DRFLAC_TRUE;
  }
  static drflac_bool32 onTell(void* user, drflac_int64* cursor) {
    int64_t p = streamTell(static_cast<Reader*>(user)->stream_.get());
    if (p < 0) return DRFLAC_FALSE;
    *cursor = p;
    return DRFLAC_TRUE;
  }
  static void onMeta(void* user, drflac_metadata* m) {
    auto* self = static_cast<Reader*>(user);
    try {
      self->meta(m);
    } catch (...) {
    }
  }
  void meta(drflac_metadata* m) {
    if (m->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
      drflac_vorbis_comment_iterator it;
      drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
      drflac_uint32 len = 0;
      const char* c;
      int guard = 0;
      while ((c = drflac_next_vorbis_comment(&it, &len)) != nullptr && ++guard < 10000) {
        if (static_cast<const uint8_t*>(m->pRawData) &&
            (c < static_cast<const char*>(m->pRawData) || c + len > static_cast<const char*>(m->pRawData) + m->rawDataSize))
          break;
        addVorbisComment(meta_, c, len);
      }
    } else if (m->type == DRFLAC_METADATA_BLOCK_TYPE_APPLICATION && m->data.application.id == kAppRiff) {
      const uint8_t* p = static_cast<const uint8_t*>(m->data.application.pData);
      uint32_t n = m->data.application.dataSize;
      if (!p || n < 8) return;
      uint32_t sz = rd32le(p + 4);
      if (sz > n - 8) return;
      char id[4];
      std::memcpy(id, p, 4);
      if (!std::memcmp(id, "RIFF", 4) || !std::memcmp(id, "fmt ", 4) || !std::memcmp(id, "data", 4)) return;
      meta_.addChunk(id, p + 8, sz);
      if (!std::memcmp(id, "cue ", 4)) mk_.cue(p + 8, sz);
      else if (!std::memcmp(id, "LIST", 4)) mk_.list(p + 8, sz);
      else if (!std::memcmp(id, "smpl", 4)) mk_.smpl(p + 8, sz);
    }
  }

  Ptr<IByteStream> stream_;
  drflac* flac_ = nullptr;
  int64_t size_ = -1;
  sq_sample_time frames_ = -1, pos_ = 0;
  std::vector<float> tmp_;
  RiffMarkerParser mk_;
};

// ---- writer ------------------------------------------------------------------------------------
class Writer final : public Object<Writer, IAudioFileWriter> {
 public:
  sq_result SQ_CALL open(const char* path, const sq_audio_file_format* format, IAttributes* options) override {
    if (configured_) return SQ_E_EXISTS;
    sq_result r = setup(format, options);
    if (SQ_FAILED(r)) return r;
    r = out_.openPath(path);
    if (SQ_FAILED(r)) configured_ = false;
    return r;
  }
  sq_result SQ_CALL openStream(IByteStream* stream, const sq_audio_file_format* format, IAttributes* options) override {
    if (configured_) return SQ_E_EXISTS;
    sq_result r = setup(format, options);
    if (SQ_FAILED(r)) return r;
    r = out_.openStream(stream);
    if (SQ_FAILED(r)) configured_ = false;
    return r;
  }
  sq_result SQ_CALL setMetadata(const char* key, const void* data, uint32_t size) override {
    if (!key) return SQ_E_INVALIDARG;
    if (finalized_) return SQ_E_CLOSED;
    std::string k(key);
    char id[4];
    if (chunkKeyId(k, id)) {
      if (!std::memcmp(id, "RIFF", 4) || !std::memcmp(id, "fmt ", 4) || !std::memcmp(id, "data", 4)) return SQ_E_INVALIDARG;
      if (size > kMaxBlockBytes - 12) return SQ_E_INVALIDARG;
    } else if (isTagKey(k)) {
      if (k.find('=') != std::string::npos || k.size() < 5) return SQ_E_INVALIDARG;
      // the VORBIS_COMMENT block (all tags) must fit one 24-bit metadata block
      uint64_t total = 4 + sizeof kVendor - 1 + 4;
      for (const auto& e : meta_.items())
        if (isTagKey(e.key) && e.key != k) total += 4 + tagToVorbisField(e.key.substr(4)).size() + 1 + e.value.size();
      if (data && size) total += 4 + tagToVorbisField(k.substr(4)).size() + 1 + size;
      if (total > kMaxBlockBytes) return SQ_E_INVALIDARG;
    } else {
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
    if (!configured_ || !out_ || finalized_ || failed_) return SQ_E_CLOSED;
    if (!blockOk(src, channels_)) return SQ_E_INVALIDARG;
    try {
      if (!started_) {
        sq_result r = begin();
        if (SQ_FAILED(r)) { failed_ = true; return r; }
      }
      for (uint32_t i = 0; i < src->frames;) {
        uint32_t take = std::min(src->frames - i, kBlockSize - fill_);
        for (uint32_t c = 0; c < channels_; ++c) {
          const float* in = src->channels[c] + i;
          int32_t* o = pcm_[c].data() + fill_;
          for (uint32_t j = 0; j < take; ++j) o[j] = floatToInt(dither_.apply(in[j], bits_), bits_);
        }
        fill_ += take;
        i += take;
        if (fill_ == kBlockSize) {
          sq_result r = flushFrame();
          if (SQ_FAILED(r)) { failed_ = true; return r; }
        }
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
      sq_result r = SQ_OK;
      if (!started_) r = begin();
      if (SQ_SUCCEEDED(r) && fill_) r = flushFrame();
      if (SQ_SUCCEEDED(r)) r = finish();
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
    if (!(f->sample_rate >= 1) || f->sample_rate > 655350 || f->sample_rate != std::floor(f->sample_rate)) return SQ_E_UNSUPPORTED;
    switch (f->sample_format) {
      case SQ_SAMPLE_I16: bits_ = 16; break;
      case SQ_SAMPLE_I24: case SQ_SAMPLE_I24_IN32: case SQ_SAMPLE_I32: case SQ_SAMPLE_F32: case SQ_SAMPLE_F64: bits_ = 24; break;
      default: return SQ_E_UNSUPPORTED;
    }
    if (f->bits_per_sample >= 8 && f->bits_per_sample <= 24 && f->sample_format != SQ_SAMPLE_F32 && f->sample_format != SQ_SAMPLE_F64)
      bits_ = f->bits_per_sample;
    channels_ = f->channels;
    sampleRate_ = static_cast<uint32_t>(f->sample_rate);
    if (options) dither_ = Dither(options->getBool("dither", SQ_FALSE) != SQ_FALSE);
    EncoderConfig cfg;
    cfg.sampleRate = sampleRate_;
    cfg.channels = channels_;
    cfg.bitsPerSample = bits_;
    cfg.blockSize = kBlockSize;
    if (options) {
      double q = options->getDouble("quality", 1.0);  // < 0.3: fixed predictors only (faster)
      if (q < 0.3) cfg.maxLpcOrder = 0;
      else if (q < 0.7) cfg.maxLpcOrder = 8;
    }
    enc_ = std::make_unique<Encoder>(cfg);
    pcm_.assign(channels_, std::vector<int32_t>(kBlockSize));
    configured_ = true;
    return SQ_OK;
  }

  // metadata blocks after STREAMINFO: VORBIS_COMMENT, APPLICATION riff*, SEEKTABLE (no PADDING)
  std::vector<uint8_t> metadataBlocks(bool withSeekTable) {
    std::vector<std::vector<uint8_t>> blocks;
    std::vector<uint8_t> types;
    // Vorbis comments (vendor + fields), little endian lengths
    Bytes vc;
    vc.u32le(sizeof kVendor - 1);
    vc.raw(kVendor, sizeof kVendor - 1);
    std::vector<std::string> fields;
    for (const auto& e : meta_.items())
      if (isTagKey(e.key)) fields.push_back(tagToVorbisField(e.key.substr(4)) + "=" + std::string(e.value.begin(), e.value.end()));
    vc.u32le(static_cast<uint32_t>(fields.size()));
    for (const auto& f : fields) {
      vc.u32le(static_cast<uint32_t>(f.size()));
      vc.raw(f.data(), f.size());
    }
    blocks.push_back(std::move(vc.v));
    types.push_back(4);
    // RIFF chunks (markers regenerate cue / adtl / smpl)
    std::vector<std::pair<std::string, std::vector<uint8_t>>> chunks;
    std::vector<uint8_t> rawSmpl;
    for (const auto& e : meta_.items()) {
      char id[4];
      if (!chunkKeyId(e.key, id)) continue;
      std::string sid(id, 4);
      if (!markers_.empty()) {
        if (sid == "cue ") continue;
        if (sid == "LIST" && e.value.size() >= 4 && !std::memcmp(e.value.data(), "adtl", 4)) continue;
        if (sid == "smpl") { rawSmpl = e.value; continue; }
      }
      chunks.emplace_back(sid, e.value);
    }
    if (!markers_.empty()) {
      RiffMarkerChunks mc = makeRiffMarkerChunks(markers_, rawSmpl, sampleRate_);
      chunks.emplace_back("cue ", std::move(mc.cue));
      if (!mc.adtl.empty()) chunks.emplace_back("LIST", std::move(mc.adtl));
      if (!mc.smpl.empty()) chunks.emplace_back("smpl", std::move(mc.smpl));
    }
    for (auto& [id, data] : chunks) {
      Bytes a;
      a.u32be(kAppRiff);
      a.fourcc(id.c_str());
      a.u32le(static_cast<uint32_t>(data.size()));
      a.raw(data.data(), data.size());
      if (data.size() & 1) a.u8(0);
      blocks.push_back(std::move(a.v));
      types.push_back(2);
    }
    if (withSeekTable && !frameOffsets_.empty()) {
      const double seconds = static_cast<double>(totalFrames_) / sampleRate_;
      size_t points = std::min<size_t>(static_cast<size_t>(seconds / 10.0) + 1, 400);
      Bytes st;
      uint64_t lastSample = ~0ull;
      for (size_t p = 0; p < points; ++p) {
        uint64_t target = totalFrames_ * p / points;
        size_t idx = static_cast<size_t>(target / kBlockSize);
        if (idx >= frameOffsets_.size()) idx = frameOffsets_.size() - 1;
        uint64_t sample = static_cast<uint64_t>(idx) * kBlockSize;
        if (sample == lastSample) continue;
        lastSample = sample;
        st.u32be(static_cast<uint32_t>(sample >> 32));
        st.u32be(static_cast<uint32_t>(sample));
        st.u32be(static_cast<uint32_t>(frameOffsets_[idx] >> 32));
        st.u32be(static_cast<uint32_t>(frameOffsets_[idx]));
        uint64_t len = std::min<uint64_t>(kBlockSize, totalFrames_ - sample);
        st.u16be(static_cast<uint32_t>(len));
      }
      blocks.push_back(std::move(st.v));
      types.push_back(3);
    }
    Bytes all;
    for (size_t i = 0; i < blocks.size(); ++i) {
      all.u8(types[i]);  // last-flag set later by the caller via PADDING
      uint32_t n = static_cast<uint32_t>(std::min<size_t>(blocks[i].size(), kMaxBlockBytes));
      all.u8(n >> 16); all.u8(n >> 8); all.u8(n);
      all.raw(blocks[i].data(), n);
    }
    return std::move(all.v);
  }

  std::vector<uint8_t> streamInfo() {
    Bytes s;
    s.u16be(kBlockSize);
    s.u16be(kBlockSize);
    s.u8(minFrame_ >> 16); s.u8(minFrame_ >> 8); s.u8(minFrame_);
    s.u8(maxFrame_ >> 16); s.u8(maxFrame_ >> 8); s.u8(maxFrame_);
    // 20 bits rate, 3 bits channels-1, 5 bits bps-1, 36 bits total samples
    uint64_t packed = (static_cast<uint64_t>(sampleRate_) << 44) | (static_cast<uint64_t>(channels_ - 1) << 41) |
                      (static_cast<uint64_t>(bits_ - 1) << 36) | (totalFrames_ & 0xFFFFFFFFFull);
    s.u32be(static_cast<uint32_t>(packed >> 32));
    s.u32be(static_cast<uint32_t>(packed));
    uint8_t digest[16] = {};
    if (finalizing_) {
      Md5 copy = md5_;
      copy.final(digest);
    }
    s.raw(digest, 16);
    return std::move(s.v);
  }

  sq_result begin() {
    IByteStream* s = out_.get();
    Bytes h;
    h.fourcc("fLaC");
    h.u8(0);  // STREAMINFO, not last
    h.u8(0); h.u8(0); h.u8(34);
    auto si = streamInfo();
    h.raw(si.data(), si.size());
    std::vector<uint8_t> blocks = metadataBlocks(false);
    h.raw(blocks.data(), blocks.size());
    h.u8(0x80 | 1);  // last block: PADDING
    h.u8(kInitialPadding >> 16); h.u8(kInitialPadding >> 8); h.u8(kInitialPadding & 0xFF);
    h.zeros(kInitialPadding);
    if (!writeAll(s, h.v.data(), h.size())) return SQ_E_IO;
    metaStart_ = 4 + 4 + 34;
    audioStart_ = static_cast<int64_t>(h.size());
    writePos_ = audioStart_;
    started_ = true;
    return SQ_OK;
  }

  sq_result flushFrame() {
    const uint32_t n = fill_;
    // MD5 over interleaved little-endian samples of ceil(bps/8) bytes
    const uint32_t bpsBytes = (bits_ + 7) / 8;
    md5buf_.resize(static_cast<size_t>(n) * channels_ * bpsBytes);
    uint8_t* p = md5buf_.data();
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t c = 0; c < channels_; ++c) {
        uint32_t v = static_cast<uint32_t>(pcm_[c][i]);
        for (uint32_t b = 0; b < bpsBytes; ++b) *p++ = static_cast<uint8_t>(v >> (8 * b));
      }
    md5_.update(md5buf_.data(), md5buf_.size());
    const int32_t* ptrs[8];
    for (uint32_t c = 0; c < channels_; ++c) ptrs[c] = pcm_[c].data();
    frame_.clear();
    enc_->encodeFrame(ptrs, n, frameOffsets_.size(), frame_);
    frameOffsets_.push_back(static_cast<uint64_t>(writePos_ - audioStart_));
    if (!writeAt(out_.get(), writePos_, frame_.data(), frame_.size())) return SQ_E_IO;
    writePos_ += static_cast<int64_t>(frame_.size());
    const uint32_t fs = static_cast<uint32_t>(frame_.size());
    minFrame_ = minFrame_ == 0 ? fs : std::min(minFrame_, fs);
    maxFrame_ = std::max(maxFrame_, fs);
    totalFrames_ += n;
    fill_ = 0;
    return SQ_OK;
  }

  sq_result finish() {
    IByteStream* s = out_.get();
    finalizing_ = true;
    std::vector<uint8_t> blocks = metadataBlocks(true);
    const int64_t region = audioStart_ - metaStart_;  // bytes available for blocks + padding
    int64_t need = static_cast<int64_t>(blocks.size()) + 4;  // + padding header
    if (need > region) {
      int64_t grow = need - region + kInitialPadding;
      if (!shiftTail(s, audioStart_, grow)) return SQ_E_IO;
      audioStart_ += grow;
    }
    // fill the gap up to the audio with PADDING blocks (several when it exceeds one 24-bit block,
    // e.g. after large early metadata was removed)
    int64_t gap = audioStart_ - metaStart_ - static_cast<int64_t>(blocks.size());
    if (gap < 4) return SQ_E_FAIL;
    Bytes tail;
    tail.raw(blocks.data(), blocks.size());
    const int64_t kMax = static_cast<int64_t>(kMaxBlockBytes);
    while (gap - 4 > kMax) {
      const int64_t len = std::min<int64_t>(kMax, gap - 8);
      tail.u8(1);
      tail.u8(static_cast<uint32_t>(len >> 16)); tail.u8(static_cast<uint32_t>(len >> 8)); tail.u8(static_cast<uint32_t>(len));
      tail.zeros(static_cast<size_t>(len));
      gap -= len + 4;
    }
    const int64_t pad = gap - 4;
    tail.u8(0x80 | 1);
    tail.u8(static_cast<uint32_t>(pad >> 16)); tail.u8(static_cast<uint32_t>(pad >> 8)); tail.u8(static_cast<uint32_t>(pad));
    tail.zeros(static_cast<size_t>(pad));
    if (!writeAt(s, metaStart_, tail.v.data(), tail.size())) return SQ_E_IO;
    auto si = streamInfo();
    if (!writeAt(s, 8, si.data(), si.size())) return SQ_E_IO;
    return SQ_OK;
  }

  WriterTarget out_;
  bool configured_ = false, started_ = false, finalized_ = false, failed_ = false, finalizing_ = false;
  uint32_t channels_ = 0, bits_ = 16, sampleRate_ = 0;
  Dither dither_;
  std::unique_ptr<Encoder> enc_;
  std::vector<std::vector<int32_t>> pcm_;
  uint32_t fill_ = 0;
  std::vector<uint8_t> frame_, md5buf_;
  Md5 md5_;
  MetaStore meta_;
  std::vector<sq_audio_file_marker> markers_;
  std::vector<uint64_t> frameOffsets_;
  uint64_t totalFrames_ = 0;
  uint32_t minFrame_ = 0, maxFrame_ = 0;
  int64_t metaStart_ = 0, audioStart_ = 0, writePos_ = 0;
};

// ---- service ------------------------------------------------------------------------------------
constexpr uint32_t kRW = SQ_CODEC_READ | SQ_CODEC_WRITE | SQ_CODEC_METADATA | SQ_CODEC_MARKERS;
constexpr FormatDesc kFormats[] = {
    {"flac", "audio/flac", "FLAC lossless audio", kRW},
    {"fla", "audio/x-flac", "FLAC lossless audio", kRW},
};
constexpr size_t kFormatCount = sizeof kFormats / sizeof kFormats[0];

class Service final : public Object<Service, ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_info)) return SQ_E_INVALIDARG;
    std::snprintf(out->name, sizeof out->name, "FLAC");
    out->flags = kRW;
    return SQ_OK;
  }
  uint32_t SQ_CALL getFormatCount() override { return static_cast<uint32_t>(kFormatCount); }
  sq_result SQ_CALL getFormatInfo(uint32_t index, sq_codec_format_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_format_info)) return SQ_E_INVALIDARG;
    if (index >= kFormatCount) return SQ_E_NOTFOUND;
    fillFormat(kFormats[index], ~0u, out);
    return SQ_OK;
  }
  sq_bool SQ_CALL supports(const char* extension, const char* mime, uint32_t flags) override {
    return formatSupports(kFormats, kFormatCount, ~0u, extension, mime, flags) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void* header, uint32_t size) override {
    const uint8_t* p = static_cast<const uint8_t*>(header);
    if (!p || size < 4) return 0;
    return std::memcmp(p, "fLaC", 4) == 0 ? 100 : 0;
  }
  sq_result SQ_CALL createReader(IAudioFileReader** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = new (std::nothrow) Reader();
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }
  sq_result SQ_CALL createWriter(const char* extension, IAudioFileWriter** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    static const char* const kExt[] = {"flac", "fla"};
    if (extension && !extIs(extension, kExt, 2)) return SQ_E_UNSUPPORTED;
    *out = new (std::nothrow) Writer();
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Service>(sq::codec::kFlacCodecClassId, sq::codec::kCategory, "FLAC Codec"),
};
constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'F', 'L', 'A'),
    .name = "codec-flac",
    .description = "FLAC lossless codec (dr_flac decoder, own encoder)",
    .classes = kClasses,
};

}  // namespace
}  // namespace sq::flac

SQ_MODULE_EXPORTS(sq::flac::kModule)
