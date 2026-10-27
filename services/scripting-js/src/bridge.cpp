// scripting-js: native binding object handed to bootstrap.js (`N`), plus IAttributes <-> JS
// conversions. Every function here runs on the engine thread inside a CallScope (it is only
// reachable from script code) and converts C++ exceptions into JS exceptions.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#include "engine.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/codec.h"
#include "sq/platform.hpp"

namespace sqjs {

namespace {

constexpr int kMaxDepth = 8;

Engine* eng(JSContext* ctx) {
  ContextData* cd = contextData(ctx);
  return cd ? cd->engine : nullptr;
}

template <class F>
std::string getStr(F&& f) {
  char small[256];
  uint32_t need = 0;
  sq_result r = f(small, static_cast<uint32_t>(sizeof small), &need);
  if (r == SQ_OK) return small;
  if (r == SQ_E_BUFFER_TOO_SMALL && need > 0) {
    std::string s(need, '\0');
    if (f(s.data(), need, &need) == SQ_OK) {
      s.resize(std::strlen(s.c_str()));
      return s;
    }
  }
  return {};
}

std::string str(JSContext* ctx, JSValueConst v) { return toStdString(ctx, v); }

bool argGuid(JSContext* ctx, JSValueConst v, sq_guid* g) { return JS_IsString(v) && parseAnyGuid(str(ctx, v), g); }

double num(JSContext* ctx, JSValueConst v, double def = 0.0) {
  double d = def;
  if (JS_IsUndefined(v) || JS_IsNull(v)) return def;
  if (JS_ToFloat64(ctx, &d, v) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return def;
  }
  return d;
}

int64_t inum(JSContext* ctx, JSValueConst v, int64_t def = 0) {
  double d = num(ctx, v, static_cast<double>(def));
  return std::isfinite(d) ? static_cast<int64_t>(d) : def;
}

template <class I>
sq::Ptr<I> iface(JSContext* ctx, JSValueConst v) {
  sq::Ptr<I> r;
  if (sq::IUnknown* u = unboxHandle(ctx, v)) u->queryInterface(&I::iid, r.putVoid());
  return r;
}

JSValue code(JSContext* ctx, sq_result r) { return JS_NewInt32(ctx, r); }

JSValue boxRaw(JSContext* ctx, sq::IUnknown* p) {
  Engine* e = eng(ctx);
  return (e && p) ? e->box(ctx, p, false) : JS_NULL;
}

// Releases `p` after boxing (for add-ref'd out-params).
template <class T>
JSValue boxAdopt(JSContext* ctx, T* p) {
  if (!p) return JS_NULL;
  JSValue v = boxRaw(ctx, p);
  p->release();
  return v;
}

JSValue throwClosed(JSContext* ctx) { return JS_ThrowInternalError(ctx, "script context is closed"); }
JSValue throwType(JSContext* ctx, const char* what) { return JS_ThrowTypeError(ctx, "%s", what); }

#define NFN(name) JSValue name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
#define GUARD_BEGIN \
  (void)argc;       \
  (void)argv;       \
  try {
#define GUARD_END                                     \
  }                                                   \
  catch (const std::bad_alloc&) {                     \
    return JS_ThrowOutOfMemory(ctx);                  \
  }                                                   \
  catch (...) {                                       \
    return JS_ThrowInternalError(ctx, "native error"); \
  }
#define NEED_ENGINE()           \
  Engine* e = eng(ctx);         \
  if (!e) return throwClosed(ctx)

sq::IHostContext* hostOf(Engine* e) { return e ? e->hostContext() : nullptr; }

// ---- misc --------------------------------------------------------------------------------------
NFN(n_log) {
  GUARD_BEGIN
  NEED_ENGINE();
  int64_t lvl = inum(ctx, argv[0], 1);
  int32_t level = lvl <= 0 ? SQ_LOG_DEBUG : lvl == 1 ? SQ_LOG_INFO : lvl == 2 ? SQ_LOG_WARN : SQ_LOG_ERROR;
  ContextData* cd = contextData(ctx);
  std::string msg = str(ctx, argv[1]);
  if (cd && cd->package) msg = "[" + cd->package->id() + "] " + msg;
  sq::logf(level, "%s", msg.c_str());
  return JS_UNDEFINED;
  GUARD_END
}

NFN(n_resultString) {
  GUARD_BEGIN
  return JS_NewString(ctx, sq_result_string(static_cast<sq_result>(inum(ctx, argv[0]))));
  GUARD_END
}

NFN(n_nestedError) {
  GUARD_BEGIN
  NEED_ENGINE();
  return newString(ctx, e->takeNestedError());
  GUARD_END
}

NFN(n_isHandle) {
  GUARD_BEGIN
  NEED_ENGINE();
  return JS_NewBool(ctx, JS_IsObject(argv[0]) && JS_GetOpaque(argv[0], e->nativeClassId()) != nullptr);
  GUARD_END
}

NFN(n_qi) {
  GUARD_BEGIN
  sq::IUnknown* u = unboxHandle(ctx, argv[0]);
  sq_guid g{};
  if (!u || !argGuid(ctx, argv[1], &g)) return JS_FALSE;
  sq::Ptr<sq::IUnknown> tmp;
  return JS_NewBool(ctx, u->queryInterface(&g, tmp.putVoid()) == SQ_OK);
  GUARD_END
}

NFN(n_same) {
  GUARD_BEGIN
  sq::Ptr<sq::IUnknown> a = jsToUnknown(ctx, argv[0]), b = jsToUnknown(ctx, argv[1]);
  if (!a || !b) return JS_FALSE;
  sq::Ptr<sq::IUnknown> ia = a.as<sq::IUnknown>(), ib = b.as<sq::IUnknown>();
  return JS_NewBool(ctx, ia.get() == ib.get());
  GUARD_END
}

// ---- class registry ------------------------------------------------------------------------
JSValue classInfoToJs(JSContext* ctx, const sq_class_info& ci) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "classId", newString(ctx, guidToString(ci.class_id)));
  JS_SetPropertyStr(ctx, o, "category", JS_NewString(ctx, ci.category ? ci.category : ""));
  JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, ci.name ? ci.name : ""));
  JS_SetPropertyStr(ctx, o, "flags", JS_NewInt64(ctx, ci.flags));
  return o;
}

NFN(n_classCount) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::IHostContext* h = hostOf(e);
  return JS_NewInt64(ctx, h ? h->getClassRegistry()->getClassCount() : 0);
  GUARD_END
}

NFN(n_classInfo) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::IHostContext* h = hostOf(e);
  if (!h) return JS_NULL;
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  if (h->getClassRegistry()->getClassInfo(static_cast<uint32_t>(inum(ctx, argv[0])), &ci) != SQ_OK) return JS_NULL;
  return classInfoToJs(ctx, ci);
  GUARD_END
}

NFN(n_findClass) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::IHostContext* h = hostOf(e);
  sq_guid g{};
  if (!h || !argGuid(ctx, argv[0], &g)) return JS_NULL;
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  if (h->getClassRegistry()->findClass(&g, &ci) != SQ_OK) return JS_NULL;
  return classInfoToJs(ctx, ci);
  GUARD_END
}

NFN(n_createInstance) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::IHostContext* h = hostOf(e);
  sq_guid cls{}, iid = sq::IUnknown::iid;
  if (!h || !argGuid(ctx, argv[0], &cls)) return throwType(ctx, "createInstance: bad class id");
  if (JS_IsString(argv[1])) argGuid(ctx, argv[1], &iid);
  void* p = nullptr;
  sq_result r = h->getClassRegistry()->createInstance(&cls, &iid, &p);
  if (r != SQ_OK || !p) return JS_NULL;
  return boxAdopt(ctx, static_cast<sq::IUnknown*>(p));
  GUARD_END
}

NFN(n_queryService) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::IHostContext* h = hostOf(e);
  sq_guid iid{};
  if (!h || !argGuid(ctx, argv[0], &iid)) return JS_NULL;
  void* p = nullptr;
  if (h->queryService(&iid, &p) != SQ_OK || !p) return JS_NULL;
  return boxAdopt(ctx, static_cast<sq::IUnknown*>(p));
  GUARD_END
}

