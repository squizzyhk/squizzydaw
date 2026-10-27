// codec-mp3: ID3v2.2/2.3/2.4 + ID3v1 reader and ID3v2.4 writer (own code).
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "sqcodec/common.hpp"

namespace sq::mp3 {

using cx::MetaStore;

inline uint32_t syncsafe(const uint8_t* p) {
  return ((p[0] & 0x7Fu) << 21) | ((p[1] & 0x7Fu) << 14) | ((p[2] & 0x7Fu) << 7) | (p[3] & 0x7Fu);
}
inline void putSyncsafe(cx::Bytes& b, uint32_t v) {
  b.u8((v >> 21) & 0x7F); b.u8((v >> 14) & 0x7F); b.u8((v >> 7) & 0x7F); b.u8(v & 0x7F);
}

inline std::vector<uint8_t> unsync(const uint8_t* p, size_t n) {
  std::vector<uint8_t> o;
  o.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    o.push_back(p[i]);
    if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) ++i;
  }
  return o;
}

// One encoded text string -> UTF-8. Consumes up to the terminator; `used` gets bytes consumed.
inline std::string id3Text(uint8_t enc, const uint8_t* p, size_t n, size_t* used = nullptr) {
  std::string out;
  size_t i = 0;
  if (enc == 1 || enc == 2) {
    bool be = enc == 2;
    if (enc == 1 && n >= 2) {
      if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
      else if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
    }
    std::u16string u;
    for (; i + 1 < n; i += 2) {
      char16_t c = static_cast<char16_t>(be ? (p[i] << 8) | p[i + 1] : (p[i + 1] << 8) | p[i]);
      if (c == 0) { i += 2; break; }
      u.push_back(c);
    }
    for (size_t k = 0; k < u.size(); ++k) {
      uint32_t cp = u[k];
      if (cp >= 0xD800 && cp < 0xDC00 && k + 1 < u.size() && u[k + 1] >= 0xDC00 && u[k + 1] < 0xE000) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (u[k + 1] - 0xDC00);
        ++k;
      } else if (cp >= 0xD800 && cp < 0xE000) {
        cp = 0xFFFD;
      }
      if (cp < 0x80) out += static_cast<char>(cp);
      else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
      else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
  } else {
    size_t e = 0;
    while (e < n && p[e]) ++e;
    std::string s(reinterpret_cast<const char*>(p), e);
    out = cx::toUtf8Lenient(s);  // UTF-8 kept, Latin-1 converted
    i = e < n ? e + 1 : e;
  }
  if (used) *used = i;
  return out;
}

inline const char* id3TagName(const std::string& id) {
  struct M { const char* v34; const char* v22; const char* tag; };
  static const M kMap[] = {{"TIT2", "TT2", "title"}, {"TPE1", "TP1", "artist"}, {"TALB", "TAL", "album"},
                           {"TCON", "TCO", "genre"}, {"TDRC", "TYE", "date"},   {"TYER", "", "date"},
                           {"TRCK", "TRK", "track"}, {"TCOP", "TCR", "copyright"}, {"TSSE", "TSS", "encoder"},
                           {"TBPM", "TBP", "bpm"},   {"TKEY", "TKE", "key"},    {"TSRC", "TRC", "isrc"}};
  for (const auto& m : kMap)
    if (id == m.v34 || (*m.v22 && id == m.v22)) return m.tag;
  return nullptr;
}
inline const char* id3FrameFor(const std::string& tag) {
  static const char* const kMap[][2] = {{"title", "TIT2"}, {"artist", "TPE1"}, {"album", "TALB"}, {"genre", "TCON"},
                                        {"date", "TDRC"},  {"track", "TRCK"},  {"copyright", "TCOP"}, {"encoder", "TSSE"},
                                        {"bpm", "TBPM"},   {"key", "TKEY"},    {"isrc", "TSRC"}};
  for (const auto& m : kMap)
    if (tag == m[0]) return m[1];
  return nullptr;
}

struct Chapter {
  uint32_t startMs = 0, endMs = 0;
  std::string name;
};

