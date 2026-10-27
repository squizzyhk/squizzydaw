// Test-only mock of the engine's document model (sq/interfaces/document.h) and of a codec
// service, so scripting-js can be tested without the engine or codec agents' modules.
// Snapshot-based transactions: every outermost transaction is one undo step.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/codec.h"
#include "sq/interfaces/document.h"
#include "sq/platform.h"
#include "sq/sdk.hpp"

namespace mock {

inline sq_guid newGuid() {
  sq_guid g{};
  sq_guid_generate(&g);
  return g;
}

inline sq_result copyStr(const std::string& s, char* buf, uint32_t size, uint32_t* needed) {
  if (needed) *needed = static_cast<uint32_t>(s.size() + 1);
  if (!buf || size < s.size() + 1) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return SQ_OK;
}

struct MClip {
  sq_guid id{};
  std::string name;
  int32_t kind = SQ_CLIP_AUDIO;
  double start = 0, length = 1, sourceOffset = 0, gainDb = 0, fadeIn = 0, fadeOut = 0;
  uint32_t flags = 0, color = 0;
  std::string source;
  std::vector<sq_clip_event> events;
};

struct MTrack {
  sq_guid id{};
  std::string name;
  int32_t kind = SQ_TRACK_AUDIO;
  uint32_t channels = 2, flags = 0, color = 0;
  double volumeDb = 0, pan = 0;
  std::vector<MClip> clips;
  void sortClips() {
    std::stable_sort(clips.begin(), clips.end(), [](const MClip& a, const MClip& b) { return a.start < b.start; });
  }
};

struct Model {
  std::vector<MTrack> tracks;
  std::vector<sq_tempo_point> tempos;
  std::vector<sq_doc_marker> markers;

  MTrack* track(const sq_guid& id) {
    for (auto& t : tracks)
      if (t.id == id) return &t;
    return nullptr;
  }
  MClip* clip(const sq_guid& id, MTrack** owner = nullptr) {
    for (auto& t : tracks)
      for (auto& c : t.clips)
        if (c.id == id) {
          if (owner) *owner = &t;
          return &c;
        }
    return nullptr;
  }
  double bpm() const { return tempos.empty() ? 120.0 : tempos.front().bpm; }
};

struct Shared;
sq::IUnknown* makeNode(const std::shared_ptr<Shared>& s, const sq_guid& id);

struct Shared {
  sq_guid docId = newGuid();
  sq_guid tempoId = newGuid();
  Model state;
  std::vector<Model> levels;  // snapshot per open transaction level
  std::string pendingDescription;
  struct Step {
    std::string description;
    Model before, after;
  };
  std::vector<Step> undo, redo;
  sq::IDocument* doc = nullptr;           // raw back-pointer (the document owns Shared)
  sq::IEditor* editor = nullptr;
  sq::IClassRegistry* registry = nullptr;  // resolves non-built-in actions (scripts)
  int executed = 0;
};

// ---- node views --------------------------------------------------------------------------------
template <class Derived, class Iface>
class NodeBase : public sq::Object<Derived, Iface> {
 public:
  NodeBase(std::shared_ptr<Shared> s, sq_guid id) : s_(std::move(s)), id_(id) {}
  sq_result SQ_CALL getId(sq_guid* out) override {
    *out = id_;
    return SQ_OK;
  }
  sq_result SQ_CALL getUrl(char* buf, uint32_t size, uint32_t* needed) override {
    char d[SQ_GUID_STRING_SIZE], n[SQ_GUID_STRING_SIZE];
    sq_guid_format(&s_->docId, d);
    sq_guid_format(&id_, n);
    return copyStr(std::string("object://doc/") + d + "/node/" + n, buf, size, needed);
  }
  uint32_t SQ_CALL getChildCount() override { return 0; }
  sq_result SQ_CALL getChild(uint32_t, sq::INode**) override { return SQ_E_NOTFOUND; }