// ---- object registry ---------------------------------------------------------------------------
NFN(n_resolve) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq_guid iid = sq::IUnknown::iid;
  if (JS_IsString(argv[1])) argGuid(ctx, argv[1], &iid);
  if (!e->objects()) return JS_NULL;
  void* p = nullptr;
  if (e->objects()->resolve(str(ctx, argv[0]).c_str(), &iid, &p) != SQ_OK || !p) return JS_NULL;
  return boxAdopt(ctx, static_cast<sq::IUnknown*>(p));
  GUARD_END
}

NFN(n_readUrl) {
  GUARD_BEGIN
  NEED_ENGINE();
  if (!e->objects()) return JS_NULL;
  sq::Ptr<sq::IByteStream> s;
  if (e->objects()->openStream(str(ctx, argv[0]).c_str(), s.put()) != SQ_OK || !s) return JS_NULL;
  std::string data;
  char buf[4096];
  for (;;) {
    uint32_t got = 0;
    if (s->read(buf, sizeof buf, &got) != SQ_OK) return JS_NULL;
    data.append(buf, got);
    if (got < sizeof buf) break;
    if (data.size() > (64u << 20)) return JS_ThrowRangeError(ctx, "resource too large");
  }
  return newString(ctx, data);
  GUARD_END
}

NFN(n_registerObject) {
  GUARD_BEGIN
  NEED_ENGINE();
  if (!e->objects()) return code(ctx, SQ_E_UNSUPPORTED);
  sq::Ptr<sq::IUnknown> o = jsToUnknown(ctx, argv[1]);
  if (!o) return code(ctx, SQ_E_INVALIDARG);
  return code(ctx, e->objects()->registerObject(str(ctx, argv[0]).c_str(), o.get()));
  GUARD_END
}

NFN(n_unregisterObject) {
  GUARD_BEGIN
  NEED_ENGINE();
  if (!e->objects()) return code(ctx, SQ_E_UNSUPPORTED);
  return code(ctx, e->objects()->unregisterObject(str(ctx, argv[0]).c_str()));
  GUARD_END
}

// ---- settings ----------------------------------------------------------------------------------
sq::ISettings* settingsOf(Engine* e) {
  sq::IHostContext* h = hostOf(e);
  return h ? h->getSettings() : nullptr;
}

NFN(n_settingsGet) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::ISettings* s = settingsOf(e);
  if (!s) return JS_UNDEFINED;
  std::string k = str(ctx, argv[0]);
  switch (s->getType(k.c_str())) {
    case SQ_SETTING_INT: return JS_NewInt64(ctx, s->getInt(k.c_str(), 0));
    case SQ_SETTING_DOUBLE: return JS_NewFloat64(ctx, s->getDouble(k.c_str(), 0.0));
    case SQ_SETTING_BOOL: return JS_NewBool(ctx, s->getBool(k.c_str(), SQ_FALSE) != 0);
    case SQ_SETTING_STRING: return newString(ctx, sq::getString(s, k.c_str()));
    default: return JS_UNDEFINED;
  }
  GUARD_END
}

NFN(n_settingsSet) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::ISettings* s = settingsOf(e);
  if (!s) return code(ctx, SQ_E_UNSUPPORTED);
  std::string k = str(ctx, argv[0]);
  JSValueConst v = argv[1];
  if (JS_IsBool(v)) return code(ctx, s->setBool(k.c_str(), JS_ToBool(ctx, v) ? SQ_TRUE : SQ_FALSE));
  if (JS_IsNumber(v)) {
    double d = num(ctx, v);
    if (std::isfinite(d) && d == std::trunc(d) && std::fabs(d) < 9.0e15) return code(ctx, s->setInt(k.c_str(), static_cast<int64_t>(d)));
    return code(ctx, s->setDouble(k.c_str(), d));
  }
  if (JS_IsUndefined(v) || JS_IsNull(v)) return code(ctx, s->remove(k.c_str()));
  return code(ctx, s->setString(k.c_str(), str(ctx, v).c_str()));
  GUARD_END
}

NFN(n_settingsHas) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::ISettings* s = settingsOf(e);
  return JS_NewBool(ctx, s && s->has(str(ctx, argv[0]).c_str()));
  GUARD_END
}

NFN(n_settingsRemove) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::ISettings* s = settingsOf(e);
  return code(ctx, s ? s->remove(str(ctx, argv[0]).c_str()) : SQ_E_UNSUPPORTED);
  GUARD_END
}

NFN(n_settingsKeys) {
  GUARD_BEGIN
  NEED_ENGINE();
  JSValue arr = JS_NewArray(ctx);
  sq::ISettings* s = settingsOf(e);
  if (!s) return arr;
  std::vector<std::string> keys = sq::listKeys(s, str(ctx, argv[0]).c_str());
  uint32_t i = 0;
  for (auto& k : keys) JS_SetPropertyUint32(ctx, arr, i++, newString(ctx, k));
  return arr;
  GUARD_END
}

NFN(n_settingsObserve) {
  GUARD_BEGIN
  NEED_ENGINE();
  if (!JS_IsFunction(ctx, argv[1])) return throwType(ctx, "observe: callback must be a function");
  return JS_NewInt32(ctx, e->addSettingsObserver(ctx, str(ctx, argv[0]), argv[1]));
  GUARD_END
}

NFN(n_settingsUnobserve) {
  GUARD_BEGIN
  NEED_ENGINE();
  e->removeSettingsObserver(static_cast<int32_t>(inum(ctx, argv[0], -1)));
  return JS_UNDEFINED;
  GUARD_END
}

// ---- document read API ---------------------------------------------------------------------------
NFN(n_nodeInfo) {
  GUARD_BEGIN
  auto n = iface<sq::INode>(ctx, argv[0]);
  if (!n) return throwType(ctx, "not a document node");
  JSValue o = JS_NewObject(ctx);
  sq_guid id{};
  n->getId(&id);
  JS_SetPropertyStr(ctx, o, "kind", JS_NewInt32(ctx, n->getKind()));
  JS_SetPropertyStr(ctx, o, "id", newString(ctx, guidToString(id)));
  JS_SetPropertyStr(ctx, o, "name", newString(ctx, getStr([&](char* b, uint32_t s, uint32_t* nd) { return n->getName(b, s, nd); })));
  JS_SetPropertyStr(ctx, o, "url", newString(ctx, getStr([&](char* b, uint32_t s, uint32_t* nd) { return n->getUrl(b, s, nd); })));
  JS_SetPropertyStr(ctx, o, "childCount", JS_NewInt64(ctx, n->getChildCount()));
  return o;
  GUARD_END
}

NFN(n_nodeProps) {
  GUARD_BEGIN
  auto n = iface<sq::INode>(ctx, argv[0]);
  if (!n) return throwType(ctx, "not a document node");
  auto a = sq::makeAttributes();
  n->getProperties(a.get());
  return attrsToJs(ctx, a.get());
  GUARD_END
}

NFN(n_nodeChild) {
  GUARD_BEGIN
  auto n = iface<sq::INode>(ctx, argv[0]);
  if (!n) return throwType(ctx, "not a document node");
  sq::INode* c = nullptr;
  if (n->getChild(static_cast<uint32_t>(inum(ctx, argv[1])), &c) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, c);
  GUARD_END
}

NFN(n_nodeParent) {
  GUARD_BEGIN
  auto n = iface<sq::INode>(ctx, argv[0]);
  if (!n) return throwType(ctx, "not a document node");
  sq::INode* p = nullptr;
  if (n->getParent(&p) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, p);
  GUARD_END
}

