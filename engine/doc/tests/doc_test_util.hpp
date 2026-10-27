// engine/doc tests - shared helpers.
#pragma once

#include <string>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/engine/doc/actions.hpp"
#include "sq/engine/doc/archive.hpp"
#include "sq/engine/doc/manager.hpp"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace doctest {

using sq::Ptr;

struct Fx {
  Ptr<sq::edoc::DocumentManagerImpl> mgr;
  Ptr<sq::IDocument> doc;
  Ptr<sq::IEditor> ed;
  sq::edoc::DocumentImpl* impl = nullptr;

  explicit Fx(int32_t kind = SQ_DOC_SONG) {
    mgr = Ptr<sq::edoc::DocumentManagerImpl>::adopt(new sq::edoc::DocumentManagerImpl());
    REQUIRE_OK(mgr->createDocument(kind, doc.put()));
    REQUIRE_OK(doc->getEditor(ed.put()));
    impl = sq::edoc::DocumentImpl::from(doc.get());
    REQUIRE(impl != nullptr);
  }
  const sq::edoc::Store& st() const { return impl->store(); }
  std::string hash() const { return impl->stateHash(); }
};

inline sq_result exec(sq::IEditor* ed, const sq_guid& action, sq::IAttributes* p, sq_guid* node = nullptr) {
  auto r = sq::makeAttributes();
  sq_result res = ed->execute(&action, p, r.get());
  if (node) {
    *node = sq_guid{};
    r->getGuid("node", node);
  }
  return res;
}

inline sq_guid addTrack(sq::IEditor* ed, int32_t kind, const char* name = nullptr, const sq_guid* parent = nullptr) {
  auto p = sq::makeAttributes();
  p->setInt("kind", kind);
  if (name) p->setString("name", name);
  if (parent) p->setGuid("parent", parent);
  sq_guid id{};
  CHECK_OK(exec(ed, sq::doc::kAddTrack, p.get(), &id));
  return id;
}

inline sq_guid addClip(sq::IEditor* ed, const sq_guid& track, int32_t kind, double start, double length,
                       const char* source = nullptr, double offset = 0.0) {
  auto p = sq::makeAttributes();
  p->setGuid("track", &track);
  p->setInt("kind", kind);
  p->setDouble("start", start);
  p->setDouble("length", length);
  if (source) p->setString("source", source);
  if (offset != 0.0) p->setDouble("source_offset", offset);
  sq_guid id{};
  CHECK_OK(exec(ed, sq::doc::kAddClip, p.get(), &id));
  return id;
}

inline sq_result setProp(sq::IEditor* ed, const sq_guid& node, const char* key, double v, bool merge = false) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setDouble("value", v);
  if (merge) p->setBool("merge", SQ_TRUE);
  return exec(ed, sq::doc::kSetProperty, p.get());
}
inline sq_result setPropB(sq::IEditor* ed, const sq_guid& node, const char* key, bool v) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setBool("value", v ? SQ_TRUE : SQ_FALSE);
  return exec(ed, sq::doc::kSetProperty, p.get());
}
inline sq_result setPropS(sq::IEditor* ed, const sq_guid& node, const char* key, const char* v) {
  auto p = sq::makeAttributes();
  p->setGuid("node", &node);
  p->setString("key", key);
  p->setString("value", v);
  return exec(ed, sq::doc::kSetProperty, p.get());
}

inline sq_result setEvents(sq::IEditor* ed, const sq_guid& clip, const std::vector<sq_clip_event>& ev) {
  auto p = sq::makeAttributes();
  p->setGuid("clip", &clip);
  p->setBlob("events", ev.data(), static_cast<uint32_t>(ev.size() * sizeof(sq_clip_event)));
  return exec(ed, sq::doc::kSetClipEvents, p.get());
}
inline sq_clip_event note(double pos, double len, uint8_t pitch, uint8_t vel = 100) {
  sq_clip_event e{};
  e.struct_size = sizeof e;
  e.position = pos;
  e.length = len;
  e.bytes[0] = 0x90;
  e.bytes[1] = pitch;
  e.bytes[2] = vel;
  e.size = 3;
  return e;
}

inline sq_result setTempo(sq::IEditor* ed, double pos, double bpm, bool ramp = false) {
  auto p = sq::makeAttributes();
  p->setDouble("position", pos);
  p->setDouble("bpm", bpm);
  if (ramp) p->setBool("ramp", SQ_TRUE);
  return exec(ed, sq::doc::kSetTempo, p.get());
}

inline sq_result setAutomation(sq::IEditor* ed, const sq_guid& track, const char* param, const std::vector<double>& pts) {
  auto p = sq::makeAttributes();
  p->setGuid("track", &track);
  p->setString("param", param);
  p->setBlob("points", pts.data(), static_cast<uint32_t>(pts.size() * sizeof(double)));
  return exec(ed, sq::edoc::kSetAutomation, p.get());
}

inline std::string tempDir(const char* tag) {
  std::string d = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), std::string("sqtest_engine_doc_") + tag + "_" +
                                                                    sq::edoc::guidStr(sq::edoc::newGuid()).substr(1, 8));
  sq_fs_create_dirs(d.c_str());
  return d;
}

}  // namespace doctest
