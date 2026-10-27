// Squizzy Studio engine/audio - document -> graph description.
#include "sq/engine/audio/doc_builder.hpp"

#include <cmath>
#include <cstdio>
#include <map>

#include "sq/cpp/attributes.hpp"

namespace sq::engine::audio {

sq_result MediaCache::get(IClassRegistry* reg, const std::string& path, MediaPtr* out) {
  auto it = map_.find(path);
  if (it != map_.end()) {
    *out = it->second;
    return SQ_OK;
  }
  MediaPtr m;
  const sq_result r = loadMediaFile(reg, path.c_str(), &m);
  if (SQ_FAILED(r)) return r;
  map_[path] = m;
  *out = std::move(m);
  return SQ_OK;
}

namespace {
// Structure-key helpers: exact (round-trippable) text for every value.
void keyNum(std::string& k, double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.17g;", v);
  k += b;
}
void keyInt(std::string& k, int64_t v) {
  k += std::to_string(v);
  k += ';';
}
void keyStr(std::string& k, const std::string& v) {
  keyInt(k, static_cast<int64_t>(v.size()));  // length-prefixed: no separator ambiguity
  k += v;
}
void keyGuid(std::string& k, const sq_guid& g) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(&g);
  static const char* hex = "0123456789abcdef";
  for (size_t i = 0; i < sizeof g; ++i) {
    k += hex[p[i] >> 4];
    k += hex[p[i] & 15];
  }
  k += ';';
}
}  // namespace

template <class F>
static std::string readString(F&& fn) {
  char small[512];
  uint32_t needed = 0;
  sq_result r = fn(small, static_cast<uint32_t>(sizeof small), &needed);
  if (r == SQ_OK) return std::string(small);
  if (r == SQ_E_BUFFER_TOO_SMALL && needed > 0) {
    std::string big(needed, '\0');
    if (fn(big.data(), needed, &needed) == SQ_OK) {
      big.resize(needed ? needed - 1 : 0);
      return big;
    }
  }
  return {};
}

std::string nodeName(INode* node) {
  if (!node) return {};
  return readString([&](char* b, uint32_t s, uint32_t* n) { return node->getName(b, s, n); });
}
std::string clipSourcePath(IClip* clip) {
  if (!clip) return {};
  return readString([&](char* b, uint32_t s, uint32_t* n) { return clip->getSourcePath(b, s, n); });
}

