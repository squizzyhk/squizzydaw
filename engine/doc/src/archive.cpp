// Squizzy Studio engine/doc - XmlArchive (ZIP of per-editor XML parts) + crash-safe file write.
#include "sq/engine/doc/archive.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

#include "internal.hpp"
#include "miniz.h"
#include "sq/platform.hpp"

namespace sq::edoc {

const char* kindName(int32_t kind) {
  switch (kind) {
    case SQ_DOC_PROJECT: return "project";
    case SQ_DOC_SHOW: return "show";
    default: return "song";
  }
}
int32_t kindFromName(const std::string& n) {
  if (n == "song") return SQ_DOC_SONG;
  if (n == "project") return SQ_DOC_PROJECT;
  if (n == "show") return SQ_DOC_SHOW;
  return 0;
}
const char* kindExtension(int32_t kind) {
  switch (kind) {
    case SQ_DOC_PROJECT: return ".sqproject";
    case SQ_DOC_SHOW: return ".sqshow";
    default: return ".sqsong";
  }
}

namespace {

using detail::fmtDouble;

std::string fmtInt(int64_t v) { return std::to_string(v); }
std::string fmtBool(bool b) { return b ? "1" : "0"; }

std::string hexEncode(const std::string& s) {
  std::string out(s.size() * 2 + 1, '\0');
  sq_hex_encode(s.data(), s.size(), out.data());
  out.resize(s.size() * 2);
  return out;
}
bool hexDecode(const std::string& h, std::string& out) {
  if (h.size() % 2) return false;
  out.resize(h.size() / 2);
  for (size_t i = 0; i < out.size(); ++i) {
    const int hi = sq::detail::guidHex(h[2 * i]), lo = sq::detail::guidHex(h[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<char>((hi << 4) | lo);
  }
  return true;
}

double parseDouble(const std::string& s, double def = 0.0) {
  double v = def;
  if (s.empty()) return def;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc() ? v : def;
}
int64_t parseInt(const std::string& s, int64_t def = 0) {
  int64_t v = def;
  if (s.empty()) return def;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc() ? v : def;
}
uint32_t parseU32(const std::string& s, uint32_t def = 0) {
  const int64_t v = parseInt(s, def);
  return (v < 0 || v > 0xFFFFFFFFll) ? def : static_cast<uint32_t>(v);
}
sq_guid parseG(const std::string& s) {
  sq_guid g{};
  if (!s.empty()) parseGuid(s, g);
  return g;
}

constexpr uint64_t kMaxPartBytes = 256ull << 20;     // one XML part
constexpr uint64_t kMaxArchiveBytes = 1024ull << 20;  // all parts together

bool fin(double v) { return std::isfinite(v); }
bool fin0(double v) { return std::isfinite(v) && v >= 0.0; }  // finite, non-negative

// ---- writers ----
const char* typeName(int32_t t) {
  switch (t) {
    case SQ_VALUE_INT: return "int";
    case SQ_VALUE_DOUBLE: return "double";
    case SQ_VALUE_BOOL: return "bool";
    case SQ_VALUE_STRING: return "string";
    case SQ_VALUE_GUID: return "guid";
    case SQ_VALUE_BLOB: return "blob";
    default: return "none";
  }
}

void writeUser(XmlNode parent, const PropertyBag& bag) {
  if (bag.empty()) return;
  XmlNode u = parent.append("User");
  for (const auto& [k, v] : bag) {
    XmlNode p = u.append("Property");
    p.setAttr("key", k);
    p.setAttr("type", typeName(v.type));
    switch (v.type) {
      case SQ_VALUE_INT: p.setAttr("value", fmtInt(v.i)); break;
      case SQ_VALUE_DOUBLE: p.setAttr("value", fmtDouble(v.d)); break;
      case SQ_VALUE_BOOL: p.setAttr("value", fmtBool(v.i != 0)); break;
      case SQ_VALUE_STRING: p.setAttr("value", v.s); break;
      case SQ_VALUE_GUID: p.setAttr("value", guidStr(v.g)); break;
      case SQ_VALUE_BLOB: p.setAttr("value", hexEncode(v.s)); break;
      default: break;
    }
  }
}
void readUser(XmlNode parent, PropertyBag& bag) {
  bag.clear();
  XmlNode u = parent.child("User");
  if (!u) return;
  for (XmlNode p : u.children("Property")) {
    const std::string key = p.attr("key"), type = p.attr("type"), val = p.attr("value");
    if (key.empty()) continue;
    Value v;
    if (type == "int") { v.type = SQ_VALUE_INT; v.i = parseInt(val); }
    else if (type == "double") { v.type = SQ_VALUE_DOUBLE; v.d = parseDouble(val); }
    else if (type == "bool") { v.type = SQ_VALUE_BOOL; v.i = val == "1" ? 1 : 0; }
    else if (type == "string") { v.type = SQ_VALUE_STRING; v.s = val; }
    else if (type == "guid") { v.type = SQ_VALUE_GUID; v.g = parseG(val); }
    else if (type == "blob") { v.type = SQ_VALUE_BLOB; if (!hexDecode(val, v.s)) continue; }
    else continue;
    bag[key] = std::move(v);
  }
}

std::string partDocument(const Store& s) {
  auto d = xmlCreate("Document");
  XmlNode r = xmlRoot(d);
  r.setAttr("id", guidStr(s.props.id));
  r.setAttr("name", s.props.name);
  r.setAttr("sampleRate", fmtDouble(s.props.sampleRate));
  r.setAttr("removeOverlaps", fmtBool(s.props.removeOverlaps));
  r.setAttr("automationFollowsEdits", fmtBool(s.props.automationFollowsEdits));
  writeUser(r, s.props.user);
  return xmlWrite(d);
}

std::string partTempo(const Store& s) {
  auto d = xmlCreate("TempoMap");
  XmlNode r = xmlRoot(d);
  r.setAttr("id", guidStr(s.tempo.id));
  for (const TempoPoint& p : s.tempo.tempos) {
    XmlNode n = r.append("Tempo");
    n.setAttr("pos", fmtDouble(p.pos));
    n.setAttr("bpm", fmtDouble(p.bpm));
    n.setAttr("flags", fmtInt(p.flags));
  }
  for (const SigPoint& p : s.tempo.sigs) {
    XmlNode n = r.append("Signature");
    n.setAttr("pos", fmtDouble(p.pos));
    n.setAttr("numerator", fmtInt(p.num));
    n.setAttr("denominator", fmtInt(p.den));
  }
  return xmlWrite(d);
}

void writeClip(XmlNode parent, const ClipRec& c, const std::string& mediaBase) {
  XmlNode n = parent.append("Clip");
  n.setAttr("id", guidStr(c.id));
  n.setAttr("kind", fmtInt(c.kind));
  n.setAttr("name", c.name);
  n.setAttr("start", fmtDouble(c.start));
  n.setAttr("length", fmtDouble(c.length));
  n.setAttr("sourceOffset", fmtDouble(c.sourceOffset));
  n.setAttr("gainDb", fmtDouble(c.gainDb));
  n.setAttr("fadeIn", fmtDouble(c.fadeIn));
  n.setAttr("fadeOut", fmtDouble(c.fadeOut));
  n.setAttr("flags", fmtInt(c.flags));
  n.setAttr("color", fmtInt(c.color));
  n.setAttr("source", (mediaBase.empty() || c.kind != SQ_CLIP_AUDIO) ? c.source : relativeMediaPath(c.source, mediaBase));
  for (const EventRec& e : c.events) {
    XmlNode ev = n.append("Event");
    ev.setAttr("pos", fmtDouble(e.pos));
    ev.setAttr("len", fmtDouble(e.len));
    ev.setAttr("size", fmtInt(e.size));
    ev.setAttr("data", hexEncode(std::string(reinterpret_cast<const char*>(e.bytes), 4)));
  }
  writeUser(n, c.user);
}

std::string partTracks(const Store& s, const std::string& mediaBase) {
  auto d = xmlCreate("Tracks");
  XmlNode r = xmlRoot(d);
  for (const sq_guid& id : s.trackOrder) {
    const TrackRec& t = s.tracks.at(id);
    XmlNode n = r.append("Track");
    n.setAttr("id", guidStr(t.id));
    n.setAttr("parent", sq_guid_is_null(&t.parent) ? std::string() : guidStr(t.parent));
    n.setAttr("kind", fmtInt(t.kind));
    n.setAttr("name", t.name);
    n.setAttr("channels", fmtInt(t.channels));
    n.setAttr("flags", fmtInt(t.flags));
    n.setAttr("color", fmtInt(t.color));
    n.setAttr("volumeDb", fmtDouble(t.volumeDb));
    n.setAttr("pan", fmtDouble(t.pan));
    n.setAttr("output", sq_guid_is_null(&t.output) ? std::string() : guidStr(t.output));
    n.setAttr("timebase", t.timebase);
    n.setAttr("chordTrack", fmtBool(t.chordTrack));
    n.setAttr("followChords", fmtBool(t.followChords));
    for (const auto& [param, pts] : t.automation) {
      XmlNode a = n.append("Automation");
      a.setAttr("param", param);
      for (const AutoPoint& p : pts) {
        XmlNode pt = a.append("Point");
        pt.setAttr("pos", fmtDouble(p.pos));
        pt.setAttr("value", fmtDouble(p.value));
      }
    }
    writeUser(n, t.user);
    for (const sq_guid& cid : s.clipsOf(id)) writeClip(n, s.clips.at(cid), mediaBase);
  }
  return xmlWrite(d);
}

std::string partMarkers(const Store& s) {
  auto d = xmlCreate("Markers");
  XmlNode r = xmlRoot(d);
  for (const sq_guid& id : s.markerOrder()) {
    const MarkerRec& m = s.markers.at(id);
    XmlNode n = r.append("Marker");
    n.setAttr("id", guidStr(m.id));
    n.setAttr("name", m.name);
    n.setAttr("position", fmtDouble(m.position));
    n.setAttr("length", fmtDouble(m.length));
    n.setAttr("flags", fmtInt(m.flags));
    n.setAttr("color", fmtInt(m.color));
    writeUser(n, m.user);
  }
  return xmlWrite(d);
}

std::string partUi(const Store& s) {
  auto d = xmlCreate("UiState");
  XmlNode r = xmlRoot(d);
  for (const auto& [k, v] : s.journal) {
    XmlNode n = r.append("Entry");
    n.setAttr("key", k);
    n.setAttr("value", v);
  }
  return xmlWrite(d);
}

std::string partMeta(const Store& s, const ArchiveMeta& meta) {
  auto d = xmlCreate("MetaInfo");
  XmlNode r = xmlRoot(d);
  r.setAttr("format", kArchiveFormat);
  r.setAttr("version", fmtInt(kArchiveVersion));
  r.setAttr("kind", kindName(s.props.kind));
  r.setAttr("docId", guidStr(s.props.id));
  r.setAttr("generator", std::string("Squizzy Studio ") + SQ_VERSION_STRING);
  for (const auto& [k, v] : meta.extra) r.setAttr(k.c_str(), v);
  return xmlWrite(d);
}

// ---- zip ----
uint16_t rd16(const std::string& b, size_t o) { return static_cast<uint16_t>(static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8)); }
uint32_t rd32(const std::string& b, size_t o) { return rd16(b, o) | (static_cast<uint32_t>(rd16(b, o + 2)) << 16); }
void wr16(std::string& b, size_t o, uint16_t v) {
  b[o] = static_cast<char>(v & 0xFF);
  b[o + 1] = static_cast<char>(v >> 8);
}

// Stamps every local + central header with DOS date 1980-01-01 00:00 so output is reproducible.
bool normalizeZipTimes(std::string& z) {
  if (z.size() < 22) return false;
  const size_t eocd = z.size() - 22;
  if (rd32(z, eocd) != 0x06054b50u) return false;
  const uint16_t count = rd16(z, eocd + 10);
  size_t cd = rd32(z, eocd + 16);
  const uint16_t kDate = (0 << 9) | (1 << 5) | 1, kTime = 0;
  for (uint16_t i = 0; i < count; ++i) {
    if (cd + 46 > z.size() || rd32(z, cd) != 0x02014b50u) return false;
    wr16(z, cd + 12, kTime);
    wr16(z, cd + 14, kDate);
    const size_t local = rd32(z, cd + 42);
    if (local + 30 > z.size() || rd32(z, local) != 0x04034b50u) return false;
    wr16(z, local + 10, kTime);
    wr16(z, local + 12, kDate);
    cd += 46u + rd16(z, cd + 28) + rd16(z, cd + 30) + rd16(z, cd + 32);
  }
  return true;
}

}  // namespace

namespace {
std::vector<std::pair<std::string, std::string>> serializePartsWith(const Store& s, const std::string& mediaBase) {
  const std::string k = kindName(s.props.kind);
  std::vector<std::pair<std::string, std::string>> parts;
  parts.emplace_back("document.xml", partDocument(s));
  parts.emplace_back(k + "/tempo.xml", partTempo(s));
  parts.emplace_back(k + "/tracks.xml", partTracks(s, mediaBase));
  parts.emplace_back(k + "/markers.xml", partMarkers(s));
  parts.emplace_back("uistate.xml", partUi(s));
  for (const auto& [name, data] : s.foreignParts) parts.emplace_back(name, data);
  return parts;
}

// '/' separators, no trailing separator (except a bare root).
std::string slashed(std::string p) {
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  while (p.size() > 1 && p.back() == '/' && !(p.size() == 3 && p[1] == ':')) p.pop_back();
  return p;
}
std::string normalizedAbsolute(const std::string& p) {
  sq::Buffer b;
  return sq_fs_absolute(p.c_str(), b.out()) == SQ_OK ? slashed(b.str()) : slashed(p);
}
bool samePathChar(char a, char b) {
#if defined(_WIN32)
  return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));  // NTFS: case-insensitive
#else
  return a == b;
#endif
}
std::string fileNameOf(const std::string& p) {
  const size_t s = p.find_last_of("/\\");
  return s == std::string::npos ? p : p.substr(s + 1);
}
}  // namespace