NFN(n_docInfo) {
  GUARD_BEGIN
  auto d = iface<sq::IDocument>(ctx, argv[0]);
  if (!d) return throwType(ctx, "not a document");
  sq_document_info di{};
  di.struct_size = sizeof di;
  d->getDocumentInfo(&di);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "kind", JS_NewInt32(ctx, di.kind));
  JS_SetPropertyStr(ctx, o, "id", newString(ctx, guidToString(di.id)));
  JS_SetPropertyStr(ctx, o, "sampleRate", JS_NewFloat64(ctx, di.sample_rate));
  JS_SetPropertyStr(ctx, o, "modified", JS_NewBool(ctx, di.modified != 0));
  JS_SetPropertyStr(ctx, o, "trackCount", JS_NewInt64(ctx, di.track_count));
  JS_SetPropertyStr(ctx, o, "length", JS_NewFloat64(ctx, di.length));
  return o;
  GUARD_END
}

NFN(n_docPath) {
  GUARD_BEGIN
  auto d = iface<sq::IDocument>(ctx, argv[0]);
  if (!d) return throwType(ctx, "not a document");
  return newString(ctx, getStr([&](char* b, uint32_t s, uint32_t* nd) { return d->getPath(b, s, nd); }));
  GUARD_END
}

NFN(n_docTrack) {
  GUARD_BEGIN
  auto d = iface<sq::IDocument>(ctx, argv[0]);
  if (!d) return throwType(ctx, "not a document");
  sq::ITrack* t = nullptr;
  if (d->getTrack(static_cast<uint32_t>(inum(ctx, argv[1])), &t) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, t);
  GUARD_END
}

NFN(n_docTempoMap) {
  GUARD_BEGIN
  auto d = iface<sq::IDocument>(ctx, argv[0]);
  if (!d) return throwType(ctx, "not a document");
  sq::ITempoMap* t = nullptr;
  if (d->getTempoMap(&t) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, t);
  GUARD_END
}

NFN(n_docMarkers) {
  GUARD_BEGIN
  auto d = iface<sq::IDocument>(ctx, argv[0]);
  if (!d) return throwType(ctx, "not a document");
  JSValue arr = JS_NewArray(ctx);
  const uint32_t n = d->getMarkerCount();
  for (uint32_t i = 0; i < n; ++i) {
    sq_doc_marker m{};
    m.struct_size = sizeof m;
    if (d->getMarker(i, &m) != SQ_OK) continue;
    m.name[sizeof m.name - 1] = '\0';
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id", newString(ctx, guidToString(m.id)));
    JS_SetPropertyStr(ctx, o, "position", JS_NewFloat64(ctx, m.position));
    JS_SetPropertyStr(ctx, o, "length", JS_NewFloat64(ctx, m.length));
    JS_SetPropertyStr(ctx, o, "flags", JS_NewInt64(ctx, m.flags));
    JS_SetPropertyStr(ctx, o, "color", JS_NewInt64(ctx, m.color));
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, m.name));
    JS_SetPropertyUint32(ctx, arr, i, o);
  }
  return arr;
  GUARD_END
}

NFN(n_docFind) {
  GUARD_BEGIN
  auto d = iface<sq::IDocument>(ctx, argv[0]);
  if (!d) return throwType(ctx, "not a document");
  sq_guid g{};
  if (!argGuid(ctx, argv[1], &g)) return JS_NULL;
  sq::INode* n = nullptr;
  if (d->findNode(&g, &n) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, n);
  GUARD_END
}

NFN(n_docEditor) {
  GUARD_BEGIN
  auto d = iface<sq::IDocument>(ctx, argv[0]);
  if (!d) return throwType(ctx, "not a document");
  sq::IEditor* ed = nullptr;
  if (d->getEditor(&ed) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, ed);
  GUARD_END
}

NFN(n_mgrActive) {
  GUARD_BEGIN
  auto m = iface<sq::IDocumentManager>(ctx, argv[0]);
  if (!m) return throwType(ctx, "not a document manager");
  sq::IDocument* d = nullptr;
  if (m->getActiveDocument(&d) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, d);
  GUARD_END
}

NFN(n_mgrCount) {
  GUARD_BEGIN
  auto m = iface<sq::IDocumentManager>(ctx, argv[0]);
  return JS_NewInt64(ctx, m ? m->getDocumentCount() : 0);
  GUARD_END
}

NFN(n_mgrGet) {
  GUARD_BEGIN
  auto m = iface<sq::IDocumentManager>(ctx, argv[0]);
  if (!m) return throwType(ctx, "not a document manager");
  sq::IDocument* d = nullptr;
  if (m->getDocument(static_cast<uint32_t>(inum(ctx, argv[1])), &d) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, d);
  GUARD_END
}

NFN(n_mgrFindUrl) {
  GUARD_BEGIN
  auto m = iface<sq::IDocumentManager>(ctx, argv[0]);
  if (!m) return throwType(ctx, "not a document manager");
  sq::INode* n = nullptr;
  if (m->findNodeByUrl(str(ctx, argv[1]).c_str(), &n) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, n);
  GUARD_END
}

NFN(n_trackInfo) {
  GUARD_BEGIN
  auto t = iface<sq::ITrack>(ctx, argv[0]);
  if (!t) return throwType(ctx, "not a track");
  sq_track_info ti{};
  ti.struct_size = sizeof ti;
  t->getTrackInfo(&ti);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "kind", JS_NewInt32(ctx, ti.kind));
  JS_SetPropertyStr(ctx, o, "channels", JS_NewInt64(ctx, ti.channels));
  JS_SetPropertyStr(ctx, o, "flags", JS_NewInt64(ctx, ti.flags));
  JS_SetPropertyStr(ctx, o, "color", JS_NewInt64(ctx, ti.color));
  JS_SetPropertyStr(ctx, o, "volumeDb", JS_NewFloat64(ctx, ti.volume_db));
  JS_SetPropertyStr(ctx, o, "pan", JS_NewFloat64(ctx, ti.pan));
  JS_SetPropertyStr(ctx, o, "output", newString(ctx, sq_guid_is_null(&ti.output) ? std::string() : guidToString(ti.output)));
  JS_SetPropertyStr(ctx, o, "clipCount", JS_NewInt64(ctx, ti.clip_count));
  JS_SetPropertyStr(ctx, o, "muted", JS_NewBool(ctx, (ti.flags & SQ_TRACK_MUTE) != 0));
  JS_SetPropertyStr(ctx, o, "soloed", JS_NewBool(ctx, (ti.flags & SQ_TRACK_SOLO) != 0));
  JS_SetPropertyStr(ctx, o, "armed", JS_NewBool(ctx, (ti.flags & SQ_TRACK_ARMED) != 0));
  return o;
  GUARD_END
}

NFN(n_trackClip) {
  GUARD_BEGIN
  auto t = iface<sq::ITrack>(ctx, argv[0]);
  if (!t) return throwType(ctx, "not a track");
  sq::IClip* c = nullptr;
  if (t->getClip(static_cast<uint32_t>(inum(ctx, argv[1])), &c) != SQ_OK) return JS_NULL;
  return boxAdopt(ctx, c);
  GUARD_END
}

NFN(n_clipInfo) {
  GUARD_BEGIN
  auto c = iface<sq::IClip>(ctx, argv[0]);
  if (!c) return throwType(ctx, "not a clip");
  sq_clip_info ci{};
  ci.struct_size = sizeof ci;
  c->getClipInfo(&ci);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "kind", JS_NewInt32(ctx, ci.kind));
  JS_SetPropertyStr(ctx, o, "start", JS_NewFloat64(ctx, ci.start));
  JS_SetPropertyStr(ctx, o, "length", JS_NewFloat64(ctx, ci.length));
  JS_SetPropertyStr(ctx, o, "sourceOffset", JS_NewFloat64(ctx, ci.source_offset_seconds));
  JS_SetPropertyStr(ctx, o, "gainDb", JS_NewFloat64(ctx, ci.gain_db));
  JS_SetPropertyStr(ctx, o, "fadeIn", JS_NewFloat64(ctx, ci.fade_in_seconds));
  JS_SetPropertyStr(ctx, o, "fadeOut", JS_NewFloat64(ctx, ci.fade_out_seconds));
  JS_SetPropertyStr(ctx, o, "flags", JS_NewInt64(ctx, ci.flags));
  JS_SetPropertyStr(ctx, o, "color", JS_NewInt64(ctx, ci.color));
  JS_SetPropertyStr(ctx, o, "eventCount", JS_NewInt64(ctx, ci.event_count));
  JS_SetPropertyStr(ctx, o, "muted", JS_NewBool(ctx, (ci.flags & SQ_CLIP_MUTED) != 0));
  return o;
  GUARD_END
}

