// Squizzy Studio engine/doc - document object, transactions, actions, side effects, undo.
//
//   Action      native command object: perform(Editing&, params, result). Every built-in action of
//               document.h (sq::doc::k*) plus the engine extras in actions.hpp is one of these.
//   Editing     the only way to mutate a Store. Each call records a Change (before/after) into the
//               open transaction and applies it.
//   SideEffect  declarative propagator: a trigger mask (ChangeKind bits) + enabled(store) +
//               propagate(new changes). Run at the outermost commit until nothing new happens;
//               their changes become part of the same undo step.
//   UndoKeeper  undo/redo stacks of committed transactions; consecutive transactions with the same
//               non-empty merge key collapse into one step.
//   Journal     UI state (selection, scroll...) stored in Store::journal. Journal changes made
//               inside a transaction ride along with it (undo restores them) but never create an
//               undo step on their own and never mark the document modified.
//
// MAIN THREAD ONLY.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sq/cpp/object.hpp"
#include "sq/cpp/ptr.hpp"
#include "sq/engine/doc/model.hpp"
#include "sq/interfaces/common.h"
#include "sq/interfaces/document.h"

namespace sq::edoc {

class DocumentImpl;

// ---- transactions + undo -------------------------------------------------------------------------
struct Transaction {
  std::string description;
  std::string mergeKey;
  std::vector<Change> changes;
  uint64_t serial = 0;
  // Merge bookkeeping: the merge key survives only if every successful native action of the
  // transaction set that same key (otherwise unrelated edits would be folded into a drag step).
  uint32_t actions = 0;
  uint32_t mergeHits = 0;
  bool mergePoisoned = false;
  bool hasDocumentChanges() const;  // anything besides journal entries
};

class UndoKeeper {
 public:
  void setLimit(size_t maxSteps) { limit_ = maxSteps; trim(); }  // 0 = unlimited
  size_t limit() const { return limit_; }
  // Pushes a committed transaction (or merges it into the top step); clears the redo stack.
  void record(Transaction&& t);
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  size_t undoCount() const { return undo_.size(); }
  size_t redoCount() const { return redo_.size(); }
  const Transaction& undoAt(size_t i) const { return undo_[undo_.size() - 1 - i]; }  // 0 = next to undo
  const Transaction& redoAt(size_t i) const { return redo_[redo_.size() - 1 - i]; }
  Transaction popUndo();
  Transaction popRedo();
  void pushUndo(Transaction&& t) { undo_.push_back(std::move(t)); }
  void pushRedo(Transaction&& t) { redo_.push_back(std::move(t)); }
  uint64_t topSerial() const { return undo_.empty() ? 0 : undo_.back().serial; }
  void clear() { undo_.clear(); redo_.clear(); }

 private:
  void trim();
  std::vector<Transaction> undo_, redo_;
  size_t limit_ = 0;
};

// ---- mutation facade ------------------------------------------------------------------------------
class Editing {
 public:
  explicit Editing(DocumentImpl& doc) : doc_(doc) {}
  DocumentImpl& document() { return doc_; }
  const Store& store() const;

  void setTag(ChangeTag t) { tag_ = t; }
  ChangeTag tag() const { return tag_; }
  void setMergeKey(const std::string& key);  // for the enclosing outermost transaction

  void setDocProps(const DocProps& p);
  void setTempo(const TempoMapRec& t);
  void putTrack(const TrackRec& t, int32_t index = -1);  // -1: keep position / append
  void removeTrack(const sq_guid& id);
  void putClip(const ClipRec& c);
  void removeClip(const sq_guid& id);
  void putMarker(const MarkerRec& m);
  void removeMarker(const sq_guid& id);
  void setJournal(const std::string& key, const std::optional<std::string>& value);

 private:
  void record(Change&& c);
  DocumentImpl& doc_;
  ChangeTag tag_ = kTagEdit;
};

// ---- actions ------------------------------------------------------------------------------------
class Action {
 public:
  virtual ~Action() = default;
  virtual const char* name() const = 0;
  // Must leave the store unchanged on failure (the editor rolls back anyway).
  virtual sq_result perform(Editing& ed, IAttributes* params, IAttributes* result) = 0;
};

// Process-wide native action table (built-ins are pre-registered). Main thread only.
void registerAction(const sq_guid& id, std::shared_ptr<Action> action);
void unregisterAction(const sq_guid& id);
Action* findAction(const sq_guid& id);
std::vector<std::pair<sq_guid, Action*>> nativeActions();

// ---- side effects -------------------------------------------------------------------------------
class SideEffect {
 public:
  virtual ~SideEffect() = default;
  virtual const char* name() const = 0;
  virtual uint32_t triggers() const = 0;  // ChangeKind mask
  virtual bool enabled(const Store&) const { return true; }
  // `fresh` = the changes recorded since this propagator last ran (copy; safe to edit meanwhile).
  virtual void propagate(Editing& ed, const std::vector<Change>& fresh) = 0;
};
// remove-overlaps, automation-follow, time-map, chord-follow (in that order).
std::vector<std::shared_ptr<SideEffect>> defaultSideEffects();
std::shared_ptr<SideEffect> makeRemoveOverlaps();
std::shared_ptr<SideEffect> makeAutomationFollow();
std::shared_ptr<SideEffect> makeTimeMap();
std::shared_ptr<SideEffect> makeChordFollow();

// ---- document -----------------------------------------------------------------------------------
// Private in-process accessor (never crosses a DLL boundary).
struct IDocumentImplAccess : IUnknown {
  SQ_INTERFACE(IDocumentImplAccess, IUnknown, "{2B6E5883-F480-46A3-B5E7-EB4750C3B947}")
  virtual DocumentImpl* SQ_CALL impl() = 0;
};

class DocumentImpl final : public Object<DocumentImpl, IDocument, IEditor, ISubject, IDocumentImplAccess> {
 public:
  // A new document of `kind` with defaults (tempo 120, 4/4, master track for song/project).
  static Ptr<DocumentImpl> create(int32_t kind, IClassRegistry* registry);
  // From an already loaded store (archive reader). Not modified, empty history.
  static Ptr<DocumentImpl> fromStore(Store&& s, IClassRegistry* registry);
  static DocumentImpl* from(IUnknown* obj);  // nullptr if not ours (borrowed)