class Id3Reader {
 public:
  // Whole tag including the 10-byte header.
  void parse(const uint8_t* tag, size_t n, MetaStore& meta, std::vector<Chapter>& chapters) {
    if (n < 10 || std::memcmp(tag, "ID3", 3) != 0) return;
    const uint8_t ver = tag[3];
    if (ver < 2 || ver > 4) return;
    const uint8_t flags = tag[5];
    size_t size = std::min<size_t>(syncsafe(tag + 6), n - 10);
    std::vector<uint8_t> body(tag + 10, tag + 10 + size);
    if ((flags & 0x80) && ver < 4) body = unsync(body.data(), body.size());
    size_t off = 0;
    if ((flags & 0x40) && ver >= 3 && body.size() >= 4) {
      uint32_t ext = ver == 3 ? cx::rd32be(body.data()) + 4 : syncsafe(body.data());
      off = ext;
    }
    frames(ver, body.data(), body.size(), off, meta, chapters, 0);
  }

 private:
  void frames(uint8_t ver, const uint8_t* b, size_t n, size_t off, MetaStore& meta, std::vector<Chapter>& chapters, int depth) {
    const size_t hdr = ver == 2 ? 6 : 10;
    int guard = 0;
    while (off + hdr <= n && ++guard < 4096) {
      if (b[off] == 0) break;  // padding
      std::string id(reinterpret_cast<const char*>(b + off), ver == 2 ? 3 : 4);
      size_t fsz = ver == 2 ? (static_cast<size_t>(b[off + 3]) << 16) | (b[off + 4] << 8) | b[off + 5]
                 : ver == 3 ? cx::rd32be(b + off + 4)
                            : syncsafe(b + off + 4);
      uint16_t fflags = ver == 2 ? 0 : cx::rd16be(b + off + 8);
      off += hdr;
      if (fsz > n - off) break;
      const uint8_t* d = b + off;
      std::vector<uint8_t> tmp;
      size_t dn = fsz;
      bool skip = false;
      if (ver == 3) {
        if (fflags & 0xC0) skip = true;  // compressed / encrypted
        if (!skip && (fflags & 0x20) && dn) { ++d; --dn; }
      } else if (ver == 4) {
        if (fflags & 0x0C) skip = true;
        if (!skip && (fflags & 0x40) && dn) { ++d; --dn; }      // group id
        if (!skip && (fflags & 0x01) && dn >= 4) { d += 4; dn -= 4; }  // data length indicator
        if (!skip && (fflags & 0x02)) { tmp = unsync(d, dn); d = tmp.data(); dn = tmp.size(); }
      }
      if (!skip && dn) frame(ver, id, d, dn, meta, chapters, depth);
      off += fsz;
    }
  }
  void frame(uint8_t ver, const std::string& id, const uint8_t* d, size_t n, MetaStore& meta, std::vector<Chapter>& chapters, int depth) {
    if (const char* t = id3TagName(id)) {
      std::string v = joined(d[0], d + 1, n - 1);
      std::string key = std::string("tag:") + t;
      if (!v.empty() && !meta.has(key)) meta.setText(key, v);
    } else if ((id == "COMM" || id == "COM") && n >= 4) {
      size_t used = 0;
      id3Text(d[0], d + 4, n - 4, &used);  // short description
      std::string v = id3Text(d[0], d + 4 + used, n - 4 - used);
      if (!v.empty() && !meta.has("tag:comment")) meta.setText("tag:comment", v);
    } else if ((id == "TXXX" || id == "TXX") && n >= 2) {
      size_t used = 0;
      std::string desc = cx::lower(id3Text(d[0], d + 1, n - 1, &used));
      std::string v = id3Text(d[0], d + 1 + used, n - 1 - used);
      if (!desc.empty() && desc.find('=') == std::string::npos && !meta.has("tag:" + desc)) meta.setText("tag:" + desc, v);
    } else if (id == "CHAP" && depth == 0 && ver >= 3) {
      size_t e = 0;
      while (e < n && d[e]) ++e;
      if (e + 1 + 16 > n) return;
      const uint8_t* q = d + e + 1;
      Chapter c;
      c.startMs = cx::rd32be(q);
      c.endMs = cx::rd32be(q + 4);
      MetaStore sub;
      std::vector<Chapter> none;
      frames(ver, q + 16, n - (e + 1 + 16), 0, sub, none, depth + 1);
      c.name = sub.text("tag:title");
      chapters.push_back(c);
    }
  }
  // v2.4 allows several NUL-separated values in one text frame
  static std::string joined(uint8_t enc, const uint8_t* p, size_t n) {
    std::string out;
    size_t off = 0;
    int guard = 0;
    while (off < n && ++guard < 64) {
      size_t used = 0;
      std::string s = id3Text(enc, p + off, n - off, &used);
      if (used == 0) break;
      if (!s.empty()) out += (out.empty() ? "" : "; ") + s;
      off += used;
    }
    return out;
  }
};