NFN(n_clipSource) {
  GUARD_BEGIN
  auto c = iface<sq::IClip>(ctx, argv[0]);
  if (!c) return throwType(ctx, "not a clip");
  return newString(ctx, getStr([&](char* b, uint32_t s, uint32_t* nd) { return c->getSourcePath(b, s, nd); }));
  GUARD_END
}

NFN(n_clipEvents) {
  GUARD_BEGIN
  auto c = iface<sq::IClip>(ctx, argv[0]);
  if (!c) return throwType(ctx, "not a clip");
  sq_clip_info ci{};
  ci.struct_size = sizeof ci;
  c->getClipInfo(&ci);
  JSValue arr = JS_NewArray(ctx);
  uint32_t k = 0;
  for (uint32_t i = 0; i < ci.event_count; ++i) {
    sq_clip_event ev{};
    ev.struct_size = sizeof ev;
    if (c->getEvent(i, &ev) != SQ_OK) continue;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "position", JS_NewFloat64(ctx, ev.position));
    JS_SetPropertyStr(ctx, o, "length", JS_NewFloat64(ctx, ev.length));
    JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, ev.bytes[0]));
    JS_SetPropertyStr(ctx, o, "data1", JS_NewInt32(ctx, ev.bytes[1]));
    JS_SetPropertyStr(ctx, o, "data2", JS_NewInt32(ctx, ev.bytes[2]));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt64(ctx, ev.size));
    JS_SetPropertyUint32(ctx, arr, k++, o);
  }
  return arr;
  GUARD_END
}

NFN(n_tempoPoints) {
  GUARD_BEGIN
  auto t = iface<sq::ITempoMap>(ctx, argv[0]);
  if (!t) return throwType(ctx, "not a tempo map");
  JSValue arr = JS_NewArray(ctx);
  const uint32_t n = t->getTempoCount();
  for (uint32_t i = 0; i < n; ++i) {
    sq_tempo_point p{};
    p.struct_size = sizeof p;
    if (t->getTempo(i, &p) != SQ_OK) continue;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "position", JS_NewFloat64(ctx, p.position));
    JS_SetPropertyStr(ctx, o, "bpm", JS_NewFloat64(ctx, p.bpm));
    JS_SetPropertyStr(ctx, o, "ramp", JS_NewBool(ctx, (p.flags & SQ_TEMPO_RAMP) != 0));
    JS_SetPropertyUint32(ctx, arr, i, o);
  }
  return arr;
  GUARD_END
}

NFN(n_tempoSignatures) {
  GUARD_BEGIN
  auto t = iface<sq::ITempoMap>(ctx, argv[0]);
  if (!t) return throwType(ctx, "not a tempo map");
  JSValue arr = JS_NewArray(ctx);
  const uint32_t n = t->getSignatureCount();
  for (uint32_t i = 0; i < n; ++i) {
    sq_time_signature s{};
    s.struct_size = sizeof s;
    if (t->getSignature(i, &s) != SQ_OK) continue;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "position", JS_NewFloat64(ctx, s.position));
    JS_SetPropertyStr(ctx, o, "numerator", JS_NewInt64(ctx, s.numerator));
    JS_SetPropertyStr(ctx, o, "denominator", JS_NewInt64(ctx, s.denominator));
    JS_SetPropertyUint32(ctx, arr, i, o);
  }
  return arr;
  GUARD_END
}

NFN(n_ppqToSeconds) {
  GUARD_BEGIN
  auto t = iface<sq::ITempoMap>(ctx, argv[0]);
  if (!t) return throwType(ctx, "not a tempo map");
  return JS_NewFloat64(ctx, t->ppqToSeconds(num(ctx, argv[1])));
  GUARD_END
}

NFN(n_secondsToPpq) {
  GUARD_BEGIN
  auto t = iface<sq::ITempoMap>(ctx, argv[0]);
  if (!t) return throwType(ctx, "not a tempo map");
  return JS_NewFloat64(ctx, t->secondsToPpq(num(ctx, argv[1])));
  GUARD_END
}

// ---- editor ------------------------------------------------------------------------------------
#define NEED_EDITOR()                                    \
  auto ed = iface<sq::IEditor>(ctx, argv[0]);            \
  if (!ed) return throwType(ctx, "not an editor")

NFN(n_edBegin) {
  GUARD_BEGIN
  NEED_ENGINE();
  NEED_EDITOR();
  sq_result r = ed->beginTransaction(str(ctx, argv[1]).c_str());
  if (r == SQ_OK) e->txBegin(ed.get());
  return code(ctx, r);
  GUARD_END
}

NFN(n_edCommit) {
  GUARD_BEGIN
  NEED_ENGINE();
  NEED_EDITOR();
  sq_result r = ed->commitTransaction();
  if (SQ_SUCCEEDED(r)) e->txEnd(ed.get());
  return code(ctx, r);
  GUARD_END
}

NFN(n_edCancel) {
  GUARD_BEGIN
  NEED_ENGINE();
  NEED_EDITOR();
  sq_result r = ed->cancelTransaction();
  if (SQ_SUCCEEDED(r)) e->txEnd(ed.get());
  return code(ctx, r);
  GUARD_END
}

NFN(n_edInTx) {
  GUARD_BEGIN
  NEED_EDITOR();
  return JS_NewBool(ctx, ed->inTransaction() != 0);
  GUARD_END
}

NFN(n_edExecute) {
  GUARD_BEGIN
  NEED_EDITOR();
  sq_guid action{};
  if (!argGuid(ctx, argv[1], &action)) return throwType(ctx, "execute: bad action class id");
  auto params = sq::makeAttributes();
  if (JS_IsObject(argv[2])) jsToAttrs(ctx, argv[2], params.get());
  auto result = sq::makeAttributes();
  sq_result r = ed->execute(&action, params.get(), result.get());
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "code", code(ctx, r));
  JS_SetPropertyStr(ctx, o, "result", attrsToJs(ctx, result.get()));
  return o;
  GUARD_END
}

NFN(n_edUndo) {
  GUARD_BEGIN
  NEED_EDITOR();
  return code(ctx, ed->undo());
  GUARD_END
}
NFN(n_edRedo) {
  GUARD_BEGIN
  NEED_EDITOR();
  return code(ctx, ed->redo());
  GUARD_END
}
NFN(n_edCanUndo) {
  GUARD_BEGIN
  NEED_EDITOR();
  return JS_NewBool(ctx, ed->canUndo() != 0);
  GUARD_END
}
NFN(n_edCanRedo) {
  GUARD_BEGIN
  NEED_EDITOR();
  return JS_NewBool(ctx, ed->canRedo() != 0);
  GUARD_END
}
NFN(n_edUndoCount) {
  GUARD_BEGIN
  NEED_EDITOR();
  return JS_NewInt64(ctx, ed->getUndoCount());
  GUARD_END
}
NFN(n_edRedoCount) {
  GUARD_BEGIN
  NEED_EDITOR();
  return JS_NewInt64(ctx, ed->getRedoCount());
  GUARD_END
}
NFN(n_edUndoDesc) {
  GUARD_BEGIN
  NEED_EDITOR();
  uint32_t i = static_cast<uint32_t>(inum(ctx, argv[1]));
  return newString(ctx, getStr([&](char* b, uint32_t s, uint32_t* nd) { return ed->getUndoDescription(i, b, s, nd); }));
  GUARD_END
}
NFN(n_edRedoDesc) {
  GUARD_BEGIN
  NEED_EDITOR();
  uint32_t i = static_cast<uint32_t>(inum(ctx, argv[1]));
  return newString(ctx, getStr([&](char* b, uint32_t s, uint32_t* nd) { return ed->getRedoDescription(i, b, s, nd); }));
  GUARD_END
}
NFN(n_edClearHistory) {
  GUARD_BEGIN
  NEED_EDITOR();
  return code(ctx, ed->clearHistory());
  GUARD_END
}

