// engine/audio tests - fakes for interfaces implemented by other agents (codec.h, document.h).
// In-memory codec ("tst" extension) + a small mock document with an action-logging editor.
#pragma once

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/platform.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/codec.h"
#include "sq/interfaces/document.h"

namespace fakes {

inline constexpr sq_guid kFakeCodecClassId = sq::guid("{B9BD417D-2EC7-46A7-92C4-D21C6397CB56}");

struct MemFile {
  double sampleRate = 48000.0;
  std::vector<std::vector<float>> data;  // planar
  bool finalized = false;
  int64_t frames() const { return data.empty() ? 0 : static_cast<int64_t>(data[0].size()); }
};

inline std::mutex& memMutex() {
  static std::mutex m;
  return m;
}
inline std::map<std::string, MemFile>& memFiles() {
  static std::map<std::string, MemFile> f;
  return f;
}
inline void putMemFile(const std::string& path, double sr, std::vector<std::vector<float>> data) {
  std::lock_guard<std::mutex> lk(memMutex());
  MemFile& m = memFiles()[path];
  m.sampleRate = sr;
  m.data = std::move(data);
  m.finalized = true;
}
inline bool getMemFile(const std::string& path, MemFile* out) {
  std::lock_guard<std::mutex> lk(memMutex());
  auto it = memFiles().find(path);
  if (it == memFiles().end()) return false;
  *out = it->second;
  return true;
}

class FakeReader final : public sq::Object<FakeReader, sq::IAudioFileReader> {
 public:
  sq_result SQ_CALL open(const char* path) override {
    if (!getMemFile(path, &f_)) return SQ_E_NOTFOUND;
    pos_ = 0;
    return SQ_OK;
  }
  sq_result SQ_CALL openStream(sq::IByteStream*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override {
    out->sample_rate = f_.sampleRate;
    out->channels = static_cast<uint32_t>(f_.data.size());
    out->sample_format = SQ_SAMPLE_F32;
    out->bits_per_sample = 32;
    out->frame_count = f_.frames();
    out->flags = SQ_AUDIOFILE_SEEKABLE | SQ_AUDIOFILE_SAMPLE_EXACT;
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* framesRead) override {
    const int64_t n = std::min<int64_t>(dst->frames, f_.frames() - pos_);
    for (uint32_t c = 0; c < dst->channel_count && c < f_.data.size(); ++c)
      std::memcpy(dst->channels[c], f_.data[c].data() + pos_, sizeof(float) * static_cast<size_t>(n));
    pos_ += n;
    *framesRead = static_cast<uint32_t>(n);
    return SQ_OK;
  }
  sq_result SQ_CALL seek(sq_sample_time frame) override {
    pos_ = frame;
    return SQ_OK;
  }
  sq_result SQ_CALL tell(sq_sample_time* frame) override {
    *frame = pos_;
    return SQ_OK;
  }
  uint32_t SQ_CALL getMetadataCount() override { return 0; }
  sq_result SQ_CALL getMetadataKey(uint32_t, char*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL getMetadata(const char*, void*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  uint32_t SQ_CALL getMarkerCount() override { return 0; }
  sq_result SQ_CALL getMarker(uint32_t, sq_audio_file_marker*) override { return SQ_E_NOTFOUND; }

 private:
  MemFile f_;
  int64_t pos_ = 0;
};

class FakeWriter final : public sq::Object<FakeWriter, sq::IAudioFileWriter> {
 public:
  sq_result SQ_CALL open(const char* path, const sq_audio_file_format* format, sq::IAttributes*) override {
    path_ = path;
    f_.sampleRate = format->sample_rate;
    f_.data.assign(format->channels, {});
    return SQ_OK;
  }
  sq_result SQ_CALL openStream(sq::IByteStream*, const sq_audio_file_format*, sq::IAttributes*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL setMetadata(const char*, const void*, uint32_t) override { return SQ_OK; }
  sq_result SQ_CALL addMarker(const sq_audio_file_marker*) override { return SQ_OK; }
  sq_result SQ_CALL write(const sq_audio_block* src) override {
    if (src->channel_count != f_.data.size()) return SQ_E_INVALIDARG;
    for (uint32_t c = 0; c < src->channel_count; ++c) f_.data[c].insert(f_.data[c].end(), src->channels[c], src->channels[c] + src->frames);
    return SQ_OK;
  }
  sq_result SQ_CALL finalize() override {
    f_.finalized = true;
    std::lock_guard<std::mutex> lk(memMutex());
    memFiles()[path_] = f_;
    return SQ_OK;
  }

 private:
  std::string path_;
  MemFile f_;
};

class FakeCodec final : public sq::Object<FakeCodec, sq::ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info* out) override {
    std::strcpy(out->name, "Fake");
    out->flags = SQ_CODEC_READ | SQ_CODEC_WRITE;
    return SQ_OK;
  }
  uint32_t SQ_CALL getFormatCount() override { return 1; }
  sq_result SQ_CALL getFormatInfo(uint32_t, sq_codec_format_info* out) override {
    std::strcpy(out->extension, "tst");
    return SQ_OK;
  }
  sq_bool SQ_CALL supports(const char* ext, const char*, uint32_t) override {
    return ext && (std::strcmp(ext, "tst") == 0) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void*, uint32_t) override { return 0; }
  sq_result SQ_CALL createReader(sq::IAudioFileReader** out) override {
    *out = new FakeReader();
    return SQ_OK;
  }
  sq_result SQ_CALL createWriter(const char*, sq::IAudioFileWriter** out) override {
    *out = new FakeWriter();
    return SQ_OK;
  }
};

template <class T>
class FnFactory final : public sq::Object<FnFactory<T>, sq::IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid*, const sq_guid* riid, void** out) override { return sq::createObject<T>(riid, out); }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

inline void registerFakeCodec(sq::IClassRegistry* reg) {
  sq_class_info info{};
  info.struct_size = sizeof info;
  info.class_id = kFakeCodecClassId;
  info.category = sq::codec::kCategory;
  info.name = "Fake Codec";
  sq::Ptr<sq::IClassFactory> f = sq::Ptr<sq::IClassFactory>::adopt(new FnFactory<FakeCodec>());
  reg->registerClass(&info, f.get());
}

inline sq_guid newGuid() {
  sq_guid g{};
  sq_guid_generate(&g);
  return g;
}

// ---------------------------------------------------------------- mock document
template <class D, class I>
class NodeBase : public sq::Object<D, I> {
 public:
  sq_guid id = newGuid();
  std::string name;
  int32_t kind = SQ_NODE_OTHER;
  int32_t SQ_CALL getKind() override { return kind; }
  sq_result SQ_CALL getId(sq_guid* out) override {
    *out = id;
    return SQ_OK;
  }
  sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) override { return str(name, buf, bufSize, needed); }
  sq_result SQ_CALL getUrl(char* buf, uint32_t bufSize, uint32_t* needed) override { return str("object://doc/active", buf, bufSize, needed); }
  sq_result SQ_CALL getParent(sq::INode**) override { return SQ_E_NOTFOUND; }
  uint32_t SQ_CALL getChildCount() override { return 0; }
  sq_result SQ_CALL getChild(uint32_t, sq::INode**) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL getProperties(sq::IAttributes* dst) override {
    for (auto& kv : intProps) dst->setInt(kv.first.c_str(), kv.second);
    for (auto& kv : strProps) dst->setString(kv.first.c_str(), kv.second.c_str());
    return SQ_OK;
  }
  std::map<std::string, int64_t> intProps;
  std::map<std::string, std::string> strProps;  // e.g. user.inserts, user.instrument, automation