std::vector<std::pair<std::string, std::string>> serializeParts(const Store& s) { return serializePartsWith(s, {}); }

bool isAbsoluteMediaPath(const std::string& p) {
  if (p.empty()) return false;
  if (p[0] == '/' || p[0] == '\\') return true;
  return p.size() >= 2 && p[1] == ':' && std::isalpha(static_cast<unsigned char>(p[0]));
}

std::string relativeMediaPath(const std::string& source, const std::string& baseDir) {
  if (baseDir.empty() || !isAbsoluteMediaPath(source) || source.find("://") != std::string::npos) return source;
  const std::string base = normalizedAbsolute(baseDir), src = normalizedAbsolute(source);
  if (base.empty() || src.size() <= base.size() + 1) return source;
  for (size_t i = 0; i < base.size(); ++i)
    if (!samePathChar(base[i], src[i])) return source;
  const size_t cut = base.back() == '/' ? base.size() : base.size() + 1;  // "/" or "C:/" roots end in '/'
  if (base.back() != '/' && src[base.size()] != '/') return source;  // "/a/song" vs "/a/songs/x"
  std::string rel = src.substr(cut);
  if (rel.empty() || rel == ".." || rel.rfind("../", 0) == 0 || rel.find("/../") != std::string::npos) return source;
  return rel;
}