// ---- generic interface calls ---------------------------------------------------------------------
NFN(n_cmdInterpret) {
  GUARD_BEGIN
  auto h = iface<sq::ICommandHandler>(ctx, argv[0]);
  if (!h) return code(ctx, SQ_E_NOINTERFACE);
  std::string cat = str(ctx, argv[1]), name = str(ctx, argv[2]);
  sq_command c{};
  c.struct_size = sizeof c;
  c.category = cat.c_str();
  c.name = name.c_str();
  sq::Ptr<sq::IAttributes> args;
  if (JS_IsObject(argv[3])) {
    args = sq::makeAttributes();
    jsToAttrs(ctx, argv[3], args.get());
  }
  return code(ctx, h->interpretCommand(&c, args.get(), JS_ToBool(ctx, argv[4]) ? SQ_TRUE : SQ_FALSE));
  GUARD_END
}

NFN(n_notify) {
  GUARD_BEGIN
  auto o = iface<sq::IObserver>(ctx, argv[0]);
  if (!o) return code(ctx, SQ_E_NOINTERFACE);
  sq::Ptr<sq::IUnknown> subject = jsToUnknown(ctx, argv[1]);
  sq::Ptr<sq::IAttributes> args;
  if (JS_IsObject(argv[3])) {
    args = sq::makeAttributes();
    jsToAttrs(ctx, argv[3], args.get());
  }
  o->onNotify(subject.get(), str(ctx, argv[2]).c_str(), args.get());
  return code(ctx, SQ_OK);
  GUARD_END
}

NFN(n_subjAdd) {
  GUARD_BEGIN
  auto s = iface<sq::ISubject>(ctx, argv[0]);
  if (!s) return code(ctx, SQ_E_NOINTERFACE);
  sq::Ptr<sq::IUnknown> u = jsToUnknown(ctx, argv[1]);
  sq::Ptr<sq::IObserver> obs = u ? u.as<sq::IObserver>() : sq::Ptr<sq::IObserver>();
  if (!obs) return code(ctx, SQ_E_NOINTERFACE);
  return code(ctx, s->addObserver(obs.get()));
  GUARD_END
}

NFN(n_subjRemove) {
  GUARD_BEGIN
  auto s = iface<sq::ISubject>(ctx, argv[0]);
  if (!s) return code(ctx, SQ_E_NOINTERFACE);
  sq::Ptr<sq::IUnknown> u = jsToUnknown(ctx, argv[1]);
  sq::Ptr<sq::IObserver> obs = u ? u.as<sq::IObserver>() : sq::Ptr<sq::IObserver>();
  if (!obs) return code(ctx, SQ_E_NOINTERFACE);
  return code(ctx, s->removeObserver(obs.get()));
  GUARD_END
}

NFN(n_ctlGet) {
  GUARD_BEGIN
  auto c = iface<sq::IController>(ctx, argv[0]);
  if (!c) return JS_UNDEFINED;
  auto a = sq::makeAttributes();
  if (c->getProperty(str(ctx, argv[1]).c_str(), a.get()) != SQ_OK) return JS_UNDEFINED;
  return attrValueToJs(ctx, a.get(), "value");
  GUARD_END
}

NFN(n_ctlSet) {
  GUARD_BEGIN
  auto c = iface<sq::IController>(ctx, argv[0]);
  if (!c) return code(ctx, SQ_E_NOINTERFACE);
  auto a = sq::makeAttributes();
  jsToAttrValue(ctx, a.get(), "value", argv[2]);
  return code(ctx, c->setProperty(str(ctx, argv[1]).c_str(), a.get()));
  GUARD_END
}

NFN(n_ctlChild) {
  GUARD_BEGIN
  auto c = iface<sq::IController>(ctx, argv[0]);
  if (!c) return JS_NULL;
  void* p = nullptr;
  if (c->getChild(str(ctx, argv[1]).c_str(), &sq::IUnknown::iid, &p) != SQ_OK || !p) return JS_NULL;
  return boxAdopt(ctx, static_cast<sq::IUnknown*>(p));
  GUARD_END
}

NFN(n_compInit) {
  GUARD_BEGIN
  auto c = iface<sq::IComponent>(ctx, argv[0]);
  if (!c) return code(ctx, SQ_E_NOINTERFACE);
  sq::Ptr<sq::IUnknown> context = jsToUnknown(ctx, argv[1]);
  return code(ctx, c->initialize(context.get()));
  GUARD_END
}

NFN(n_compTerm) {
  GUARD_BEGIN
  auto c = iface<sq::IComponent>(ctx, argv[0]);
  if (!c) return code(ctx, SQ_E_NOINTERFACE);
  return code(ctx, c->terminate());
  GUARD_END
}

NFN(n_actPerform) {
  GUARD_BEGIN
  auto a = iface<sq::IAction>(ctx, argv[0]);
  JSValue o = JS_NewObject(ctx);
  if (!a) {
    JS_SetPropertyStr(ctx, o, "code", code(ctx, SQ_E_NOINTERFACE));
    return o;
  }
  auto doc = iface<sq::IDocument>(ctx, argv[1]);
  auto ed = iface<sq::IEditor>(ctx, argv[2]);
  auto params = sq::makeAttributes();
  if (JS_IsObject(argv[3])) jsToAttrs(ctx, argv[3], params.get());
  auto result = sq::makeAttributes();
  sq_result r = a->perform(doc.get(), ed.get(), params.get(), result.get());
  JS_SetPropertyStr(ctx, o, "code", code(ctx, r));
  JS_SetPropertyStr(ctx, o, "result", attrsToJs(ctx, result.get()));
  return o;
  GUARD_END
}

// ---- audio / midi helpers ------------------------------------------------------------------------
NFN(n_audioPeak) {
  GUARD_BEGIN
  NEED_ENGINE();
  sq::IHostContext* h = hostOf(e);
  if (!h) return JS_NULL;
  const std::string path = str(ctx, argv[0]);
  // Non-finite / absurd ranges are normalised before any frame arithmetic (no float->int overflow).
  constexpr double kMaxSeconds = 1.0e7;  // ~115 days
  double startSec = num(ctx, argv[1]);
  if (!(startSec > 0)) startSec = 0;  // also NaN
  double durSec = num(ctx, argv[2], -1.0);
  if (!std::isfinite(durSec) || durSec > kMaxSeconds) durSec = -1.0;  // "to the end"
  if (startSec > kMaxSeconds) return JS_NULL;
  std::string ext;
  size_t dot = path.rfind('.');
  if (dot != std::string::npos) ext = path.substr(dot + 1);
  for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  sq::Ptr<sq::ICodecService> codec = sq::Ptr<sq::ICodecService>::adopt(
      sq::codec::findCodec(h->getClassRegistry(), ext.empty() ? nullptr : ext.c_str(), nullptr, SQ_CODEC_READ));
  if (!codec) return JS_NULL;
  sq::Ptr<sq::IAudioFileReader> reader;
  if (codec->createReader(reader.put()) != SQ_OK || !reader) return JS_NULL;
  if (reader->open(path.c_str()) != SQ_OK) return JS_NULL;
  sq_audio_file_format fmt{};
  fmt.struct_size = sizeof fmt;
  if (reader->getFormat(&fmt) != SQ_OK || fmt.channels == 0 || fmt.channels > 64 || !(fmt.sample_rate > 0)) return JS_NULL;
  if (!(fmt.sample_rate < 1.0e7)) return JS_NULL;
  const sq_sample_time startFrame = static_cast<sq_sample_time>(startSec * fmt.sample_rate);
  if (startFrame > 0 && reader->seek(startFrame) != SQ_OK) return JS_NULL;
  int64_t remaining = durSec >= 0 ? static_cast<int64_t>(durSec * fmt.sample_rate + 0.5) : INT64_MAX;
  constexpr uint32_t kBlock = 4096;
  std::vector<float> storage(static_cast<size_t>(kBlock) * fmt.channels);
  std::vector<float*> chans(fmt.channels);
  for (uint32_t c = 0; c < fmt.channels; ++c) chans[c] = storage.data() + static_cast<size_t>(c) * kBlock;
  double peak = 0.0;
  int64_t frames = 0;
  while (remaining > 0) {
    const uint32_t want = static_cast<uint32_t>(std::min<int64_t>(remaining, kBlock));
    sq_audio_block blk{chans.data(), fmt.channels, want, 0};
    uint32_t got = 0;
    if (reader->read(&blk, &got) != SQ_OK) break;
    for (uint32_t c = 0; c < fmt.channels; ++c)
      for (uint32_t i = 0; i < got; ++i) peak = std::max(peak, static_cast<double>(std::fabs(chans[c][i])));
    frames += got;
    remaining -= got;
    if (got < want) break;
    // Native loop: honour the script time limit too (the interrupt handler keeps firing after).
    if (e->checkInterrupt()) return JS_ThrowInternalError(ctx, "script time limit exceeded (reading %s)", path.c_str());
  }
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "peak", JS_NewFloat64(ctx, peak));
  JS_SetPropertyStr(ctx, o, "peakDb", JS_NewFloat64(ctx, peak > 0 ? 20.0 * std::log10(peak) : -INFINITY));
  JS_SetPropertyStr(ctx, o, "frames", JS_NewInt64(ctx, frames));
  JS_SetPropertyStr(ctx, o, "sampleRate", JS_NewFloat64(ctx, fmt.sample_rate));
  JS_SetPropertyStr(ctx, o, "channels", JS_NewInt64(ctx, fmt.channels));
  return o;
  GUARD_END
}

