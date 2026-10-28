// Squizzy Studio engine/doc - DocumentManager, object://doc/ URLs, autosave, host registration.
#include "sq/engine/doc/manager.hpp"

#include <algorithm>
#include <cstring>

#include "internal.hpp"
#include "sq/engine/doc/archive.hpp"
#include "sq/platform.hpp"

namespace sq::edoc {

namespace {
// Folder of a document file (media paths are stored relative to it).
std::string folderOf(const std::string& path) {
  const size_t s = path.find_last_of("/\\");
  if (s == std::string::npos) return ".";
  if (s == 0) return path.substr(0, 1);
  if (s == 2 && path[1] == ':') return path.substr(0, 3);  // "C:\" (a bare "C:" is drive-relative)
  return path.substr(0, s);
}
}  // namespace

DocumentManagerImpl::~DocumentManagerImpl() {
  for (auto& d : docs_) d->markClosed();
}

DocumentImpl* DocumentManagerImpl::find(const sq_guid& docId) const {
  for (const auto& d : docs_)
    if (d->store().props.id == docId) return d.get();
  return nullptr;
}

void DocumentManagerImpl::notify(const char* topic, const sq_guid& docId) {
  auto a = makeAttributes();
  a->setGuid("docId", &docId);
  std::vector<Ptr<IObserver>> obs = observers_;
  for (auto& o : obs) o->onNotify(static_cast<IDocumentManager*>(this), topic, a.get());
}

sq_result DocumentManagerImpl::adopt(Ptr<DocumentImpl> d, IDocument** out) {
  const sq_guid id = d->store().props.id;
  docs_.push_back(d);
  notify("document.opened", id);
  if (!find(active_)) {
    active_ = id;
    notify("document.activated", id);
  }
  if (out) {
    *out = d.get();
    d->addRef();
  }
  return SQ_OK;
}

sq_result SQ_CALL DocumentManagerImpl::createDocument(int32_t kind, IDocument** out) {
  if (out) *out = nullptr;
  if (kind != SQ_DOC_SONG && kind != SQ_DOC_PROJECT && kind != SQ_DOC_SHOW) return SQ_E_INVALIDARG;
  try {
    return adopt(DocumentImpl::create(kind, registry_), out);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result SQ_CALL DocumentManagerImpl::openDocument(const char* path, IDocument** out) {
  if (out) *out = nullptr;
  if (!path || !*path) return SQ_E_INVALIDARG;
  try {
    auto bytes = readFile(path);
    if (!bytes) return sq_fs_exists(path) ? SQ_E_IO : SQ_E_NOTFOUND;
    Store s;
    std::string err;
    ArchiveMeta meta;
    meta.mediaBase = folderOf(path);
    if (sq_result r = readArchive(*bytes, s, &meta, &err); r != SQ_OK) {
      sq_log_printf(SQ_LOG_WARN, "doc", "cannot open '%s': %s", path, err.c_str());
      return r;
    }
    if (DocumentImpl* existing = find(s.props.id)) {  // already open: hand out the open one
      if (out) {
        *out = existing;
        existing->addRef();
      }
      return SQ_S_FALSE;
    }
    Ptr<DocumentImpl> d = DocumentImpl::fromStore(std::move(s), registry_);
    d->setPath(path);
    return adopt(d, out);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL DocumentManagerImpl::saveDocument(IDocument* doc, const char* path) {
  DocumentImpl* d = DocumentImpl::from(doc);
  if (!d) return SQ_E_INVALIDARG;
  if (d->closed()) return SQ_E_CLOSED;
  if (d->inTransaction()) return SQ_E_BUSY;
  const std::string target = (path && *path) ? std::string(path) : d->path();
  if (target.empty()) return SQ_E_INVALIDARG;
  try {
    ArchiveMeta meta;
    meta.mediaBase = folderOf(target);
    const std::string bytes = writeArchive(d->store(), meta);
    if (bytes.empty()) return SQ_E_FAIL;
    if (sq_result r = safeWriteFile(target, bytes); r != SQ_OK) return r;
    d->setPath(target);
    d->markSaved();
    discardAutosave(*d);
    auto a = makeAttributes();
    a->setGuid("docId", &d->store().props.id);
    d->notify("document.saved", a.get());
    notify("document.saved", d->store().props.id);
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL DocumentManagerImpl::closeDocument(IDocument* doc) {
  DocumentImpl* d = DocumentImpl::from(doc);
  if (!d) return SQ_E_INVALIDARG;
  auto it = std::find_if(docs_.begin(), docs_.end(), [&](const Ptr<DocumentImpl>& p) { return p.get() == d; });
  if (it == docs_.end()) return SQ_E_NOTFOUND;
  if (d->executing()) return SQ_E_BUSY;  // called from inside one of its own actions
  while (d->inTransaction())
    if (SQ_FAILED(d->cancelTransaction())) return SQ_E_BUSY;
  Ptr<DocumentImpl> keep = *it;  // alive until the notifications are out
  docs_.erase(it);
  const sq_guid id = d->store().props.id;
  discardAutosave(*d);
  d->markClosed();
  notify("document.closed", id);
  if (active_ == id) {
    active_ = sq_guid{};
    if (!docs_.empty()) {
      active_ = docs_.back()->store().props.id;
      notify("document.activated", active_);
    }
  }
  return SQ_OK;
}

sq_result SQ_CALL DocumentManagerImpl::getDocument(uint32_t index, IDocument** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (index >= docs_.size()) return SQ_E_NOTFOUND;
  *out = docs_[index].get();
  (*out)->addRef();
  return SQ_OK;
}

sq_result SQ_CALL DocumentManagerImpl::getActiveDocument(IDocument** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  DocumentImpl* d = find(active_);
  if (!d) return SQ_E_NOTFOUND;
  *out = d;
  d->addRef();
  return SQ_OK;
}

sq_result SQ_CALL DocumentManagerImpl::setActiveDocument(IDocument* doc) {
  DocumentImpl* d = DocumentImpl::from(doc);
  if (!d || !find(d->store().props.id) || find(d->store().props.id) != d) return SQ_E_NOTFOUND;
  if (active_ == d->store().props.id) return SQ_S_FALSE;
  active_ = d->store().props.id;
  notify("document.activated", active_);
  return SQ_OK;
}

// object://doc/<docGuid|active>[/node/<guid> | /track/<index> | /tempomap]
sq_result SQ_CALL DocumentManagerImpl::findNodeByUrl(const char* url, INode** out) {
  if (!out || !url) return SQ_E_INVALIDARG;
  *out = nullptr;
  const std::string prefix = kDocUrlPrefix;
  std::string u(url);
  if (u.compare(0, prefix.size(), prefix) != 0) return SQ_E_INVALIDARG;
  std::vector<std::string> seg;
  size_t at = prefix.size();
  while (at <= u.size()) {
    size_t slash = u.find('/', at);
    if (slash == std::string::npos) slash = u.size();
    if (slash > at) seg.push_back(u.substr(at, slash - at));
    at = slash + 1;
  }
  if (seg.empty()) return SQ_E_NOTFOUND;
  DocumentImpl* d = nullptr;
  if (seg[0] == "active") {
    d = find(active_);
  } else {
    sq_guid id{};
    if (!parseGuid(seg[0], id)) return SQ_E_INVALIDARG;
    d = find(id);
  }
  if (!d) return SQ_E_NOTFOUND;
  if (seg.size() == 1) return d->queryInterface(&INode::iid, reinterpret_cast<void**>(out));
  if (seg.size() == 2 && seg[1] == "tempomap") return d->makeNode(d->store().tempo.id, out);
  if (seg.size() != 3) return SQ_E_NOTFOUND;
  if (seg[1] == "node") {
    sq_guid id{};
    if (!parseGuid(seg[2], id)) return SQ_E_INVALIDARG;
    return d->makeNode(id, out);
  }
  if (seg[1] == "track") {
    char* end = nullptr;
    const unsigned long idx = std::strtoul(seg[2].c_str(), &end, 10);
    if (!end || *end || seg[2].empty()) return SQ_E_INVALIDARG;
    if (idx >= d->store().trackOrder.size()) return SQ_E_NOTFOUND;
    return d->makeNode(d->store().trackOrder[idx], out);
  }
  return SQ_E_NOTFOUND;
}

sq_result SQ_CALL DocumentManagerImpl::addObserver(IObserver* observer) {
  if (!observer) return SQ_E_INVALIDARG;
  for (auto& o : observers_)
    if (o.get() == observer) return SQ_S_FALSE;
  observers_.emplace_back(observer);
  return SQ_OK;
}
sq_result SQ_CALL DocumentManagerImpl::removeObserver(IObserver* observer) {
  for (auto it = observers_.begin(); it != observers_.end(); ++it)
    if (it->get() == observer) {
      observers_.erase(it);
      return SQ_OK;
    }
  return SQ_E_NOTFOUND;
}

// ---- autosave ----------------------------------------------------------------------------------------
void DocumentManagerImpl::setAutosave(const std::string& dir, int64_t intervalMs) {
  autosaveDir_ = dir;
  autosaveInterval_ = intervalMs;
  lastTick_ = -1;
}

std::string DocumentManagerImpl::autosavePath(const DocumentImpl& doc) const {
  if (autosaveDir_.empty()) return {};
  std::string id = guidStr(doc.store().props.id);
  id = id.substr(1, id.size() - 2);  // no braces in file names
  return joinPath(autosaveDir_, id + ".autosave");
}

sq_result DocumentManagerImpl::autosaveNow(DocumentImpl& doc) {
  const std::string p = autosavePath(doc);
  if (p.empty()) return SQ_E_INVALIDARG;
  if (doc.inTransaction()) return SQ_E_BUSY;  // never snapshot half an edit
  try {
    ArchiveMeta meta;
    meta.extra["autosave"] = "1";
    if (!doc.path().empty()) meta.extra["originalPath"] = doc.path();
    const std::string bytes = writeArchive(doc.store(), meta);
    if (bytes.empty()) return SQ_E_FAIL;
    if (sq_result r = safeWriteFile(p, bytes); r != SQ_OK) return r;
    const sq_guid id = doc.store().props.id;
    auto it = std::find_if(autosaved_.begin(), autosaved_.end(), [&](const auto& e) { return e.first == id; });
    if (it == autosaved_.end()) autosaved_.emplace_back(id, doc.editSerial());
    else it->second = doc.editSerial();
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

void DocumentManagerImpl::tick(int64_t nowMs) {
  if (autosaveDir_.empty() || autosaveInterval_ <= 0) return;
  if (lastTick_ < 0) {
    lastTick_ = nowMs;
    return;
  }
  if (nowMs - lastTick_ < autosaveInterval_) return;
  lastTick_ = nowMs;
  for (auto& d : docs_) {
    if (!d->modified() || d->inTransaction()) continue;
    const sq_guid id = d->store().props.id;
    auto it = std::find_if(autosaved_.begin(), autosaved_.end(), [&](const auto& e) { return e.first == id; });
    if (it != autosaved_.end() && it->second == d->editSerial()) continue;
    if (SQ_FAILED(autosaveNow(*d))) sq_log_printf(SQ_LOG_WARN, "doc", "autosave failed for %s", guidStr(id).c_str());
  }
}

void DocumentManagerImpl::discardAutosave(const DocumentImpl& doc) {
  const sq_guid id = doc.store().props.id;
  autosaved_.erase(std::remove_if(autosaved_.begin(), autosaved_.end(), [&](const auto& e) { return e.first == id; }), autosaved_.end());
  const std::string p = autosavePath(doc);
  if (!p.empty() && sq_fs_exists(p.c_str())) sq_fs_remove(p.c_str());
}

std::vector<std::string> DocumentManagerImpl::listRecoverable() const {
  std::vector<std::string> out;
  if (autosaveDir_.empty()) return out;
  struct Ctx {
    std::vector<std::string>* v;
    const std::string* dir;
  } ctx{&out, &autosaveDir_};
  sq_fs_list_dir(
      autosaveDir_.c_str(),
      [](void* user, const char* name, sq_bool isDir) -> sq_bool {
        auto* c = static_cast<Ctx*>(user);
        const std::string n = name;
        const std::string ext = ".autosave";
        if (!isDir && n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) c->v->push_back(joinPath(*c->dir, n));
        return SQ_TRUE;
      },
      &ctx);
  std::sort(out.begin(), out.end());
  return out;
}

sq_result DocumentManagerImpl::recover(const char* autosaveFile, IDocument** out) {
  if (out) *out = nullptr;
  if (!autosaveFile) return SQ_E_INVALIDARG;
  try {
    auto bytes = readFile(autosaveFile);
    if (!bytes) return SQ_E_NOTFOUND;
    Store s;
    ArchiveMeta meta;
    if (sq_result r = readArchive(*bytes, s, &meta); r != SQ_OK) return r;
    if (find(s.props.id)) return SQ_E_EXISTS;
    Ptr<DocumentImpl> d = DocumentImpl::fromStore(std::move(s), registry_);
    auto op = meta.extra.find("originalPath");
    if (op != meta.extra.end()) d->setPath(op->second);
    d->markModified();
    return adopt(d, out);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

// ---- URL resolver ---------------------------------------------------------------------------------------
sq_result SQ_CALL DocUrlResolver::resolve(const char* url, const sq_guid* riid, void** out) {
  if (!out || !riid) return SQ_E_INVALIDARG;
  *out = nullptr;
  Ptr<INode> n;
  sq_result r = mgr_->findNodeByUrl(url, n.put());
  if (SQ_FAILED(r)) return r;
  return n->queryInterface(riid, out);
}

// ---- action classes ----------------------------------------------------------------------------------------
namespace {

class NativeActionObject final : public Object<NativeActionObject, IAction> {
 public:
  explicit NativeActionObject(const sq_guid& id) : id_(id) {}
  sq_result SQ_CALL perform(IDocument*, IEditor* editor, IAttributes* params, IAttributes* result) override {
    if (!editor) return SQ_E_INVALIDARG;
    return editor->execute(&id_, params, result);  // the editor runs native ids directly
  }

 private:
  sq_guid id_;
};

class ActionClassFactory final : public Object<ActionClassFactory, IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info* out) override {
    if (!out || out->struct_size < sizeof(sq_module_info)) return SQ_E_INVALIDARG;
    out->fourcc = SQ_FOURCC('S', 'Q', 'D', 'C');
    out->name = "engine-doc";
    out->version = SQ_VERSION;
    out->abi_min = SQ_ABI_DEFAULT_MIN;
    out->abi_max = SQ_ABI_DEFAULT_MAX;
    out->sdk_abi = SQ_PLATFORM_ABI;
    out->build_stamp = SQ_BUILD_STAMP;
    out->description = "document actions";
    return SQ_OK;
  }
  uint32_t SQ_CALL getClassCount() override { return static_cast<uint32_t>(nativeActions().size()); }
  sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) override {
    if (!out || out->struct_size < sizeof(sq_class_info)) return SQ_E_INVALIDARG;
    auto all = nativeActions();
    if (index >= all.size()) return SQ_E_NOTFOUND;
    out->class_id = all[index].first;
    out->category = sq::doc::kActionCategory;
    out->name = all[index].second->name();
    out->flags = 0;
    out->module_fourcc = 0;
    return SQ_OK;
  }
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!classId || !riid) return SQ_E_INVALIDARG;
    if (!findAction(*classId)) return SQ_E_NOTFOUND;
    Ptr<NativeActionObject> o = Ptr<NativeActionObject>::adopt(new NativeActionObject(*classId));
    return o->queryInterface(riid, out);
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

struct Registration {
  Ptr<DocumentManagerImpl> mgr;
  Ptr<DocUrlResolver> resolver;
  Ptr<IObjectRegistry> objects;
};
Registration& registration() {
  // Intentionally never destroyed: releasing a foreign module's IObjectRegistry from a static
  // destructor at process exit could call into an already unloaded DLL.
  static Registration* r = new Registration();
  return *r;
}

}  // namespace

Ptr<IClassFactory> makeActionClassFactory() { return Ptr<IClassFactory>::adopt(new ActionClassFactory()); }

sq_result registerActionClasses(IClassRegistry* reg) {
  if (!reg) return SQ_E_INVALIDARG;
  Ptr<IClassFactory> f = makeActionClassFactory();
  sq_result result = SQ_OK;
  for (auto& [id, a] : nativeActions()) {
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    ci.class_id = id;
    ci.category = sq::doc::kActionCategory;
    ci.name = a->name();
    sq_result r = reg->registerClass(&ci, f.get());
    if (SQ_FAILED(r) && r != SQ_E_EXISTS) result = r;
  }
  return result;
}

void unregisterActionClasses(IClassRegistry* reg) {
  if (!reg) return;
  for (auto& [id, a] : nativeActions()) reg->unregisterClass(&id);
}

sq_result registerDocumentServices(IHostContext* host, DocumentManagerImpl** manager) {
  if (!host) return SQ_E_INVALIDARG;
  Registration& st = registration();
  if (st.mgr) return SQ_E_EXISTS;
  Ptr<DocumentManagerImpl> mgr = Ptr<DocumentManagerImpl>::adopt(new DocumentManagerImpl(host->getClassRegistry()));
  if (sq_result r = host->registerService(&IDocumentManager::iid, static_cast<IDocumentManager*>(mgr.get())); SQ_FAILED(r)) return r;
  registerActionClasses(host->getClassRegistry());
  Ptr<IObjectRegistry> objects;
  if (host->queryService(&IObjectRegistry::iid, objects.putVoid()) == SQ_OK && objects) {
    Ptr<DocUrlResolver> res = Ptr<DocUrlResolver>::adopt(new DocUrlResolver(mgr.get()));
    if (objects->registerResolver(kDocUrlPrefix, res.get()) == SQ_OK) {
      st.resolver = res;
      st.objects = objects;
    }
  }
  st.mgr = mgr;
  if (manager) *manager = mgr.get();
  return SQ_OK;
}

void unregisterDocumentServices(IHostContext* host) {
  Registration& st = registration();
  if (!st.mgr) return;
  if (st.objects && st.resolver) st.objects->unregisterResolver(kDocUrlPrefix, st.resolver.get());
  st.objects = nullptr;
  st.resolver = nullptr;
  if (host) {
    unregisterActionClasses(host->getClassRegistry());
    host->unregisterService(&IDocumentManager::iid, static_cast<IDocumentManager*>(st.mgr.get()));
  }
  st.mgr = nullptr;
}

}  // namespace sq::edoc
