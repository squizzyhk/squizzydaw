// codec-wav: WAV / RF64 / BW64 / WAVE_FORMAT_EXTENSIBLE and AIFF / AIFF-C reader.
#include "wav_internal.hpp"

namespace sq::wav {

using namespace sq::cx;

namespace {

constexpr uint64_t kMaxMetaChunk = 64ull << 20;  // larger non-audio chunks are skipped, not exposed

bool isFiller(const char* id) {
  return !std::memcmp(id, "JUNK", 4) || !std::memcmp(id, "junk", 4) || !std::memcmp(id, "PAD ", 4) ||
         !std::memcmp(id, "FLLR", 4) || !std::memcmp(id, "fake", 4);
}

double ext80ToDouble(const uint8_t* p) {
  int exp = ((p[0] & 0x7F) << 8) | p[1];
  uint64_t mant = 0;
  for (int i = 0; i < 8; ++i) mant = (mant << 8) | p[2 + i];
  if (exp == 0 && mant == 0) return 0.0;
  if (exp == 0x7FFF) return 0.0;  // inf / nan: invalid rate
  double v = std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
  return (p[0] & 0x80) ? -v : v;
}

const char* infoIdForTag(const std::string& t) {
  for (const auto& m : kInfoMap)
    if (t == m.tag) return m.id;
  return nullptr;
}

}  // namespace

const char* infoTagForId(const char* id) {
  for (const auto& m : kInfoMap)
    if (!std::memcmp(id, m.id, 4)) return m.tag;
  return nullptr;
}
const char* infoIdFor(const std::string& tag) { return infoIdForTag(tag); }

// ---- Reader ------------------------------------------------------------------------------------
sq_result Reader::open(const char* path) {
  if (stream_) return SQ_E_EXISTS;
  Ptr<IByteStream> s;
  sq_result r = FileStream::open(path, false, s);
  if (SQ_FAILED(r)) return r;
  return attach(s);
}

sq_result Reader::openStream(IByteStream* stream) {
  if (stream_) return SQ_E_EXISTS;
  if (!stream) return SQ_E_INVALIDARG;
  stream->addRef();
  return attach(Ptr<IByteStream>::adopt(stream));
}

sq_result Reader::attach(Ptr<IByteStream> s) {
  stream_ = std::move(s);
  sq_result r;
  try {
    r = parse();
  } catch (const std::bad_alloc&) {
    r = SQ_E_OUTOFMEMORY;
  } catch (...) {
    r = SQ_E_FAIL;
  }
  if (SQ_FAILED(r)) {
    stream_ = nullptr;
    meta_.clear();
    markers_.clear();
  }
  return r;
}

sq_result Reader::parse() {
  uint8_t h[12];
  fileSize_ = streamSize(stream_.get());
  if (fileSize_ < 12 || !readAt(stream_.get(), 0, h, 12)) return SQ_E_PARSE;
  if ((!std::memcmp(h, "RIFF", 4) || !std::memcmp(h, "RF64", 4) || !std::memcmp(h, "BW64", 4)) && !std::memcmp(h + 8, "WAVE", 4))
    return parseRiff(std::memcmp(h, "RIFF", 4) != 0);
  if (!std::memcmp(h, "FORM", 4) && (!std::memcmp(h + 8, "AIFF", 4) || !std::memcmp(h + 8, "AIFC", 4)))
    return parseAiff(!std::memcmp(h + 8, "AIFC", 4));
  return SQ_E_PARSE;
}

bool Reader::loadChunk(int64_t off, uint64_t size, std::vector<uint8_t>& out) {
  if (size > kMaxMetaChunk || off < 0 || static_cast<uint64_t>(off) + size > static_cast<uint64_t>(fileSize_)) return false;
  out.resize(static_cast<size_t>(size));
  return readAt(stream_.get(), off, out.data(), static_cast<uint32_t>(size));
}

sq_result Reader::parseRiff(bool rf64) {
  std::snprintf(container_, sizeof container_, "%s", rf64 ? "rf64" : "wav");
  bigEndian_ = false;
  uint64_t ds64Data = 0;
  bool haveDs64 = false;
  std::vector<std::pair<std::string, uint64_t>> table;
  bool haveFmt = false, haveData = false;
  RiffMarkerParser mk;
  std::vector<uint8_t> payload;
  std::string infoComment;
  int64_t off = 12;
  int guard = 0;
  while (off + 8 <= fileSize_ && ++guard < 100000) {
    uint8_t ch[8];
    if (!readAt(stream_.get(), off, ch, 8)) break;
    char id[4];
    std::memcpy(id, ch, 4);
    uint64_t size = rd32le(ch + 4);
    if (rf64 && size == 0xFFFFFFFFu) {
      if (!std::memcmp(id, "data", 4) && haveDs64) size = ds64Data;
      else
        for (const auto& t : table)
          if (!std::memcmp(t.first.data(), id, 4)) size = t.second;
    }
    const int64_t body = off + 8;
    const uint64_t remain = static_cast<uint64_t>(fileSize_ - body);
    if (!std::memcmp(id, "data", 4)) {
      if (!haveData) {
        haveData = true;
        dataOffset_ = body;
        // Streaming writers leave 0 / 0xFFFFFFFF; truncated files: use what is there.
        dataSize_ = (size == 0 || size > remain) ? remain : size;
        if (size == 0xFFFFFFFFu && !rf64) dataSize_ = remain;
      }
      if (size == 0 || (size == 0xFFFFFFFFu && !rf64)) size = remain;
      if (size > remain) break;
    } else if (!std::memcmp(id, "ds64", 4)) {
      if (loadChunk(body, std::min<uint64_t>(size, 4096), payload) && payload.size() >= 28) {
        ds64Data = rd64le(payload.data() + 8);
        haveDs64 = true;
        uint32_t n = rd32le(payload.data() + 24);
        for (uint32_t i = 0; i < n && 28 + (i + 1) * 12 <= payload.size(); ++i) {
          const uint8_t* e = payload.data() + 28 + i * 12;
          table.emplace_back(std::string(reinterpret_cast<const char*>(e), 4), rd64le(e + 4));
        }
      }
    } else if (!std::memcmp(id, "fmt ", 4)) {
      if (!haveFmt) {
        if (size < 16 || !loadChunk(body, std::min<uint64_t>(size, 4096), payload)) return SQ_E_PARSE;
        sq_result r = parseFmt(payload.data(), payload.size());
        if (SQ_FAILED(r)) return r;
        haveFmt = true;
      }
    } else if (!std::memcmp(id, "fact", 4) || isFiller(id)) {
      // structural / filler
    } else if (loadChunk(body, size, payload)) {
      meta_.addChunk(id, payload.data(), payload.size());
      if (!std::memcmp(id, "cue ", 4)) mk.cue(payload.data(), payload.size());
      else if (!std::memcmp(id, "smpl", 4)) mk.smpl(payload.data(), payload.size());
      else if (!std::memcmp(id, "LIST", 4)) {
        mk.list(payload.data(), payload.size());
        if (payload.size() >= 4 && !std::memcmp(payload.data(), "INFO", 4)) parseInfo(payload.data(), payload.size());
      } else if (!std::memcmp(id, "bext", 4)) {
        parseBext(payload.data(), payload.size());
      }
    }
    if (size > remain) break;
    off = body + static_cast<int64_t>(size) + static_cast<int64_t>(size & 1);
  }
  if (!haveFmt || !haveData) return SQ_E_PARSE;
  frames_ = static_cast<sq_sample_time>(dataSize_ / blockAlign_);
  markers_ = mk.build(frames_);
  return SQ_OK;
}

sq_result Reader::parseFmt(const uint8_t* p, size_t n) {
  uint32_t tag = rd16le(p);
  channels_ = rd16le(p + 2);
  sampleRate_ = rd32le(p + 4);
  blockAlign_ = rd16le(p + 12);
  uint32_t bits = rd16le(p + 14);
  if (tag == 0xFFFE) {
    if (n < 40) return SQ_E_PARSE;
    validBits_ = rd16le(p + 18);
    tag = rd16le(p + 24);  // sub-format GUID data1 (low 16 bits)
  } else {
    validBits_ = 0;
  }
  if (channels_ == 0 || channels_ > kMaxChannels || sampleRate_ <= 0 || sampleRate_ > 1e7 || blockAlign_ == 0) return SQ_E_PARSE;
  if (blockAlign_ % channels_) return SQ_E_PARSE;
  bytes_ = blockAlign_ / channels_;
  if (tag == 1) {
    if (bytes_ < 1 || bytes_ > 4) return SQ_E_UNSUPPORTED;
    enc_ = bytes_ == 1 ? Enc::U8 : Enc::Int;
  } else if (tag == 3) {
    if (bytes_ != 4 && bytes_ != 8) return SQ_E_UNSUPPORTED;
    enc_ = bytes_ == 4 ? Enc::F32 : Enc::F64;
  } else {
    return SQ_E_UNSUPPORTED;  // compressed WAV payloads (ADPCM, a-law...) are not handled
  }
  if (validBits_ == 0 || validBits_ > bytes_ * 8) validBits_ = (bits > 0 && bits <= bytes_ * 8) ? bits : bytes_ * 8;
  return SQ_OK;
}

void Reader::parseInfo(const uint8_t* p, size_t n) {
  size_t off = 4;
  while (off + 8 <= n) {
    uint32_t sz = rd32le(p + off + 4);
    if (sz > n - off - 8) break;
    char id[5] = {0};
    std::memcpy(id, p + off, 4);
    if (const char* tag = infoTagForId(id)) {
      std::string key = std::string("tag:") + tag;
      if (!meta_.has(key)) meta_.setText(key, toUtf8Lenient(fieldText(p + off + 8, sz)));
    }
    off += 8 + sz + (sz & 1);
  }
}

void Reader::parseBext(const uint8_t* p, size_t n) {
  if (n < kBextFixed) return;
  std::string orig = toUtf8Lenient(fieldText(p + 256, 32));
  std::string date = fieldText(p + 320, 10);
  std::string time = fieldText(p + 330, 8);
  if (!orig.empty() && !meta_.has("tag:originator")) meta_.setText("tag:originator", orig);
  if (!date.empty() && !meta_.has("tag:origination_date"))
    meta_.setText("tag:origination_date", time.empty() ? date : date + " " + time);
}

sq_result Reader::parseAiff(bool aifc) {
  std::snprintf(container_, sizeof container_, "%s", aifc ? "aifc" : "aif");
  bigEndian_ = true;
  bool haveComm = false, haveSsnd = false;
  uint32_t commFrames = 0;
  std::vector<uint8_t> payload, markPayload, instPayload;
  int64_t off = 12;
  int guard = 0;
  while (off + 8 <= fileSize_ && ++guard < 100000) {
    uint8_t ch[8];
    if (!readAt(stream_.get(), off, ch, 8)) break;
    char id[4];
    std::memcpy(id, ch, 4);
    uint64_t size = rd32be(ch + 4);
    const int64_t body = off + 8;
    const uint64_t remain = static_cast<uint64_t>(fileSize_ - body);
    if (!std::memcmp(id, "COMM", 4)) {
      if (!haveComm) {
        if (size < 18 || !loadChunk(body, std::min<uint64_t>(size, 4096), payload)) return SQ_E_PARSE;
        sq_result r = parseComm(payload.data(), payload.size(), aifc, commFrames);
        if (SQ_FAILED(r)) return r;
        haveComm = true;
      }
    } else if (!std::memcmp(id, "SSND", 4)) {
      if (!haveSsnd) {
        uint8_t s8[8];
        if (size < 8 || !readAt(stream_.get(), body, s8, 8)) return SQ_E_PARSE;
        uint64_t dataOff = rd32be(s8);
        dataOffset_ = body + 8 + static_cast<int64_t>(dataOff);
        uint64_t avail = std::min<uint64_t>(size, remain);
        dataSize_ = avail > 8 + dataOff ? avail - 8 - dataOff : 0;
        haveSsnd = true;
      }
    } else if (!std::memcmp(id, "FVER", 4) || isFiller(id)) {
    } else if (loadChunk(body, size, payload)) {
      meta_.addChunk(id, payload.data(), payload.size());
      std::string text = toUtf8Lenient(fieldText(payload.data(), payload.size()));
      const char* tag = !std::memcmp(id, "NAME", 4)   ? "tag:title"
                        : !std::memcmp(id, "AUTH", 4) ? "tag:artist"
                        : !std::memcmp(id, "(c) ", 4) ? "tag:copyright"
                        : !std::memcmp(id, "ANNO", 4) ? "tag:comment"
                                                      : nullptr;
      if (tag && !meta_.has(tag)) meta_.setText(tag, text);
      if (!std::memcmp(id, "MARK", 4)) markPayload = payload;
      if (!std::memcmp(id, "INST", 4)) instPayload = payload;
    }
    if (size > remain) break;
    off = body + static_cast<int64_t>(size) + static_cast<int64_t>(size & 1);
  }
  if (!haveComm || !haveSsnd) return SQ_E_PARSE;
  blockAlign_ = bytes_ * channels_;
  frames_ = static_cast<sq_sample_time>(std::min<uint64_t>(dataSize_ / blockAlign_, commFrames));
  dataSize_ = static_cast<uint64_t>(frames_) * blockAlign_;
  parseAiffMarkers(markPayload, instPayload);
  return SQ_OK;
}

sq_result Reader::parseComm(const uint8_t* p, size_t n, bool aifc, uint32_t& frames) {
  channels_ = rd16be(p);
  frames = rd32be(p + 2);
  uint32_t bits = rd16be(p + 6);
  sampleRate_ = ext80ToDouble(p + 8);
  if (channels_ == 0 || channels_ > kMaxChannels || !(sampleRate_ > 0) || sampleRate_ > 1e7 || bits == 0 || bits > 64)
    return SQ_E_PARSE;
  char comp[4] = {'N', 'O', 'N', 'E'};
  if (aifc) {
    if (n < 22) return SQ_E_PARSE;
    std::memcpy(comp, p + 18, 4);
  }
  validBits_ = bits;
  bytes_ = (bits + 7) / 8;
  if (!std::memcmp(comp, "NONE", 4) || !std::memcmp(comp, "twos", 4) || !std::memcmp(comp, "in24", 4) ||
      !std::memcmp(comp, "in32", 4)) {
    if (!std::memcmp(comp, "in24", 4)) { bytes_ = 3; validBits_ = 24; }
    if (!std::memcmp(comp, "in32", 4)) { bytes_ = 4; validBits_ = 32; }
    if (bytes_ > 4) return SQ_E_UNSUPPORTED;
    enc_ = Enc::Int;
  } else if (!std::memcmp(comp, "sowt", 4)) {
    if (bytes_ > 4) return SQ_E_UNSUPPORTED;
    enc_ = Enc::IntLE;
  } else if (!std::memcmp(comp, "fl32", 4) || !std::memcmp(comp, "FL32", 4)) {
    enc_ = Enc::F32; bytes_ = 4; validBits_ = 32;
  } else if (!std::memcmp(comp, "fl64", 4) || !std::memcmp(comp, "FL64", 4)) {
    enc_ = Enc::F64; bytes_ = 8; validBits_ = 64;
  } else if (!std::memcmp(comp, "raw ", 4)) {
    enc_ = Enc::U8; bytes_ = 1; validBits_ = 8;
  } else {
    return SQ_E_UNSUPPORTED;  // compressed AIFF-C (ima4, ulaw, ...)
  }
  return SQ_OK;
}

void Reader::parseAiffMarkers(const std::vector<uint8_t>& mark, const std::vector<uint8_t>& inst) {
  struct Mark { uint32_t id; uint32_t pos; std::string name; };
  std::vector<Mark> marks;
  if (mark.size() >= 2) {
    uint32_t count = rd16be(mark.data());
    size_t off = 2;
    for (uint32_t i = 0; i < count && off + 7 <= mark.size(); ++i) {
      Mark m;
      m.id = rd16be(mark.data() + off);
      m.pos = rd32be(mark.data() + off + 2);
      uint32_t len = mark[off + 6];
      if (off + 7 + len > mark.size()) break;
      m.name = toUtf8Lenient(std::string(reinterpret_cast<const char*>(mark.data() + off + 7), len));
      off += 7 + len + ((len + 1) & 1);  // count byte + text padded to even
      marks.push_back(std::move(m));
    }
  }
  auto findMark = [&](uint32_t id) -> const Mark* {
    for (const auto& m : marks)
      if (m.id == id) return &m;
    return nullptr;
  };
  struct LoopRef { uint32_t begin, end; };
  std::vector<LoopRef> loops;
  if (inst.size() >= 20) {
    for (int l = 0; l < 2; ++l) {
      const uint8_t* lp = inst.data() + 8 + l * 6;
      if (rd16be(lp) != 0) loops.push_back(LoopRef{rd16be(lp + 2), rd16be(lp + 4)});
    }
  }
  std::vector<uint32_t> hidden;  // marks used only as loop ends
  for (const auto& lr : loops)
    if (lr.end != lr.begin) hidden.push_back(lr.end);
  for (const auto& m : marks) {
    if (std::find(hidden.begin(), hidden.end(), m.id) != hidden.end()) continue;
    if (frames_ >= 0 && m.pos > frames_) continue;
    sq_sample_time len = 0;
    uint32_t flags = 0;
    for (const auto& lr : loops) {
      const Mark* e = findMark(lr.end);
      if (lr.begin == m.id && e && e->pos > m.pos) {
        len = static_cast<sq_sample_time>(e->pos) - m.pos;
        flags = SQ_MARKER_LOOP;
        break;
      }
    }
    markers_.push_back(makeMarker(m.id, m.pos, len, flags, m.name));
  }
}

sq_result Reader::getFormat(sq_audio_file_format* out) {
  if (!out || out->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
  if (!stream_) return SQ_E_CLOSED;
  out->sample_rate = sampleRate_;
  out->channels = channels_;
  switch (enc_) {
    case Enc::F32: out->sample_format = SQ_SAMPLE_F32; break;
    case Enc::F64: out->sample_format = SQ_SAMPLE_F64; break;
    case Enc::U8: out->sample_format = SQ_SAMPLE_I16; break;  // 8-bit: bits_per_sample says 8
    default:
      out->sample_format = bytes_ <= 2 ? SQ_SAMPLE_I16 : bytes_ == 3 ? SQ_SAMPLE_I24 : SQ_SAMPLE_I32;
  }
  out->bits_per_sample = validBits_;
  out->frame_count = frames_;
  out->bitrate_kbps = static_cast<uint32_t>(sampleRate_ * blockAlign_ * 8 / 1000);
  out->flags = SQ_AUDIOFILE_SEEKABLE | SQ_AUDIOFILE_SAMPLE_EXACT;
  std::snprintf(out->container, sizeof out->container, "%s", container_);
  return SQ_OK;
}

sq_result Reader::read(sq_audio_block* dst, uint32_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!stream_) return SQ_E_CLOSED;
  if (!blockOk(dst, channels_)) return SQ_E_INVALIDARG;
  uint32_t done = 0;
  while (done < dst->frames && pos_ < frames_) {
    uint32_t n = static_cast<uint32_t>(std::min<int64_t>({static_cast<int64_t>(dst->frames - done), frames_ - pos_, 8192}));
    const uint32_t bytes = n * blockAlign_;
    buf_.resize(bytes);
    uint32_t got = 0;
    if (SQ_FAILED(stream_->seek(dataOffset_ + pos_ * blockAlign_, SQ_SEEK_SET, nullptr)) ||
        SQ_FAILED(stream_->read(buf_.data(), bytes, &got)))
      return done ? SQ_OK : SQ_E_IO;
    n = got / blockAlign_;
    if (n == 0) { frames_ = pos_; break; }  // truncated: the real end
    convert(buf_.data(), n, dst, done);
    done += n;
    pos_ += n;
  }
  if (framesRead) *framesRead = done;
  return SQ_OK;
}

void Reader::convert(const uint8_t* src, uint32_t n, sq_audio_block* dst, uint32_t at) const {
  const uint32_t ch = channels_;
  for (uint32_t c = 0; c < ch; ++c) {
    float* out = dst->channels[c] + at;
    const uint8_t* p = src + c * bytes_;
    for (uint32_t i = 0; i < n; ++i, p += blockAlign_) {
      switch (enc_) {
        case Enc::U8: out[i] = (static_cast<int>(p[0]) - 128) * (1.0f / 128.0f); break;
        case Enc::Int:
        case Enc::IntLE: {
          uint32_t v = 0;
          const bool be = enc_ == Enc::Int ? bigEndian_ : false;
          for (uint32_t b = 0; b < bytes_; ++b) v = (v << 8) | (be ? p[b] : p[bytes_ - 1 - b]);
          v <<= (32 - 8 * bytes_);  // left-justify into 32 bits
          out[i] = static_cast<float>(static_cast<int32_t>(v) * (1.0 / 2147483648.0));
          break;
        }
        case Enc::F32: {
          uint32_t v = bigEndian_ ? rd32be(p) : rd32le(p);
          float f;
          std::memcpy(&f, &v, 4);
          out[i] = f;
          break;
        }
        case Enc::F64: {
          uint64_t v = bigEndian_ ? (static_cast<uint64_t>(rd32be(p)) << 32) | rd32be(p + 4) : rd64le(p);
          double d;
          std::memcpy(&d, &v, 8);
          out[i] = static_cast<float>(d);
          break;
        }
      }
    }
  }
}

sq_result Reader::seek(sq_sample_time frame) {
  if (!stream_) return SQ_E_CLOSED;
  if (frame < 0 || frame > frames_) return SQ_E_INVALIDARG;
  pos_ = frame;
  return SQ_OK;
}

sq_result Reader::tell(sq_sample_time* frame) {
  if (!frame) return SQ_E_INVALIDARG;
  if (!stream_) return SQ_E_CLOSED;
  *frame = pos_;
  return SQ_OK;
}

}  // namespace sq::wav