 protected:
  std::shared_ptr<Shared> s_;
  sq_guid id_;
};

class ClipView final : public NodeBase<ClipView, sq::IClip> {
 public:
  using NodeBase::NodeBase;
  int32_t SQ_CALL getKind() override { return SQ_NODE_CLIP; }
  sq_result SQ_CALL getName(char* buf, uint32_t size, uint32_t* needed) override {
    MClip* c = s_->state.clip(id_);
    return c ? copyStr(c->name, buf, size, needed) : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getParent(sq::INode** out) override;
  sq_result SQ_CALL getProperties(sq::IAttributes* dst) override {
    MClip* c = s_->state.clip(id_);
    if (!c) return SQ_E_NOTFOUND;
    dst->setString("name", c->name.c_str());
    dst->setDouble("gainDb", c->gainDb);
    dst->setDouble("fadeIn", c->fadeIn);
    dst->setDouble("fadeOut", c->fadeOut);
    dst->setBool("muted", (c->flags & SQ_CLIP_MUTED) ? SQ_TRUE : SQ_FALSE);
    dst->setBool("locked", (c->flags & SQ_CLIP_LOCKED) ? SQ_TRUE : SQ_FALSE);
    return SQ_OK;
  }
  sq_result SQ_CALL getClipInfo(sq_clip_info* out) override {
    MClip* c = s_->state.clip(id_);
    if (!c) return SQ_E_NOTFOUND;
    out->kind = c->kind;
    out->start = c->start;
    out->length = c->length;
    out->source_offset_seconds = c->sourceOffset;
    out->gain_db = c->gainDb;
    out->fade_in_seconds = c->fadeIn;
    out->fade_out_seconds = c->fadeOut;
    out->flags = c->flags;
    out->color = c->color;
    out->event_count = static_cast<uint32_t>(c->events.size());
    return SQ_OK;
  }
  sq_result SQ_CALL getSourcePath(char* buf, uint32_t size, uint32_t* needed) override {
    MClip* c = s_->state.clip(id_);
    return c ? copyStr(c->source, buf, size, needed) : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getEvent(uint32_t index, sq_clip_event* out) override {
    MClip* c = s_->state.clip(id_);
    if (!c || index >= c->events.size()) return SQ_E_NOTFOUND;
    *out = c->events[index];
    return SQ_OK;
  }
};

class TrackView final : public NodeBase<TrackView, sq::ITrack> {
 public:
  using NodeBase::NodeBase;
  int32_t SQ_CALL getKind() override { return SQ_NODE_TRACK; }
  sq_result SQ_CALL getName(char* buf, uint32_t size, uint32_t* needed) override {
    MTrack* t = s_->state.track(id_);
    return t ? copyStr(t->name, buf, size, needed) : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getParent(sq::INode** out) override {
    s_->doc->addRef();
    *out = s_->doc;
    return SQ_OK;
  }
  uint32_t SQ_CALL getChildCount() override {
    MTrack* t = s_->state.track(id_);
    return t ? static_cast<uint32_t>(t->clips.size()) : 0;
  }
  sq_result SQ_CALL getChild(uint32_t i, sq::INode** out) override {
    sq::IClip* c = nullptr;
    sq_result r = getClip(i, &c);
    *out = c;
    return r;
  }
  sq_result SQ_CALL getProperties(sq::IAttributes* dst) override {
    MTrack* t = s_->state.track(id_);
    if (!t) return SQ_E_NOTFOUND;
    dst->setString("name", t->name.c_str());
    dst->setDouble("volumeDb", t->volumeDb);
    dst->setDouble("pan", t->pan);
    return SQ_OK;
  }
  sq_result SQ_CALL getTrackInfo(sq_track_info* out) override {
    MTrack* t = s_->state.track(id_);
    if (!t) return SQ_E_NOTFOUND;
    out->kind = t->kind;
    out->channels = t->channels;
    out->flags = t->flags;
    out->color = t->color;
    out->volume_db = t->volumeDb;
    out->pan = t->pan;
    out->output = sq_guid{};
    out->clip_count = static_cast<uint32_t>(t->clips.size());
    return SQ_OK;
  }
  sq_result SQ_CALL getClip(uint32_t index, sq::IClip** out) override {
    MTrack* t = s_->state.track(id_);
    if (!t || index >= t->clips.size()) return SQ_E_NOTFOUND;
    *out = new ClipView(s_, t->clips[index].id);
    return SQ_OK;
  }
};

inline sq_result SQ_CALL ClipView::getParent(sq::INode** out) {
  MTrack* t = nullptr;
  if (!s_->state.clip(id_, &t)) return SQ_E_NOTFOUND;
  *out = new TrackView(s_, t->id);
  return SQ_OK;
}

class TempoView final : public NodeBase<TempoView, sq::ITempoMap> {
 public:
  using NodeBase::NodeBase;
  int32_t SQ_CALL getKind() override { return SQ_NODE_TEMPO_MAP; }
  sq_result SQ_CALL getName(char* buf, uint32_t size, uint32_t* needed) override { return copyStr("Tempo", buf, size, needed); }
  sq_result SQ_CALL getParent(sq::INode** out) override {
    s_->doc->addRef();
    *out = s_->doc;
    return SQ_OK;
  }
  sq_result SQ_CALL getProperties(sq::IAttributes*) override { return SQ_OK; }
  uint32_t SQ_CALL getTempoCount() override { return static_cast<uint32_t>(s_->state.tempos.size()); }
  sq_result SQ_CALL getTempo(uint32_t i, sq_tempo_point* out) override {
    if (i >= s_->state.tempos.size()) return SQ_E_NOTFOUND;
    *out = s_->state.tempos[i];
    return SQ_OK;
  }
  uint32_t SQ_CALL getSignatureCount() override { return 0; }
  sq_result SQ_CALL getSignature(uint32_t, sq_time_signature*) override { return SQ_E_NOTFOUND; }
  double SQ_CALL ppqToSeconds(sq_ppq_time ppq) override { return ppq * 60.0 / s_->state.bpm(); }
  sq_ppq_time SQ_CALL secondsToPpq(double s) override { return s * s_->state.bpm() / 60.0; }
};

inline sq::IUnknown* makeNode(const std::shared_ptr<Shared>& s, const sq_guid& id) {
  if (s->state.track(id)) return static_cast<sq::ITrack*>(new TrackView(s, id));
  if (s->state.clip(id)) return static_cast<sq::IClip*>(new ClipView(s, id));
  if (id == s->tempoId) return static_cast<sq::ITempoMap*>(new TempoView(s, id));
  return nullptr;
}

// ---- editor ----------------------------------------------------------------------------------------
class Editor final : public sq::Object<Editor, sq::IEditor> {
 public:
  explicit Editor(std::shared_ptr<Shared> s) : s_(std::move(s)) {}

  sq_result SQ_CALL beginTransaction(const char* description) override {
    if (s_->levels.empty()) s_->pendingDescription = description ? description : "";
    s_->levels.push_back(s_->state);
    return SQ_OK;
  }
  sq_result SQ_CALL commitTransaction() override {
    if (s_->levels.empty()) return SQ_E_INVALIDARG;
    Model before = std::move(s_->levels.back());
    s_->levels.pop_back();
    if (s_->levels.empty()) {
      s_->undo.push_back({s_->pendingDescription, std::move(before), s_->state});
      s_->redo.clear();
    }
    return SQ_OK;
  }
  sq_result SQ_CALL cancelTransaction() override {
    if (s_->levels.empty()) return SQ_E_INVALIDARG;
    s_->state = std::move(s_->levels.back());
    s_->levels.pop_back();
    return SQ_OK;
  }
  sq_bool SQ_CALL inTransaction() override { return s_->levels.empty() ? SQ_FALSE : SQ_TRUE; }

  sq_result SQ_CALL execute(const sq_guid* action, sq::IAttributes* params, sq::IAttributes* result) override {
    if (!action) return SQ_E_INVALIDARG;
    const bool implicit = s_->levels.empty();
    if (implicit) beginTransaction("Edit");
    Model before = s_->state;
    sq_result r = apply(*action, params, result);
    if (SQ_FAILED(r)) s_->state = std::move(before);
    if (implicit) {
      if (SQ_FAILED(r))
        cancelTransaction();
      else
        commitTransaction();
    }
    if (SQ_SUCCEEDED(r)) ++s_->executed;
    return r;
  }
  sq_bool SQ_CALL canUndo() override { return s_->undo.empty() ? SQ_FALSE : SQ_TRUE; }
  sq_bool SQ_CALL canRedo() override { return s_->redo.empty() ? SQ_FALSE : SQ_TRUE; }
  sq_result SQ_CALL undo() override {
    if (s_->undo.empty() || !s_->levels.empty()) return SQ_E_INVALIDARG;
    auto step = std::move(s_->undo.back());
    s_->undo.pop_back();
    s_->state = step.before;
    s_->redo.push_back(std::move(step));
    return SQ_OK;
  }
  sq_result SQ_CALL redo() override {
    if (s_->redo.empty() || !s_->levels.empty()) return SQ_E_INVALIDARG;
    auto step = std::move(s_->redo.back());
    s_->redo.pop_back();
    s_->state = step.after;
    s_->undo.push_back(std::move(step));
    return SQ_OK;
  }
  uint32_t SQ_CALL getUndoCount() override { return static_cast<uint32_t>(s_->undo.size()); }
  uint32_t SQ_CALL getRedoCount() override { return static_cast<uint32_t>(s_->redo.size()); }
  sq_result SQ_CALL getUndoDescription(uint32_t i, char* buf, uint32_t size, uint32_t* needed) override {
    if (i >= s_->undo.size()) return SQ_E_NOTFOUND;
    return copyStr(s_->undo[s_->undo.size() - 1 - i].description, buf, size, needed);
  }
  sq_result SQ_CALL getRedoDescription(uint32_t i, char* buf, uint32_t size, uint32_t* needed) override {
    if (i >= s_->redo.size()) return SQ_E_NOTFOUND;
    return copyStr(s_->redo[s_->redo.size() - 1 - i].description, buf, size, needed);
  }
  sq_result SQ_CALL clearHistory() override {
    s_->undo.clear();
    s_->redo.clear();
    return SQ_OK;
  }

 private:
  static bool numeric(sq::IAttributes* p, const char* key) {
    int32_t t = p ? p->getType(key) : SQ_VALUE_NONE;
    return t == SQ_VALUE_INT || t == SQ_VALUE_DOUBLE;
  }
  static bool guidParam(sq::IAttributes* p, const char* key, sq_guid* g) { return p && p->getGuid(key, g) == SQ_OK; }

  sq_result apply(const sq_guid& a, sq::IAttributes* p, sq::IAttributes* result) {
    Model& m = s_->state;
    sq_guid node{};
    if (a == sq::doc::kAddTrack) {
      MTrack t;
      t.id = newGuid();
      t.kind = static_cast<int32_t>(p->getInt("kind", SQ_TRACK_AUDIO));
      t.name = sq::getAttrString(p, "name");
      t.channels = static_cast<uint32_t>(p->getInt("channels", 2));
      int64_t index = p->getInt("index", static_cast<int64_t>(m.tracks.size()));
      index = std::clamp<int64_t>(index, 0, static_cast<int64_t>(m.tracks.size()));
      m.tracks.insert(m.tracks.begin() + index, t);
      if (result) result->setGuid("node", &t.id);
      return SQ_OK;
    }
    if (a == sq::doc::kSetProperty) {
      if (!guidParam(p, "node", &node)) return SQ_E_INVALIDARG;
      const std::string key = sq::getAttrString(p, "key");
      if (MTrack* t = m.track(node)) {
        if (key == "name" && p->getType("value") == SQ_VALUE_STRING) t->name = sq::getAttrString(p, "value");
        else if (key == "volumeDb" && numeric(p, "value")) t->volumeDb = p->getDouble("value", 0);
        else if (key == "pan" && numeric(p, "value")) t->pan = p->getDouble("value", 0);
        else if (key == "color" && numeric(p, "value")) t->color = static_cast<uint32_t>(p->getInt("value", 0));
        else return SQ_E_INVALIDARG;
        return SQ_OK;
      }
      if (MClip* c = m.clip(node)) {
        if (key == "name" && p->getType("value") == SQ_VALUE_STRING) c->name = sq::getAttrString(p, "value");
        else if (key == "gainDb" && numeric(p, "value")) c->gainDb = p->getDouble("value", 0);
        else if (key == "fadeIn" && numeric(p, "value")) c->fadeIn = p->getDouble("value", 0);
        else if (key == "fadeOut" && numeric(p, "value")) c->fadeOut = p->getDouble("value", 0);
        else if (key == "muted" && p->getType("value") == SQ_VALUE_BOOL)
          c->flags = p->getBool("value", SQ_FALSE) ? (c->flags | SQ_CLIP_MUTED) : (c->flags & ~SQ_CLIP_MUTED);
        else if (key == "locked" && p->getType("value") == SQ_VALUE_BOOL)
          c->flags = p->getBool("value", SQ_FALSE) ? (c->flags | SQ_CLIP_LOCKED) : (c->flags & ~SQ_CLIP_LOCKED);
        else return SQ_E_INVALIDARG;
        return SQ_OK;
      }
      return SQ_E_NOTFOUND;
    }
    if (a == sq::doc::kRemoveNode) {
      if (!guidParam(p, "node", &node)) return SQ_E_INVALIDARG;
      for (auto it = m.tracks.begin(); it != m.tracks.end(); ++it)
        if (it->id == node) {
          m.tracks.erase(it);
          return SQ_OK;
        }
      MTrack* owner = nullptr;
      if (MClip* c = m.clip(node, &owner)) {
        owner->clips.erase(owner->clips.begin() + (c - owner->clips.data()));
        return SQ_OK;
      }
      for (auto it = m.markers.begin(); it != m.markers.end(); ++it)
        if (it->id == node) {
          m.markers.erase(it);
          return SQ_OK;
        }
      return SQ_E_NOTFOUND;
    }
    if (a == sq::doc::kAddClip) {
      sq_guid tid{};
      if (!guidParam(p, "track", &tid) || !numeric(p, "start") || !numeric(p, "length")) return SQ_E_INVALIDARG;
      MTrack* t = m.track(tid);
      if (!t) return SQ_E_NOTFOUND;
      MClip c;
      c.id = newGuid();
      c.kind = static_cast<int32_t>(p->getInt("kind", SQ_CLIP_AUDIO));
      c.start = p->getDouble("start", 0);
      c.length = p->getDouble("length", 1);
      c.source = sq::getAttrString(p, "source");
      c.sourceOffset = p->getDouble("source_offset", 0);
      t->clips.push_back(c);
      t->sortClips();
      if (result) result->setGuid("node", &c.id);
      return SQ_OK;
    }
    if (a == sq::doc::kMoveClip) {
      if (!guidParam(p, "node", &node) || !numeric(p, "start")) return SQ_E_INVALIDARG;
      MTrack* owner = nullptr;
      MClip* c = m.clip(node, &owner);
      if (!c) return SQ_E_NOTFOUND;
      c->start = p->getDouble("start", 0);
      owner->sortClips();
      return SQ_OK;
    }
    if (a == sq::doc::kSplitClip) {
      if (!guidParam(p, "node", &node) || !numeric(p, "at")) return SQ_E_INVALIDARG;
      MTrack* owner = nullptr;
      MClip* c = m.clip(node, &owner);
      if (!c) return SQ_E_NOTFOUND;
      const double at = p->getDouble("at", 0);
      if (!(at > c->start && at < c->start + c->length)) return SQ_E_INVALIDARG;
      MClip right = *c;
      right.id = newGuid();
      right.start = at;
      right.length = c->start + c->length - at;
      right.sourceOffset = c->sourceOffset + (at - c->start) * 60.0 / m.bpm();
      c->length = at - c->start;
      owner->clips.push_back(right);
      owner->sortClips();
      if (result) result->setGuid("node", &right.id);
      return SQ_OK;
    }
    if (a == sq::doc::kSetClipEvents) {
      if (!guidParam(p, "clip", &node)) return SQ_E_INVALIDARG;
      MClip* c = m.clip(node);
      if (!c) return SQ_E_NOTFOUND;
      uint32_t need = 0;
      if (p->getType("events") != SQ_VALUE_BLOB) return SQ_E_TYPE;
      p->getBlob("events", nullptr, 0, &need);
      if (need % sizeof(sq_clip_event) != 0) return SQ_E_INVALIDARG;
      std::vector<sq_clip_event> evs(need / sizeof(sq_clip_event));
      if (need) p->getBlob("events", evs.data(), need, &need);
      for (auto& e : evs)
        if (e.struct_size != sizeof(sq_clip_event)) return SQ_E_INVALIDARG;
      c->events = std::move(evs);
      return SQ_OK;
    }
    if (a == sq::doc::kAddMarker) {
      if (!numeric(p, "position")) return SQ_E_INVALIDARG;
      sq_doc_marker mk{};
      mk.struct_size = sizeof mk;
      mk.id = newGuid();
      mk.position = p->getDouble("position", 0);
      mk.length = p->getDouble("length", 0);
      mk.flags = static_cast<uint32_t>(p->getInt("flags", 0));
      std::string name = sq::getAttrString(p, "name");
      std::snprintf(mk.name, sizeof mk.name, "%s", name.c_str());
      m.markers.push_back(mk);
      std::sort(m.markers.begin(), m.markers.end(), [](auto& x, auto& y) { return x.position < y.position; });
      if (result) result->setGuid("node", &mk.id);
      return SQ_OK;
    }
    if (a == sq::doc::kSetTempo) {
      if (!numeric(p, "position") || !numeric(p, "bpm")) return SQ_E_INVALIDARG;
      sq_tempo_point tp{};
      tp.struct_size = sizeof tp;
      tp.position = p->getDouble("position", 0);
      tp.bpm = p->getDouble("bpm", 120);
      tp.flags = p->getBool("ramp", SQ_FALSE) ? SQ_TEMPO_RAMP : 0u;
      for (auto& x : m.tempos)
        if (x.position == tp.position) {
          x = tp;
          return SQ_OK;
        }
      m.tempos.push_back(tp);
      std::sort(m.tempos.begin(), m.tempos.end(), [](auto& x, auto& y) { return x.position < y.position; });
      return SQ_OK;
    }
    // Not built in: an IAction class from the registry (e.g. a script edit task).
    if (!s_->registry) return SQ_E_NOTFOUND;
    sq::Ptr<sq::IAction> act;
    if (s_->registry->createInstance(&a, &sq::IAction::iid, act.putVoid()) != SQ_OK || !act) return SQ_E_NOTFOUND;
    return act->perform(s_->doc, s_->editor, p, result);
  }

  std::shared_ptr<Shared> s_;
};

// ---- document + manager -----------------------------------------------------------------------------
class Document final : public sq::Object<Document, sq::IDocument> {
 public:
  explicit Document(sq::IClassRegistry* reg) : s_(std::make_shared<Shared>()) {
    s_->doc = this;
    s_->registry = reg;
    editor_ = sq::Ptr<Editor>::adopt(new Editor(s_));
    s_->editor = editor_.get();
    sq_tempo_point tp{sizeof(sq_tempo_point), 0.0, 120.0, 0};
    s_->state.tempos.push_back(tp);
  }
  ~Document() override {
    s_->doc = nullptr;
    s_->editor = nullptr;
  }

  Shared& shared() { return *s_; }
  Model& model() { return s_->state; }
  sq::IEditor* editor() { return editor_.get(); }

  MTrack& addTrack(const std::string& name, int32_t kind = SQ_TRACK_AUDIO) {
    MTrack t;
    t.id = newGuid();
    t.name = name;
    t.kind = kind;
    s_->state.tracks.push_back(t);
    return s_->state.tracks.back();
  }
  static MClip& addClip(MTrack& t, const std::string& name, double start, double length, int32_t kind = SQ_CLIP_AUDIO) {
    MClip c;
    c.id = newGuid();
    c.name = name;
    c.start = start;
    c.length = length;
    c.kind = kind;
    t.clips.push_back(c);
    t.sortClips();
    for (auto& x : t.clips)
      if (x.id == c.id) return x;
    return t.clips.back();
  }

  // INode
  int32_t SQ_CALL getKind() override { return SQ_NODE_DOCUMENT; }
  sq_result SQ_CALL getId(sq_guid* out) override {
    *out = s_->docId;
    return SQ_OK;
  }
  sq_result SQ_CALL getName(char* buf, uint32_t size, uint32_t* needed) override { return copyStr("Mock Song", buf, size, needed); }
  sq_result SQ_CALL getUrl(char* buf, uint32_t size, uint32_t* needed) override {
    char d[SQ_GUID_STRING_SIZE];
    sq_guid_format(&s_->docId, d);
    return copyStr(std::string("object://doc/") + d, buf, size, needed);
  }
  sq_result SQ_CALL getParent(sq::INode**) override { return SQ_E_NOTFOUND; }
  uint32_t SQ_CALL getChildCount() override { return static_cast<uint32_t>(s_->state.tracks.size()); }
  sq_result SQ_CALL getChild(uint32_t i, sq::INode** out) override {
    sq::ITrack* t = nullptr;
    sq_result r = getTrack(i, &t);
    *out = t;
    return r;
  }
  sq_result SQ_CALL getProperties(sq::IAttributes* dst) override {
    dst->setString("name", "Mock Song");
    return SQ_OK;
  }
  // IDocument
  sq_result SQ_CALL getDocumentInfo(sq_document_info* out) override {
    out->kind = SQ_DOC_SONG;
    out->id = s_->docId;
    out->sample_rate = 48000;
    out->modified = s_->undo.empty() ? SQ_FALSE : SQ_TRUE;
    out->track_count = static_cast<uint32_t>(s_->state.tracks.size());
    out->length = 0;
    return SQ_OK;
  }
  sq_result SQ_CALL getPath(char* buf, uint32_t size, uint32_t* needed) override { return copyStr("", buf, size, needed); }
  sq_result SQ_CALL getTrack(uint32_t i, sq::ITrack** out) override {
    if (i >= s_->state.tracks.size()) return SQ_E_NOTFOUND;
    *out = new TrackView(s_, s_->state.tracks[i].id);
    return SQ_OK;
  }
  sq_result SQ_CALL getTempoMap(sq::ITempoMap** out) override {
    *out = new TempoView(s_, s_->tempoId);
    return SQ_OK;
  }
  uint32_t SQ_CALL getMarkerCount() override { return static_cast<uint32_t>(s_->state.markers.size()); }
  sq_result SQ_CALL getMarker(uint32_t i, sq_doc_marker* out) override {
    if (i >= s_->state.markers.size()) return SQ_E_NOTFOUND;
    *out = s_->state.markers[i];
    return SQ_OK;
  }
  sq_result SQ_CALL findNode(const sq_guid* id, sq::INode** out) override {
    sq::IUnknown* n = makeNode(s_, *id);
    if (!n) return SQ_E_NOTFOUND;
    sq_result r = n->queryInterface(&sq::INode::iid, reinterpret_cast<void**>(out));
    n->release();
    return r;
  }
  sq_result SQ_CALL getEditor(sq::IEditor** out) override {
    editor_->addRef();
    *out = editor_.get();
    return SQ_OK;
  }

 private:
  std::shared_ptr<Shared> s_;
  sq::Ptr<Editor> editor_;
};

class DocumentManager final : public sq::Object<DocumentManager, sq::IDocumentManager> {
 public:
  sq::Ptr<Document> active;
  sq_result SQ_CALL createDocument(int32_t, sq::IDocument**) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL openDocument(const char*, sq::IDocument**) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL saveDocument(sq::IDocument*, const char*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL closeDocument(sq::IDocument*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getDocumentCount() override { return active ? 1u : 0u; }
  sq_result SQ_CALL getDocument(uint32_t i, sq::IDocument** out) override { return i == 0 ? getActiveDocument(out) : SQ_E_NOTFOUND; }
  sq_result SQ_CALL getActiveDocument(sq::IDocument** out) override {
    if (!active) return SQ_E_NOTFOUND;
    active->addRef();
    *out = active.get();
    return SQ_OK;
  }
  sq_result SQ_CALL setActiveDocument(sq::IDocument*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL findNodeByUrl(const char*, sq::INode**) override { return SQ_E_NOTIMPL; }
};

// ---- codec: ".tone" files are a constant-amplitude square wave, 2 s, 48 kHz stereo ----------------
class ToneReader final : public sq::Object<ToneReader, sq::IAudioFileReader> {
 public:
  static constexpr double kAmplitude = 0.5;
  static constexpr int64_t kFrames = 96000;
  sq_result SQ_CALL open(const char* path) override {
    std::string p = path ? path : "";
    return p.size() > 5 && p.compare(p.size() - 5, 5, ".tone") == 0 ? SQ_OK : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL openStream(sq::IByteStream*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL getFormat(sq_audio_file_format* f) override {
    f->sample_rate = 48000;
    f->channels = 2;
    f->sample_format = SQ_SAMPLE_F32;
    f->bits_per_sample = 32;
    f->frame_count = kFrames;
    f->flags = SQ_AUDIOFILE_SEEKABLE;
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* got) override {
    const int64_t n = std::min<int64_t>(dst->frames, kFrames - pos_);
    for (uint32_t c = 0; c < dst->channel_count; ++c)
      for (int64_t i = 0; i < n; ++i) dst->channels[c][i] = static_cast<float>(((pos_ + i) & 1) ? kAmplitude : -kAmplitude);
    pos_ += n;
    *got = static_cast<uint32_t>(n);
    return SQ_OK;
  }
  sq_result SQ_CALL seek(sq_sample_time frame) override {
    pos_ = std::clamp<int64_t>(frame, 0, kFrames);
    return SQ_OK;
  }
  sq_result SQ_CALL tell(sq_sample_time* f) override {
    *f = pos_;
    return SQ_OK;
  }
  uint32_t SQ_CALL getMetadataCount() override { return 0; }
  sq_result SQ_CALL getMetadataKey(uint32_t, char*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL getMetadata(const char*, void*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  uint32_t SQ_CALL getMarkerCount() override { return 0; }
  sq_result SQ_CALL getMarker(uint32_t, sq_audio_file_marker*) override { return SQ_E_NOTFOUND; }

 private:
  int64_t pos_ = 0;
};

class ToneCodec final : public sq::Object<ToneCodec, sq::ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info* out) override {
    std::snprintf(out->name, sizeof out->name, "Tone (test)");
    out->flags = SQ_CODEC_READ;
    return SQ_OK;
  }
  uint32_t SQ_CALL getFormatCount() override { return 1; }
  sq_result SQ_CALL getFormatInfo(uint32_t, sq_codec_format_info*) override { return SQ_E_NOTIMPL; }
  sq_bool SQ_CALL supports(const char* ext, const char*, uint32_t flags) override {
    return (ext && std::strcmp(ext, "tone") == 0 && !(flags & SQ_CODEC_WRITE)) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void*, uint32_t) override { return 0; }
  sq_result SQ_CALL createReader(sq::IAudioFileReader** out) override {
    *out = new ToneReader();
    return SQ_OK;
  }
  sq_result SQ_CALL createWriter(const char*, sq::IAudioFileWriter**) override { return SQ_E_NOTIMPL; }
};

// Factory for registering a test class in the app registry.
template <class T, class I>
class SimpleFactory final : public sq::Object<SimpleFactory<T, I>, sq::IClassFactory> {
 public:
  explicit SimpleFactory(sq_guid id) : id_(id) {}
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid* cls, const sq_guid* riid, void** out) override {
    if (!(*cls == id_)) return SQ_E_NOTFOUND;
    T* t = new T();
    sq_result r = t->queryInterface(riid, out);
    t->release();
    return r;
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }

 private:
  sq_guid id_;
};

}  // namespace mock