inline void parseId3v1(const uint8_t* t, MetaStore& meta) {
  if (std::memcmp(t, "TAG", 3) != 0) return;
  auto set = [&](const char* key, const uint8_t* p, size_t n) {
    std::string v = cx::toUtf8Lenient(cx::fieldText(p, n));
    if (!v.empty() && !meta.has(key)) meta.setText(key, v);
  };
  set("tag:title", t + 3, 30);
  set("tag:artist", t + 33, 30);
  set("tag:album", t + 63, 30);
  set("tag:date", t + 93, 4);
  set("tag:comment", t + 97, t[125] == 0 && t[126] ? 28 : 30);
  if (t[125] == 0 && t[126] && !meta.has("tag:track")) meta.setText("tag:track", std::to_string(t[126]));
}

// Frames of an existing ID3v2.3/2.4 tag that the regenerated tag does not replace, re-encoded as
// ID3v2.4 frames (syncsafe sizes). Used when raw "chunk:ID3 " data is combined with tag:* keys or
// markers, so frames without a tag mapping (pictures, private data, lyrics...) are not lost.
// v2.3 frames that are compressed, encrypted or grouped are dropped (cannot be re-flagged safely).
inline std::vector<uint8_t> id3KeptFrames(const std::vector<uint8_t>& tag, const MetaStore& meta, bool replaceChapters) {
  cx::Bytes out;
  if (tag.size() < 10 || std::memcmp(tag.data(), "ID3", 3) != 0) return {};
  const uint8_t ver = tag[3], flags = tag[5];
  if (ver != 3 && ver != 4) return {};
  const size_t size = std::min<size_t>(syncsafe(tag.data() + 6), tag.size() - 10);
  std::vector<uint8_t> body(tag.begin() + 10, tag.begin() + 10 + static_cast<ptrdiff_t>(size));
  if ((flags & 0x80) && ver == 3) body = unsync(body.data(), body.size());
  size_t off = 0;
  if ((flags & 0x40) && body.size() >= 4) off = ver == 3 ? size_t(cx::rd32be(body.data())) + 4 : syncsafe(body.data());
  int guard = 0;
  while (off + 10 <= body.size() && ++guard < 4096) {
    const uint8_t* h = body.data() + off;
    if (h[0] == 0) break;  // padding
    const std::string id(reinterpret_cast<const char*>(h), 4);
    const size_t fsz = ver == 3 ? cx::rd32be(h + 4) : syncsafe(h + 4);
    uint16_t fflags = cx::rd16be(h + 8);
    off += 10;
    if (fsz > body.size() - off) break;
    const uint8_t* d = body.data() + off;
    off += fsz;
    bool drop = false;
    if (const char* t = id3TagName(id)) drop = meta.has(std::string("tag:") + t);
    else if (id == "COMM") drop = meta.has("tag:comment");
    else if (id == "TXXX" && fsz >= 1 && !(ver == 4 && (fflags & 0x03))) {  // plain payload only
      std::string desc = cx::lower(id3Text(d[0], d + 1, fsz - 1));
      drop = meta.has("tag:" + desc);
    } else if (id == "CHAP" || id == "CTOC") drop = replaceChapters;
    if (drop) continue;
    for (char c : id)
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { drop = true; break; }
    if (drop) continue;
    if (ver == 3) {
      if (fflags & 0x00E0) continue;  // compressed / encrypted / grouped
      fflags = static_cast<uint16_t>((fflags & 0xE000) >> 1);  // status flags move one bit right in v2.4
    }
    out.fourcc(id.c_str());
    putSyncsafe(out, static_cast<uint32_t>(fsz));
    out.u16be(fflags);
    out.raw(d, fsz);
  }
  return std::move(out.v);
}