std::string resolveMediaPath(const std::string& stored, const std::string& baseDir) {
  if (stored.empty() || baseDir.empty() || stored.find("://") != std::string::npos) return stored;
  if (!isAbsoluteMediaPath(stored)) return normalizedAbsolute(sq::joinPath(baseDir, stored));
  if (sq_fs_exists(stored.c_str())) return stored;
  // Moved project: an absolute path that is gone, with the file next to the song (or in a media
  // folder there) -> use that copy.
  const std::string name = fileNameOf(slashed(stored));
  if (name.empty()) return stored;
  for (const char* sub : {"", "Media", "Audio"}) {
    const std::string cand = *sub ? sq::joinPath(sq::joinPath(baseDir, sub), name) : sq::joinPath(baseDir, name);
    if (sq_fs_is_file(cand.c_str())) return normalizedAbsolute(cand);
  }
  return stored;
}

std::string writeArchive(const Store& s, const ArchiveMeta& meta) {
  std::vector<std::pair<std::string, std::string>> parts;
  parts.emplace_back("metainfo.xml", partMeta(s, meta));
  for (auto& p : serializePartsWith(s, meta.mediaBase)) parts.push_back(std::move(p));

  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) return {};
  MZ_TIME_T fixed = 315532800;  // 1980-01-01 UTC; overwritten by normalizeZipTimes anyway
  for (const auto& [name, data] : parts) {
    if (!mz_zip_writer_add_mem_ex_v2(&zip, name.c_str(), data.data(), data.size(), nullptr, 0, MZ_DEFAULT_LEVEL, 0, 0, &fixed,
                                     nullptr, 0, nullptr, 0)) {
      mz_zip_writer_end(&zip);
      return {};
    }
  }
  void* buf = nullptr;
  size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
    mz_zip_writer_end(&zip);
    return {};
  }
  std::string out(static_cast<const char*>(buf), size);
  mz_free(buf);  // ownership moved to us by finalize_heap_archive
  mz_zip_writer_end(&zip);
  if (!normalizeZipTimes(out)) return {};
  return out;
}