  static sq_result str(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed) {
    const uint32_t need = static_cast<uint32_t>(s.size() + 1);
    if (needed) *needed = need;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, s.c_str(), need);
    return SQ_OK;
  }
};

class MClip final : public NodeBase<MClip, sq::IClip> {
 public:
  MClip() {
    kind = SQ_NODE_CLIP;
    info.struct_size = sizeof info;
    info.kind = SQ_CLIP_AUDIO;
  }
  sq_clip_info info{};
  std::string path;
  std::vector<sq_clip_event> events;
  sq_result SQ_CALL getClipInfo(sq_clip_info* out) override {
    *out = info;
    out->event_count = static_cast<uint32_t>(events.size());
    return SQ_OK;
  }
  sq_result SQ_CALL getSourcePath(char* buf, uint32_t bufSize, uint32_t* needed) override { return str(path, buf, bufSize, needed); }
  sq_result SQ_CALL getEvent(uint32_t index, sq_clip_event* out) override {
    if (index >= events.size()) return SQ_E_NOTFOUND;
    *out = events[index];
    return SQ_OK;
  }
};

class MTrack final : public NodeBase<MTrack, sq::ITrack> {
 public:
  MTrack() {
    kind = SQ_NODE_TRACK;
    info.struct_size = sizeof info;
    info.kind = SQ_TRACK_AUDIO;
    info.channels = 2;
  }
  sq_track_info info{};
  std::vector<sq::Ptr<MClip>> clips;
  sq_result SQ_CALL getTrackInfo(sq_track_info* out) override {
    *out = info;
    out->clip_count = static_cast<uint32_t>(clips.size());
    return SQ_OK;
  }
  sq_result SQ_CALL getClip(uint32_t index, sq::IClip** out) override {
    if (index >= clips.size()) return SQ_E_NOTFOUND;
    *out = clips[index].get();
    (*out)->addRef();
    return SQ_OK;
  }
};

