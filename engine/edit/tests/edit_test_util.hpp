// engine/edit tests - shared fixture, helpers and a fake codec (ICodecService is another agent's
// interface: mocked here, registered in an sq::app::ClassRegistry).
#pragma once

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "sq/app/class_registry.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/engine/doc/actions.hpp"
#include "sq/engine/doc/manager.hpp"
#include "sq/engine/edit/edit.hpp"
#include "sq/interfaces/codec.h"
#include "sqtest.hpp"

namespace edittest {

using sq::Ptr;
using sq::edoc::ClipRec;

// ---- fake media ------------------------------------------------------------------------------------
struct FakeMedia {
  double rate = 1000.0;
  std::vector<float> samples;  // mono
};
inline std::map<std::string, FakeMedia>& media() {
  static std::map<std::string, FakeMedia> m;
  return m;
}

class FakeReader final : public sq::Object<FakeReader, sq::IAudioFileReader> {
 public:
  sq_result SQ_CALL open(const char* path) override {
    auto it = media().find(path ? path : "");
    if (it == media().end()) return SQ_E_NOTFOUND;
    m_ = &it->second;
    return SQ_OK;
  }
  sq_result SQ_CALL openStream(sq::IByteStream*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override {
    if (!m_) return SQ_E_FAIL;
    out->sample_rate = m_->rate;
    out->channels = 1;
    out->sample_format = SQ_SAMPLE_F32;
    out->bits_per_sample = 32;
    out->frame_count = static_cast<sq_sample_time>(m_->samples.size());
    out->flags = SQ_AUDIOFILE_SEEKABLE | SQ_AUDIOFILE_SAMPLE_EXACT;
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* got) override {
    if (!m_ || !dst || dst->channel_count != 1) return SQ_E_INVALIDARG;
    uint32_t n = 0;
    while (n < dst->frames && pos_ < m_->samples.size()) dst->channels[0][n++] = m_->samples[pos_++];
    *got = n;
    return SQ_OK;
  }
  sq_result SQ_CALL seek(sq_sample_time f) override {
    if (f < 0) return SQ_E_INVALIDARG;
    pos_ = static_cast<size_t>(f);
    return SQ_OK;
  }
  sq_result SQ_CALL tell(sq_sample_time* f) override {
    *f = static_cast<sq_sample_time>(pos_);
    return SQ_OK;
  }
  uint32_t SQ_CALL getMetadataCount() override { return 0; }
  sq_result SQ_CALL getMetadataKey(uint32_t, char*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL getMetadata(const char*, void*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  uint32_t SQ_CALL getMarkerCount() override { return 0; }
  sq_result SQ_CALL getMarker(uint32_t, sq_audio_file_marker*) override { return SQ_E_NOTFOUND; }

 private:
  FakeMedia* m_ = nullptr;
  size_t pos_ = 0;
};

class FakeCodec final : public sq::Object<FakeCodec, sq::ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getFormatCount() override { return 0; }
  sq_result SQ_CALL getFormatInfo(uint32_t, sq_codec_format_info*) override { return SQ_E_NOTIMPL; }
  sq_bool SQ_CALL supports(const char* ext, const char*, uint32_t flags) override {
    return ext && std::string(ext) == "fake" && (flags & SQ_CODEC_READ) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void*, uint32_t) override { return 0; }
  sq_result SQ_CALL createReader(sq::IAudioFileReader** out) override {
    *out = new FakeReader();
    return SQ_OK;
  }
  sq_result SQ_CALL createWriter(const char*, sq::IAudioFileWriter** out) override {
    *out = nullptr;
    return SQ_E_UNSUPPORTED;
  }
};

class FakeCodecFactory final : public sq::Object<FakeCodecFactory, sq::IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid*, const sq_guid* riid, void** out) override {
    Ptr<FakeCodec> c = Ptr<FakeCodec>::adopt(new FakeCodec());
    return c->queryInterface(riid, out);
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

// A class id for the fake codec (generated for these tests).
inline constexpr sq_guid kFakeCodecClassId = sq::guid("{4F8871B5-B4C7-4F73-8DF0-96BB7B298D66}");

inline void registerFakeCodec(sq::app::ClassRegistry& reg) {
  Ptr<FakeCodecFactory> f = Ptr<FakeCodecFactory>::adopt(new FakeCodecFactory());
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  ci.class_id = kFakeCodecClassId;
  ci.category = sq::codec::kCategory;
  ci.name = "Fake Codec";
  REQUIRE_OK(reg.registerClass(&ci, f.get()));
}

// ---- fixture ---------------------------------------------------------------------------------------
struct Fx {
  Ptr<sq::edoc::DocumentManagerImpl> mgr;
  Ptr<sq::IDocument> doc;
  Ptr<sq::IEditor> ed;
  sq::edoc::DocumentImpl* impl = nullptr;