sq_result readArchive(const std::string& bytes, Store& out, ArchiveMeta* meta, std::string* err) {
  auto fail = [&](sq_result r, const std::string& m) {
    if (err) *err = m;
    return r;
  };
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) return fail(SQ_E_PARSE, "not a zip container");
  std::map<std::string, std::string> parts;
  std::vector<std::string> order;
  const mz_uint n = mz_zip_reader_get_num_files(&zip);
  uint64_t total = 0;
  for (mz_uint i = 0; i < n; ++i) {
    char name[512];
    mz_zip_reader_get_filename(&zip, i, name, sizeof name);
    if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_uncomp_size > kMaxPartBytes || (total += st.m_uncomp_size) > kMaxArchiveBytes) {
      mz_zip_reader_end(&zip);
      return fail(SQ_E_PARSE, std::string("part too large or unreadable: ") + name);  // zip-bomb guard
    }
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
    if (!p) {
      mz_zip_reader_end(&zip);
      return fail(SQ_E_PARSE, std::string("cannot extract ") + name);
    }
    parts[name].assign(static_cast<const char*>(p), sz);
    order.push_back(name);
    mz_free(p);
  }
  mz_zip_reader_end(&zip);

  auto parsePart = [&](const std::string& name, const char* root, XmlDocPtr& doc) -> sq_result {
    auto it = parts.find(name);
    if (it == parts.end()) return fail(SQ_E_PARSE, "missing part " + name);
    std::string e;
    doc = xmlParse(it->second, &e);
    if (!doc) return fail(SQ_E_PARSE, name + ": " + e);
    if (xmlRoot(doc).name() != root) return fail(SQ_E_PARSE, name + ": unexpected root element");
    parts.erase(it);
    return SQ_OK;
  };

  XmlDocPtr mi;
  if (sq_result r = parsePart("metainfo.xml", "MetaInfo", mi); r != SQ_OK) return r;
  XmlNode m = xmlRoot(mi);
  if (m.attr("format") != kArchiveFormat) return fail(SQ_E_PARSE, "not a Squizzy Studio document");
  if (parseInt(m.attr("version")) > kArchiveVersion) return fail(SQ_E_UNSUPPORTED, "document written by a newer version");
  const int32_t kind = kindFromName(m.attr("kind"));
  if (!kind) return fail(SQ_E_UNSUPPORTED, "unknown document kind");
  if (meta) {
    meta->extra.clear();
    for (uint32_t i = 0; i < sq_xml_attr_count(m.raw()); ++i) {
      const std::string an = sq_xml_attr_name(m.raw(), i);
      if (an != "format" && an != "version" && an != "kind" && an != "docId" && an != "generator") meta->extra[an] = sq_xml_attr_value(m.raw(), i);
    }
  }

  Store s;
  s.props.kind = kind;
  const std::string k = kindName(kind);

  XmlDocPtr dd;
  if (sq_result r = parsePart("document.xml", "Document", dd); r != SQ_OK) return r;
  XmlNode d = xmlRoot(dd);
  s.props.id = parseG(d.attr("id"));
  if (sq_guid_is_null(&s.props.id)) return fail(SQ_E_PARSE, "document id missing");
  s.props.name = d.attr("name");
  s.props.sampleRate = parseDouble(d.attr("sampleRate"), 48000.0);
  if (!fin(s.props.sampleRate) || s.props.sampleRate < 8000.0 || s.props.sampleRate > 768000.0) return fail(SQ_E_PARSE, "bad sample rate");
  s.props.removeOverlaps = d.attr("removeOverlaps") == "1";
  s.props.automationFollowsEdits = d.attr("automationFollowsEdits") == "1";
  readUser(d, s.props.user);

  XmlDocPtr td;
  if (sq_result r = parsePart(k + "/tempo.xml", "TempoMap", td); r != SQ_OK) return r;
  XmlNode tm = xmlRoot(td);
  s.tempo.id = parseG(tm.attr("id"));
  if (sq_guid_is_null(&s.tempo.id)) s.tempo.id = newGuid();
  s.tempo.tempos.clear();
  s.tempo.sigs.clear();
  // Validate before sorting: NaN keys would break the sort's ordering contract.
  for (XmlNode p : tm.children("Tempo")) {
    TempoPoint tp{parseDouble(p.attr("pos")), parseDouble(p.attr("bpm"), 120.0), parseU32(p.attr("flags"))};
    if (!fin0(tp.pos) || !fin(tp.bpm) || tp.bpm < 1.0 || tp.bpm > 999.0) return fail(SQ_E_PARSE, "bad tempo point");
    s.tempo.tempos.push_back(tp);
  }
  for (XmlNode p : tm.children("Signature")) {
    SigPoint sp{parseDouble(p.attr("pos")), parseU32(p.attr("numerator"), 4), parseU32(p.attr("denominator"), 4)};
    if (!fin0(sp.pos) || sp.num < 1 || sp.num > 64 || sp.den < 1 || sp.den > 64 || (sp.den & (sp.den - 1)))
      return fail(SQ_E_PARSE, "bad time signature");
    s.tempo.sigs.push_back(sp);
  }
  auto byPos = [](const auto& a, const auto& b) { return a.pos < b.pos; };
  std::stable_sort(s.tempo.tempos.begin(), s.tempo.tempos.end(), byPos);
  std::stable_sort(s.tempo.sigs.begin(), s.tempo.sigs.end(), byPos);
  if (s.tempo.tempos.empty() || s.tempo.tempos[0].pos != 0.0) s.tempo.tempos.insert(s.tempo.tempos.begin(), TempoPoint{});
  if (s.tempo.sigs.empty() || s.tempo.sigs[0].pos != 0.0) s.tempo.sigs.insert(s.tempo.sigs.begin(), SigPoint{});

  XmlDocPtr trd;
  if (sq_result r = parsePart(k + "/tracks.xml", "Tracks", trd); r != SQ_OK) return r;
  for (XmlNode tn : xmlRoot(trd).children("Track")) {
    TrackRec t;
    t.id = parseG(tn.attr("id"));
    if (sq_guid_is_null(&t.id) || s.tracks.count(t.id)) return fail(SQ_E_PARSE, "bad or duplicate track id");
    t.parent = parseG(tn.attr("parent"));
    t.kind = static_cast<int32_t>(parseInt(tn.attr("kind"), SQ_TRACK_AUDIO));
    t.name = tn.attr("name");
    t.channels = parseU32(tn.attr("channels"), 2);
    t.flags = parseU32(tn.attr("flags"));
    t.color = parseU32(tn.attr("color"));
    t.volumeDb = parseDouble(tn.attr("volumeDb"));
    t.pan = parseDouble(tn.attr("pan"));
    t.output = parseG(tn.attr("output"));
    t.timebase = tn.attr("timebase", "musical");
    t.chordTrack = tn.attr("chordTrack") == "1";
    t.followChords = tn.attr("followChords") == "1";
    if (t.kind < SQ_TRACK_AUDIO || t.kind > SQ_TRACK_MASTER || t.channels < 1 || t.channels > 64 || !fin(t.volumeDb) || !fin(t.pan) ||
        t.pan < -1.0 || t.pan > 1.0 || (t.timebase != "musical" && t.timebase != "time"))
      return fail(SQ_E_PARSE, "bad track attributes");
    for (XmlNode an : tn.children("Automation")) {
      const std::string param = an.attr("param");
      auto& lane = t.automation[param];
      // Gain lanes (dB) are clamped to the fader range so a hand-edited file cannot blow up the mix.
      const bool gainDb = param == "volume" || param.rfind("send.", 0) == 0;
      for (XmlNode pn : an.children("Point")) {
        AutoPoint ap{parseDouble(pn.attr("pos")), parseDouble(pn.attr("value"))};
        if (!fin0(ap.pos) || !fin(ap.value)) return fail(SQ_E_PARSE, "bad automation point");
        if (gainDb) ap.value = std::clamp(ap.value, -144.0, 24.0);
        lane.push_back(ap);
      }
      std::stable_sort(lane.begin(), lane.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.pos < b.pos; });
    }
    readUser(tn, t.user);
    for (XmlNode cn : tn.children("Clip")) {
      ClipRec c;
      c.id = parseG(cn.attr("id"));
      if (sq_guid_is_null(&c.id) || s.clips.count(c.id)) return fail(SQ_E_PARSE, "bad or duplicate clip id");
      c.track = t.id;
      c.kind = static_cast<int32_t>(parseInt(cn.attr("kind"), SQ_CLIP_AUDIO));
      c.name = cn.attr("name");
      c.start = parseDouble(cn.attr("start"));
      c.length = parseDouble(cn.attr("length"));
      c.sourceOffset = parseDouble(cn.attr("sourceOffset"));
      c.gainDb = parseDouble(cn.attr("gainDb"));
      c.fadeIn = parseDouble(cn.attr("fadeIn"));
      c.fadeOut = parseDouble(cn.attr("fadeOut"));
      c.flags = parseU32(cn.attr("flags"));
      c.color = parseU32(cn.attr("color"));
      c.source = cn.attr("source");
      if (meta && !meta->mediaBase.empty() && c.kind == SQ_CLIP_AUDIO) c.source = resolveMediaPath(c.source, meta->mediaBase);
      if ((c.kind != SQ_CLIP_AUDIO && c.kind != SQ_CLIP_MIDI) || !fin0(c.start) || !fin(c.length) || c.length <= 0.0 ||
          !fin0(c.sourceOffset) || !fin(c.gainDb) || !fin0(c.fadeIn) || !fin0(c.fadeOut))
        return fail(SQ_E_PARSE, "bad clip attributes");
      for (XmlNode en : cn.children("Event")) {
        EventRec e;
        e.pos = parseDouble(en.attr("pos"));
        e.len = parseDouble(en.attr("len"));
        if (!fin(e.pos) || !fin0(e.len)) return fail(SQ_E_PARSE, "bad clip event");
        e.size = std::min<uint32_t>(parseU32(en.attr("size")), 4);
        std::string raw;
        if (hexDecode(en.attr("data"), raw) && raw.size() == 4) std::memcpy(e.bytes, raw.data(), 4);
        c.events.push_back(e);
      }
      std::stable_sort(c.events.begin(), c.events.end(), [](const EventRec& a, const EventRec& b) { return a.pos < b.pos; });
      readUser(cn, c.user);
      s.clips[c.id] = std::move(c);
    }
    s.trackOrder.push_back(t.id);
    s.tracks[t.id] = std::move(t);
  }
  // Structure: dangling references are dropped (e.g. a part edited by another tool); references
  // to the wrong kind of track and cycles make the document unusable and are rejected.
  for (auto& [id, t] : s.tracks) {
    if (!sq_guid_is_null(&t.parent)) {
      const TrackRec* p = s.track(t.parent);
      if (!p) t.parent = sq_guid{};
      else if (p->kind != SQ_TRACK_FOLDER || p->id == id) return fail(SQ_E_PARSE, "track parent is not a folder");
    }
    if (!sq_guid_is_null(&t.output)) {
      const TrackRec* o = s.track(t.output);
      if (!o) t.output = sq_guid{};
      else if (o->id == id || (o->kind != SQ_TRACK_BUS && o->kind != SQ_TRACK_FX && o->kind != SQ_TRACK_MASTER))
        return fail(SQ_E_PARSE, "bad track output routing");
    }
  }
  for (const auto& [id, t] : s.tracks) {
    sq_guid p = t.parent;
    for (size_t steps = 0; !sq_guid_is_null(&p); ++steps) {
      if (steps > s.tracks.size()) return fail(SQ_E_PARSE, "track parent cycle");
      p = s.tracks.at(p).parent;
    }
    if (!sq_guid_is_null(&t.output) && detail::routesTo(s, t.output, id)) return fail(SQ_E_PARSE, "track output cycle");
  }

  XmlDocPtr md;
  if (sq_result r = parsePart(k + "/markers.xml", "Markers", md); r != SQ_OK) return r;
  for (XmlNode mn : xmlRoot(md).children("Marker")) {
    MarkerRec mk;
    mk.id = parseG(mn.attr("id"));
    if (sq_guid_is_null(&mk.id) || s.markers.count(mk.id)) return fail(SQ_E_PARSE, "bad or duplicate marker id");
    mk.name = mn.attr("name");
    mk.position = parseDouble(mn.attr("position"));
    mk.length = parseDouble(mn.attr("length"));
    mk.flags = parseU32(mn.attr("flags"));
    mk.color = parseU32(mn.attr("color"));
    if (!fin0(mk.position) || !fin0(mk.length)) return fail(SQ_E_PARSE, "bad marker position");
    readUser(mn, mk.user);
    s.markers[mk.id] = std::move(mk);
  }

  if (parts.count("uistate.xml")) {
    XmlDocPtr ud;
    if (sq_result r = parsePart("uistate.xml", "UiState", ud); r != SQ_OK) return r;
    for (XmlNode en : xmlRoot(ud).children("Entry")) s.journal[en.attr("key")] = en.attr("value");
  }
  for (const std::string& name : order)
    if (parts.count(name)) s.foreignParts[name] = parts[name];

  s.invalidate();
  out = std::move(s);
  return SQ_OK;
}

sq_result safeWriteFile(const std::string& path, const std::string& data) {
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos) sq_fs_create_dirs(path.substr(0, slash).c_str());
  const std::string tmp = path + ".tmp";
  sq_file* f = nullptr;
  sq_result r = sq_file_open(tmp.c_str(), SQ_FILE_WRITE | SQ_FILE_CREATE | SQ_FILE_TRUNCATE, &f);
  if (r != SQ_OK) return r;
  r = sq_file_write(f, data.data(), data.size());
  if (r == SQ_OK) r = sq_file_flush(f);  // FlushFileBuffers / fsync before the rename
  sq_file_close(f);
  if (r == SQ_OK) {
    auto back = readFile(tmp);  // verify what reached the disk before replacing the old file
    if (!back || *back != data) r = SQ_E_IO;
  }
  if (r != SQ_OK) {
    sq_fs_remove(tmp.c_str());
    return r;
  }
  r = sq_fs_rename(tmp.c_str(), path.c_str());
  if (r != SQ_OK) sq_fs_remove(tmp.c_str());
  return r;
}

}  // namespace sq::edoc
