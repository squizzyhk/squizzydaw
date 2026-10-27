// Squizzy Studio formats - DAWproject subset (see dawproject.hpp). Own implementation.
#include "sq/formats/dawproject/dawproject.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <new>
#include <set>
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
  std::map<std::string, std::string> mediaNames;  // source path -> archive member
  std::set<std::string> usedNames;
  int idCounter = 0;
  for (uint32_t ti = 0; ti < di.track_count; ++ti) {
    sq::Ptr<ITrack> t;
    if (doc->getTrack(ti, t.put()) != SQ_OK || !t) continue;
    sq_track_info info{};
    info.struct_size = sizeof info;
    if (t->getTrackInfo(&info) != SQ_OK) continue;
    if (info.kind != SQ_TRACK_AUDIO && info.kind != SQ_TRACK_INSTRUMENT) {
      rep.warnings += "track '" + nodeName(t.get()) + "' skipped (only audio and instrument tracks are exported)\n";
      continue;
    }
    std::string tid = "track" + std::to_string(++idCounter);
    XmlNode tr = structure.append("Track");
    tr.setAttr("id", tid);
    tr.setAttr("name", nodeName(t.get()));
    tr.setAttr("contentType", info.kind == SQ_TRACK_AUDIO ? "audio" : "notes");
    tr.setAttr("loaded", "true");
    if (info.color) tr.setAttr("color", colorStr(info.color));
    XmlNode ch = tr.append("Channel");
    ch.setAttr("id", tid + "-channel");
    ch.setAttr("role", "regular");
    ch.setAttr("audioChannels", std::to_string(info.channels ? info.channels : 2));
    ch.setAttr("solo", (info.flags & SQ_TRACK_SOLO) ? "true" : "false");
    XmlNode vol = ch.append("Volume");
    vol.setAttr("unit", "decibel");
    vol.setAttr("value", fmt(info.volume_db));
    XmlNode pan = ch.append("Pan");
    pan.setAttr("unit", "normalized");
    pan.setAttr("value", fmt((info.pan + 1.0) / 2.0));
    XmlNode mute = ch.append("Mute");
    mute.setAttr("value", (info.flags & SQ_TRACK_MUTE) ? "true" : "false");
    ++rep.tracks;

    XmlNode tl = lanes.append("Lanes");
    tl.setAttr("track", tid);
    XmlNode clips = tl.append("Clips");
    for (uint32_t ci = 0; ci < info.clip_count; ++ci) {
      sq::Ptr<IClip> c;
      if (t->getClip(ci, c.put()) != SQ_OK || !c) continue;
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
  // Tracks.
  std::map<std::string, std::pair<sq_guid, int32_t>> trackIds;  // dawproject id -> (guid, kind)
  for (XmlNode t : root.child("Structure").children("Track")) {
    if (SQ_FAILED(r)) break;
    std::string ct = t.attr("contentType");
    int32_t kind = ct.find("notes") != std::string::npos ? SQ_TRACK_INSTRUMENT : ct.find("audio") != std::string::npos ? SQ_TRACK_AUDIO : 0;
    if (!kind) {
      rep.warnings += "track '" + t.attr("name") + "' skipped (contentType " + ct + ")\n";
      continue;
    }
    XmlNode ch = t.child("Channel");
    auto p = sq::makeAttributes();
    p->setInt("kind", kind);
    p->setString("name", t.attr("name").c_str());
    int64_t chans = 2;
    if (ch) parseInt(ch.attr("audioChannels", "2"), chans);
    p->setInt("channels", chans == 1 ? 1 : 2);
    auto res = sq::makeAttributes();
    r = exec(sq::doc::kAddTrack, p.get(), res.get());
    if (SQ_FAILED(r)) break;
    sq_guid g{};
    res->getGuid("node", &g);
    trackIds[t.attr("id")] = {g, kind};
    ++rep.tracks;
    auto setProp = [&](const char* key, auto&& setter) {
      auto q = sq::makeAttributes();
      q->setGuid("node", &g);
      q->setString("key", key);
      setter(q.get());
      return exec(sq::doc::kSetProperty, q.get(), nullptr);
    };
    if (ch) {
      if (XmlNode v = ch.child("Volume")) {
        double db = attrD(v, "value", 0.0);
        if (v.attr("unit") == "linear") db = db > 0 ? 20.0 * std::log10(db) : -144.0;
        if (db != 0) r = setProp("volumeDb", [&](IAttributes* a) { a->setDouble("value", db); });
      }
      if (XmlNode pn = ch.child("Pan"); pn && SQ_SUCCEEDED(r)) {
        double v = attrD(pn, "value", 0.5);
        double pan = pn.attr("unit") == "normalized" ? v * 2.0 - 1.0 : v;
        if (pan != 0) r = setProp("pan", [&](IAttributes* a) { a->setDouble("value", std::fmax(-1.0, std::fmin(1.0, pan))); });
      }
      if (XmlNode m = ch.child("Mute"); m && m.attr("value") == "true" && SQ_SUCCEEDED(r))
        r = setProp("mute", [&](IAttributes* a) { a->setBool("value", SQ_TRUE); });
    }
    uint32_t rgba = 0;
    if (SQ_SUCCEEDED(r) && t.hasAttr("color") && parseColor(t.attr("color"), rgba))
      r = setProp("color", [&](IAttributes* a) { a->setInt("value", rgba); });
  }
  // Clips.
  XmlNode arrangement = root.child("Arrangement");
  std::map<std::string, std::string> extracted;
  if (SQ_SUCCEEDED(r)) sq_fs_create_dirs(mediaDir.c_str());
  for (XmlNode tl : arrangement.child("Lanes").children("Lanes")) {
    if (SQ_FAILED(r)) break;
    auto it = trackIds.find(tl.attr("track"));
    if (it == trackIds.end()) continue;
    const sq_guid trackId = it->second.first;
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
