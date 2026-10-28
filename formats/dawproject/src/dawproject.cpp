// Squizzy Studio formats - DAWproject subset (see dawproject.hpp). Own implementation.
#include "sq/formats/dawproject/dawproject.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <new>
#include <set>
#include <string_view>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/formats/core/common.hpp"
#include "sq/formats/core/zip.hpp"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"
#include "sq/version.h"

namespace sq::formats::dawproject {
namespace {

std::string fmt(double v) {
  if (!std::isfinite(v)) v = 0;
  return formatDouble(v);
}

std::string nodeName(INode* n) {
  char buf[512];
  uint32_t need = 0;
  if (n && n->getName(buf, sizeof buf, &need) == SQ_OK) return buf;
  if (n && need > 0 && need < (1u << 20)) {
    std::string big(need, '\0');
    if (n->getName(big.data(), need, &need) == SQ_OK) return std::string(big.c_str());
  }
  return {};
}

std::string clipSource(IClip* c) {
  char buf[2048];
  uint32_t need = 0;
  return c->getSourcePath(buf, sizeof buf, &need) == SQ_OK ? std::string(buf) : std::string();
}

std::string colorStr(uint32_t rgba) {
  char b[16];
  std::snprintf(b, sizeof b, "#%06X", (rgba >> 8) & 0xFFFFFFu);
  return b;
}

double attrD(const XmlNode& n, const char* k, double def = 0) {
  double v = def;
  if (n.hasAttr(k) && parseDouble(n.attr(k), v) && std::isfinite(v)) return v;
  return def;
}

sq_result fail(std::string* error, sq_result code, const std::string& msg) {
  if (error) *error = msg;
  return code;
}

// ---- Squizzy track properties (see engine/audio automation.hpp / builtin.hpp) -------------------
std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t b = 0;
    while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
    if (b < line.size()) out.push_back(line.substr(b));
  }
  return out;
}

// One insert / instrument spec: "[!]<format>|<id>|<path>[|<index>=<value>]...".
struct Spec {
  bool bypassed = false;
  std::string format, id, path;
  std::vector<std::pair<uint32_t, double>> params;
};
bool parseSpecLine(const std::string& text, Spec& s) {
  std::string line = text;
  s = Spec{};
  if (!line.empty() && line[0] == '!') {
    s.bypassed = true;
    line = line.substr(1);
  }
  std::vector<std::string> parts;
  size_t p = 0;
  for (;;) {
    const size_t e = line.find('|', p);
    parts.push_back(line.substr(p, e == std::string::npos ? std::string::npos : e - p));
    if (e == std::string::npos) break;
    p = e + 1;
  }
  if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) return false;
  s.format = parts[0];
  s.id = parts[1];
  if (parts.size() > 2) s.path = parts[2];
  for (size_t i = 3; i < parts.size(); ++i) {
    const size_t eq = parts[i].find('=');
    int64_t idx = 0;
    double v = 0;
    if (eq == std::string::npos || !parseInt(std::string_view(parts[i]).substr(0, eq), idx) || idx < 0 || idx > 0xFFFFFF ||
        !parseDouble(std::string_view(parts[i]).substr(eq + 1), v))
      continue;
    s.params.emplace_back(static_cast<uint32_t>(idx), v);
  }
  return true;
}
std::string formatSpecLine(const Spec& s) {
  std::string out = (s.bypassed ? "!" : "") + s.format + "|" + s.id + "|" + s.path;
  for (const auto& [i, v] : s.params) out += "|" + std::to_string(i) + "=" + fmt(v);
  return out;
}

struct Lane {
  std::string target;
  std::vector<std::pair<double, double>> points;
};
std::vector<Lane> parseLanes(const std::string& text) {
  std::vector<Lane> out;
  for (const std::string& line : lines(text)) {
    Lane l;
    size_t sp = line.find(' ');
    l.target = line.substr(0, sp);
    while (sp != std::string::npos) {
      const size_t b = sp + 1;
      sp = line.find(' ', b);
      const std::string tok = line.substr(b, sp == std::string::npos ? std::string::npos : sp - b);
      const size_t colon = tok.find(':');
      double pos = 0, v = 0;
      if (colon != std::string::npos && parseDouble(std::string_view(tok).substr(0, colon), pos) &&
          parseDouble(std::string_view(tok).substr(colon + 1), v))
        l.points.emplace_back(pos, v);
    }
    if (!l.target.empty()) out.push_back(std::move(l));
  }
  return out;
}

// DAWproject device element <-> Squizzy spec format.
const char* deviceElement(const std::string& format) {
  if (format == "vst3") return "Vst3Plugin";
  if (format == "clap") return "ClapPlugin";
  if (format == "vst2" || format == "vst") return "Vst2Plugin";
  if (format == "au") return "AuPlugin";
  return "BuiltinDevice";
}
constexpr const char* kVendor = "Squizzy Studio";

// Writes one device; `automated` = parameter indices automation lanes refer to (they need an
// element to target even when the spec keeps their default). Returns the parameter id per index.
std::map<uint32_t, std::string> writeDevice(XmlNode devices, const Spec& s, const char* role, const std::string& devId,
                                            const std::set<uint32_t>& automated) {
  XmlNode d = devices.append(deviceElement(s.format));
  d.setAttr("id", devId);
  d.setAttr("deviceRole", role);
  d.setAttr("deviceID", s.id);
  d.setAttr("deviceName", s.id);
  d.setAttr("loaded", "true");
  if (s.format == "builtin") d.setAttr("deviceVendor", kVendor);
  // Extension attributes (ignored by other readers): the Squizzy spec fields.
  d.setAttr("sqFormat", s.format);
  if (!s.path.empty()) d.setAttr("sqPath", s.path);
  XmlNode en = d.append("Enabled");
  en.setAttr("id", devId + "-enabled");
  en.setAttr("value", s.bypassed ? "false" : "true");
  std::map<uint32_t, double> values(s.params.begin(), s.params.end());
  std::set<uint32_t> all = automated;
  for (const auto& [i, v] : values) all.insert(i);
  std::map<uint32_t, std::string> ids;
  if (all.empty()) return ids;
  XmlNode params = d.append("Parameters");
  for (uint32_t i : all) {
    XmlNode p = params.append("RealParameter");
    const std::string pid = devId + "-p" + std::to_string(i);
    p.setAttr("id", pid);
    p.setAttr("parameterID", std::to_string(i));
    p.setAttr("name", "Parameter " + std::to_string(i));
    p.setAttr("unit", "linear");
    auto it = values.find(i);
    if (it != values.end()) p.setAttr("value", fmt(it->second));
    ids[i] = pid;
  }
  return ids;
}