double propNum(JSContext* ctx, JSValueConst o, const char* name, double def) {
  JSValue v = JS_GetPropertyStr(ctx, o, name);
  double d = JS_IsException(v) ? (JS_FreeValue(ctx, JS_GetException(ctx)), def) : num(ctx, v, def);
  JS_FreeValue(ctx, v);
  return d;
}

// NaN-safe double -> [0, hi] byte (std::clamp passes NaN through, and NaN -> int is UB).
uint8_t clampByte(double d, uint8_t hi) {
  if (!(d > 0)) return 0;
  return d >= hi ? hi : static_cast<uint8_t>(d);
}

NFN(n_packEvents) {
  GUARD_BEGIN
  if (!JS_IsArray(argv[0])) return throwType(ctx, "packEvents: array expected");
  int64_t n = 0;
  if (JS_GetLength(ctx, argv[0], &n) != 0) return JS_EXCEPTION;
  if (n < 0 || n > 1000000) return JS_ThrowRangeError(ctx, "packEvents: too many events");
  std::vector<sq_clip_event> evs(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    JSValue o = JS_GetPropertyUint32(ctx, argv[0], static_cast<uint32_t>(i));
    sq_clip_event& ev = evs[static_cast<size_t>(i)];
    std::memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev;
    ev.position = propNum(ctx, o, "position", 0);
    ev.length = propNum(ctx, o, "length", 0);
    ev.bytes[0] = clampByte(propNum(ctx, o, "status", 0x90), 255);
    ev.bytes[1] = clampByte(propNum(ctx, o, "data1", 60), 127);
    ev.bytes[2] = clampByte(propNum(ctx, o, "data2", 100), 127);
    // size counts payload bytes of `bytes[4]`: never more than 4 (consumers index bytes[size-1]).
    ev.size = clampByte(propNum(ctx, o, "size", 3), static_cast<uint8_t>(sizeof ev.bytes));
    JS_FreeValue(ctx, o);
  }
  return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(evs.data()), evs.size() * sizeof(sq_clip_event));
  GUARD_END
}

NFN(n_unpackGuids) {
  GUARD_BEGIN
  size_t size = 0;
  uint8_t* p = JS_IsArrayBuffer(argv[0]) ? JS_GetArrayBuffer(ctx, &size, argv[0]) : nullptr;
  JSValue arr = JS_NewArray(ctx);
  if (!p) return arr;
  for (size_t i = 0; i + sizeof(sq_guid) <= size; i += sizeof(sq_guid)) {
    sq_guid g;
    std::memcpy(&g, p + i, sizeof g);
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i / sizeof(sq_guid)), newString(ctx, guidToString(g)));
  }
  return arr;
  GUARD_END
}

// ---- timers + package helpers ----------------------------------------------------------------------
NFN(n_timerAdd) {
  GUARD_BEGIN
  NEED_ENGINE();
  if (!JS_IsFunction(ctx, argv[0])) return throwType(ctx, "timer callback must be a function");
  return JS_NewInt32(ctx, e->addTimer(ctx, argv[0], num(ctx, argv[1]), JS_ToBool(ctx, argv[2]) != 0));
  GUARD_END
}

NFN(n_timerCancel) {
  GUARD_BEGIN
  NEED_ENGINE();
  e->cancelTimer(static_cast<int32_t>(inum(ctx, argv[0], -1)));
  return JS_UNDEFINED;
  GUARD_END
}

NFN(n_include) {
  GUARD_BEGIN
  NEED_ENGINE();
  ContextData* cd = contextData(ctx);
  if (!cd || !cd->package) return JS_ThrowInternalError(ctx, "include() is only available inside a script package");
  sq_result r = e->ensureSource(cd->package, str(ctx, argv[0]));
  if (SQ_FAILED(r)) {
    std::string msg = e->takeNestedError();
    return JS_ThrowInternalError(ctx, "include(%s) failed: %s%s%s", str(ctx, argv[0]).c_str(), sq_result_string(r),
                                 msg.empty() ? "" : "\n", msg.c_str());
  }
  return JS_TRUE;
  GUARD_END
}

NFN(n_readPackageText) {
  GUARD_BEGIN
  ContextData* cd = contextData(ctx);
  if (!cd || !cd->package) return JS_NULL;
  std::string data;
  if (!cd->package->readFile(str(ctx, argv[0]), data)) return JS_NULL;
  return newString(ctx, data);
  GUARD_END
}

struct FnEntry {
  const char* name;
  JSCFunction* fn;
  int length;
};

const FnEntry kFunctions[] = {
    {"log", n_log, 2},
    {"resultString", n_resultString, 1},
    {"nestedError", n_nestedError, 0},
    {"isHandle", n_isHandle, 1},
    {"qi", n_qi, 2},
    {"same", n_same, 2},
    {"classCount", n_classCount, 0},
    {"classInfo", n_classInfo, 1},
    {"findClass", n_findClass, 1},
    {"createInstance", n_createInstance, 2},
    {"queryService", n_queryService, 1},
    {"resolve", n_resolve, 2},
    {"readUrl", n_readUrl, 1},
    {"registerObject", n_registerObject, 2},
    {"unregisterObject", n_unregisterObject, 1},
    {"settingsGet", n_settingsGet, 1},
    {"settingsSet", n_settingsSet, 2},
    {"settingsHas", n_settingsHas, 1},
    {"settingsRemove", n_settingsRemove, 1},
    {"settingsKeys", n_settingsKeys, 1},
    {"settingsObserve", n_settingsObserve, 2},
    {"settingsUnobserve", n_settingsUnobserve, 1},
    {"nodeInfo", n_nodeInfo, 1},
    {"nodeProps", n_nodeProps, 1},
    {"nodeChild", n_nodeChild, 2},
    {"nodeParent", n_nodeParent, 1},
    {"docInfo", n_docInfo, 1},
    {"docPath", n_docPath, 1},
    {"docTrack", n_docTrack, 2},
    {"docTempoMap", n_docTempoMap, 1},
    {"docMarkers", n_docMarkers, 1},
    {"docFind", n_docFind, 2},
    {"docEditor", n_docEditor, 1},
    {"mgrActive", n_mgrActive, 1},
    {"mgrCount", n_mgrCount, 1},
    {"mgrGet", n_mgrGet, 2},
    {"mgrFindUrl", n_mgrFindUrl, 2},
    {"trackInfo", n_trackInfo, 1},
    {"trackClip", n_trackClip, 2},
    {"clipInfo", n_clipInfo, 1},
    {"clipSource", n_clipSource, 1},
    {"clipEvents", n_clipEvents, 1},
    {"tempoPoints", n_tempoPoints, 1},
    {"tempoSignatures", n_tempoSignatures, 1},
    {"ppqToSeconds", n_ppqToSeconds, 2},
    {"secondsToPpq", n_secondsToPpq, 2},
    {"edBegin", n_edBegin, 2},
    {"edCommit", n_edCommit, 1},
    {"edCancel", n_edCancel, 1},
    {"edInTx", n_edInTx, 1},
    {"edExecute", n_edExecute, 3},
    {"edUndo", n_edUndo, 1},
    {"edRedo", n_edRedo, 1},
    {"edCanUndo", n_edCanUndo, 1},
    {"edCanRedo", n_edCanRedo, 1},
    {"edUndoCount", n_edUndoCount, 1},
    {"edRedoCount", n_edRedoCount, 1},
    {"edUndoDesc", n_edUndoDesc, 2},
    {"edRedoDesc", n_edRedoDesc, 2},
    {"edClearHistory", n_edClearHistory, 1},
    {"cmdInterpret", n_cmdInterpret, 5},
    {"notify", n_notify, 4},
    {"subjAdd", n_subjAdd, 2},
    {"subjRemove", n_subjRemove, 2},
    {"ctlGet", n_ctlGet, 2},
    {"ctlSet", n_ctlSet, 3},
    {"ctlChild", n_ctlChild, 2},
    {"compInit", n_compInit, 2},
    {"compTerm", n_compTerm, 1},
    {"actPerform", n_actPerform, 4},
    {"audioPeak", n_audioPeak, 3},
    {"packEvents", n_packEvents, 1},
    {"unpackGuids", n_unpackGuids, 1},
    {"timerAdd", n_timerAdd, 3},
    {"timerCancel", n_timerCancel, 1},
    {"include", n_include, 1},
    {"readPackageText", n_readPackageText, 1},
};

