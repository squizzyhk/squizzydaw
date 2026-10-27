// Metadata-only readers for the folder scanner: container headers and tag chunks of audio
// files (WAV/RF64/BW64, AIFF/AIFC, FLAC; other formats through a loaded codec service),
// Standard MIDI Files (header + meta events) and presets (ZIP with metainfo.xml, or plain XML).
// Nothing here decodes audio samples.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "miniz.h"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/codec.h"
#include "sq/platform.hpp"
#include "sqlite_index.hpp"

namespace sq::sqlite_index {

void FileInfo::setInt(const std::string& k, int64_t v) {
  MetaValue m;
  m.key = k;
  m.type = SQ_VALUE_INT;
  m.i = v;
  for (auto& e : meta)
    if (e.key == k) return void(e = std::move(m));
  meta.push_back(std::move(m));
}
void FileInfo::setDouble(const std::string& k, double v) {
  MetaValue m;
  m.key = k;
  m.type = SQ_VALUE_DOUBLE;
  m.d = v;
  for (auto& e : meta)
    if (e.key == k) return void(e = std::move(m));
  meta.push_back(std::move(m));
}
void FileInfo::setBool(const std::string& k, bool v) {
  MetaValue m;
  m.key = k;
  m.type = SQ_VALUE_BOOL;
  m.i = v ? 1 : 0;
  for (auto& e : meta)
    if (e.key == k) return void(e = std::move(m));
  meta.push_back(std::move(m));
}
void FileInfo::setString(const std::string& k, const std::string& v) {
  if (v.empty()) return;
  MetaValue m;
  m.key = k;
  m.type = SQ_VALUE_STRING;
  m.s = v;
  for (auto& e : meta)
    if (e.key == k) return void(e = std::move(m));
  meta.push_back(std::move(m));
}
const MetaValue* FileInfo::find(const std::string& k) const {
  for (const auto& e : meta)
    if (e.key == k) return &e;
  return nullptr;
}

namespace {

constexpr uint64_t kMaxChunkRead = 256u * 1024u;   // tag chunks larger than this are skipped
constexpr uint64_t kMaxMidiSize = 8u * 1024u * 1024u;
constexpr uint64_t kMaxPresetXml = 4u * 1024u * 1024u;

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// Text from a fixed-size, possibly NUL-padded field; trimmed, control chars dropped.
std::string cleanText(const char* p, size_t n) {
  std::string s;
  for (size_t k = 0; k < n && p[k]; ++k) {
    unsigned char c = static_cast<unsigned char>(p[k]);
    s += (c < 0x20) ? ' ' : static_cast<char>(c);
  }
  size_t b = s.find_first_not_of(' ');
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(' ');
  s = s.substr(b, e - b + 1);
  // keep valid UTF-8 only (legacy 8-bit text is dropped rather than stored broken)
  if (sq_utf8_validate(s.data(), s.size()) != SQ_TRUE) {
    std::string a;
    for (char c : s)
      if (static_cast<unsigned char>(c) < 0x80) a += c;
    return a;
  }
  return s;
}

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// IEEE 754 80-bit extended (AIFF sample rate).
double extended80(const uint8_t* p) {
  const int exponent = ((p[0] & 0x7F) << 8) | p[1];
  uint64_t mant = 0;
  for (int k = 0; k < 8; ++k) mant = (mant << 8) | p[2 + k];
  if (exponent == 0 && mant == 0) return 0.0;
  double v = std::ldexp(static_cast<double>(mant), exponent - 16383 - 63);
  return (p[0] & 0x80) ? -v : v;
}

class File {
 public:
  explicit File(const std::string& path) { sq_file_open(path.c_str(), SQ_FILE_READ, &f_); }
  ~File() {
    if (f_) sq_file_close(f_);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  bool ok() const { return f_ != nullptr; }
  uint64_t size() {
    uint64_t s = 0;
    if (f_) sq_file_size(f_, &s);
    return s;
  }
  bool seek(uint64_t pos) { return f_ && sq_file_seek(f_, static_cast<int64_t>(pos), SQ_SEEK_SET, nullptr) == SQ_OK; }
  // Relative skip; false if it would end past EOF.
  bool skip(uint64_t n) {
    uint64_t at = 0;
    if (!f_ || sq_file_seek(f_, static_cast<int64_t>(n), SQ_SEEK_CUR, &at) != SQ_OK) return false;
    return at <= size();
  }
  bool read(void* buf, size_t n) {
    size_t got = 0;
    return f_ && sq_file_read(f_, buf, n, &got) == SQ_OK && got == n;
  }
  size_t readSome(void* buf, size_t n) {
    size_t got = 0;
    if (!f_ || sq_file_read(f_, buf, n, &got) != SQ_OK) return 0;
    return got;
  }
  bool readVec(std::vector<uint8_t>& v, size_t n) {
    v.resize(n);
    return n == 0 || read(v.data(), n);
  }

 private:
  sq_file* f_ = nullptr;
};

void addTag(FileInfo& out, std::string t) {
  t = lower(t);
  size_t b = t.find_first_not_of(' ');
  if (b == std::string::npos) return;
  t = t.substr(b, t.find_last_not_of(' ') - b + 1);
  for (char& c : t)
    if (c == ';') c = ',';
  if (std::find(out.tags.begin(), out.tags.end(), t) == out.tags.end()) out.tags.push_back(t);
}

void setTextTag(FileInfo& out, const std::string& key, const std::string& value) {
  if (value.empty()) return;
  out.setString(key, value);
  if (key == "title" && out.title.empty()) out.title = value;
}

// Normalizes a free-form tag name ("TITLE", "INAM", "TBPM"...) to our text tag keys.
const char* textTagKey(const std::string& name) {
  const std::string n = lower(name);
  if (n == "title" || n == "inam" || n == "name") return "title";
  if (n == "artist" || n == "iart" || n == "auth" || n == "author") return "artist";
  if (n == "genre" || n == "ignr") return "genre";
  if (n == "comment" || n == "comments" || n == "icmt" || n == "anno") return "comment";
  if (n == "description") return "description";
  if (n == "bpm" || n == "tempo") return "bpm";
  if (n == "key" || n == "initialkey") return "key";
  if (n == "album" || n == "iprd") return "album";
  if (n == "copyright" || n == "icop" || n == "(c) ") return "copyright";
  if (n == "keywords" || n == "ikey") return "keywords";
  return nullptr;
}

// ---- WAV / RF64 / BW64 -------------------------------------------------------------------

void parseInfoList(const std::vector<uint8_t>& d, FileInfo& out) {
  if (d.size() < 4 || std::memcmp(d.data(), "INFO", 4) != 0) return;
  size_t p = 4;
  while (p + 8 <= d.size()) {
    std::string id(reinterpret_cast<const char*>(d.data() + p), 4);
    uint32_t len = le32(d.data() + p + 4);
    p += 8;
    if (len > d.size() - p) break;
    if (const char* key = textTagKey(id)) setTextTag(out, key, cleanText(reinterpret_cast<const char*>(d.data() + p), len));
    p += len + (len & 1);
  }
}

bool parseRiff(File& f, FileInfo& out) {
  uint8_t hdr[12];
  if (!f.seek(0) || !f.read(hdr, 12)) return false;
  const bool rf64 = std::memcmp(hdr, "RF64", 4) == 0 || std::memcmp(hdr, "BW64", 4) == 0;
  if ((std::memcmp(hdr, "RIFF", 4) != 0 && !rf64) || std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;
  out.setString("audio.container", rf64 ? "rf64" : "wav");
  const uint64_t fileSize = f.size();
  uint64_t pos = 12, dataSize = 0, ds64Data = 0;
  uint32_t channels = 0, rate = 0, bits = 0, blockAlign = 0;
  bool haveData = false;
  for (int guard = 0; guard < 4096 && pos + 8 <= fileSize; ++guard) {
    uint8_t ch[8];
    if (!f.seek(pos) || !f.read(ch, 8)) break;
    uint64_t len = le32(ch + 4);
    const uint64_t body = pos + 8;
    if (std::memcmp(ch, "ds64", 4) == 0 && len >= 24) {
      uint8_t b[24];
      if (f.read(b, 24)) ds64Data = le64(b + 8);
    } else if (std::memcmp(ch, "fmt ", 4) == 0 && len >= 16) {
      uint8_t b[16];
      if (f.read(b, 16)) {
        channels = le16(b + 2);
        rate = le32(b + 4);
        blockAlign = le16(b + 12);
        bits = le16(b + 14);
      }
    } else if (std::memcmp(ch, "data", 4) == 0) {
      haveData = true;
      if (len == 0xFFFFFFFFu && ds64Data) len = ds64Data;
      // Never trust a size beyond the end of the file (truncated / streamed / hostile headers).
      dataSize = std::min<uint64_t>(len, fileSize > body ? fileSize - body : 0);
    } else if (std::memcmp(ch, "LIST", 4) == 0 && len <= kMaxChunkRead) {
      std::vector<uint8_t> d;
      if (f.readVec(d, static_cast<size_t>(len))) parseInfoList(d, out);
    } else if (std::memcmp(ch, "bext", 4) == 0 && len >= 256 + 32) {
      char b[256 + 32];
      if (f.read(b, sizeof b)) {
        setTextTag(out, "description", cleanText(b, 256));
        setTextTag(out, "originator", cleanText(b + 256, 32));
      }
    } else if (std::memcmp(ch, "smpl", 4) == 0 && len >= 36) {
      uint8_t b[36];
      if (f.read(b, 36)) {
        out.setInt("audio.root_note", le32(b + 12));
        if (le32(b + 28) > 0) out.setBool("audio.loop", true);
      }
    }
    if (len >= fileSize - body) break;  // chunk runs to / past EOF (also avoids 64-bit wrap)
    pos = body + len + (len & 1);
  }
  if (channels) out.setInt("audio.channels", channels);
  if (rate) out.setInt("audio.sample_rate", rate);
  if (bits) out.setInt("audio.bits", bits);
  if (haveData && blockAlign) {
    const uint64_t frames = dataSize / blockAlign;
    out.setInt("audio.frames", static_cast<int64_t>(frames));
    if (rate) out.setDouble("audio.duration", static_cast<double>(frames) / rate);
  }
  return true;
}

// ---- AIFF / AIFC ---------------------------------------------------------------------------

bool parseAiff(File& f, FileInfo& out) {
  uint8_t hdr[12];
  if (!f.seek(0) || !f.read(hdr, 12)) return false;
  if (std::memcmp(hdr, "FORM", 4) != 0 || (std::memcmp(hdr + 8, "AIFF", 4) != 0 && std::memcmp(hdr + 8, "AIFC", 4) != 0))
    return false;
  out.setString("audio.container", std::memcmp(hdr + 8, "AIFC", 4) == 0 ? "aifc" : "aif");
  const uint64_t fileSize = f.size();
  uint64_t pos = 12;
  for (int guard = 0; guard < 4096 && pos + 8 <= fileSize; ++guard) {
    uint8_t ch[8];
    if (!f.seek(pos) || !f.read(ch, 8)) break;
    const uint64_t len = be32(ch + 4);
    const std::string id(reinterpret_cast<const char*>(ch), 4);
    if (id == "COMM" && len >= 18) {
      uint8_t b[18];
      if (f.read(b, 18)) {
        const uint32_t channels = be16(b);
        const uint32_t frames = be32(b + 2);
        const uint32_t bits = be16(b + 6);
        const double rate = extended80(b + 8);
        out.setInt("audio.channels", channels);
        out.setInt("audio.frames", frames);
        out.setInt("audio.bits", bits);
        if (rate > 0 && rate < 1e7) {
          out.setInt("audio.sample_rate", static_cast<int64_t>(std::llround(rate)));
          out.setDouble("audio.duration", frames / rate);
        }
      }
    } else if ((id == "NAME" || id == "AUTH" || id == "ANNO" || id == "(c) ") && len <= kMaxChunkRead) {
      std::vector<uint8_t> d;
      if (f.readVec(d, static_cast<size_t>(len)))
        if (const char* key = textTagKey(id)) setTextTag(out, key, cleanText(reinterpret_cast<const char*>(d.data()), d.size()));
    } else if (id == "INST" && len >= 20) {
      uint8_t b[20];
      if (f.read(b, 20)) {
        out.setInt("audio.root_note", b[0]);
        if (be16(b + 8) != 0) out.setBool("audio.loop", true);  // sustain loop play mode
      }
    }
    pos = pos + 8 + len + (len & 1);
  }
  return true;
}

// ---- FLAC ---------------------------------------------------------------------------------

void parseVorbisComments(const std::vector<uint8_t>& d, FileInfo& out) {
  size_t p = 0;
  if (d.size() < 8) return;
  const uint32_t vendorLen = le32(d.data());
  p = 4 + static_cast<size_t>(vendorLen);
  if (p + 4 > d.size()) return;
  uint32_t count = le32(d.data() + p);
  p += 4;
  for (uint32_t k = 0; k < count && p + 4 <= d.size(); ++k) {
    const uint32_t len = le32(d.data() + p);
    p += 4;
    if (len > d.size() - p) break;
    std::string entry(reinterpret_cast<const char*>(d.data() + p), len);
    p += len;
    const size_t eq = entry.find('=');
    if (eq == std::string::npos) continue;
    if (const char* key = textTagKey(entry.substr(0, eq)))
      setTextTag(out, key, cleanText(entry.data() + eq + 1, entry.size() - eq - 1));
  }
}

bool parseFlac(File& f, FileInfo& out) {
  uint8_t magic[4];
  if (!f.seek(0) || !f.read(magic, 4) || std::memcmp(magic, "fLaC", 4) != 0) return false;
  out.setString("audio.container", "flac");
  for (int guard = 0; guard < 128; ++guard) {
    uint8_t bh[4];
    if (!f.read(bh, 4)) break;
    const bool last = (bh[0] & 0x80) != 0;
    const uint32_t type = bh[0] & 0x7F;
    const uint32_t len = (bh[1] << 16) | (bh[2] << 8) | bh[3];
    std::vector<uint8_t> d;
    if ((type == 0 || type == 4) && len <= kMaxChunkRead) {
      if (!f.readVec(d, len)) break;
      if (type == 0 && len >= 18) {
        const uint32_t rate = (d[10] << 12) | (d[11] << 4) | (d[12] >> 4);
        const uint32_t channels = ((d[12] >> 1) & 0x07) + 1;
        const uint32_t bits = (((d[12] & 0x01) << 4) | (d[13] >> 4)) + 1;
        const uint64_t frames = (static_cast<uint64_t>(d[13] & 0x0F) << 32) | be32(d.data() + 14);
        out.setInt("audio.sample_rate", rate);
        out.setInt("audio.channels", channels);
        out.setInt("audio.bits", bits);
        if (frames) {
          out.setInt("audio.frames", static_cast<int64_t>(frames));
          if (rate) out.setDouble("audio.duration", static_cast<double>(frames) / rate);
        }
      } else if (type == 4) {
        parseVorbisComments(d, out);
      }
    } else if (!f.skip(len)) {  // pictures / padding / seek tables: seek, never read
      break;
    }
    if (last) break;
  }
  return true;
}

// ---- other audio through a loaded codec service --------------------------------------------

void probeWithCodec(const std::string& path, const std::string& ext, FileInfo& out) {
  IClassRegistry* reg = classRegistry();
  if (!reg) return;
  Ptr<ICodecService> codec = Ptr<ICodecService>::adopt(codec::findCodec(reg, ext.c_str(), nullptr, SQ_CODEC_READ));
  if (!codec) return;
  Ptr<IAudioFileReader> reader;
  if (codec->createReader(reader.put()) != SQ_OK || !reader) return;
  if (reader->open(path.c_str()) != SQ_OK) return;
  sq_audio_file_format fmt{};
  fmt.struct_size = sizeof fmt;
  if (reader->getFormat(&fmt) == SQ_OK) {
    if (fmt.container[0]) out.setString("audio.container", cleanText(fmt.container, sizeof fmt.container));
    if (fmt.sample_rate > 0) out.setInt("audio.sample_rate", static_cast<int64_t>(std::llround(fmt.sample_rate)));
    if (fmt.channels) out.setInt("audio.channels", fmt.channels);
    if (fmt.bits_per_sample) out.setInt("audio.bits", fmt.bits_per_sample);
    if (fmt.frame_count > 0) {
      out.setInt("audio.frames", fmt.frame_count);
      if (fmt.sample_rate > 0) out.setDouble("audio.duration", static_cast<double>(fmt.frame_count) / fmt.sample_rate);
    }
  }
  const uint32_t n = reader->getMetadataCount();
  for (uint32_t k = 0; k < n && k < 256; ++k) {
    char key[128];
    uint32_t need = 0;
    if (reader->getMetadataKey(k, key, sizeof key, &need) != SQ_OK) continue;
    if (std::strncmp(key, "tag:", 4) != 0) continue;
    const char* tk = textTagKey(key + 4);
    if (!tk) continue;
    need = 0;
    if (reader->getMetadata(key, nullptr, 0, &need) != SQ_E_BUFFER_TOO_SMALL || need == 0 || need > 64 * 1024) continue;
    std::string v(need, '\0');
    if (reader->getMetadata(key, v.data(), need, &need) != SQ_OK) continue;
    setTextTag(out, tk, cleanText(v.data(), v.size()));
  }
}

// ---- Standard MIDI File ------------------------------------------------------------------

const char* keyName(int sf, bool minor) {
  static const char* kMajor[] = {"Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#"};
  static const char* kMinor[] = {"Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#"};
  if (sf < -7 || sf > 7) return nullptr;
  return minor ? kMinor[sf + 7] : kMajor[sf + 7];
}

bool readVlq(const std::vector<uint8_t>& d, size_t& p, size_t end, uint32_t& v) {
  v = 0;
  for (int k = 0; k < 4; ++k) {
    if (p >= end) return false;
    const uint8_t b = d[p++];
    v = (v << 7) | (b & 0x7F);
    if (!(b & 0x80)) return true;
  }
  return false;
}

bool parseMidi(const std::string& path, FileInfo& out) {
  File f(path);
  if (!f.ok()) return false;
  const uint64_t size = f.size();
  if (size < 14 || size > kMaxMidiSize) return size >= 14;  // too large: file info only
  std::vector<uint8_t> d;
  if (!f.readVec(d, static_cast<size_t>(size))) return false;
  size_t p = 0;
  // RIFF-wrapped MIDI ("RMID")
  if (d.size() >= 20 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "RMID", 4) == 0) p = 20;
  if (p + 14 > d.size() || std::memcmp(d.data() + p, "MThd", 4) != 0) return true;
  const uint32_t hlen = be32(d.data() + p + 4);
  const uint16_t format = be16(d.data() + p + 8);
  const uint16_t ntrks = be16(d.data() + p + 10);
  const uint16_t division = be16(d.data() + p + 12);
  out.setInt("midi.format", format);
  out.setInt("midi.tracks", ntrks);
  const bool smpte = (division & 0x8000) != 0;
  if (!smpte) out.setInt("midi.ppq", division);
  p += 8 + static_cast<size_t>(hlen);

  struct Tempo {
    uint64_t tick;
    uint32_t uspq;
  };
  std::vector<Tempo> tempos;
  std::string trackNames;
  uint64_t lengthTicks = 0, notes = 0;
  bool haveSig = false, haveKey = false;
  for (uint32_t t = 0; t < ntrks && p + 8 <= d.size(); ++t) {
    if (std::memcmp(d.data() + p, "MTrk", 4) != 0) {
      p += 8 + static_cast<size_t>(be32(d.data() + p + 4));  // skip alien chunk
      --t;
      continue;
    }
    const size_t end = std::min(d.size(), p + 8 + static_cast<size_t>(be32(d.data() + p + 4)));
    size_t q = p + 8;
    uint64_t tick = 0;
    uint8_t running = 0;
    bool named = false;
    while (q < end) {
      uint32_t delta;
      if (!readVlq(d, q, end, delta)) break;
      tick += delta;
      if (q >= end) break;
      uint8_t status = d[q];
      if (status == 0xFF) {
        if (q + 2 > end) break;
        const uint8_t type = d[q + 1];
        q += 2;
        uint32_t len;
        if (!readVlq(d, q, end, len) || len > end - q) break;
        const uint8_t* m = d.data() + q;
        if (type == 0x03 && !named && len) {
          std::string n = cleanText(reinterpret_cast<const char*>(m), len);
          if (!n.empty()) {
            if (!trackNames.empty()) trackNames += "; ";
            trackNames += n;
            named = true;
          }
        } else if (type == 0x51 && len == 3) {
          tempos.push_back({tick, static_cast<uint32_t>((m[0] << 16) | (m[1] << 8) | m[2])});
        } else if (type == 0x58 && len >= 2 && !haveSig) {
          out.setString("midi.signature", std::to_string(m[0]) + "/" + std::to_string(1u << std::min<uint32_t>(m[1], 6)));
          haveSig = true;
        } else if (type == 0x59 && len == 2 && !haveKey) {
          if (const char* k = keyName(static_cast<int8_t>(m[0]), m[1] != 0)) {
            out.setString("midi.key", std::string(k) + (m[1] ? " minor" : " major"));
            haveKey = true;
          }
        } else if (type == 0x2F) {
          q += len;
          break;
        }
        q += len;
        running = 0;
      } else if (status == 0xF0 || status == 0xF7) {
        ++q;
        uint32_t len;
        if (!readVlq(d, q, end, len) || len > end - q) break;
        q += len;
        running = 0;
      } else {
        if (status & 0x80) {
          running = status;
          ++q;
        } else if (!running) {
          break;  // corrupt: data byte without status
        }
        const uint8_t kind = running & 0xF0;
        const size_t need = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
        if (q + need > end) break;
        if (kind == 0x90 && d[q + 1] != 0) ++notes;
        q += need;
      }
    }
    lengthTicks = std::max(lengthTicks, tick);
    p = end;
  }
  out.setInt("midi.length_ticks", static_cast<int64_t>(lengthTicks));
  out.setInt("midi.notes", static_cast<int64_t>(notes));
  if (!trackNames.empty()) out.setString("midi.track_names", trackNames);
  std::stable_sort(tempos.begin(), tempos.end(), [](const Tempo& a, const Tempo& b) { return a.tick < b.tick; });
  const uint32_t firstUspq = tempos.empty() ? 0u : tempos.front().uspq;  // no event: unknown
  if (firstUspq) out.setDouble("midi.tempo", std::round(60000000.0 / firstUspq * 1000.0) / 1000.0);
  if (!smpte && division) {
    double seconds = 0.0;
    uint64_t lastTick = 0;
    uint32_t uspq = 500000;
    for (const Tempo& tp : tempos) {
      if (tp.tick >= lengthTicks) break;
      seconds += static_cast<double>(tp.tick - lastTick) * uspq / (1e6 * division);
      lastTick = tp.tick;
      uspq = tp.uspq;
    }
    seconds += static_cast<double>(lengthTicks - lastTick) * uspq / (1e6 * division);
    out.setDouble("midi.duration", seconds);
  } else if (smpte) {
    const int fps = -static_cast<int8_t>(division >> 8);
    const int tpf = division & 0xFF;
    if (fps > 0 && tpf > 0) out.setDouble("midi.duration", static_cast<double>(lengthTicks) / (fps * tpf));
  }
  return true;
}

// ---- presets (ZIP + metainfo.xml, or plain XML) ------------------------------------------

std::string lastSegment(const std::string& key) {
  const size_t p = key.find_last_of(":.");
  return lower(p == std::string::npos ? key : key.substr(p + 1));
}

void collectXml(const XmlNode& n, FileInfo& out, int depth) {
  if (depth > 6) return;
  for (XmlNode c : n.children()) {
    std::string key, value;
    for (const char* k : {"id", "name", "key"}) {
      if (c.hasAttr(k) && c.hasAttr("value")) {
        key = c.attr(k);
        value = c.attr("value");
        break;
      }
    }
    if (key.empty() && !c.child() && !c.text().empty()) {
      key = std::string(c.name());
      value = std::string(c.text());
    }
    if (!key.empty()) {
      value = cleanText(value.data(), value.size());
      if (!value.empty()) out.setString("preset." + key, value);
    }
    collectXml(c, out, depth + 1);
  }
}

void applyPresetKeys(FileInfo& out) {
  std::vector<std::pair<std::string, std::string>> kv;
  for (const auto& m : out.meta)
    if (m.type == SQ_VALUE_STRING && m.key.rfind("preset.", 0) == 0) kv.emplace_back(lastSegment(m.key.substr(7)), m.s);
  for (const auto& [k, v] : kv) {
    if ((k == "title" || k == "name") && out.title.empty()) out.title = v;
    if (k == "tags" || k == "keywords") {
      std::string cur;
      for (char c : v + ";") {
        if (c == ';' || c == ',') {
          addTag(out, cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
    }
    if (k == "category" || k == "subcategory" || k == "genre" || k == "instrument") addTag(out, v);
    if (const char* tk = textTagKey(k)) {
      if (std::string(tk) != "title") out.setString(tk, v);
    }
  }
}

bool parsePreset(const std::string& path, FileInfo& out) {
  File f(path);
  if (!f.ok()) return false;
  uint8_t magic[4] = {};
  const size_t got = f.readSome(magic, 4);
  std::string xml;
  if (got == 4 && magic[0] == 'P' && magic[1] == 'K') {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return true;
    int idx = mz_zip_reader_locate_file(&zip, "metainfo.xml", nullptr, 0);
    if (idx >= 0) {
      mz_zip_archive_file_stat st;
      if (mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(idx), &st) && st.m_uncomp_size <= kMaxPresetXml) {
        size_t n = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(idx), &n, 0);
        if (p) {
          xml.assign(static_cast<const char*>(p), n);
          mz_free(p);
        }
      }
    }
    mz_zip_reader_end(&zip);
  } else if (got >= 1 && (magic[0] == '<' || magic[0] == 0xEF)) {
    if (f.size() <= kMaxPresetXml) {
      auto data = readFile(path);
      if (data) xml = std::move(*data);
    }
  }
  if (xml.empty()) return true;
  auto doc = xmlParse(xml);
  if (!doc) return true;
  XmlNode root = xmlRoot(doc);
  // root attributes count as metadata too
  if (sq_xml_node* r = root.raw()) {
    const uint32_t n = sq_xml_attr_count(r);
    for (uint32_t k = 0; k < n; ++k) {
      std::string v = sq_xml_attr_value(r, k);
      out.setString(std::string("preset.") + sq_xml_attr_name(r, k), cleanText(v.data(), v.size()));
    }
  }
  collectXml(root, out, 0);
  applyPresetKeys(out);
  return true;
}

std::string extensionOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  return lower(path.substr(dot + 1));
}

// "Kick 128bpm.wav", "loop_95_BPM" -> 128 / 95
int bpmFromName(const std::string& name) {
  const std::string n = lower(name);
  size_t p = n.find("bpm");
  while (p != std::string::npos) {
    size_t e = p;
    while (e > 0 && (n[e - 1] == ' ' || n[e - 1] == '_' || n[e - 1] == '-')) --e;
    size_t b = e;
    while (b > 0 && n[b - 1] >= '0' && n[b - 1] <= '9') --b;
    if (e - b >= 2 && e - b <= 3) {
      const int v = std::stoi(n.substr(b, e - b));
      if (v >= 40 && v <= 300) return v;
    }
    p = n.find("bpm", p + 3);
  }
  return 0;
}

}  // namespace

const char* kindForExtension(const std::string& ext) {
  static const char* kAudio[] = {"wav", "wave", "bwf", "rf64", "w64", "aif", "aiff", "aifc", "flac",
                                 "mp3", "ogg", "oga", "opus", "m4a", "aac", "caf"};
  static const char* kMidi[] = {"mid", "midi", "smf", "rmi"};
  static const char* kPreset[] = {"preset", "multipreset", "trackpreset", "instrument", "fxchain"};
  for (const char* e : kAudio)
    if (ext == e) return "audio";
  for (const char* e : kMidi)
    if (ext == e) return "midi";
  for (const char* e : kPreset)
    if (ext == e) return "preset";
  return nullptr;
}

bool extractFileInfo(const std::string& path, FileInfo& out) {
  const std::string ext = extensionOf(path);
  const char* kind = kindForExtension(ext);
  if (!kind) return false;
  out.kind = kind;
  if (!sq_fs_is_file(path.c_str())) return false;
  const size_t slash = path.find_last_of("/\\");
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::string stem = name.substr(0, name.size() - ext.size() - 1);
  out.setString("file.name", name);
  out.setString("file.ext", ext);

  bool ok = true;
  if (out.kind == "audio") {
    File f(path);
    if (!f.ok()) return false;
    bool parsed = false;
    if (ext == "wav" || ext == "wave" || ext == "bwf" || ext == "rf64") parsed = parseRiff(f, out);
    else if (ext == "aif" || ext == "aiff" || ext == "aifc") parsed = parseAiff(f, out);
    else if (ext == "flac") parsed = parseFlac(f, out);
    if (!parsed) probeWithCodec(path, ext, out);
  } else if (out.kind == "midi") {
    ok = parseMidi(path, out);
  } else {
    ok = parsePreset(path, out);
  }
  if (!ok) return false;

  if (out.title.empty()) out.title = stem;
  // automatic tags
  addTag(out, out.kind);
  addTag(out, ext);
  int bpm = 0;
  if (const MetaValue* m = out.find("bpm")) bpm = std::atoi(m->s.c_str());
  if (!bpm) bpm = bpmFromName(stem);
  if (const MetaValue* m = out.find("midi.tempo"); m && !bpm) bpm = static_cast<int>(std::lround(m->d));
  if (bpm >= 20 && bpm <= 400) {
    if (!out.find("bpm")) out.setString("bpm", std::to_string(bpm));
    addTag(out, std::to_string(bpm) + "bpm");
  }
  if (const MetaValue* m = out.find("genre")) addTag(out, m->s);
  const std::string lstem = lower(stem);
  if (const MetaValue* m = out.find("audio.loop"); (m && m->i) || lstem.find("loop") != std::string::npos) addTag(out, "loop");
  return true;
}

}  // namespace sq::sqlite_index