struct TrackOut {
  sq::Ptr<ITrack> t;
  sq_track_info info{};
  sq_guid id{}, parent{};
  std::string tid, name, chanId;  // chanId empty: no channel (folder)
  sq::Ptr<IAttributes> props;
};

}  // namespace

sq_result exportProject(IDocument* doc, const std::string& path, ExportReport* report, std::string* error) {
  if (!doc || path.empty()) return fail(error, SQ_E_INVALIDARG, "no document or path");
  ExportReport rep;
  ZipArchive zip;
  auto xd = xmlCreate("Project");
  XmlNode root = xmlRoot(xd);
  root.setAttr("version", "1.0");
  XmlNode app = root.append("Application");
  app.setAttr("name", "Squizzy Studio");
  app.setAttr("version", SQ_VERSION_STRING);

  sq::Ptr<ITempoMap> tm;
  double bpm = 120.0;
  uint32_t num = 4, den = 4;
  if (doc->getTempoMap(tm.put()) == SQ_OK && tm) {
    sq_tempo_point tp{};
    tp.struct_size = sizeof tp;
    if (tm->getTempoCount() > 0 && tm->getTempo(0, &tp) == SQ_OK) bpm = tp.bpm;
    if (tm->getTempoCount() > 1) rep.warnings += "only the first tempo point is exported\n";
    sq_time_signature ts{};
    ts.struct_size = sizeof ts;
    if (tm->getSignatureCount() > 0 && tm->getSignature(0, &ts) == SQ_OK) {
      num = ts.numerator;
      den = ts.denominator;
    }
  }
  XmlNode transport = root.append("Transport");
  XmlNode tempo = transport.append("Tempo");
  tempo.setAttr("unit", "bpm");
  tempo.setAttr("value", fmt(bpm));
  XmlNode sig = transport.append("TimeSignature");
  sig.setAttr("numerator", std::to_string(num));
  sig.setAttr("denominator", std::to_string(den));

  XmlNode structure = root.append("Structure");
  XmlNode arrangement = root.append("Arrangement");
  arrangement.setAttr("id", "arrangement");
  XmlNode lanes = arrangement.append("Lanes");
  lanes.setAttr("timeUnit", "beats");

  sq_document_info di{};
  di.struct_size = sizeof di;
  doc->getDocumentInfo(&di);

  // Pass 1: every track with its ids (routing needs the channel ids of later tracks).
  std::vector<TrackOut> tracks;
  std::map<sq_guid, size_t> byId;
  std::string masterChan;
  for (uint32_t ti = 0; ti < di.track_count; ++ti) {
    TrackOut o;
    if (doc->getTrack(ti, o.t.put()) != SQ_OK || !o.t) continue;
    o.info.struct_size = sizeof o.info;
    if (o.t->getTrackInfo(&o.info) != SQ_OK) continue;
    o.t->getId(&o.id);
    o.props = sq::makeAttributes();
    if (SQ_FAILED(o.t->getProperties(o.props.get()))) o.props = sq::makeAttributes();
    o.props->getGuid("parent", &o.parent);
    o.name = nodeName(o.t.get());
    o.tid = "track" + std::to_string(tracks.size() + 1);
    if (o.info.kind != SQ_TRACK_FOLDER) o.chanId = o.tid + "-channel";
    if (o.info.kind == SQ_TRACK_MASTER && masterChan.empty()) masterChan = o.chanId;
    byId[o.id] = tracks.size();
    tracks.push_back(std::move(o));
  }
  auto chanOf = [&](const sq_guid& g) -> std::string {
    auto it = byId.find(g);
    return it == byId.end() ? std::string() : tracks[it->second].chanId;
  };

  std::map<std::string, std::string> mediaNames;  // source path -> archive member
  std::set<std::string> usedNames;
  std::map<size_t, XmlNode> trackNodes;
  for (size_t idx = 0; idx < tracks.size(); ++idx) {
    TrackOut& o = tracks[idx];
    const sq_track_info& info = o.info;
    const bool master = info.kind == SQ_TRACK_MASTER, folder = info.kind == SQ_TRACK_FOLDER;
    // Folder children nest inside the folder's <Track> (folders come before their children).
    XmlNode parentNode = structure;
    if (auto pit = byId.find(o.parent); !sq_guid_is_null(&o.parent) && pit != byId.end() && trackNodes.count(pit->second))
      parentNode = trackNodes[pit->second];
    XmlNode tr = parentNode.append("Track");
    trackNodes[idx] = tr;
    tr.setAttr("id", o.tid);
    tr.setAttr("name", o.name);
    tr.setAttr("contentType", folder ? "tracks" : info.kind == SQ_TRACK_INSTRUMENT ? "notes" : "audio");
    tr.setAttr("loaded", "true");
    if (info.color) tr.setAttr("color", colorStr(info.color));
    if (master) rep.master = true;
    else ++rep.tracks;
    if (info.kind == SQ_TRACK_BUS || info.kind == SQ_TRACK_FX) ++rep.buses;

    const std::vector<Lane> autoLanes = parseLanes(sq::getAttrString(o.props.get(), "automation"));
    std::map<std::string, std::string> targetIds;  // automation target -> parameter id
    if (!folder) {
      XmlNode ch = tr.append("Channel");
      ch.setAttr("id", o.chanId);
      ch.setAttr("role", master ? "master" : info.kind == SQ_TRACK_FX ? "effect" : info.kind == SQ_TRACK_BUS ? "submix" : "regular");
      ch.setAttr("audioChannels", std::to_string(info.channels ? info.channels : 2));
      ch.setAttr("solo", (info.flags & SQ_TRACK_SOLO) ? "true" : "false");
      if (!master) {
        const std::string dest = sq_guid_is_null(&info.output) ? masterChan : chanOf(info.output);
        if (!dest.empty()) ch.setAttr("destination", dest);
      }
      // Devices: the instrument, then the inserts (bypassed ones disabled).
      std::map<uint32_t, std::set<uint32_t>> insertAuto;
      std::set<uint32_t> instAuto;
      for (const Lane& l : autoLanes) {
        int64_t a = 0, b = 0;
        if (l.target.rfind("insert.", 0) == 0) {
          const size_t dot = l.target.find('.', 7);
          if (dot != std::string::npos && parseInt(std::string_view(l.target).substr(7, dot - 7), a) &&
              parseInt(std::string_view(l.target).substr(dot + 1), b) && a >= 0 && b >= 0 && a < 4096 && b <= 0xFFFFFF)
            insertAuto[static_cast<uint32_t>(a)].insert(static_cast<uint32_t>(b));
        } else if (l.target.rfind("instrument.", 0) == 0 && parseInt(std::string_view(l.target).substr(11), a) && a >= 0 && a <= 0xFFFFFF) {
          instAuto.insert(static_cast<uint32_t>(a));
        }
      }
      const std::string instText = info.kind == SQ_TRACK_INSTRUMENT ? sq::getAttrString(o.props.get(), "user.instrument") : std::string();
      const std::vector<std::string> inserts = lines(sq::getAttrString(o.props.get(), "user.inserts"));
      if (!lines(instText).empty() || !inserts.empty()) {
        XmlNode devices = ch.append("Devices");
        Spec s;
        const std::vector<std::string> inst = lines(instText);
        if (!inst.empty() && parseSpecLine(inst[0], s)) {
          for (const auto& [i, pid] : writeDevice(devices, s, "instrument", o.tid + "-instrument", instAuto))
            targetIds["instrument." + std::to_string(i)] = pid;
          ++rep.devices;
        }
        for (size_t k = 0; k < inserts.size(); ++k) {
          if (!parseSpecLine(inserts[k], s)) {
            rep.warnings += "track '" + o.name + "': insert " + std::to_string(k) + " not exported (unreadable spec)\n";
            continue;
          }
          const std::set<uint32_t> none;
          auto ia = insertAuto.find(static_cast<uint32_t>(k));
          for (const auto& [i, pid] : writeDevice(devices, s, "audioFX", o.tid + "-insert" + std::to_string(k), ia == insertAuto.end() ? none : ia->second))
            targetIds["insert." + std::to_string(k) + "." + std::to_string(i)] = pid;
          ++rep.devices;
        }
      }
      XmlNode vol = ch.append("Volume");
      vol.setAttr("id", o.tid + "-volume");
      vol.setAttr("name", "Volume");
      vol.setAttr("unit", "decibel");
      vol.setAttr("value", fmt(info.volume_db));
      targetIds["volume"] = o.tid + "-volume";
      XmlNode pan = ch.append("Pan");
      pan.setAttr("id", o.tid + "-pan");
      pan.setAttr("name", "Pan");
      pan.setAttr("unit", "normalized");
      pan.setAttr("min", "0");
      pan.setAttr("max", "1");
      pan.setAttr("value", fmt((info.pan + 1.0) / 2.0));
      targetIds["pan"] = o.tid + "-pan";
      XmlNode mute = ch.append("Mute");
      mute.setAttr("id", o.tid + "-mute");
      mute.setAttr("name", "Mute");
      mute.setAttr("value", (info.flags & SQ_TRACK_MUTE) ? "true" : "false");
      targetIds["mute"] = o.tid + "-mute";
      // Sends: "<bus guid> <dB> [pre]" per line.
      const std::vector<std::string> sends = lines(sq::getAttrString(o.props.get(), "user.sends"));
      if (!sends.empty()) {
        XmlNode xs = ch.append("Sends");
        for (size_t k = 0; k < sends.size(); ++k) {
          const size_t sp = sends[k].find(' ');
          sq_guid bus{};
          std::string dest;
          if (sq::parseGuid(sends[k].substr(0, sp), bus)) dest = chanOf(bus);
          if (dest.empty()) {
            rep.warnings += "track '" + o.name + "': send " + std::to_string(k) + " not exported (no such bus)\n";
            continue;
          }
          double db = 0;
          if (sp != std::string::npos) {
            const std::string rest = sends[k].substr(sp + 1);
            parseDouble(std::string_view(rest).substr(0, rest.find(' ')), db);
          }
          const std::string sid = o.tid + "-send" + std::to_string(k);
          XmlNode x = xs.append("Send");
          x.setAttr("id", sid);
          x.setAttr("destination", dest);
          x.setAttr("type", sends[k].find(" pre", sp) != std::string::npos ? "pre" : "post");
          XmlNode sv = x.append("Volume");
          sv.setAttr("id", sid + "-volume");
          sv.setAttr("name", "Send");
          sv.setAttr("unit", "decibel");
          sv.setAttr("value", fmt(db));
          targetIds["send." + std::to_string(k)] = sid + "-volume";
          ++rep.sends;
        }
      }
    }

    XmlNode tl;
    auto trackLanes = [&]() {
      if (!tl) {
        tl = lanes.append("Lanes");
        tl.setAttr("track", o.tid);
      }
      return tl;
    };
    // Automation lanes as <Points> targeting the parameters above (values in the target's unit).
    for (const Lane& l : autoLanes) {
      auto it = targetIds.find(l.target);
      if (it == targetIds.end()) {
        rep.warnings += "track '" + o.name + "': automation '" + l.target + "' not exported\n";
        continue;
      }
      XmlNode pts = trackLanes().append("Points");
      pts.setAttr("id", it->second + "-automation");
      XmlNode target = pts.append("Target");
      target.setAttr("parameter", it->second);
      const bool isMute = l.target == "mute", isPan = l.target == "pan";
      const bool isDb = l.target == "volume" || l.target.rfind("send.", 0) == 0;
      pts.setAttr("unit", isDb ? "decibel" : isPan ? "normalized" : "linear");
      for (const auto& [pos, v] : l.points) {
        XmlNode pt = pts.append(isMute ? "BoolPoint" : "RealPoint");
        pt.setAttr("time", fmt(pos));
        pt.setAttr("value", isMute ? (v >= 0.5 ? "true" : "false") : fmt(isPan ? (v + 1.0) / 2.0 : v));
      }
      ++rep.automation;
    }

    if (info.clip_count == 0) continue;
    XmlNode clips = trackLanes().append("Clips");
    for (uint32_t ci = 0; ci < info.clip_count; ++ci) {
      sq::Ptr<IClip> c;
      if (o.t->getClip(ci, c.put()) != SQ_OK || !c) continue;
      sq_clip_info cinfo{};
      cinfo.struct_size = sizeof cinfo;
      if (c->getClipInfo(&cinfo) != SQ_OK) continue;
      XmlNode clip = clips.append("Clip");
      clip.setAttr("time", fmt(cinfo.start));
      clip.setAttr("duration", fmt(cinfo.length));
      std::string cname = nodeName(c.get());
      if (!cname.empty()) clip.setAttr("name", cname);
      if (cinfo.flags & SQ_CLIP_MUTED) clip.setAttr("enable", "false");
      if (cinfo.kind == SQ_CLIP_AUDIO) {
        clip.setAttr("contentTimeUnit", "seconds");
        clip.setAttr("playStart", fmt(cinfo.source_offset_seconds));
        if (cinfo.gain_db != 0) clip.setAttr("gainDb", fmt(cinfo.gain_db));  // extension attribute
        std::string src = clipSource(c.get());
        auto it = mediaNames.find(src);
        if (it == mediaNames.end()) {
          std::string data;
          Status st = readFileBytes(src, data);
          if (!st.ok()) {
            rep.warnings += "media missing: " + src + "\n";
            continue;
          }
          std::string base = fileBaseName(src);
          if (!isSafeIdentifier(base)) base = "media.wav";
          std::string name = "audio/" + base;
          for (int k = 2; usedNames.count(name); ++k) name = "audio/" + std::to_string(k) + "-" + base;
          usedNames.insert(name);
          zip.add(name, std::move(data));
          it = mediaNames.emplace(src, name).first;
          ++rep.media;
        }
        XmlNode audio = clip.append("Audio");
        audio.setAttr("algorithm", "raw");
        audio.setAttr("channels", std::to_string(info.channels ? info.channels : 2));
        XmlNode file = audio.append("File");
        file.setAttr("path", it->second);
      } else {
        clip.setAttr("playStart", "0");
        XmlNode notes = clip.append("Notes");
        for (uint32_t e = 0; e < cinfo.event_count; ++e) {
          sq_clip_event ev{};
          ev.struct_size = sizeof ev;
          if (c->getEvent(e, &ev) != SQ_OK || (ev.bytes[0] & 0xF0u) != 0x90u || ev.bytes[2] == 0) continue;
          XmlNode note = notes.append("Note");
          note.setAttr("time", fmt(ev.position));
          note.setAttr("duration", fmt(ev.length));
          note.setAttr("channel", std::to_string(ev.bytes[0] & 0x0Fu));
          note.setAttr("key", std::to_string(ev.bytes[1]));
          note.setAttr("vel", fmt(ev.bytes[2] / 127.0));
          note.setAttr("rel", "0.5");
          ++rep.notes;
        }
      }
      ++rep.clips;
    }
  }
  XmlNode markers = arrangement.append("Markers");
  for (uint32_t m = 0; m < doc->getMarkerCount(); ++m) {
    sq_doc_marker mk{};
    mk.struct_size = sizeof mk;
    if (doc->getMarker(m, &mk) != SQ_OK) continue;
    XmlNode x = markers.append("Marker");
    x.setAttr("time", fmt(mk.position));
    x.setAttr("name", mk.name);
    ++rep.markers;
  }
  zip.add("project.xml", xmlWrite(xd, true));
  auto md = xmlCreate("MetaData");
  XmlNode mroot = xmlRoot(md);
  XmlNode title = mroot.append("Title");
  title.setText(fileStem(path));
  XmlNode mapp = mroot.append("Application");
  mapp.setText("Squizzy Studio");
  zip.add("metadata.xml", xmlWrite(md, true));
  Status st = zipWriteFile(zip, path, 6);
  if (!st.ok()) return fail(error, st.code, st.message);
  if (report) *report = rep;
  return SQ_OK;
}