// ID3v2.4 tag (UTF-8 text) from tag:* keys and markers, padded with `padding` zero bytes.
// `extraFrames` (already encoded v2.4 frames, see id3KeptFrames) are appended verbatim.
inline std::vector<uint8_t> buildId3v24(const MetaStore& meta, const std::vector<sq_audio_file_marker>& markers, double sampleRate,
                                        size_t padding, const std::vector<uint8_t>& extraFrames = {}) {
  cx::Bytes frames;
  auto frame = [&](const char* id, const cx::Bytes& data) {
    frames.fourcc(id);
    putSyncsafe(frames, static_cast<uint32_t>(data.size()));
    frames.u16be(0);
    frames.raw(data.v.data(), data.size());
  };
  for (const auto& e : meta.items()) {
    if (!cx::isTagKey(e.key)) continue;
    std::string t = e.key.substr(4);
    cx::Bytes d;
    d.u8(3);
    if (t == "comment") {
      d.raw("eng", 3);
      d.u8(0);
      d.raw(e.value.data(), e.value.size());
      frame("COMM", d);
    } else if (const char* id = id3FrameFor(t)) {
      d.raw(e.value.data(), e.value.size());
      frame(id, d);
    } else {
      d.raw(t.data(), t.size());
      d.u8(0);
      d.raw(e.value.data(), e.value.size());
      frame("TXXX", d);
    }
  }
  if (!markers.empty() && sampleRate > 0) {
    cx::Bytes toc;
    toc.raw("toc", 3);
    toc.u8(0);
    toc.u8(0x03);  // top level, ordered
    toc.u8(static_cast<uint32_t>(std::min<size_t>(markers.size(), 255)));
    for (size_t i = 0; i < markers.size() && i < 255; ++i) {
      std::string cid = "chp" + std::to_string(i);
      toc.raw(cid.data(), cid.size());
      toc.u8(0);
    }
    frame("CTOC", toc);
    for (size_t i = 0; i < markers.size() && i < 255; ++i) {
      const auto& m = markers[i];
      cx::Bytes c;
      std::string cid = "chp" + std::to_string(i);
      c.raw(cid.data(), cid.size());
      c.u8(0);
      auto ms = [&](sq_sample_time f) { return static_cast<uint32_t>(std::llround(static_cast<double>(f) * 1000.0 / sampleRate)); };
      c.u32be(ms(m.position));
      c.u32be(ms(m.position + m.length));
      c.u32be(0xFFFFFFFFu);
      c.u32be(0xFFFFFFFFu);
      size_t nl = strnlen(m.name, sizeof m.name);
      if (nl) {
        c.fourcc("TIT2");
        putSyncsafe(c, static_cast<uint32_t>(nl + 1));
        c.u16be(0);
        c.u8(3);
        c.raw(m.name, nl);
      }
      frame("CHAP", c);
    }
  }
  frames.raw(extraFrames.data(), extraFrames.size());
  cx::Bytes tag;
  tag.raw("ID3", 3);
  tag.u8(4);
  tag.u8(0);
  tag.u8(0);
  putSyncsafe(tag, static_cast<uint32_t>(frames.size() + padding));
  tag.raw(frames.v.data(), frames.size());
  tag.zeros(padding);
  return std::move(tag.v);
}

}  // namespace sq::mp3
