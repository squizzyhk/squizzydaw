// Squizzy Studio engine/doc - read-only COM views of the model (INode/ITrack/IClip/ITempoMap,
// IDocument read side). Views hold the document and a node id and look the record up per call, so
// they stay valid across undo/redo (SQ_E_NOTFOUND while the node does not exist).
#include <algorithm>
#include <cstring>

#include "internal.hpp"

namespace sq::edoc {

namespace {

sq_result outNode(INode* n, INode** out) {
  *out = n;
  return SQ_OK;
}

template <class D, class I>
class NodeBase : public Object<D, I> {
 public:
  NodeBase(DocumentImpl* doc, const sq_guid& id) : doc_(doc), id_(id) {}
  sq_result SQ_CALL getId(sq_guid* out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = id_;
    return SQ_OK;
  }
  sq_result SQ_CALL getUrl(char* buf, uint32_t bufSize, uint32_t* needed) override {
    return copyString(doc_->urlOf(id_), buf, bufSize, needed);
  }

 protected:
  const Store& st() const { return doc_->store(); }
  sq_result docAsNode(INode** out) {
    if (!out) return SQ_E_INVALIDARG;
    return doc_->queryInterface(&INode::iid, reinterpret_cast<void**>(out));
  }
  Ptr<DocumentImpl> doc_;
  sq_guid id_;
};

class TempoNode final : public NodeBase<TempoNode, ITempoMap> {
 public:
  using NodeBase::NodeBase;
  int32_t SQ_CALL getKind() override { return SQ_NODE_TEMPO_MAP; }
  sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) override {
    return copyString("Tempo", buf, bufSize, needed);
  }
  sq_result SQ_CALL getParent(INode** out) override { return docAsNode(out); }
  uint32_t SQ_CALL getChildCount() override { return 0; }
  sq_result SQ_CALL getChild(uint32_t, INode** out) override {
    if (out) *out = nullptr;
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getProperties(IAttributes* dst) override {
    if (!dst) return SQ_E_INVALIDARG;
    dst->setInt("tempoCount", static_cast<int64_t>(st().tempo.tempos.size()));
    dst->setInt("signatureCount", static_cast<int64_t>(st().tempo.sigs.size()));
    return SQ_OK;
  }
  uint32_t SQ_CALL getTempoCount() override { return static_cast<uint32_t>(st().tempo.tempos.size()); }
  sq_result SQ_CALL getTempo(uint32_t index, sq_tempo_point* out) override {
    if (!out) return SQ_E_INVALIDARG;
    if (index >= st().tempo.tempos.size()) return SQ_E_NOTFOUND;
    const TempoPoint& p = st().tempo.tempos[index];
    out->struct_size = sizeof *out;
    out->position = p.pos;
    out->bpm = p.bpm;
    out->flags = p.flags;
    return SQ_OK;
  }
  uint32_t SQ_CALL getSignatureCount() override { return static_cast<uint32_t>(st().tempo.sigs.size()); }
  sq_result SQ_CALL getSignature(uint32_t index, sq_time_signature* out) override {
    if (!out) return SQ_E_INVALIDARG;
    if (index >= st().tempo.sigs.size()) return SQ_E_NOTFOUND;
    const SigPoint& p = st().tempo.sigs[index];
    out->struct_size = sizeof *out;
    out->position = p.pos;
    out->numerator = p.num;
    out->denominator = p.den;
    return SQ_OK;
  }
  double SQ_CALL ppqToSeconds(sq_ppq_time ppq) override { return st().tempo.ppqToSeconds(ppq); }
  sq_ppq_time SQ_CALL secondsToPpq(double seconds) override { return st().tempo.secondsToPpq(seconds); }
};

class MarkerNode final : public NodeBase<MarkerNode, INode> {
 public:
  using NodeBase::NodeBase;
  int32_t SQ_CALL getKind() override { return SQ_NODE_MARKER; }
  sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) override {
    const MarkerRec* m = st().marker(id_);
    return m ? copyString(m->name, buf, bufSize, needed) : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getParent(INode** out) override { return docAsNode(out); }
  uint32_t SQ_CALL getChildCount() override { return 0; }
  sq_result SQ_CALL getChild(uint32_t, INode** out) override {
    if (out) *out = nullptr;
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getProperties(IAttributes* dst) override {
    if (!dst) return SQ_E_INVALIDARG;
    const MarkerRec* m = st().marker(id_);
    if (!m) return SQ_E_NOTFOUND;
    detail::markerProps(*m, dst);
    return SQ_OK;
  }
};

class ClipNode final : public NodeBase<ClipNode, IClip> {
 public:
  using NodeBase::NodeBase;
  int32_t SQ_CALL getKind() override { return SQ_NODE_CLIP; }
  sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) override {
    const ClipRec* c = st().clip(id_);
    return c ? copyString(c->name, buf, bufSize, needed) : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getParent(INode** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    const ClipRec* c = st().clip(id_);
    return c ? doc_->makeNode(c->track, out) : SQ_E_NOTFOUND;
  }
  uint32_t SQ_CALL getChildCount() override { return 0; }
  sq_result SQ_CALL getChild(uint32_t, INode** out) override {
    if (out) *out = nullptr;
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getProperties(IAttributes* dst) override {
    if (!dst) return SQ_E_INVALIDARG;
    const ClipRec* c = st().clip(id_);
    if (!c) return SQ_E_NOTFOUND;
    detail::clipProps(*c, dst);
    return SQ_OK;
  }
  sq_result SQ_CALL getClipInfo(sq_clip_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    const ClipRec* c = st().clip(id_);
    if (!c) return SQ_E_NOTFOUND;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
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
  sq_result SQ_CALL getSourcePath(char* buf, uint32_t bufSize, uint32_t* needed) override {
    const ClipRec* c = st().clip(id_);
    return c ? copyString(c->source, buf, bufSize, needed) : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getEvent(uint32_t index, sq_clip_event* out) override {
    if (!out) return SQ_E_INVALIDARG;
    const ClipRec* c = st().clip(id_);
    if (!c) return SQ_E_NOTFOUND;
    if (index >= c->events.size()) return SQ_E_NOTFOUND;
    const EventRec& e = c->events[index];
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->position = e.pos;
    out->length = e.len;
    std::memcpy(out->bytes, e.bytes, 4);
    out->size = e.size;
    return SQ_OK;
  }
};

class TrackNode final : public NodeBase<TrackNode, ITrack> {
 public:
  using NodeBase::NodeBase;
  int32_t SQ_CALL getKind() override { return SQ_NODE_TRACK; }
  sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) override {
    const TrackRec* t = st().track(id_);
    return t ? copyString(t->name, buf, bufSize, needed) : SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getParent(INode** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    const TrackRec* t = st().track(id_);
    if (!t) return SQ_E_NOTFOUND;
    if (sq_guid_is_null(&t->parent)) return docAsNode(out);
    return doc_->makeNode(t->parent, out);
  }
  // children: child tracks (display order), then clips (by start)
  uint32_t SQ_CALL getChildCount() override {
    if (!st().track(id_)) return 0;
    return static_cast<uint32_t>(childTracks().size() + st().clipsOf(id_).size());
  }
  sq_result SQ_CALL getChild(uint32_t index, INode** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!st().track(id_)) return SQ_E_NOTFOUND;
    auto kids = childTracks();
    if (index < kids.size()) return doc_->makeNode(kids[index], out);
    index -= static_cast<uint32_t>(kids.size());
    const auto& clips = st().clipsOf(id_);
    if (index < clips.size()) return doc_->makeNode(clips[index], out);
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL getProperties(IAttributes* dst) override {
    if (!dst) return SQ_E_INVALIDARG;
    const TrackRec* t = st().track(id_);
    if (!t) return SQ_E_NOTFOUND;
    detail::trackProps(*t, dst);
    return SQ_OK;
  }
  sq_result SQ_CALL getTrackInfo(sq_track_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    const TrackRec* t = st().track(id_);
    if (!t) return SQ_E_NOTFOUND;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->kind = t->kind;
    out->channels = t->channels;
    out->flags = t->flags;
    out->color = t->color;
    out->volume_db = t->volumeDb;
    out->pan = t->pan;
    out->output = t->output;
    out->clip_count = static_cast<uint32_t>(st().clipsOf(id_).size());
    return SQ_OK;
  }
  sq_result SQ_CALL getClip(uint32_t index, IClip** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    const auto& clips = st().clipsOf(id_);
    if (index >= clips.size()) return SQ_E_NOTFOUND;
    *out = new ClipNode(doc_.get(), clips[index]);
    return SQ_OK;
  }

 private:
  std::vector<sq_guid> childTracks() const {
    std::vector<sq_guid> v;
    for (const sq_guid& id : st().trackOrder)
      if (st().tracks.at(id).parent == id_) v.push_back(id);
    return v;
  }
};

}  // namespace

// ---- DocumentImpl read side ------------------------------------------------------------------------
std::string DocumentImpl::urlOf(const sq_guid& node) const {
  std::string u = "object://doc/" + guidStr(store_.props.id);
  if (node != store_.props.id) u += "/node/" + guidStr(node);
  return u;
}

sq_result DocumentImpl::makeNode(const sq_guid& id, INode** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (id == store_.props.id) return queryInterface(&INode::iid, reinterpret_cast<void**>(out));
  if (id == store_.tempo.id) return outNode(new TempoNode(this, id), out);
  if (store_.track(id)) return outNode(new TrackNode(this, id), out);
  if (store_.clip(id)) return outNode(new ClipNode(this, id), out);
  if (store_.marker(id)) return outNode(new MarkerNode(this, id), out);
  return SQ_E_NOTFOUND;
}

sq_result SQ_CALL DocumentImpl::getId(sq_guid* out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = store_.props.id;
  return SQ_OK;
}
sq_result SQ_CALL DocumentImpl::getName(char* buf, uint32_t bufSize, uint32_t* needed) {
  return copyString(store_.props.name, buf, bufSize, needed);
}
sq_result SQ_CALL DocumentImpl::getUrl(char* buf, uint32_t bufSize, uint32_t* needed) {
  return copyString(urlOf(store_.props.id), buf, bufSize, needed);
}
sq_result SQ_CALL DocumentImpl::getParent(INode** out) {
  if (out) *out = nullptr;
  return SQ_E_NOTFOUND;
}

// children: tempo map, top-level tracks (display order), markers (by position)
uint32_t SQ_CALL DocumentImpl::getChildCount() {
  uint32_t n = 1;
  for (const sq_guid& id : store_.trackOrder)
    if (sq_guid_is_null(&store_.tracks.at(id).parent)) ++n;
  return n + static_cast<uint32_t>(store_.markers.size());
}
sq_result SQ_CALL DocumentImpl::getChild(uint32_t index, INode** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (index == 0) return makeNode(store_.tempo.id, out);
  --index;
  for (const sq_guid& id : store_.trackOrder) {
    if (!sq_guid_is_null(&store_.tracks.at(id).parent)) continue;
    if (index == 0) return makeNode(id, out);
    --index;
  }
  const auto& mo = store_.markerOrder();
  if (index < mo.size()) return makeNode(mo[index], out);
  return SQ_E_NOTFOUND;
}
sq_result SQ_CALL DocumentImpl::getProperties(IAttributes* dst) {
  if (!dst) return SQ_E_INVALIDARG;
  detail::docProps(store_.props, dst);
  return SQ_OK;
}

sq_result SQ_CALL DocumentImpl::getDocumentInfo(sq_document_info* out) {
  if (!out) return SQ_E_INVALIDARG;
  std::memset(out, 0, sizeof *out);
  out->struct_size = sizeof *out;
  out->kind = store_.props.kind;
  out->id = store_.props.id;
  out->sample_rate = store_.props.sampleRate;
  out->modified = modified() ? SQ_TRUE : SQ_FALSE;
  out->track_count = static_cast<uint32_t>(store_.trackOrder.size());
  out->length = store_.contentLength();
  return SQ_OK;
}
sq_result SQ_CALL DocumentImpl::getPath(char* buf, uint32_t bufSize, uint32_t* needed) {
  return copyString(path_, buf, bufSize, needed);
}
sq_result SQ_CALL DocumentImpl::getTrack(uint32_t index, ITrack** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (index >= store_.trackOrder.size()) return SQ_E_NOTFOUND;
  *out = new TrackNode(this, store_.trackOrder[index]);
  return SQ_OK;
}
sq_result SQ_CALL DocumentImpl::getTempoMap(ITempoMap** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = new TempoNode(this, store_.tempo.id);
  return SQ_OK;
}
uint32_t SQ_CALL DocumentImpl::getMarkerCount() { return static_cast<uint32_t>(store_.markers.size()); }
sq_result SQ_CALL DocumentImpl::getMarker(uint32_t index, sq_doc_marker* out) {
  if (!out) return SQ_E_INVALIDARG;
  const auto& mo = store_.markerOrder();
  if (index >= mo.size()) return SQ_E_NOTFOUND;
  const MarkerRec& m = store_.markers.at(mo[index]);
  std::memset(out, 0, sizeof *out);
  out->struct_size = sizeof *out;
  out->id = m.id;
  out->position = m.position;
  out->length = m.length;
  out->flags = m.flags;
  out->color = m.color;
  size_t n = std::min(m.name.size(), sizeof(out->name) - 1);
  if (n < m.name.size()) {  // truncating: back off to a UTF-8 sequence boundary
    while (n > 0 && (static_cast<unsigned char>(m.name[n]) & 0xC0) == 0x80) --n;
  }
  std::memcpy(out->name, m.name.data(), n);
  out->name[n] = '\0';
  return SQ_OK;
}
sq_result SQ_CALL DocumentImpl::findNode(const sq_guid* id, INode** out) {
  if (!id) return SQ_E_INVALIDARG;
  return makeNode(*id, out);
}
sq_result SQ_CALL DocumentImpl::getEditor(IEditor** out) {
  if (!out) return SQ_E_INVALIDARG;
  return queryInterface(&IEditor::iid, reinterpret_cast<void**>(out));
}

}  // namespace sq::edoc