sq_result buildGraphDesc(IDocument* doc, IClassRegistry* reg, MediaCache& cache, double engineSampleRate, GraphDesc* out,
                         DocBuildResult* info, const InsertResolver* inserts) {
  if (!doc || !out) return SQ_E_INVALIDARG;
  sq_document_info di{};
  di.struct_size = sizeof di;
  sq_result r = doc->getDocumentInfo(&di);
  if (SQ_FAILED(r)) return r;
  GraphDesc d;
  const double sr = engineSampleRate > 0 ? engineSampleRate : (di.sample_rate > 0 ? di.sample_rate : 48000.0);
  d.sampleRate = sr;
  std::string key;
  keyNum(key, sr);

  sq::Ptr<ITempoMap> tm;
  if (SQ_FAILED(doc->getTempoMap(tm.put()))) tm = nullptr;
  auto ppqToFrame = [&](sq_ppq_time p) -> int64_t {
    const double secs = tm ? tm->ppqToSeconds(p) : p * 0.5;
    return static_cast<int64_t>(std::llround(secs * sr));
  };
  if (tm) {
    const uint32_t nt = tm->getTempoCount();
    for (uint32_t i = 0; i < nt; ++i) {
      sq_tempo_point tp{};
      tp.struct_size = sizeof tp;
      if (SQ_FAILED(tm->getTempo(i, &tp))) continue;
      d.tempo.push_back(TempoSegment{ppqToFrame(tp.position), tp.position, tp.bpm});
      keyNum(key, tp.position);
      keyNum(key, tp.bpm);
    }
    if (tm->getSignatureCount() > 0) {
      sq_time_signature ts{};
      ts.struct_size = sizeof ts;
      if (SQ_SUCCEEDED(tm->getSignature(0, &ts))) {
        d.numerator = ts.numerator ? ts.numerator : 4;
        d.denominator = ts.denominator ? ts.denominator : 4;
      }
    }
  }
  key += "sig:";
  keyInt(key, d.numerator);
  keyInt(key, d.denominator);

  std::map<sq_guid, int32_t> busIndex;
  std::vector<sq_guid> trackOut, busOut;
  for (uint32_t i = 0; i < di.track_count; ++i) {
    sq::Ptr<ITrack> t;
    if (SQ_FAILED(doc->getTrack(i, t.put())) || !t) continue;
    sq_track_info ti{};
    ti.struct_size = sizeof ti;
    if (SQ_FAILED(t->getTrackInfo(&ti))) continue;
    sq_guid id{};
    t->getId(&id);
    const uint32_t ch = ti.channels == 0 ? 2 : std::min<uint32_t>(ti.channels, kMaxChannels);
    key += "T:";
    keyInt(key, ti.kind);
    keyGuid(key, id);
    keyInt(key, ch);
    keyGuid(key, ti.output);
    switch (ti.kind) {
      case SQ_TRACK_BUS:
      case SQ_TRACK_FX: {
        BusDesc b;
        b.id = id;
        b.name = nodeName(t.get());
        b.channels = ch;
        b.volume = dbToGain(ti.volume_db);
        b.pan = static_cast<float>(ti.pan);
        b.mute = (ti.flags & SQ_TRACK_MUTE) != 0;
        busIndex[id] = static_cast<int32_t>(d.buses.size());
        d.buses.push_back(std::move(b));
        busOut.push_back(ti.output);
        break;
      }
      case SQ_TRACK_MASTER:
        d.masterVolume = dbToGain(ti.volume_db);
        break;
      case SQ_TRACK_AUDIO:
      case SQ_TRACK_INSTRUMENT: {
        TrackDesc td;
        td.id = id;
        td.name = nodeName(t.get());
        td.channels = ch;
        td.volume = dbToGain(ti.volume_db);
        td.pan = static_cast<float>(ti.pan);
        td.mute = (ti.flags & SQ_TRACK_MUTE) != 0;
        td.solo = (ti.flags & SQ_TRACK_SOLO) != 0;
        td.armed = (ti.flags & SQ_TRACK_ARMED) != 0;
        td.monitor = (ti.flags & SQ_TRACK_MONITOR) != 0;
        int64_t inCh = 0;
        {
          auto props = sq::makeAttributes();
          if (SQ_SUCCEEDED(t->getProperties(props.get()))) {
            inCh = props->getInt("input_channel", props->getInt("user.input_channel", 0));
            std::string spec = sq::getAttrString(props.get(), "user.inserts");
            size_t pos = 0;
            while (pos < spec.size()) {
              size_t nl = spec.find('\n', pos);
              std::string line = spec.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
              pos = nl == std::string::npos ? spec.size() : nl + 1;
              while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
              if (line.empty()) continue;
              key += "I:";
              keyStr(key, line);
              std::string err;
              ProcessorProto proto = inserts && *inserts ? (*inserts)(line, &err) : nullptr;
              key += proto ? "ok;" : "na;";
              if (proto) td.inserts.push_back(std::move(proto));
              else if (info) info->warnings.push_back("insert not available: " + line + (err.empty() ? "" : " (" + err + ")"));
            }
          }
        }
        if (inCh < 0) inCh = 0;
        key += "in:";
        keyInt(key, inCh);
        td.inputChannels.push_back(static_cast<uint32_t>(inCh));
        if (ch >= 2) td.inputChannels.push_back(static_cast<uint32_t>(inCh + 1));
        for (uint32_t c = 0; c < ti.clip_count; ++c) {
          sq::Ptr<IClip> clip;
          if (SQ_FAILED(t->getClip(c, clip.put())) || !clip) continue;
          sq_clip_info ci{};
          ci.struct_size = sizeof ci;
          if (SQ_FAILED(clip->getClipInfo(&ci)) || (ci.flags & SQ_CLIP_MUTED)) continue;
          const int64_t s = ppqToFrame(ci.start);
          const int64_t e = ppqToFrame(ci.start + ci.length);
          if (e <= s) continue;
          key += "C:";
          keyInt(key, ci.kind);
          keyInt(key, s);
          keyInt(key, e);
          // Audio clips play on instrument tracks too: bounce-in-place ("transform to realtime
          // audio") replaces an instrument track's MIDI clips with an audio clip on that track.
          if (ci.kind == SQ_CLIP_AUDIO) {
            const std::string path = clipSourcePath(clip.get());
            keyStr(key, path);
            keyNum(key, ci.source_offset_seconds);
            keyNum(key, ci.gain_db);
            keyNum(key, ci.fade_in_seconds);
            keyNum(key, ci.fade_out_seconds);
            MediaPtr media;
            if (path.empty() || SQ_FAILED(cache.get(reg, path, &media))) {
              key += "miss;";
              if (info) info->warnings.push_back("missing media: " + path);
              continue;
            }
            ClipParams p;
            p.start = s;
            p.length = e - s;
            p.sourceOffset = static_cast<int64_t>(std::llround(ci.source_offset_seconds * media->sampleRate));
            p.gain = dbToGain(ci.gain_db);
            p.fadeIn = static_cast<int64_t>(std::llround(ci.fade_in_seconds * sr));
            p.fadeOut = static_cast<int64_t>(std::llround(ci.fade_out_seconds * sr));
            td.sources.push_back(std::make_shared<ClipNode>(std::make_shared<FileNode>(media), p));
          } else if (ci.kind == SQ_CLIP_MIDI) {
            std::vector<TimedMidi> ev;
            for (uint32_t k = 0; k < ci.event_count; ++k) {
              sq_clip_event ce{};
              ce.struct_size = sizeof ce;
              if (SQ_FAILED(clip->getEvent(k, &ce))) continue;
              if (ce.position < 0 || ce.position >= ci.length) continue;
              const int64_t f = ppqToFrame(ci.start + ce.position);
              keyInt(key, f);
              keyNum(key, ce.length);
              keyInt(key, (ce.bytes[0] << 16) | (ce.bytes[1] << 8) | ce.bytes[2]);
              keyInt(key, ce.size);
              TimedMidi m{f, {ce.bytes[0], ce.bytes[1], ce.bytes[2]}, static_cast<uint8_t>(ce.size ? ce.size : 3)};
              ev.push_back(m);
              if ((ce.bytes[0] & 0xF0u) == 0x90 && ce.length > 0) {
                const double endPpq = std::min(ce.position + ce.length, ci.length);
                TimedMidi off{ppqToFrame(ci.start + endPpq), {static_cast<uint8_t>(0x80u | (ce.bytes[0] & 0x0Fu)), ce.bytes[1], 0}, 3};
                ev.push_back(off);
              }
            }
            td.sources.push_back(std::make_shared<EventNode>(std::move(ev), std::make_shared<SineSynth>()));
          }
        }
        d.tracks.push_back(std::move(td));
        trackOut.push_back(ti.output);
        break;
      }
      default:
        break;
    }
  }
  auto resolve = [&](const sq_guid& g, int32_t self) -> int32_t {
    if (sq_guid_is_null(&g)) return -1;
    auto it = busIndex.find(g);
    if (it == busIndex.end() || it->second == self) return -1;
    return it->second;
  };
  for (size_t i = 0; i < d.tracks.size(); ++i) d.tracks[i].output = resolve(trackOut[i], -1);
  for (size_t i = 0; i < d.buses.size(); ++i) d.buses[i].output = resolve(busOut[i], static_cast<int32_t>(i));
  if (info) info->structureKey = std::move(key);
  *out = std::move(d);
  return SQ_OK;
}

}  // namespace sq::engine::audio