void setGuidProp(JSContext* ctx, JSValueConst obj, const char* name, const sq_guid& g) {
  JS_SetPropertyStr(ctx, obj, name, newString(ctx, guidToString(g)));
}

bool hasOwnFunction(JSContext* ctx, JSValueConst obj) {
  JSPropertyEnum* tab = nullptr;
  uint32_t len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, JS_GPN_STRING_MASK) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  bool found = false;
  for (uint32_t i = 0; i < len && !found; ++i) {
    JSValue v = JS_GetProperty(ctx, obj, tab[i].atom);
    found = JS_IsFunction(ctx, v);
    JS_FreeValue(ctx, v);
  }
  JS_FreePropertyEnum(ctx, tab, len);
  return found;
}

}  // namespace

// ============================================================================================
JSValue makeNativeBinding(JSContext* ctx) {
  JSValue n = JS_NewObject(ctx);
  for (const FnEntry& f : kFunctions) JS_SetPropertyStr(ctx, n, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.length));
  JSValue iid = JS_NewObject(ctx);
  setGuidProp(ctx, iid, "IUnknown", sq::IUnknown::iid);
  setGuidProp(ctx, iid, "ICommandHandler", sq::ICommandHandler::iid);
  setGuidProp(ctx, iid, "IObserver", sq::IObserver::iid);
  setGuidProp(ctx, iid, "IController", sq::IController::iid);
  setGuidProp(ctx, iid, "IComponent", sq::IComponent::iid);
  setGuidProp(ctx, iid, "IAction", sq::IAction::iid);
  setGuidProp(ctx, iid, "ISubject", sq::ISubject::iid);
  setGuidProp(ctx, iid, "IAttributes", sq::IAttributes::iid);
  setGuidProp(ctx, iid, "IByteStream", sq::IByteStream::iid);
  setGuidProp(ctx, iid, "INode", sq::INode::iid);
  setGuidProp(ctx, iid, "IDocument", sq::IDocument::iid);
  setGuidProp(ctx, iid, "ITrack", sq::ITrack::iid);
  setGuidProp(ctx, iid, "IClip", sq::IClip::iid);
  setGuidProp(ctx, iid, "ITempoMap", sq::ITempoMap::iid);
  setGuidProp(ctx, iid, "IEditor", sq::IEditor::iid);
  setGuidProp(ctx, iid, "IDocumentManager", sq::IDocumentManager::iid);
  setGuidProp(ctx, iid, "IObjectRegistry", sq::IObjectRegistry::iid);
  setGuidProp(ctx, iid, "IUrlResolver", sq::IUrlResolver::iid);
  setGuidProp(ctx, iid, "IScriptEngine", sq::IScriptEngine::iid);
  setGuidProp(ctx, iid, "IScriptPackage", sq::IScriptPackage::iid);
  setGuidProp(ctx, iid, "ICodecService", sq::ICodecService::iid);
  JS_SetPropertyStr(ctx, n, "iid", iid);
  JSValue act = JS_NewObject(ctx);
  setGuidProp(ctx, act, "SetProperty", sq::doc::kSetProperty);
  setGuidProp(ctx, act, "AddTrack", sq::doc::kAddTrack);
  setGuidProp(ctx, act, "RemoveNode", sq::doc::kRemoveNode);
  setGuidProp(ctx, act, "AddClip", sq::doc::kAddClip);
  setGuidProp(ctx, act, "MoveClip", sq::doc::kMoveClip);
  setGuidProp(ctx, act, "ResizeClip", sq::doc::kResizeClip);
  setGuidProp(ctx, act, "SplitClip", sq::doc::kSplitClip);
  setGuidProp(ctx, act, "SetClipEvents", sq::doc::kSetClipEvents);
  setGuidProp(ctx, act, "AddMarker", sq::doc::kAddMarker);
  setGuidProp(ctx, act, "SetTempo", sq::doc::kSetTempo);
  setGuidProp(ctx, act, "RemoveTempo", sq::doc::kRemoveTempo);
  setGuidProp(ctx, act, "SetSignature", sq::doc::kSetSignature);
  JS_SetPropertyStr(ctx, n, "actions", act);
  Engine* e = eng(ctx);
  JS_SetPropertyStr(ctx, n, "engineInfo", newString(ctx, e ? e->engineInfo() : std::string("QuickJS-ng")));
  return n;
}

sq::IUnknown* unboxHandle(JSContext* ctx, JSValueConst v) {
  Engine* e = eng(ctx);
  if (!e || !JS_IsObject(v)) return nullptr;
  if (void* p = JS_GetOpaque(v, e->nativeClassId())) return static_cast<sq::IUnknown*>(p);
  JSValue h = JS_GetPropertyStr(ctx, v, "__sqHandle");
  if (JS_IsException(h)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return nullptr;
  }
  void* p = JS_IsObject(h) ? JS_GetOpaque(h, e->nativeClassId()) : nullptr;
  JS_FreeValue(ctx, h);
  return static_cast<sq::IUnknown*>(p);
}

sq::Ptr<sq::IUnknown> jsToUnknown(JSContext* ctx, JSValueConst v) {
  if (sq::IUnknown* u = unboxHandle(ctx, v)) return sq::Ptr<sq::IUnknown>(u);
  Engine* e = eng(ctx);
  sq::Ptr<sq::IUnknown> r;
  if (e && JS_IsObject(v)) e->wrapScriptValue(ctx, v, nullptr, &sq::IUnknown::iid, r.putVoid());
  return r;
}

JSValue wrapForScript(JSContext* ctx, JSValue v) {
  ContextData* cd = contextData(ctx);
  if (!cd || !JS_IsFunction(ctx, cd->wrapFn)) return v;
  JSValue r = JS_Call(ctx, cd->wrapFn, JS_UNDEFINED, 1, &v);
  JS_FreeValue(ctx, v);
  if (JS_IsException(r)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_NULL;
  }
  return r;
}

sq_result resultFromJs(JSContext* ctx, JSValueConst v, sq_result disabledCode) {
  if (JS_IsUndefined(v) || JS_IsNull(v)) return SQ_OK;
  if (JS_IsBool(v)) return JS_ToBool(ctx, v) ? SQ_OK : disabledCode;
  if (JS_IsNumber(v)) {
    double d = num(ctx, v, 0);
    if (!std::isfinite(d)) return SQ_E_FAIL;
    return static_cast<sq_result>(std::clamp(d, -2147483648.0, 2147483647.0));
  }
  return SQ_OK;
}