namespace {

// engine/doc's kSetAutomation action (sq/engine/doc/actions.hpp): track:guid param:string
// points:blob(double[2n]). Same class id; this library does not link engine/doc.
constexpr sq_guid kSetAutomation = sq::guid("{14D5E2BA-6EC7-4616-A188-F9A29E14D634}");

// One <Track> of the structure, flattened (nesting -> parent folder).
struct TrackIn {
  XmlNode node, channel;
  std::string tid, parentTid;
  int32_t kind = 0;
  sq_guid guid{};
};
void collectTracks(XmlNode parent, const std::string& parentTid, std::vector<TrackIn>& out, std::string& warnings) {
  for (XmlNode t : parent.children("Track")) {
    TrackIn in;
    in.node = t;
    in.channel = t.child("Channel");
    in.tid = t.attr("id");
    in.parentTid = parentTid;
    const std::string ct = t.attr("contentType"), role = in.channel ? in.channel.attr("role") : std::string();
    const bool hasChildren = static_cast<bool>(t.child("Track"));
    if (role == "master") in.kind = SQ_TRACK_MASTER;
    else if (hasChildren || (ct.find("tracks") != std::string::npos && ct.find("notes") == std::string::npos && ct.find("audio") == std::string::npos))
      in.kind = SQ_TRACK_FOLDER;
    else if (role == "effect") in.kind = SQ_TRACK_FX;
    else if (role == "submix") in.kind = SQ_TRACK_BUS;
    else if (ct.find("notes") != std::string::npos) in.kind = SQ_TRACK_INSTRUMENT;
    else if (ct.find("audio") != std::string::npos) in.kind = SQ_TRACK_AUDIO;
    if (!in.kind) warnings += "track '" + t.attr("name") + "' skipped (contentType " + ct + ")\n";
    else out.push_back(in);
    if (hasChildren) collectTracks(t, in.kind == SQ_TRACK_FOLDER ? in.tid : parentTid, out, warnings);
  }
}
void collectPoints(XmlNode n, std::vector<XmlNode>& out) {
  for (XmlNode c : n.children()) {
    if (c.name() == "Points") out.push_back(c);
    else if (c.name() == "Lanes") collectPoints(c, out);
  }
}
double toDb(const XmlNode& p, double v) {
  if (p.attr("unit") == "linear") return v > 0 ? 20.0 * std::log10(v) : -144.0;
  return v;
}

// An automatable parameter found in the structure.
struct ParamRef {
  sq_guid track{};
  std::string target;  // Squizzy automation target
  enum Kind { Db, Pan, Mute, Plain } kind = Plain;
  bool linear = false;  // Db stored as linear gain
};

sq_result importProjectImpl(const std::string& path, IDocument* doc, const std::string& mediaDir, ExportReport* report, std::string* error) {
  if (!doc) return fail(error, SQ_E_INVALIDARG, "no document");
  ZipArchive zip;
  // Embedded media may be large, but a member may not expand like a compression bomb.
  ZipLimits limits;
  limits.maxRatio = 1100;  // above deflate's ~1032:1 maximum: only crafted streams hit it
  Status st = zipReadFile(path, zip, limits);
  if (!st.ok()) return fail(error, st.code, st.message);
  const std::string* projectXml = zip.find("project.xml");
  if (!projectXml) return fail(error, SQ_E_PARSE, "project.xml missing");
  std::string perr;
  auto xd = xmlParse(*projectXml, &perr);
  if (!xd || xmlRoot(xd).name() != "Project") return fail(error, SQ_E_PARSE, "project.xml: " + (perr.empty() ? "not a <Project>" : perr));
  XmlNode root = xmlRoot(xd);
  sq::Ptr<IEditor> ed;
  if (doc->getEditor(ed.put()) != SQ_OK || !ed) return fail(error, SQ_E_FAIL, "document has no editor");

  ExportReport rep;
  auto exec = [&](const sq_guid& action, IAttributes* p, IAttributes* res) { return ed->execute(&action, p, res); };
  ed->beginTransaction("Import DAWproject");
  struct TxGuard {  // an exception (out of memory) must not leave the import transaction open
    IEditor* ed;
    int pending = std::uncaught_exceptions();
    ~TxGuard() {
      if (std::uncaught_exceptions() > pending) ed->cancelTransaction();
    }
  } txGuard{ed.get()};
  sq_result r = SQ_OK;
  // Transport.
  XmlNode transport = root.child("Transport");
  if (XmlNode t = transport.child("Tempo")) {
    double bpm = attrD(t, "value", 120.0);
    if (bpm > 0 && bpm < 1000) {
      auto p = sq::makeAttributes();
      p->setDouble("position", 0.0);
      p->setDouble("bpm", bpm);
      r = exec(sq::doc::kSetTempo, p.get(), nullptr);
    }
  }
  if (XmlNode s = transport.child("TimeSignature"); s && SQ_SUCCEEDED(r)) {
    int64_t n = 4, d = 4;
    parseInt(s.attr("numerator", "4"), n);
    parseInt(s.attr("denominator", "4"), d);
    auto p = sq::makeAttributes();
    p->setDouble("position", 0.0);
    p->setInt("numerator", n);
    p->setInt("denominator", d);
    r = exec(sq::doc::kSetSignature, p.get(), nullptr);
  }
  // Tracks: create them all first (routing and sends refer to later ones).
  std::vector<TrackIn> ins;
  collectTracks(root.child("Structure"), {}, ins, rep.warnings);
  std::map<std::string, size_t> byTid, byChannel;
  sq_guid existingMaster{};
  {
    sq_document_info di{};
    di.struct_size = sizeof di;
    doc->getDocumentInfo(&di);
    for (uint32_t i = 0; i < di.track_count; ++i) {
      sq::Ptr<ITrack> t;
      sq_track_info info{};
      info.struct_size = sizeof info;
      if (doc->getTrack(i, t.put()) == SQ_OK && t->getTrackInfo(&info) == SQ_OK && info.kind == SQ_TRACK_MASTER) t->getId(&existingMaster);
    }
  }
  for (size_t k = 0; k < ins.size() && SQ_SUCCEEDED(r); ++k) {
    TrackIn& in = ins[k];
    if (in.kind == SQ_TRACK_MASTER && !sq_guid_is_null(&existingMaster)) {
      in.guid = existingMaster;
    } else {
      auto p = sq::makeAttributes();
      p->setInt("kind", in.kind);
      p->setString("name", in.node.attr("name").c_str());
      int64_t chans = 2;
      if (in.channel) parseInt(in.channel.attr("audioChannels", "2"), chans);
      p->setInt("channels", chans == 1 ? 1 : 2);
      auto pt = byTid.find(in.parentTid);
      if (!in.parentTid.empty() && pt != byTid.end()) p->setGuid("parent", &ins[pt->second].guid);
      auto res = sq::makeAttributes();
      r = exec(sq::doc::kAddTrack, p.get(), res.get());
      if (SQ_FAILED(r)) break;
      res->getGuid("node", &in.guid);
      if (in.kind == SQ_TRACK_MASTER) existingMaster = in.guid;
    }
    if (in.kind == SQ_TRACK_MASTER) rep.master = true;
    else ++rep.tracks;
    if (in.kind == SQ_TRACK_BUS || in.kind == SQ_TRACK_FX) ++rep.buses;
    if (!in.tid.empty()) byTid[in.tid] = k;
    if (in.channel && in.channel.hasAttr("id")) byChannel[in.channel.attr("id")] = k;
  }
  // Mixer state: volume / pan / mute, routing, sends, devices; collects automatable parameters.
  std::map<std::string, ParamRef> params;  // parameter element id -> target
  auto setProp = [&](const sq_guid& g, const char* key, auto&& setter) {
    auto q = sq::makeAttributes();
    q->setGuid("node", &g);
    q->setString("key", key);
    setter(q.get());
    return exec(sq::doc::kSetProperty, q.get(), nullptr);
  };
  auto busOf = [&](const std::string& chan) -> const TrackIn* {
    auto it = byChannel.find(chan);
    return it == byChannel.end() ? nullptr : &ins[it->second];
  };
  for (TrackIn& in : ins) {
    if (SQ_FAILED(r)) break;
    const sq_guid g = in.guid;
    const std::string tname = in.node.attr("name");
    if (XmlNode ch = in.channel) {
      if (XmlNode v = ch.child("Volume")) {
        const double db = toDb(v, attrD(v, "value", v.attr("unit") == "linear" ? 1.0 : 0.0));
        if (db != 0) r = setProp(g, "volumeDb", [&](IAttributes* a) { a->setDouble("value", std::fmin(24.0, db)); });
        if (v.hasAttr("id")) params[v.attr("id")] = ParamRef{g, "volume", ParamRef::Db, v.attr("unit") == "linear"};
      }
      if (XmlNode pn = ch.child("Pan"); pn && SQ_SUCCEEDED(r)) {
        double v = attrD(pn, "value", 0.5);
        double pan = pn.attr("unit") == "normalized" ? v * 2.0 - 1.0 : v;
        if (pan != 0) r = setProp(g, "pan", [&](IAttributes* a) { a->setDouble("value", std::fmax(-1.0, std::fmin(1.0, pan))); });
        if (pn.hasAttr("id")) params[pn.attr("id")] = ParamRef{g, "pan", pn.attr("unit") == "normalized" ? ParamRef::Pan : ParamRef::Plain, false};
      }
      if (XmlNode m = ch.child("Mute"); m && SQ_SUCCEEDED(r)) {
        if (m.attr("value") == "true") r = setProp(g, "mute", [&](IAttributes* a) { a->setBool("value", SQ_TRUE); });
        if (m.hasAttr("id")) params[m.attr("id")] = ParamRef{g, "mute", ParamRef::Mute, false};
      }
      if (ch.attr("solo") == "true" && in.kind != SQ_TRACK_MASTER && SQ_SUCCEEDED(r))
        r = setProp(g, "solo", [&](IAttributes* a) { a->setBool("value", SQ_TRUE); });
      // Output routing: a bus / effect channel (the master channel = the default route).
      if (ch.hasAttr("destination") && in.kind != SQ_TRACK_MASTER && SQ_SUCCEEDED(r)) {
        const TrackIn* dest = busOf(ch.attr("destination"));
        if (dest && (dest->kind == SQ_TRACK_BUS || dest->kind == SQ_TRACK_FX) && dest != &in)
          r = setProp(g, "output", [&](IAttributes* a) { a->setGuid("value", &dest->guid); });
        else if (!dest || dest->kind != SQ_TRACK_MASTER)
          rep.warnings += "track '" + tname + "': output '" + ch.attr("destination") + "' not imported\n";
      }
      // Sends.
      std::string sends;
      uint32_t sendIndex = 0;
      for (XmlNode s : ch.child("Sends").children("Send")) {
        const TrackIn* dest = busOf(s.attr("destination"));
        if (!dest || (dest->kind != SQ_TRACK_BUS && dest->kind != SQ_TRACK_FX) || dest == &in) {
          rep.warnings += "track '" + tname + "': send to '" + s.attr("destination") + "' not imported\n";
          continue;
        }
        XmlNode v = s.child("Volume");
        const double db = v ? toDb(v, attrD(v, "value", v.attr("unit") == "linear" ? 1.0 : 0.0)) : 0.0;
        if (v && v.hasAttr("id")) params[v.attr("id")] = ParamRef{g, "send." + std::to_string(sendIndex), ParamRef::Db, v.attr("unit") == "linear"};
        sends += sq::toString(dest->guid) + " " + fmt(std::fmax(-144.0, std::fmin(24.0, db))) + (s.attr("type") == "pre" ? " pre" : "") + "\n";
        ++sendIndex;
        ++rep.sends;
      }
      if (!sends.empty() && SQ_SUCCEEDED(r)) r = setProp(g, "user.sends", [&](IAttributes* a) { a->setString("value", sends.c_str()); });
      // Devices: the first instrument -> "user.instrument", audio effects -> "user.inserts".
      std::string inserts, instrument;
      uint32_t slot = 0;
      for (XmlNode dv : ch.child("Devices").children()) {
        const std::string el(dv.name());
        Spec s;
        s.bypassed = dv.child("Enabled").attr("value") == "false";
        s.id = dv.attr("deviceID");
        s.path = dv.attr("sqPath");
        if (dv.hasAttr("sqFormat")) s.format = dv.attr("sqFormat");
        else if (el == "Vst3Plugin") s.format = "vst3";
        else if (el == "ClapPlugin") s.format = "clap";
        else if (el == "Vst2Plugin") s.format = "vst2";
        else if (el == "AuPlugin") s.format = "au";
        else if (el == "Equalizer" || el == "Compressor" || el == "NoiseGate" || el == "Limiter") {
          s.format = "builtin";  // the format's generic devices -> the closest built-in (default settings)
          s.id = el == "Equalizer" ? "eq" : el == "Compressor" ? "comp" : el == "NoiseGate" ? "gate" : "limiter";
          rep.warnings += "track '" + tname + "': " + el + " imported as built-in '" + s.id + "' with default settings\n";
        } else if (el == "BuiltinDevice" && dv.attr("deviceVendor") == kVendor) {
          s.format = "builtin";
        }
        if (s.format.empty() || s.id.empty() || s.id.find('|') != std::string::npos || s.path.find('|') != std::string::npos ||
            s.path.find('\n') != std::string::npos || s.id.find('\n') != std::string::npos) {
          rep.warnings += "track '" + tname + "': device '" + dv.attr("deviceName", el) + "' not imported\n";
          continue;
        }
        const std::string role = dv.attr("deviceRole");
        const bool isInstrument = role == "instrument";
        if (isInstrument && (in.kind != SQ_TRACK_INSTRUMENT || !instrument.empty())) {
          rep.warnings += "track '" + tname + "': instrument '" + s.id + "' not imported (one per instrument track)\n";
          continue;
        }
        std::vector<std::pair<std::string, uint32_t>> ids;
        for (XmlNode p : dv.child("Parameters").children()) {
          int64_t idx = 0;
          if (!parseInt(p.attr("parameterID"), idx) || idx < 0 || idx > 0xFFFFFF) continue;
          if (p.hasAttr("id")) ids.emplace_back(p.attr("id"), static_cast<uint32_t>(idx));
          double v = 0;
          if (p.hasAttr("value") && parseDouble(p.attr("value"), v) && std::isfinite(v)) s.params.emplace_back(static_cast<uint32_t>(idx), v);
        }
        const std::string prefix = isInstrument ? std::string("instrument.") : "insert." + std::to_string(slot) + ".";
        for (const auto& [pid, idx] : ids) params[pid] = ParamRef{g, prefix + std::to_string(idx), ParamRef::Plain, false};
        if (isInstrument) {
          instrument = formatSpecLine(Spec{false, s.format, s.id, s.path, s.params});
        } else {
          inserts += formatSpecLine(s) + "\n";
          ++slot;
        }
        ++rep.devices;
      }
      if (!instrument.empty() && SQ_SUCCEEDED(r)) r = setProp(g, "user.instrument", [&](IAttributes* a) { a->setString("value", instrument.c_str()); });
      if (!inserts.empty() && SQ_SUCCEEDED(r)) r = setProp(g, "user.inserts", [&](IAttributes* a) { a->setString("value", inserts.c_str()); });
    }
    uint32_t rgba = 0;
    if (SQ_SUCCEEDED(r) && in.node.hasAttr("color") && parseColor(in.node.attr("color"), rgba))
      r = setProp(g, "color", [&](IAttributes* a) { a->setInt("value", rgba); });
  }
  // Clips.
  XmlNode arrangement = root.child("Arrangement");
  std::map<std::string, std::string> extracted;
  if (SQ_SUCCEEDED(r)) sq_fs_create_dirs(mediaDir.c_str());
  for (XmlNode tl : arrangement.child("Lanes").children("Lanes")) {
    if (SQ_FAILED(r)) break;
    auto it = byTid.find(tl.attr("track"));
    if (it == byTid.end()) continue;
    const sq_guid trackId = ins[it->second].guid;
    if (ins[it->second].kind != SQ_TRACK_AUDIO && ins[it->second].kind != SQ_TRACK_INSTRUMENT) continue;
    for (XmlNode clip : tl.child("Clips").children("Clip")) {
      if (SQ_FAILED(r)) break;
      double time = attrD(clip, "time"), dur = attrD(clip, "duration");
      if (!(dur > 0) || time < 0) continue;
      auto p = sq::makeAttributes();
      p->setGuid("track", &trackId);
      p->setDouble("start", time);
      p->setDouble("length", dur);
      XmlNode audio = clip.child("Audio");
      XmlNode notes = clip.child("Notes");
      if (audio) {
        std::string member = audio.child("File").attr("path");
        const std::string* data = zip.find(member);
        if (!data) {
          rep.warnings += "media missing in archive: " + member + "\n";
          continue;
        }
        auto ex = extracted.find(member);
        if (ex == extracted.end()) {
          std::string out = sq::joinPath(mediaDir, fileBaseName(member));
          for (int k = 2; sq_fs_exists(out.c_str()); ++k) out = sq::joinPath(mediaDir, std::to_string(k) + "-" + fileBaseName(member));
          Status ws = writeFileBytes(out, *data);
          if (!ws.ok()) {
            r = ws.code;
            break;
          }
          ex = extracted.emplace(member, out).first;
          ++rep.media;
        }
        p->setInt("kind", SQ_CLIP_AUDIO);
        p->setString("source", ex->second.c_str());
        p->setDouble("source_offset", attrD(clip, "playStart"));
      } else if (notes) {
        p->setInt("kind", SQ_CLIP_MIDI);
      } else {
        continue;
      }
      auto res = sq::makeAttributes();
      r = exec(sq::doc::kAddClip, p.get(), res.get());
      if (SQ_FAILED(r)) break;
      sq_guid cid{};
      res->getGuid("node", &cid);
      ++rep.clips;
      auto setClip = [&](const char* key, auto&& setter) {
        auto q = sq::makeAttributes();
        q->setGuid("node", &cid);
        q->setString("key", key);
        setter(q.get());
        return exec(sq::doc::kSetProperty, q.get(), nullptr);
      };
      if (clip.hasAttr("name")) {
        std::string nm = clip.attr("name");
        r = setClip("name", [&](IAttributes* a) { a->setString("value", nm.c_str()); });
      }
      if (SQ_SUCCEEDED(r) && clip.hasAttr("gainDb"))
        r = setClip("gainDb", [&](IAttributes* a) { a->setDouble("value", attrD(clip, "gainDb")); });
      if (SQ_SUCCEEDED(r) && clip.attr("enable") == "false") r = setClip("muted", [&](IAttributes* a) { a->setBool("value", SQ_TRUE); });
      if (SQ_SUCCEEDED(r) && notes) {
        std::vector<sq_clip_event> evs;
        for (XmlNode n : notes.children("Note")) {
          int64_t key = 60, chn = 0;
          parseInt(n.attr("key", "60"), key);
          parseInt(n.attr("channel", "0"), chn);
          double vel = attrD(n, "vel", 0.8);
          sq_clip_event e{};
          e.struct_size = sizeof e;
          e.position = attrD(n, "time");
          e.length = attrD(n, "duration");
          e.bytes[0] = static_cast<uint8_t>(0x90u | (static_cast<uint32_t>(chn) & 0x0Fu));
          e.bytes[1] = static_cast<uint8_t>(key < 0 ? 0 : key > 127 ? 127 : key);
          long v = std::lround(vel * 127.0);
          e.bytes[2] = static_cast<uint8_t>(v < 1 ? 1 : v > 127 ? 127 : v);
          e.size = 3;
          if (e.length > 0 && e.position >= 0) evs.push_back(e);
        }
        rep.notes += static_cast<int>(evs.size());
        if (!evs.empty()) {
          auto q = sq::makeAttributes();
          q->setGuid("clip", &cid);
          q->setBlob("events", evs.data(), static_cast<uint32_t>(evs.size() * sizeof(sq_clip_event)));
          r = exec(sq::doc::kSetClipEvents, q.get(), nullptr);
        }
      }
    }
  }
  for (XmlNode m : arrangement.child("Markers").children("Marker")) {
    if (SQ_FAILED(r)) break;
    auto p = sq::makeAttributes();
    p->setDouble("position", attrD(m, "time"));
    p->setString("name", m.attr("name").c_str());
    r = exec(sq::doc::kAddMarker, p.get(), nullptr);
    if (SQ_SUCCEEDED(r)) ++rep.markers;
  }
  // Automation: <Points> whose target is one of the parameters above (values in its unit).
  std::vector<XmlNode> pointLists;
  collectPoints(arrangement, pointLists);
  for (XmlNode pts : pointLists) {
    if (SQ_FAILED(r)) break;
    const std::string pid = pts.child("Target").attr("parameter");
    auto it = params.find(pid);
    if (it == params.end()) {
      if (!pid.empty()) rep.warnings += "automation of '" + pid + "' not imported\n";
      continue;
    }
    const ParamRef& ref = it->second;
    std::vector<double> flat;
    for (XmlNode pt : pts.children()) {
      if (pt.name() != "RealPoint" && pt.name() != "BoolPoint" && pt.name() != "IntegerPoint") continue;
      const double t = attrD(pt, "time", -1.0);
      if (!(t >= 0)) continue;
      double v = 0;
      if (pt.name() == "BoolPoint") v = pt.attr("value") == "true" ? 1.0 : 0.0;
      else v = attrD(pt, "value", 0.0);
      switch (ref.kind) {
        case ParamRef::Db: v = std::fmax(-144.0, std::fmin(24.0, ref.linear ? (v > 0 ? 20.0 * std::log10(v) : -144.0) : v)); break;
        case ParamRef::Pan: v = std::fmax(-1.0, std::fmin(1.0, v * 2.0 - 1.0)); break;
        case ParamRef::Mute: v = v >= 0.5 ? 1.0 : 0.0; break;
        case ParamRef::Plain: break;
      }
      flat.push_back(t);
      flat.push_back(v);
    }
    if (flat.empty()) continue;
    auto p = sq::makeAttributes();
    p->setGuid("track", &ref.track);
    p->setString("param", ref.target.c_str());
    p->setBlob("points", flat.data(), static_cast<uint32_t>(flat.size() * sizeof(double)));
    const sq_result ar = exec(kSetAutomation, p.get(), nullptr);
    if (SQ_FAILED(ar)) rep.warnings += "automation '" + ref.target + "' not imported (" + sq_result_string(ar) + ")\n";
    else ++rep.automation;
  }
  if (SQ_FAILED(r)) {
    ed->cancelTransaction();
    return fail(error, r, std::string("import failed: ") + sq_result_string(r));
  }
  r = ed->commitTransaction();
  if (report) *report = rep;
  return r;
}
}  // namespace

sq_result importProject(const std::string& path, IDocument* doc, const std::string& mediaDir, ExportReport* report, std::string* error) {
  try {
    return importProjectImpl(path, doc, mediaDir, report, error);
  } catch (const std::bad_alloc&) {
    return fail(error, SQ_E_OUTOFMEMORY, "out of memory importing " + path);
  } catch (...) {
    return fail(error, SQ_E_FAIL, "unexpected error importing " + path);
  }
}

}  // namespace sq::formats::dawproject
