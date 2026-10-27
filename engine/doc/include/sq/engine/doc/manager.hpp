// Squizzy Studio engine/doc - DocumentManager (IDocumentManager), object:// resolution, autosave,
// and host registration.
//
// Host wiring (whoever owns the engine startup, e.g. the app or an engine service):
//   sq::edoc::registerDocumentServices(host);   // IDocumentManager service, built-in "Action"
//                                               // classes, "object://doc/" resolver (if an
//                                               // IObjectRegistry service exists)
//   ...
//   sq::edoc::unregisterDocumentServices(host);
// MAIN THREAD ONLY.
#pragma once

#include <string>
#include <vector>

#include "sq/engine/doc/document.hpp"
#include "sq/interfaces/scripting.h"
#include "sq/module.h"

namespace sq::edoc {

class DocumentManagerImpl final : public Object<DocumentManagerImpl, IDocumentManager, ISubject> {
 public:
  explicit DocumentManagerImpl(IClassRegistry* registry = nullptr) : registry_(registry) {}
  ~DocumentManagerImpl() override;

  // ---- IDocumentManager ----
  sq_result SQ_CALL createDocument(int32_t kind, IDocument** out) override;
  sq_result SQ_CALL openDocument(const char* path, IDocument** out) override;
  sq_result SQ_CALL saveDocument(IDocument* doc, const char* path) override;
  sq_result SQ_CALL closeDocument(IDocument* doc) override;
  uint32_t SQ_CALL getDocumentCount() override { return static_cast<uint32_t>(docs_.size()); }
  sq_result SQ_CALL getDocument(uint32_t index, IDocument** out) override;
  sq_result SQ_CALL getActiveDocument(IDocument** out) override;
  sq_result SQ_CALL setActiveDocument(IDocument* doc) override;
  sq_result SQ_CALL findNodeByUrl(const char* url, INode** out) override;
  // ---- ISubject ----
  sq_result SQ_CALL addObserver(IObserver* observer) override;
  sq_result SQ_CALL removeObserver(IObserver* observer) override;

  // ---- C++ extras ----
  const std::vector<Ptr<DocumentImpl>>& documents() const { return docs_; }
  DocumentImpl* find(const sq_guid& docId) const;

  // Autosave. dir "" disables. tick() is called from the main loop with a monotonic time in ms;
  // every `intervalMs` it writes each modified document whose editSerial changed since its last
  // autosave to <dir>/<docId>.autosave (crash-safe write). A normal save or close deletes it.
  void setAutosave(const std::string& dir, int64_t intervalMs);
  void tick(int64_t nowMs);
  sq_result autosaveNow(DocumentImpl& doc);
  std::string autosavePath(const DocumentImpl& doc) const;
  // Autosave files present in the autosave dir (full paths, sorted).
  std::vector<std::string> listRecoverable() const;
  // Opens an autosave file: the document keeps its original path (if any) and is marked modified.
  sq_result recover(const char* autosaveFile, IDocument** out);

 private:
  void notify(const char* topic, const sq_guid& docId);
  sq_result adopt(Ptr<DocumentImpl> d, IDocument** out);
  void discardAutosave(const DocumentImpl& doc);

  IClassRegistry* registry_;
  std::vector<Ptr<DocumentImpl>> docs_;
  sq_guid active_{};
  std::vector<Ptr<IObserver>> observers_;
  std::string autosaveDir_;
  int64_t autosaveInterval_ = 0;
  int64_t lastTick_ = -1;
  std::vector<std::pair<sq_guid, uint64_t>> autosaved_;  // doc id -> editSerial at last autosave
};

// object://doc/ resolver backed by a manager.
class DocUrlResolver final : public Object<DocUrlResolver, IUrlResolver> {
 public:
  explicit DocUrlResolver(IDocumentManager* mgr) : mgr_(mgr) {}
  sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) override;
  sq_result SQ_CALL openStream(const char*, IByteStream** out) override {
    if (out) *out = nullptr;
    return SQ_E_NOTIMPL;
  }

 private:
  Ptr<IDocumentManager> mgr_;
};

inline constexpr const char* kDocUrlPrefix = "object://doc/";

// Registers the manager service, the built-in action classes and the URL resolver. Returns the
// manager (borrowed; owned by the host's service table until unregister).
sq_result registerDocumentServices(IHostContext* host, DocumentManagerImpl** manager = nullptr);
void unregisterDocumentServices(IHostContext* host);

// Class factory serving every native action (sq::doc::k* and actions.hpp) as an IAction whose
// perform() runs the native action through the editor. Exposed for tests / custom registries.
Ptr<IClassFactory> makeActionClassFactory();
sq_result registerActionClasses(IClassRegistry* reg);
void unregisterActionClasses(IClassRegistry* reg);

}  // namespace sq::edoc