  Fx() {
    sq::edit::registerEditActions();
    mgr = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl());
    REQUIRE_OK(mgr->createDocument(SQ_DOC_SONG, doc.put()));
    REQUIRE_OK(doc->getEditor(ed.put()));
    impl = sq::edoc::DocumentImpl::from(doc.get());
    REQUIRE(impl != nullptr);
    sq::edit::installSideEffects(*impl);
  }
  const sq::edoc::Store& st() const { return impl->store(); }
  std::string hash() const { return impl->stateHash(); }
  const ClipRec& clip(const sq_guid& id) const {
    const ClipRec* c = st().clip(id);
    REQUIRE(c != nullptr);
    return *c;
  }
  bool exists(const sq_guid& id) const { return st().clip(id) != nullptr; }
};

inline sq_result exec(sq::IEditor* ed, const sq_guid& action, sq::IAttributes* p, sq::IAttributes* result = nullptr) {
  return ed->execute(&action, p, result);
}
inline sq_result exec(Fx& fx, const sq_guid& action, sq::IAttributes* p, sq::IAttributes* result = nullptr) {
  return exec(fx.ed.get(), action, p, result);
}

inline sq_guid addTrack(Fx& fx, int32_t kind, const char* name = nullptr) {
  auto p = sq::makeAttributes();
  p->setInt("kind", kind);
  if (name) p->setString("name", name);
  auto r = sq::makeAttributes();
  CHECK_OK(exec(fx, sq::doc::kAddTrack, p.get(), r.get()));
  sq_guid id{};
  r->getGuid("node", &id);
  return id;
}

inline sq_guid addClip(Fx& fx, const sq_guid& track, int32_t kind, double start, double length, const char* source = nullptr,
                       double offset = 0.0) {
  auto p = sq::makeAttributes();
  p->setGuid("track", &track);
  p->setInt("kind", kind);
  p->setDouble("start", start);
  p->setDouble("length", length);
  if (source) p->setString("source", source);
  if (offset != 0.0) p->setDouble("source_offset", offset);
  auto r = sq::makeAttributes();
  CHECK_OK(exec(fx, sq::doc::kAddClip, p.get(), r.get()));
  sq_guid id{};
  r->getGuid("node", &id);
  return id;
}

inline sq_result setProp(Fx& fx, const sq_guid& node, const char* key, double v) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setDouble("value", v);
  return exec(fx, sq::doc::kSetProperty, p.get());
}
inline sq_result setPropB(Fx& fx, const sq_guid& node, const char* key, bool v) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setBool("value", v ? SQ_TRUE : SQ_FALSE);
  return exec(fx, sq::doc::kSetProperty, p.get());
}
inline sq_result setPropS(Fx& fx, const sq_guid& node, const char* key, const char* v) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setString("value", v);
  return exec(fx, sq::doc::kSetProperty, p.get());
}

inline sq_result setEvents(Fx& fx, const sq_guid& clip, const std::vector<sq_clip_event>& evs) {
  auto p = sq::makeAttributes();
  p->setGuid("clip", &clip);
  p->setBlob("events", evs.data(), static_cast<uint32_t>(evs.size() * sizeof(sq_clip_event)));
  return exec(fx, sq::doc::kSetClipEvents, p.get());
}
inline sq_clip_event note(double pos, double len, uint8_t key, uint8_t vel = 100) {
  sq_clip_event e{};
  e.struct_size = sizeof e;
  e.position = pos;
  e.length = len;
  e.bytes[0] = 0x90;
  e.bytes[1] = key;
  e.bytes[2] = vel;
  e.size = 3;
  return e;
}

inline Ptr<sq::IAttributes> nodes(std::initializer_list<sq_guid> ids) {
  std::vector<sq_guid> v(ids);
  auto p = sq::makeAttributes();
  p->setBlob("nodes", v.data(), static_cast<uint32_t>(v.size() * sizeof(sq_guid)));
  return p;
}
inline std::vector<sq_guid> resultNodes(sq::IAttributes* r) {
  std::vector<sq_guid> out;
  uint32_t need = 0;
  r->getBlob("nodes", nullptr, 0, &need);
  out.resize(need / sizeof(sq_guid));
  if (need) r->getBlob("nodes", out.data(), need, &need);
  return out;
}

inline bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// Executes `action`, then checks undo restores the exact prior state hash and redo the exact
// post state hash. Returns the action's result.
inline sq_result execUndoRedo(Fx& fx, const sq_guid& action, sq::IAttributes* p, sq::IAttributes* result = nullptr) {
  const std::string before = fx.hash();
  const uint32_t undoCount = fx.ed->getUndoCount();
  const sq_result r = exec(fx, action, p, result);
  if (SQ_FAILED(r)) {
    CHECK_EQ(fx.hash(), before);  // a failing action leaves the document unchanged
    return r;
  }
  const std::string after = fx.hash();
  if (after == before) return r;  // no-op: no undo step
  CHECK_EQ(fx.ed->getUndoCount(), undoCount + 1);
  CHECK_OK(fx.ed->undo());
  CHECK_EQ(fx.hash(), before);
  CHECK_OK(fx.ed->redo());
  CHECK_EQ(fx.hash(), after);
  return r;
}

}  // namespace edittest