class MTempo final : public NodeBase<MTempo, sq::ITempoMap> {
 public:
  double bpm = 120.0;
  uint32_t SQ_CALL getTempoCount() override { return 1; }
  sq_result SQ_CALL getTempo(uint32_t, sq_tempo_point* out) override {
    out->position = 0;
    out->bpm = bpm;
    out->flags = 0;
    return SQ_OK;
  }
  uint32_t SQ_CALL getSignatureCount() override { return 1; }
  sq_result SQ_CALL getSignature(uint32_t, sq_time_signature* out) override {
    out->position = 0;
    out->numerator = 4;
    out->denominator = 4;
    return SQ_OK;
  }
  double SQ_CALL ppqToSeconds(sq_ppq_time ppq) override { return ppq * 60.0 / bpm; }
  sq_ppq_time SQ_CALL secondsToPpq(double s) override { return s * bpm / 60.0; }
};

struct ActionLog {
  std::string action;  // "split", "remove", "add", "begin", "commit", "cancel"
  sq_guid node{};
  double a = 0, b = 0;
  std::string source;
  std::vector<sq_clip_event> events;  // "events" (kSetClipEvents)
  int64_t kind = 0;                   // "add": clip kind
};

class MDoc;

class MEditor final : public sq::Object<MEditor, sq::IEditor> {
 public:
  MDoc* doc = nullptr;
  std::vector<ActionLog> log;
  int depth = 0;
  sq_result SQ_CALL beginTransaction(const char*) override {
    ++depth;
    log.push_back(ActionLog{"begin", {}, 0, 0, {}, {}, 0});
    return SQ_OK;
  }
  sq_result SQ_CALL commitTransaction() override;
  sq_result SQ_CALL cancelTransaction() override {
    --depth;
    log.push_back(ActionLog{"cancel", {}, 0, 0, {}, {}, 0});
    return SQ_OK;
  }
  sq_bool SQ_CALL inTransaction() override { return depth > 0; }
  sq_result SQ_CALL execute(const sq_guid* id, sq::IAttributes* p, sq::IAttributes* result) override {
    ActionLog l;
    if (*id == sq::doc::kSplitClip) {
      l.action = "split";
      p->getGuid("node", &l.node);
      l.a = p->getDouble("at", 0);
      if (result) {
        sq_guid g = newGuid();
        result->setGuid("node", &g);
      }
    } else if (*id == sq::doc::kRemoveNode) {
      l.action = "remove";
      p->getGuid("node", &l.node);
    } else if (*id == sq::doc::kAddClip) {
      l.action = "add";
      p->getGuid("track", &l.node);
      l.a = p->getDouble("start", 0);
      l.b = p->getDouble("length", 0);
      l.kind = p->getInt("kind", 0);
      char buf[1024];
      uint32_t need = 0;
      if (p->getString("source", buf, sizeof buf, &need) == SQ_OK) l.source = buf;
      if (result) {
        sq_guid g = newGuid();
        result->setGuid("node", &g);
      }
    } else if (*id == sq::doc::kSetClipEvents) {
      l.action = "events";
      p->getGuid("clip", &l.node);
      uint32_t need = 0;
      p->getBlob("events", nullptr, 0, &need);
      l.events.resize(need / sizeof(sq_clip_event));
      if (need) p->getBlob("events", l.events.data(), need, &need);
    } else {
      return SQ_E_NOTIMPL;
    }
    log.push_back(l);
    return SQ_OK;
  }
  sq_bool SQ_CALL canUndo() override { return SQ_FALSE; }
  sq_bool SQ_CALL canRedo() override { return SQ_FALSE; }
  sq_result SQ_CALL undo() override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL redo() override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getUndoCount() override { return 0; }
  uint32_t SQ_CALL getRedoCount() override { return 0; }
  sq_result SQ_CALL getUndoDescription(uint32_t, char*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL getRedoDescription(uint32_t, char*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL clearHistory() override { return SQ_OK; }
};

class MDoc final : public NodeBase<MDoc, sq::IDocument>, public sq::ISubject {
 public:
  MDoc() {
    kind = SQ_NODE_DOCUMENT;
    tempo = sq::Ptr<MTempo>::adopt(new MTempo());
    editor = sq::Ptr<MEditor>::adopt(new MEditor());
    editor->doc = this;
  }
  ~MDoc() override { editor->doc = nullptr; }
  double sampleRate = 48000.0;
  std::vector<sq::Ptr<MTrack>> tracks;
  sq::Ptr<MTempo> tempo;
  sq::Ptr<MEditor> editor;
  std::vector<sq::Ptr<sq::IObserver>> observers;
  int changedFired = 0;

  // IUnknown: route through the first base, add ISubject.
  sq_result SQ_CALL queryInterface(const sq_guid* riid, void** out) override {
    if (riid && *riid == sq::ISubject::iid) {
      addRef();
      *out = static_cast<sq::ISubject*>(this);
      return SQ_OK;
    }
    return NodeBase<MDoc, sq::IDocument>::queryInterface(riid, out);
  }
  uint32_t SQ_CALL addRef() override { return NodeBase<MDoc, sq::IDocument>::addRef(); }
  uint32_t SQ_CALL release() override { return NodeBase<MDoc, sq::IDocument>::release(); }

  sq_result SQ_CALL getDocumentInfo(sq_document_info* out) override {
    out->kind = SQ_DOC_SONG;
    out->id = id;
    out->sample_rate = sampleRate;
    out->modified = SQ_FALSE;
    out->track_count = static_cast<uint32_t>(tracks.size());
    out->length = 0;
    return SQ_OK;
  }
  sq_result SQ_CALL getPath(char* buf, uint32_t bufSize, uint32_t* needed) override { return str("", buf, bufSize, needed); }
  sq_result SQ_CALL getTrack(uint32_t index, sq::ITrack** out) override {
    if (index >= tracks.size()) return SQ_E_NOTFOUND;
    *out = tracks[index].get();
    (*out)->addRef();
    return SQ_OK;
  }
  sq_result SQ_CALL getTempoMap(sq::ITempoMap** out) override {
    *out = tempo.get();
    (*out)->addRef();
    return SQ_OK;
  }
  uint32_t SQ_CALL getMarkerCount() override { return 0; }
  sq_result SQ_CALL getMarker(uint32_t, sq_doc_marker*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL findNode(const sq_guid* nid, sq::INode** out) override {
    for (auto& t : tracks) {
      if (t->id == *nid) {
        *out = static_cast<sq::ITrack*>(t.get());
        (*out)->addRef();
        return SQ_OK;
      }
    }
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getEditor(sq::IEditor** out) override {
    *out = editor.get();
    (*out)->addRef();
    return SQ_OK;
  }
  sq_result SQ_CALL addObserver(sq::IObserver* o) override {
    observers.emplace_back(o);
    return SQ_OK;
  }
  sq_result SQ_CALL removeObserver(sq::IObserver* o) override {
    for (auto it = observers.begin(); it != observers.end(); ++it)
      if (it->get() == o) {
        observers.erase(it);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }
  void fireChanged() {
    ++changedFired;
    auto obs = observers;
    for (auto& o : obs) o->onNotify(static_cast<sq::IDocument*>(this), "document.changed", nullptr);
  }
};

inline sq_result MEditor::commitTransaction() {
  --depth;
  log.push_back(ActionLog{"commit", {}, 0, 0, {}, {}, 0});
  if (depth == 0 && doc) doc->fireChanged();
  return SQ_OK;
}

}  // namespace fakes