  const Store& store() const { return store_; }
  std::string stateHash() const;  // sha256 hex over the serialized model (incl. journal)
  UndoKeeper& undoKeeper() { return undo_; }

  void addSideEffect(std::shared_ptr<SideEffect> se) { sideEffects_.push_back(std::move(se)); }
  void clearSideEffects() { sideEffects_.clear(); }

  // Journals (UI state). Inside a transaction the change rides along with it.
  void setJournal(const std::string& key, const std::string& value);
  void removeJournal(const std::string& key);
  std::string journal(const std::string& key) const;

  const std::string& path() const { return path_; }
  void setPath(const std::string& p) { path_ = p; }
  uint64_t editSerial() const { return editSerial_; }  // bumps on every commit/undo/redo
  bool modified() const { return forceModified_ || undo_.topSerial() != savedSerial_; }
  void markSaved();
  void markModified();  // e.g. recovered from autosave
  bool closed() const { return closed_; }
  // Closing drops the observers: they commonly hold the document, which would leak both.
  void markClosed() {
    closed_ = true;
    observers_.clear();
  }
  // True while an action's perform() is on the stack (closing the document then is refused).
  bool executing() const { return !floors_.empty(); }
  void notify(const char* topic, IAttributes* args);
  sq_result makeNode(const sq_guid& id, INode** out);
  std::string urlOf(const sq_guid& node) const;

  // ---- INode ----
  int32_t SQ_CALL getKind() override { return SQ_NODE_DOCUMENT; }
  sq_result SQ_CALL getId(sq_guid* out) override;
  sq_result SQ_CALL getName(char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL getUrl(char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL getParent(INode** out) override;
  uint32_t SQ_CALL getChildCount() override;
  sq_result SQ_CALL getChild(uint32_t index, INode** out) override;
  sq_result SQ_CALL getProperties(IAttributes* dst) override;
  // ---- IDocument ----
  sq_result SQ_CALL getDocumentInfo(sq_document_info* out) override;
  sq_result SQ_CALL getPath(char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL getTrack(uint32_t index, ITrack** out) override;
  sq_result SQ_CALL getTempoMap(ITempoMap** out) override;
  uint32_t SQ_CALL getMarkerCount() override;
  sq_result SQ_CALL getMarker(uint32_t index, sq_doc_marker* out) override;
  sq_result SQ_CALL findNode(const sq_guid* id, INode** out) override;
  sq_result SQ_CALL getEditor(IEditor** out) override;
  // ---- IEditor ----
  sq_result SQ_CALL beginTransaction(const char* description) override;
  sq_result SQ_CALL commitTransaction() override;
  sq_result SQ_CALL cancelTransaction() override;
  sq_bool SQ_CALL inTransaction() override { return levels_.empty() ? SQ_FALSE : SQ_TRUE; }
  sq_result SQ_CALL execute(const sq_guid* actionClassId, IAttributes* params, IAttributes* result) override;
  sq_bool SQ_CALL canUndo() override { return undo_.canUndo() ? SQ_TRUE : SQ_FALSE; }
  sq_bool SQ_CALL canRedo() override { return undo_.canRedo() ? SQ_TRUE : SQ_FALSE; }
  sq_result SQ_CALL undo() override;
  sq_result SQ_CALL redo() override;
  uint32_t SQ_CALL getUndoCount() override { return static_cast<uint32_t>(undo_.undoCount()); }
  uint32_t SQ_CALL getRedoCount() override { return static_cast<uint32_t>(undo_.redoCount()); }
  sq_result SQ_CALL getUndoDescription(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL getRedoDescription(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL clearHistory() override;
  // ---- ISubject ----
  sq_result SQ_CALL addObserver(IObserver* observer) override;
  sq_result SQ_CALL removeObserver(IObserver* observer) override;
  // ---- IDocumentImplAccess ----
  DocumentImpl* SQ_CALL impl() override { return this; }

 private:
  friend class Editing;
  explicit DocumentImpl(IClassRegistry* registry);
  void rollbackTo(size_t mark);
  void finishOutermost();
  void runSideEffects();
  void afterHistoryMove(const Transaction& t, const char* reason, bool wasModified);
  void emitChanged(const Transaction& t, const char* reason);
  void emitModifiedIfToggled(bool wasModified);

  Store store_;
  IClassRegistry* registry_ = nullptr;  // borrowed (host registry outlives documents)
  UndoKeeper undo_;
  Transaction pending_;
  std::vector<size_t> levels_;  // change-count marks of the open transaction levels
  // levels_.size() at entry of each running action: an action may only commit/cancel levels it
  // opened itself, never the one execute() opened for it or the caller's.
  std::vector<size_t> floors_;
  std::vector<std::shared_ptr<SideEffect>> sideEffects_;
  std::vector<Ptr<IObserver>> observers_;
  std::string path_;
  uint64_t nextSerial_ = 1;
  uint64_t savedSerial_ = 0;
  uint64_t editSerial_ = 0;
  bool forceModified_ = false;
  bool closed_ = false;
  bool inSideEffects_ = false;
};

// Out-string helper (contract 4.3).
sq_result copyString(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed);

}  // namespace sq::edoc
