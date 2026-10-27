// codec-wav: WAV / RF64 and AIFF / AIFF-C writer.
//
// Layout (WAV):  RIFF WAVE | JUNK(28, ds64 reservation) | fmt | [fact] | pre-data chunks | data |
//                post-data chunks
// Chunks set before the first write() are emitted before the audio; everything set later, plus the
// chunks generated from tags/markers, goes after it. A pre-data chunk changed later is patched in
// place when its size is unchanged, otherwise renamed to JUNK and re-emitted after the audio.
// RF64: rf64=true always, rf64=false never (fail above 4 GB), absent = only when needed.
#include "wav_internal.hpp"

namespace sq::wav {

using namespace sq::cx;

namespace {

bool isStructural(Kind k, const char* id) {
  static const char* const kWav[] = {"fmt ", "data", "ds64", "RIFF", "RF64", "BW64", "fact", "JUNK"};
  static const char* const kAiff[] = {"COMM", "SSND", "FVER", "FORM"};
  if (k == Kind::Wav) {
    for (const char* s : kWav)
      if (!std::memcmp(s, id, 4)) return true;
  } else {
    for (const char* s : kAiff)
      if (!std::memcmp(s, id, 4)) return true;
  }
  return false;
}

void doubleToExt80(double v, uint8_t* p) {
  std::memset(p, 0, 10);
  if (!(v > 0)) return;
  int exp = 0;
  double m = std::frexp(v, &exp);  // v = m * 2^exp, 0.5 <= m < 1
  uint64_t mant = static_cast<uint64_t>(std::ldexp(m, 64));
  int e = exp - 1 + 16383;
  p[0] = static_cast<uint8_t>((e >> 8) & 0x7F);
  p[1] = static_cast<uint8_t>(e);
  for (int i = 0; i < 8; ++i) p[2 + i] = static_cast<uint8_t>(mant >> (56 - 8 * i));
}

const char* aiffTextId(const std::string& tag) {
  if (tag == "title") return "NAME";
  if (tag == "artist") return "AUTH";
  if (tag == "copyright") return "(c) ";
  if (tag == "comment") return "ANNO";
  return nullptr;
}

std::vector<uint8_t> textBytes(const std::string& s, bool nul) {
  std::vector<uint8_t> v(s.begin(), s.end());
  if (nul) v.push_back(0);
  return v;
}

void setField(std::vector<uint8_t>& b, size_t off, size_t width, const std::string& s) {
  std::memset(b.data() + off, 0, width);
  std::memcpy(b.data() + off, s.data(), std::min(width, s.size()));
}

}  // namespace

sq_result Writer::setup(const sq_audio_file_format* f, IAttributes* options) {
  if (!f || f->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
  if (f->channels == 0 || f->channels > kMaxChannels || !(f->sample_rate > 0) || f->sample_rate > 4.0e6) return SQ_E_INVALIDARG;
  if (kind_ == Kind::Wav && f->sample_rate != std::floor(f->sample_rate)) return SQ_E_UNSUPPORTED;
  channels_ = f->channels;
  sampleRate_ = f->sample_rate;
  switch (f->sample_format) {
    case SQ_SAMPLE_I16:
      bytes_ = 2;
      enc_ = Enc::Int;
      if (f->bits_per_sample == 8) {  // 8-bit PCM (WAV: unsigned, AIFF: signed)
        bytes_ = 1;
        enc_ = kind_ == Kind::Wav ? Enc::U8 : Enc::Int;
      }
      break;
    case SQ_SAMPLE_I24: bytes_ = 3; enc_ = Enc::Int; break;
    case SQ_SAMPLE_I24_IN32: bytes_ = 4; enc_ = Enc::Int; break;
    case SQ_SAMPLE_I32: bytes_ = 4; enc_ = Enc::Int; break;
    case SQ_SAMPLE_F32: bytes_ = 4; enc_ = Enc::F32; break;
    case SQ_SAMPLE_F64: bytes_ = 8; enc_ = Enc::F64; break;
    default: return SQ_E_UNSUPPORTED;
  }
  validBits_ = bytes_ * 8;
  if (enc_ == Enc::Int && f->bits_per_sample >= 8 && f->bits_per_sample < validBits_) validBits_ = f->bits_per_sample;
  if (f->sample_format == SQ_SAMPLE_I24_IN32) validBits_ = 24;
  if (kind_ == Kind::Aiff && (enc_ == Enc::F32 || enc_ == Enc::F64)) kind_ = Kind::Aifc;  // float needs AIFF-C
  // AIFF derives the stored width from COMM sampleSize: pack to the smallest container that holds
  // the valid bits (24-in-32 becomes 3 bytes), otherwise readers would mis-frame the samples.
  if (kind_ != Kind::Wav && enc_ == Enc::Int) bytes_ = (validBits_ + 7) / 8;
  blockAlign_ = bytes_ * channels_;
  if (options) {
    if (options->getType("rf64") != SQ_VALUE_NONE) rf64Mode_ = options->getBool("rf64", SQ_FALSE) ? 1 : 0;
    dither_ = Dither(options->getBool("dither", SQ_FALSE) != SQ_FALSE);
  }
  configured_ = true;
  return SQ_OK;
}

sq_result Writer::open(const char* path, const sq_audio_file_format* format, IAttributes* options) {
  if (configured_) return SQ_E_EXISTS;
  sq_result r = setup(format, options);
  if (SQ_FAILED(r)) return r;
  r = out_.openPath(path);
  if (SQ_FAILED(r)) configured_ = false;
  return r;
}

sq_result Writer::openStream(IByteStream* stream, const sq_audio_file_format* format, IAttributes* options) {
  if (configured_) return SQ_E_EXISTS;
  sq_result r = setup(format, options);
  if (SQ_FAILED(r)) return r;
  r = out_.openStream(stream);
  if (SQ_FAILED(r)) configured_ = false;
  return r;
}

sq_result Writer::setMetadata(const char* key, const void* data, uint32_t size) {
  if (!key) return SQ_E_INVALIDARG;
  if (finalized_) return SQ_E_CLOSED;
  std::string k(key);
  char id[4];
  if (chunkKeyId(k, id)) {
    if (isStructural(kind_, id)) return SQ_E_INVALIDARG;
  } else if (isTagKey(k)) {
    std::string t = k.substr(4);
    bool ok = kind_ == Kind::Wav ? (infoIdFor(t) != nullptr || t == "originator" || t == "origination_date")
                                 : aiffTextId(t) != nullptr;
    if (!ok) return SQ_E_UNSUPPORTED;
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

sq_result Writer::addMarker(const sq_audio_file_marker* marker) {
  if (!marker || marker->struct_size < sizeof(sq_audio_file_marker) || marker->position < 0 || marker->length < 0)
    return SQ_E_INVALIDARG;
  if (finalized_) return SQ_E_CLOSED;
  sq_audio_file_marker m = *marker;
  m.name[sizeof m.name - 1] = '\0';
  markers_.push_back(m);
  return SQ_OK;
}

uint32_t Writer::channelMask() const {
  if (const MetaEntry* e = meta_.find("chunk:CHAN")) {
    // CoreAudio channel layout (big endian): tag, bitmap, ndesc. tag 0x10000 = use bitmap.
    if (e->value.size() >= 12 && rd32be(e->value.data()) == 0x10000u) {
      uint32_t bm = rd32be(e->value.data() + 4);
      uint32_t bits = 0;
      for (uint32_t m = bm; m; m &= m - 1) ++bits;
      if (bm && bits == channels_) return bm;  // a mask naming a different channel count is invalid
    }
  }
  switch (channels_) {
    case 1: return 0x4;
    case 2: return 0x3;
    case 3: return 0x7;
    case 4: return 0x33;
    case 5: return 0x37;
    case 6: return 0x3F;
    case 7: return 0x13F;
    case 8: return 0x63F;
    default: return 0;
  }
}

sq_result Writer::emitChunk(const char* id, const std::vector<uint8_t>& data) {
  IByteStream* s = out_.get();
  uint8_t h[8];
  std::memcpy(h, id, 4);
  if (kind_ == Kind::Wav) wr32le(h + 4, static_cast<uint32_t>(data.size()));
  else wr32be(h + 4, static_cast<uint32_t>(data.size()));
  static const uint8_t pad = 0;
  if (!writeAll(s, h, 8) || !writeAll(s, data.data(), data.size()) || ((data.size() & 1) && !writeAll(s, &pad, 1)))
    return SQ_E_IO;
  return SQ_OK;
}

sq_result Writer::begin() {
  IByteStream* s = out_.get();
  if (!s) return SQ_E_CLOSED;
  Bytes h;
  if (kind_ == Kind::Wav) {
    h.fourcc("RIFF");
    h.u32le(0);
    h.fourcc("WAVE");
    if (rf64Mode_ != 0) {
      h.fourcc("JUNK");
      h.u32le(28);
      h.zeros(28);
    }
    const bool isFloat = enc_ == Enc::F32 || enc_ == Enc::F64;
    const bool ext = channels_ > 2 || validBits_ != bytes_ * 8;
    Bytes f;
    f.u16le(ext ? 0xFFFE : (isFloat ? 3 : 1));
    f.u16le(channels_);
    f.u32le(static_cast<uint32_t>(sampleRate_));
    f.u32le(static_cast<uint32_t>(sampleRate_) * blockAlign_);
    f.u16le(blockAlign_);
    f.u16le(bytes_ * 8);
    if (ext) {
      f.u16le(22);
      f.u16le(validBits_);
      f.u32le(channelMask());
      // KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT: {0000000X-0000-0010-8000-00AA00389B71}
      static const uint8_t kTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
      f.u16le(isFloat ? 3 : 1);
      f.raw(kTail, sizeof kTail);
    } else if (isFloat) {
      f.u16le(0);
    }
    h.fourcc("fmt ");
    h.u32le(static_cast<uint32_t>(f.size()));
    h.raw(f.v.data(), f.size());
    if (isFloat) {
      factOffset_ = static_cast<int64_t>(h.size()) + 8;
      h.fourcc("fact");
      h.u32le(4);
      h.u32le(0);
    }
  } else {
    const bool aifc = kind_ == Kind::Aifc;
    h.fourcc("FORM");
    h.u32be(0);
    h.fourcc(aifc ? "AIFC" : "AIFF");
    if (aifc) {
      h.fourcc("FVER");
      h.u32be(4);
      h.u32be(0xA2805140u);
    }
    const char* comp = enc_ == Enc::F32 ? "fl32" : enc_ == Enc::F64 ? "fl64" : "NONE";
    const char* compName = enc_ == Enc::Int ? "not compressed" : "IEEE float";
    Bytes c;
    c.u16be(channels_);
    c.u32be(0);  // frames, patched
    c.u16be(validBits_);
    uint8_t ext[10];
    doubleToExt80(sampleRate_, ext);
    c.raw(ext, 10);
    if (aifc) {
      c.fourcc(comp);
      size_t n = std::strlen(compName);
      c.u8(static_cast<uint32_t>(n));
      c.raw(compName, n);
      if ((n + 1) & 1) c.u8(0);
    }
    commOffset_ = static_cast<int64_t>(h.size()) + 8;
    h.fourcc("COMM");
    h.u32be(static_cast<uint32_t>(c.size()));
    h.raw(c.v.data(), c.size());
  }
  if (!writeAll(s, h.v.data(), h.size())) return SQ_E_IO;
  int64_t at = static_cast<int64_t>(h.size());
  // raw chunks known now go before the audio
  for (const auto& e : meta_.items()) {
    char id[4];
    if (!chunkKeyId(e.key, id)) continue;
    pre_.push_back(Emitted{e.key, at, e.value});
    sq_result r = emitChunk(id, e.value);
    if (SQ_FAILED(r)) return r;
    at += 8 + static_cast<int64_t>(e.value.size() + (e.value.size() & 1));
  }
  dataHeader_ = at;
  Bytes d;
  if (kind_ == Kind::Wav) {
    d.fourcc("data");
    d.u32le(0);
  } else {
    d.fourcc("SSND");
    d.u32be(0);
    d.u32be(0);  // offset
    d.u32be(0);  // block size
  }
  if (!writeAll(s, d.v.data(), d.size())) return SQ_E_IO;
  dataStart_ = at + static_cast<int64_t>(d.size());
  started_ = true;
  return SQ_OK;
}

sq_result Writer::write(const sq_audio_block* src) {
  if (!configured_ || !out_) return SQ_E_CLOSED;
  if (finalized_ || failed_) return SQ_E_CLOSED;
  if (!blockOk(src, channels_)) return SQ_E_INVALIDARG;
  if (!started_) {
    sq_result r = begin();
    if (SQ_FAILED(r)) { failed_ = true; return r; }
  }
  const uint64_t add = static_cast<uint64_t>(src->frames) * blockAlign_;
  const bool limit32 = kind_ != Kind::Wav || rf64Mode_ == 0;
  if (limit32 && static_cast<uint64_t>(dataStart_) + dataBytes_ + add > 0xFFFFFFF0ull) return SQ_E_UNSUPPORTED;
  const bool be = kind_ != Kind::Wav;
  uint32_t done = 0;
  while (done < src->frames) {
    uint32_t n = std::min<uint32_t>(src->frames - done, 8192);
    buf_.resize(static_cast<size_t>(n) * blockAlign_);
    for (uint32_t c = 0; c < channels_; ++c) {
      const float* in = src->channels[c] + done;
      uint8_t* p = buf_.data() + c * bytes_;
      for (uint32_t i = 0; i < n; ++i, p += blockAlign_) {
        switch (enc_) {
          case Enc::U8: p[0] = static_cast<uint8_t>(floatToInt(dither_.apply(in[i], 8), 8) + 128); break;
          case Enc::Int:
          case Enc::IntLE: {
            int32_t v = floatToInt(dither_.apply(in[i], validBits_), validBits_);
            uint32_t u = static_cast<uint32_t>(v) << (bytes_ * 8 - validBits_);  // left-justified in container
            for (uint32_t b = 0; b < bytes_; ++b) {
              uint8_t byte = static_cast<uint8_t>(u >> (8 * b));
              p[be ? bytes_ - 1 - b : b] = byte;
            }
            break;
          }
          case Enc::F32: {
            uint32_t u;
            std::memcpy(&u, &in[i], 4);
            if (be) wr32be(p, u); else wr32le(p, u);
            break;
          }
          case Enc::F64: {
            double d = in[i];
            uint64_t u;
            std::memcpy(&u, &d, 8);
            if (be) { wr32be(p, static_cast<uint32_t>(u >> 32)); wr32be(p + 4, static_cast<uint32_t>(u)); }
            else wr64le(p, u);
            break;
          }
        }
      }
    }
    if (!writeAt(out_.get(), dataStart_ + static_cast<int64_t>(dataBytes_), buf_.data(), buf_.size())) {
      failed_ = true;
      return SQ_E_IO;
    }
    dataBytes_ += buf_.size();
    done += n;
  }
  frames_ += src->frames;
  return SQ_OK;
}

// ---- derived chunks -------------------------------------------------------------------------------
std::vector<Writer::Chunk> Writer::finalChunks() {
  std::vector<Chunk> cs;
  for (const auto& e : meta_.items()) {
    Chunk c;
    if (!chunkKeyId(e.key, c.id)) continue;
    c.key = e.key;
    c.data = e.value;
    cs.push_back(std::move(c));
  }
  if (kind_ == Kind::Wav) wavDerived(cs);
  else aiffDerived(cs);
  return cs;
}

static bool isList(const std::vector<uint8_t>& d, const char* type) { return d.size() >= 4 && !std::memcmp(d.data(), type, 4); }

void Writer::wavDerived(std::vector<Chunk>& cs) {
  // LIST/INFO from tags, merged into the first raw INFO list (other INFO fields are kept)
  std::vector<std::pair<std::string, std::string>> info;  // id -> text
  for (const auto& m : kInfoMap) {
    std::string key = std::string("tag:") + m.tag;
    if (meta_.has(key)) info.emplace_back(m.id, meta_.text(key));
  }
  if (!info.empty()) {
    Chunk* target = nullptr;
    for (auto& c : cs)
      if (!std::memcmp(c.id, "LIST", 4) && isList(c.data, "INFO")) { target = &c; break; }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> fields;
    if (target) {
      const auto& d = target->data;
      size_t off = 4;
      while (off + 8 <= d.size()) {
        uint32_t sz = rd32le(d.data() + off + 4);
        if (sz > d.size() - off - 8) break;
        fields.emplace_back(std::string(reinterpret_cast<const char*>(d.data() + off), 4),
                            std::vector<uint8_t>(d.begin() + static_cast<ptrdiff_t>(off + 8), d.begin() + static_cast<ptrdiff_t>(off + 8 + sz)));
        off += 8 + sz + (sz & 1);
      }
    }
    for (const auto& [id, text] : info) {
      auto bytes = textBytes(text, true);
      bool found = false;
      for (auto& f : fields)
        if (f.first == id) {
          // keep identical payloads byte-exact (e.g. extra NUL padding written by other tools)
          if (fieldText(f.second.data(), f.second.size()) != text) f.second = bytes;
          found = true;
        }
      if (!found) fields.emplace_back(id, bytes);
    }
    Bytes l;
    l.fourcc("INFO");
    for (const auto& f : fields) putSubChunk(l, f.first.c_str(), f.second.data(), f.second.size());
    if (target) {
      target->data = std::move(l.v);
    } else {
      Chunk c{"gen:INFO", {'L', 'I', 'S', 'T'}, std::move(l.v)};
      cs.push_back(std::move(c));
    }
  }
  // bext from originator / origination_date
  const bool hasOrig = meta_.has("tag:originator"), hasDate = meta_.has("tag:origination_date");
  if (hasOrig || hasDate) {
    Chunk* b = nullptr;
    for (auto& c : cs)
      if (!std::memcmp(c.id, "bext", 4)) { b = &c; break; }
    if (!b) {
      cs.push_back(Chunk{"chunk:bext", {'b', 'e', 'x', 't'}, std::vector<uint8_t>(kBextFixed, 0)});
      b = &cs.back();
      wr16le(b->data.data() + 346, 1);  // version
    }
    if (b->data.size() < kBextFixed) b->data.resize(kBextFixed, 0);
    if (hasOrig) setField(b->data, 256, 32, meta_.text("tag:originator"));
    if (hasDate) {
      std::string d = meta_.text("tag:origination_date");
      setField(b->data, 320, 10, d.substr(0, 10));
      setField(b->data, 330, 8, d.size() > 11 ? d.substr(11, 8) : std::string());
    }
  }
  // markers -> cue + LIST/adtl + smpl loops (replacing raw ones)
  if (!markers_.empty()) {
    std::vector<uint8_t> rawSmpl;
    for (const auto& c : cs)
      if (!std::memcmp(c.id, "smpl", 4)) { rawSmpl = c.data; break; }
    RiffMarkerChunks mc = makeRiffMarkerChunks(markers_, rawSmpl, sampleRate_);
    std::vector<Chunk> kept;
    std::string adtlKey = "gen:adtl";
    for (auto& c : cs) {
      if (!std::memcmp(c.id, "cue ", 4)) continue;
      if (!std::memcmp(c.id, "LIST", 4) && isList(c.data, "adtl")) { adtlKey = c.key; continue; }
      if (!std::memcmp(c.id, "smpl", 4)) {
        if (!mc.smpl.empty()) { c.data = mc.smpl; mc.smpl.clear(); }
      }
      kept.push_back(std::move(c));
    }
    cs = std::move(kept);
    cs.push_back(Chunk{"chunk:cue ", {'c', 'u', 'e', ' '}, std::move(mc.cue)});
    if (!mc.adtl.empty()) cs.push_back(Chunk{adtlKey, {'L', 'I', 'S', 'T'}, std::move(mc.adtl)});
    if (!mc.smpl.empty()) cs.push_back(Chunk{"chunk:smpl", {'s', 'm', 'p', 'l'}, std::move(mc.smpl)});
  }
}

void Writer::aiffDerived(std::vector<Chunk>& cs) {
  for (const char* t : {"title", "artist", "copyright", "comment"}) {
    std::string key = std::string("tag:") + t;
    if (!meta_.has(key)) continue;
    const char* id = aiffTextId(t);
    auto bytes = textBytes(meta_.text(key), false);
    bool done = false;
    for (auto& c : cs)
      if (!std::memcmp(c.id, id, 4)) {
        if (fieldText(c.data.data(), c.data.size()) != meta_.text(key)) c.data = bytes;
        done = true;
        break;
      }
    if (!done) {
      Chunk c;
      c.key = std::string("chunk:") + id;
      std::memcpy(c.id, id, 4);
      c.data = bytes;
      cs.push_back(std::move(c));
    }
  }
  if (!markers_.empty()) {
    std::vector<sq_audio_file_marker> ms = markers_;
    std::vector<uint8_t> rawInst;
    std::vector<Chunk> kept;
    for (auto& c : cs) {
      if (!std::memcmp(c.id, "MARK", 4)) continue;
      if (!std::memcmp(c.id, "INST", 4)) { rawInst = c.data; continue; }
      kept.push_back(std::move(c));
    }
    cs = std::move(kept);
    if (ms.size() > 0x7FFE) ms.resize(0x7FFE);  // MARK ids are 1..0x7FFF; leave room for loop ends
    for (auto& m : ms) m.id = (m.id >= 1 && m.id <= 0x7FFF) ? m.id : 0;
    assignMarkerIds(ms);
    std::vector<bool> usedId(0x8000, false);
    for (const auto& m : ms) usedId[m.id & 0x7FFF] = true;
    uint32_t scan = 1;
    auto freeId = [&]() -> uint32_t {  // smallest unused MARK id, 0 when exhausted
      while (scan <= 0x7FFF && usedId[scan]) ++scan;
      if (scan > 0x7FFF) return 0;
      usedId[scan] = true;
      return scan;
    };
    Bytes mark;
    uint32_t count = 0;
    Bytes list;
    auto addMark = [&](uint32_t id, sq_sample_time pos, const char* name) {
      size_t n = std::min<size_t>(strnlen(name, 127), 255);
      list.u16be(id & 0x7FFF);
      list.u32be(clampU32(pos));
      list.u8(static_cast<uint32_t>(n));
      list.raw(name, n);
      if ((n + 1) & 1) list.u8(0);
      ++count;
    };
    struct L { uint32_t b, e; };
    std::vector<L> loops;
    for (const auto& m : ms) {
      addMark(m.id, m.position, m.name);
      if ((m.flags & SQ_MARKER_LOOP) && m.length > 0 && loops.size() < 2) {
        uint32_t endId = freeId();
        if (!endId) continue;
        addMark(endId, m.position + m.length, "");
        loops.push_back(L{m.id, endId});
      }
    }
    mark.u16be(count);
    mark.raw(list.v.data(), list.size());
    cs.push_back(Chunk{"chunk:MARK", {'M', 'A', 'R', 'K'}, std::move(mark.v)});
    if (!loops.empty() || !rawInst.empty()) {
      std::vector<uint8_t> inst(20, 0);
      if (rawInst.size() >= 20) std::memcpy(inst.data(), rawInst.data(), 8);
      else { inst[0] = 60; inst[2] = 0; inst[3] = 127; inst[4] = 1; inst[5] = 127; }
      for (size_t l = 0; l < loops.size(); ++l) {
        uint8_t* p = inst.data() + 8 + l * 6;
        wr16be(p, 1);  // forward
        wr16be(p + 2, loops[l].b);
        wr16be(p + 4, loops[l].e);
      }
      cs.push_back(Chunk{"chunk:INST", {'I', 'N', 'S', 'T'}, std::move(inst)});
    }
  }
}

// ---- finalize -----------------------------------------------------------------------------------
sq_result Writer::finalize() {
  if (finalized_) return SQ_S_FALSE;
  if (!configured_ || !out_) return SQ_E_CLOSED;
  if (failed_) return SQ_E_IO;
  sq_result r = SQ_OK;
  try {
    if (!started_) r = begin();
    if (SQ_FAILED(r)) { failed_ = true; return r; }
    IByteStream* s = out_.get();
    int64_t end = dataStart_ + static_cast<int64_t>(dataBytes_);
    static const uint8_t zero = 0;
    if ((dataBytes_ & 1) && !writeAt(s, end, &zero, 1)) return SQ_E_IO;
    end += static_cast<int64_t>(dataBytes_ & 1);
    if (SQ_FAILED(s->seek(end, SQ_SEEK_SET, nullptr))) return SQ_E_IO;

    std::vector<Chunk> cs = finalChunks();
    std::vector<bool> handled(pre_.size(), false);
    std::vector<const Chunk*> post;
    for (const auto& c : cs) {
      bool done = false;
      for (size_t i = 0; i < pre_.size(); ++i) {
        if (handled[i] || pre_[i].key != c.key) continue;
        handled[i] = true;
        if (pre_[i].data == c.data) { done = true; }
        else if (pre_[i].data.size() == c.data.size()) {
          if (!writeAt(s, pre_[i].offset + 8, c.data.data(), c.data.size())) return SQ_E_IO;
          done = true;
        } else {
          if (!writeAt(s, pre_[i].offset, "JUNK", 4)) return SQ_E_IO;
        }
        break;
      }
      if (!done) post.push_back(&c);
    }
    for (size_t i = 0; i < pre_.size(); ++i)
      if (!handled[i] && !writeAt(s, pre_[i].offset, "JUNK", 4)) return SQ_E_IO;
    if (SQ_FAILED(s->seek(end, SQ_SEEK_SET, nullptr))) return SQ_E_IO;
    for (const Chunk* c : post) {
      r = emitChunk(c->id, c->data);
      if (SQ_FAILED(r)) return r;
      end += 8 + static_cast<int64_t>(c->data.size() + (c->data.size() & 1));
    }
    r = kind_ == Kind::Wav ? finishWav(end) : finishAiff(end);
    if (SQ_FAILED(r)) return r;
    finalized_ = true;
    return out_.commit();
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result Writer::finishWav(int64_t end) {
  IByteStream* s = out_.get();
  const uint64_t riff = static_cast<uint64_t>(end) - 8;
  const bool big = riff > 0xFFFFFFFFull || dataBytes_ > 0xFFFFFFFFull;
  const bool rf64 = rf64Mode_ == 1 || (rf64Mode_ == -1 && big);
  if (big && !rf64) return SQ_E_UNSUPPORTED;
  uint8_t b[4];
  if (factOffset_ >= 0) {
    wr32le(b, static_cast<uint32_t>(std::min<uint64_t>(frames_, 0xFFFFFFFFu)));
    if (!writeAt(s, factOffset_, b, 4)) return SQ_E_IO;
  }
  if (rf64) {
    Bytes d;
    d.fourcc("ds64");
    d.u32le(28);
    d.u64le(riff);
    d.u64le(dataBytes_);
    d.u64le(frames_);
    d.u32le(0);
    uint8_t h[8];
    std::memcpy(h, "RF64", 4);
    wr32le(h + 4, 0xFFFFFFFFu);
    wr32le(b, 0xFFFFFFFFu);
    if (!writeAt(s, 0, h, 8) || !writeAt(s, 12, d.v.data(), d.size()) || !writeAt(s, dataHeader_ + 4, b, 4)) return SQ_E_IO;
  } else {
    wr32le(b, static_cast<uint32_t>(riff));
    if (!writeAt(s, 4, b, 4)) return SQ_E_IO;
    wr32le(b, static_cast<uint32_t>(dataBytes_));
    if (!writeAt(s, dataHeader_ + 4, b, 4)) return SQ_E_IO;
  }
  return SQ_OK;
}

sq_result Writer::finishAiff(int64_t end) {
  IByteStream* s = out_.get();
  uint8_t b[4];
  wr32be(b, static_cast<uint32_t>(end - 8));
  if (!writeAt(s, 4, b, 4)) return SQ_E_IO;
  wr32be(b, static_cast<uint32_t>(frames_));
  if (!writeAt(s, commOffset_ + 2, b, 4)) return SQ_E_IO;
  wr32be(b, static_cast<uint32_t>(dataBytes_ + 8));
  if (!writeAt(s, dataHeader_ + 4, b, 4)) return SQ_E_IO;
  return SQ_OK;
}

}  // namespace sq::wav