bool jsToAttrValue(JSContext* ctx, sq::IAttributes* a, const char* key, JSValueConst v, int depth) {
  if (!a || !key || JS_IsUndefined(v) || JS_IsNull(v)) return false;
  if (JS_IsBool(v)) return a->setBool(key, JS_ToBool(ctx, v) ? SQ_TRUE : SQ_FALSE) == SQ_OK;
  if (JS_IsNumber(v)) {
    double d = num(ctx, v);
    if (std::isfinite(d) && d == std::trunc(d) && std::fabs(d) < 9.0e15) return a->setInt(key, static_cast<int64_t>(d)) == SQ_OK;
    return a->setDouble(key, d) == SQ_OK;
  }
  if (JS_IsBigInt(v)) {
    int64_t i = 0;
    if (JS_ToInt64Ext(ctx, &i, v) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return false;
    }
    return a->setInt(key, i) == SQ_OK;
  }
  if (JS_IsString(v)) {
    std::string s = str(ctx, v);
    sq_guid g{};
    if (parseBracedGuid(s.data(), s.size(), &g)) return a->setGuid(key, &g) == SQ_OK;
    return a->setString(key, s.c_str()) == SQ_OK;
  }
  if (!JS_IsObject(v)) return a->setString(key, str(ctx, v).c_str()) == SQ_OK;

  // Explicitly typed value: Host.Values.int(x) etc.
  JSValue t = JS_GetPropertyStr(ctx, v, "__sqType");
  if (JS_IsException(t)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    t = JS_UNDEFINED;
  }
  if (JS_IsString(t)) {
    std::string type = str(ctx, t);
    JS_FreeValue(ctx, t);
    JSValue inner = JS_GetPropertyStr(ctx, v, "value");
    bool ok = false;
    if (type == "int") ok = a->setInt(key, inum(ctx, inner)) == SQ_OK;
    else if (type == "double") ok = a->setDouble(key, num(ctx, inner)) == SQ_OK;
    else if (type == "string") ok = a->setString(key, str(ctx, inner).c_str()) == SQ_OK;
    else if (type == "guid") {
      sq_guid g{};
      ok = parseAnyGuid(str(ctx, inner), &g) && a->setGuid(key, &g) == SQ_OK;
    }
    JS_FreeValue(ctx, inner);
    return ok;
  }
  JS_FreeValue(ctx, t);

  if (sq::IUnknown* u = unboxHandle(ctx, v)) return a->setObject(key, u) == SQ_OK;

  if (JS_IsArrayBuffer(v)) {
    size_t size = 0;
    uint8_t* p = JS_GetArrayBuffer(ctx, &size, v);
    if (!p && size) return false;
    return a->setBlob(key, p ? p : reinterpret_cast<const uint8_t*>(""), static_cast<uint32_t>(size)) == SQ_OK;
  }
  if (JS_GetTypedArrayType(v) >= 0) {
    size_t off = 0, len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return false;
    }
    size_t size = 0;
    uint8_t* p = JS_GetArrayBuffer(ctx, &size, buf);
    bool ok = p && off + len <= size && a->setBlob(key, p + off, static_cast<uint32_t>(len)) == SQ_OK;
    JS_FreeValue(ctx, buf);
    return ok;
  }
  if (JS_IsArray(v)) {
    int64_t n = 0;
    if (JS_GetLength(ctx, v, &n) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return false;
    }
    std::vector<sq_guid> guids;
    bool allGuids = n > 0;
    for (int64_t i = 0; i < n && allGuids; ++i) {
      JSValue el = JS_GetPropertyUint32(ctx, v, static_cast<uint32_t>(i));
      sq_guid g{};
      if (JS_IsString(el)) {
        std::string s = str(ctx, el);
        allGuids = parseBracedGuid(s.data(), s.size(), &g);
      } else {
        allGuids = false;
      }
      JS_FreeValue(ctx, el);
      if (allGuids) guids.push_back(g);
    }
    if (allGuids) return a->setBlob(key, guids.data(), static_cast<uint32_t>(guids.size() * sizeof(sq_guid))) == SQ_OK;
  }
  if (JS_IsFunction(ctx, v) || (!JS_IsArray(v) && hasOwnFunction(ctx, v))) {
    sq::Ptr<sq::IUnknown> u = jsToUnknown(ctx, v);
    return u && a->setObject(key, u.get()) == SQ_OK;
  }
  if (!JS_IsArray(v) && depth < kMaxDepth) {
    auto nested = sq::makeAttributes();
    jsToAttrs(ctx, v, nested.get(), depth + 1);
    return a->setObject(key, nested.get()) == SQ_OK;
  }
  JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  bool ok = a->setString(key, str(ctx, json).c_str()) == SQ_OK;
  JS_FreeValue(ctx, json);
  return ok;
}

void jsToAttrs(JSContext* ctx, JSValueConst obj, sq::IAttributes* a, int depth) {
  if (!a || !JS_IsObject(obj)) return;
  JSPropertyEnum* tab = nullptr;
  uint32_t len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return;
  }
  for (uint32_t i = 0; i < len; ++i) {
    const char* k = JS_AtomToCString(ctx, tab[i].atom);
    if (!k) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      continue;
    }
    JSValue v = JS_GetProperty(ctx, obj, tab[i].atom);
    if (JS_IsException(v))
      JS_FreeValue(ctx, JS_GetException(ctx));
    else
      jsToAttrValue(ctx, a, k, v, depth);
    JS_FreeValue(ctx, v);
    JS_FreeCString(ctx, k);
  }
  JS_FreePropertyEnum(ctx, tab, len);
}

JSValue attrValueToJs(JSContext* ctx, sq::IAttributes* a, const char* key, int depth) {
  switch (a->getType(key)) {
    case SQ_VALUE_INT: return JS_NewInt64(ctx, a->getInt(key, 0));
    case SQ_VALUE_DOUBLE: return JS_NewFloat64(ctx, a->getDouble(key, 0.0));
    case SQ_VALUE_BOOL: return JS_NewBool(ctx, a->getBool(key, SQ_FALSE) != 0);
    case SQ_VALUE_STRING: return newString(ctx, sq::getAttrString(a, key));
    case SQ_VALUE_GUID: {
      sq_guid g{};
      a->getGuid(key, &g);
      return newString(ctx, guidToString(g));
    }
    case SQ_VALUE_BLOB: {
      uint32_t need = 0;
      a->getBlob(key, nullptr, 0, &need);
      std::vector<uint8_t> buf(need);
      if (need && a->getBlob(key, buf.data(), need, &need) != SQ_OK) return JS_NULL;
      return JS_NewArrayBufferCopy(ctx, buf.data(), buf.size());
    }
    case SQ_VALUE_OBJECT: {
      sq::Ptr<sq::IUnknown> u;
      if (a->getObject(key, &sq::IUnknown::iid, u.putVoid()) != SQ_OK || !u) return JS_NULL;
      if (depth < kMaxDepth) {
        if (auto nested = u.as<sq::IAttributes>()) return attrsToJs(ctx, nested.get(), depth + 1);
      }
      Engine* e = eng(ctx);
      return e ? e->box(ctx, u.get(), true) : JS_NULL;
    }
    default: return JS_UNDEFINED;
  }
}

JSValue attrsToJs(JSContext* ctx, sq::IAttributes* a, int depth) {
  JSValue o = JS_NewObject(ctx);
  if (!a) return o;
  const uint32_t n = a->getCount();
  for (uint32_t i = 0; i < n; ++i) {
    std::string key = getStr([&](char* b, uint32_t s, uint32_t* nd) { return a->getKey(i, b, s, nd); });
    if (key.empty()) continue;
    JS_SetPropertyStr(ctx, o, key.c_str(), attrValueToJs(ctx, a, key.c_str(), depth));
  }
  return o;
}

}  // namespace sqjs
