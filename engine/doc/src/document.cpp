// Squizzy Studio engine/doc - DocumentImpl: transactions, execute, undo/redo, observers; Editing;
// UndoKeeper; native action table.
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_set>

#include "internal.hpp"
#include "sq/engine/doc/archive.hpp"
#include "sq/platform.hpp"

namespace sq::edoc {

sq_result copyString(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed) {
  const size_t need = s.size() + 1;
  if (needed) *needed = static_cast<uint32_t>(need);
  if (!buf || bufSize < need) {
    if (buf && bufSize) {
      std::memcpy(buf, s.data(), bufSize - 1);
      buf[bufSize - 1] = '\0';
    }
    return SQ_E_BUFFER_TOO_SMALL;
  }
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  return SQ_OK;
}

// ---- native action table ----------------------------------------------------------------------------
namespace {
std::map<sq_guid, std::shared_ptr<Action>>& actionTable() {
  static std::map<sq_guid, std::shared_ptr<Action>> t;
  return t;
}
void ensureBuiltins() {
  static bool done = false;
  if (!done) {
    done = true;
    detail::registerBuiltinActions();
  }
}
}  // namespace

void registerAction(const sq_guid& id, std::shared_ptr<Action> action) {
  ensureBuiltins();
  actionTable()[id] = std::move(action);
}
void unregisterAction(const sq_guid& id) {
  ensureBuiltins();
  actionTable().erase(id);
}
Action* findAction(const sq_guid& id) {
  ensureBuiltins();
  auto it = actionTable().find(id);
  return it == actionTable().end() ? nullptr : it->second.get();
}
std::vector<std::pair<sq_guid, Action*>> nativeActions() {
  ensureBuiltins();
  std::vector<std::pair<sq_guid, Action*>> v;
  for (auto& [id, a] : actionTable()) v.emplace_back(id, a.get());
  return v;
}

// ---- undo keeper --------------------------------------------------------------------------------------
bool Transaction::hasDocumentChanges() const {
  for (const Change& c : changes)
    if (c.kind != kChangeJournal) return true;
  return false;
}

void UndoKeeper::record(Transaction&& t) {
  const bool canMerge = redo_.empty() && !t.mergeKey.empty() && !undo_.empty() && undo_.back().mergeKey == t.mergeKey;
  redo_.clear();
  if (canMerge) {
    Transaction& top = undo_.back();
    for (Change& c : t.changes) top.changes.push_back(std::move(c));
    top.serial = t.serial;
    return;
  }
  undo_.push_back(std::move(t));
  trim();
}
void UndoKeeper::trim() {
  if (limit_ && undo_.size() > limit_) undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(undo_.size() - limit_));
}
Transaction UndoKeeper::popUndo() {
  Transaction t = std::move(undo_.back());
  undo_.pop_back();
  return t;
}
Transaction UndoKeeper::popRedo() {
  Transaction t = std::move(redo_.back());
  redo_.pop_back();
  return t;
}

// ---- Editing --------------------------------------------------------------------------------------------
const Store& Editing::store() const { return doc_.store_; }

void Editing::record(Change&& c) {
  c.tag = tag_;
  doc_.store_.apply(c, true);
  doc_.pending_.changes.push_back(std::move(c));
}
void Editing::setMergeKey(const std::string& key) {
  Transaction& p = doc_.pending_;
  if (key.empty()) return;
  if (p.mergeKey.empty() && !p.mergePoisoned) {
    p.mergeKey = key;
    p.mergeHits = 1;
  } else if (p.mergeKey == key) {
    ++p.mergeHits;
  } else {
    p.mergePoisoned = true;
  }
}

void Editing::setDocProps(const DocProps& p) {
  Change c;
  c.kind = kChangeDoc;
  c.id = p.id;
  c.data = Delta<DocProps>{doc_.store_.props, p};
  record(std::move(c));
}
void Editing::setTempo(const TempoMapRec& t) {
  Change c;
  c.kind = kChangeTempo;
  c.id = t.id;
  c.data = Delta<TempoMapRec>{doc_.store_.tempo, t};
  record(std::move(c));
}
void Editing::putTrack(const TrackRec& t, int32_t index) {
  const Store& s = doc_.store_;
  TrackDelta d;
  const TrackRec* cur = s.track(t.id);
  const int32_t curIdx = s.trackIndex(t.id);
  if (cur) d.before = *cur;
  d.indexBefore = curIdx;
  d.after = t;
  const int32_t size = static_cast<int32_t>(s.trackOrder.size());
  if (cur) {
    d.indexAfter = index < 0 ? curIdx : std::min(index, size - 1);
  } else {
    d.indexAfter = (index < 0 || index > size) ? size : index;
  }
  Change c;
  c.kind = kChangeTrack;
  c.id = t.id;
  c.data = std::move(d);
  record(std::move(c));
}
void Editing::removeTrack(const sq_guid& id) {
  const TrackRec* cur = doc_.store_.track(id);
  if (!cur) return;
  TrackDelta d;
  d.before = *cur;
  d.indexBefore = doc_.store_.trackIndex(id);
  Change c;
  c.kind = kChangeTrack;
  c.id = id;
  c.data = std::move(d);
  record(std::move(c));
}
void Editing::putClip(const ClipRec& rec) {
  Delta<ClipRec> d;
  if (const ClipRec* cur = doc_.store_.clip(rec.id)) d.before = *cur;
  d.after = rec;
  Change c;
  c.kind = kChangeClip;
  c.id = rec.id;
  c.data = std::move(d);
  record(std::move(c));
}
void Editing::removeClip(const sq_guid& id) {
  const ClipRec* cur = doc_.store_.clip(id);
  if (!cur) return;
  Change c;
  c.kind = kChangeClip;
  c.id = id;
  c.data = Delta<ClipRec>{*cur, std::nullopt};
  record(std::move(c));
}
void Editing::putMarker(const MarkerRec& m) {
  Delta<MarkerRec> d;
  if (const MarkerRec* cur = doc_.store_.marker(m.id)) d.before = *cur;
  d.after = m;
  Change c;
  c.kind = kChangeMarker;
  c.id = m.id;
  c.data = std::move(d);
  record(std::move(c));
}
void Editing::removeMarker(const sq_guid& id) {
  const MarkerRec* cur = doc_.store_.marker(id);
  if (!cur) return;
  Change c;
  c.kind = kChangeMarker;
  c.id = id;
  c.data = Delta<MarkerRec>{*cur, std::nullopt};
  record(std::move(c));
}
void Editing::setJournal(const std::string& key, const std::optional<std::string>& value) {
  JournalDelta d;
  d.key = key;
  auto it = doc_.store_.journal.find(key);
  if (it != doc_.store_.journal.end()) d.before = it->second;
  d.after = value;
  if (d.before == d.after) return;
  Change c;
  c.kind = kChangeJournal;
  c.id = doc_.store_.props.id;
  c.data = std::move(d);
  record(std::move(c));
}

// ---- DocumentImpl -------------------------------------------------------------------------------------
DocumentImpl::DocumentImpl(IClassRegistry* registry) : registry_(registry) {
  for (auto& se : defaultSideEffects()) sideEffects_.push_back(se);
}

Ptr<DocumentImpl> DocumentImpl::create(int32_t kind, IClassRegistry* registry) {
  Ptr<DocumentImpl> d = Ptr<DocumentImpl>::adopt(new DocumentImpl(registry));
  Store& s = d->store_;
  s.props.kind = kind;
  s.props.id = newGuid();
  s.tempo.id = newGuid();
  if (kind == SQ_DOC_SONG || kind == SQ_DOC_PROJECT) {
    TrackRec m;
    m.id = newGuid();
    m.kind = SQ_TRACK_MASTER;
    m.name = "Master";
    s.tracks[m.id] = m;
    s.trackOrder.push_back(m.id);
  }
  s.invalidate();
  return d;
}

Ptr<DocumentImpl> DocumentImpl::fromStore(Store&& s, IClassRegistry* registry) {
  Ptr<DocumentImpl> d = Ptr<DocumentImpl>::adopt(new DocumentImpl(registry));
  d->store_ = std::move(s);
  d->store_.invalidate();
  return d;
}

DocumentImpl* DocumentImpl::from(IUnknown* obj) {
  if (!obj) return nullptr;
  Ptr<IDocumentImplAccess> a;
  if (SQ_FAILED(obj->queryInterface(&IDocumentImplAccess::iid, a.putVoid())) || !a) return nullptr;
  return a->impl();
}

std::string DocumentImpl::stateHash() const {
  std::string all;
  for (const auto& [name, xml] : serializeParts(store_)) {
    all += name;
    all += '\n';
    all += xml;
  }
  return sha256Hex(all);
}

void DocumentImpl::setJournal(const std::string& key, const std::string& value) {
  if (!levels_.empty()) {
    Editing ed(*this);
    ed.setJournal(key, value);
  } else {
    store_.journal[key] = value;
  }
}
void DocumentImpl::removeJournal(const std::string& key) {
  if (!levels_.empty()) {
    Editing ed(*this);
    ed.setJournal(key, std::nullopt);
  } else {
    store_.journal.erase(key);
  }
}
std::string DocumentImpl::journal(const std::string& key) const {
  auto it = store_.journal.find(key);
  return it == store_.journal.end() ? std::string() : it->second;
}

void DocumentImpl::markSaved() {
  const bool was = modified();
  savedSerial_ = undo_.topSerial();
  forceModified_ = false;
  emitModifiedIfToggled(was);
}
void DocumentImpl::markModified() {
  const bool was = modified();
  forceModified_ = true;
  emitModifiedIfToggled(was);
}

void DocumentImpl::notify(const char* topic, IAttributes* args) {
  std::vector<Ptr<IObserver>> obs = observers_;  // observers may (un)register during the callback
  for (auto& o : obs) o->onNotify(static_cast<IDocument*>(this), topic, args);
}

void DocumentImpl::emitModifiedIfToggled(bool wasModified) {
  const bool now = modified();
  if (now == wasModified) return;
  auto a = makeAttributes();
  a->setGuid("docId", &store_.props.id);
  a->setBool("modified", now ? SQ_TRUE : SQ_FALSE);
  notify("document.modified", a.get());
}

void DocumentImpl::emitChanged(const Transaction& t, const char* reason) {
  std::vector<sq_guid> nodes;
  std::unordered_set<sq_guid> seen;
  for (const Change& c : t.changes) {
    if (c.kind == kChangeJournal) continue;
    if (seen.insert(c.id).second) nodes.push_back(c.id);
  }
  auto a = makeAttributes();
  a->setGuid("docId", &store_.props.id);
  a->setString("reason", reason);
  a->setString("description", t.description.c_str());
  a->setBlob("nodes", nodes.data(), static_cast<uint32_t>(nodes.size() * sizeof(sq_guid)));
  notify("document.changed", a.get());
}

// ---- transactions ----
sq_result SQ_CALL DocumentImpl::beginTransaction(const char* description) {
  if (closed_) return SQ_E_CLOSED;
  if (inSideEffects_) return SQ_E_BUSY;
  if (levels_.empty()) {
    pending_ = Transaction{};
    pending_.description = description ? description : "";
  }
  levels_.push_back(pending_.changes.size());
  return SQ_OK;
}

sq_result SQ_CALL DocumentImpl::commitTransaction() {
  if (levels_.empty()) return SQ_E_FAIL;
  if (!floors_.empty() && levels_.size() <= floors_.back()) return SQ_E_ACCESS;  // not the action's level
  levels_.pop_back();
  if (levels_.empty()) finishOutermost();
  return SQ_OK;
}

sq_result SQ_CALL DocumentImpl::cancelTransaction() {
  if (levels_.empty()) return SQ_E_FAIL;
  if (!floors_.empty() && levels_.size() <= floors_.back()) return SQ_E_ACCESS;
  rollbackTo(levels_.back());
  levels_.pop_back();
  if (levels_.empty()) pending_ = Transaction{};
  return SQ_OK;
}

void DocumentImpl::rollbackTo(size_t mark) {
  while (pending_.changes.size() > mark) {
    store_.apply(pending_.changes.back(), false);
    pending_.changes.pop_back();
  }
}

void DocumentImpl::runSideEffects() {
  if (sideEffects_.empty()) return;
  inSideEffects_ = true;
  size_t from = 0;
  for (int pass = 0; pass < 8 && from < pending_.changes.size(); ++pass) {
    std::vector<Change> fresh(pending_.changes.begin() + static_cast<std::ptrdiff_t>(from), pending_.changes.end());
    from = pending_.changes.size();
    uint32_t mask = 0;
    for (const Change& c : fresh) mask |= c.kind;
    for (auto& se : sideEffects_) {
      if (!(se->triggers() & mask) || !se->enabled(store_)) continue;
      Editing ed(*this);
      ed.setTag(kTagSideEffect);
      try {
        se->propagate(ed, fresh);
      } catch (...) {
        sq_log_printf(SQ_LOG_ERROR, "doc", "side effect '%s' threw; its partial changes are kept", se->name());
      }
    }
  }
  inSideEffects_ = false;
}

void DocumentImpl::finishOutermost() {
  const bool wasModified = modified();
  runSideEffects();
  Transaction t = std::move(pending_);
  pending_ = Transaction{};
  if (!t.hasDocumentChanges()) return;  // empty or journal-only: no undo step
  if (t.mergePoisoned || t.mergeHits != t.actions) t.mergeKey.clear();
  t.serial = nextSerial_++;
  ++editSerial_;
  Transaction copyForNotify;
  copyForNotify.description = t.description;
  copyForNotify.changes = t.changes;
  undo_.record(std::move(t));
  emitChanged(copyForNotify, "commit");
  emitModifiedIfToggled(wasModified);
}

sq_result SQ_CALL DocumentImpl::execute(const sq_guid* actionClassId, IAttributes* params, IAttributes* result) {
  if (!actionClassId) return SQ_E_INVALIDARG;
  if (closed_) return SQ_E_CLOSED;
  if (inSideEffects_) return SQ_E_BUSY;
  Action* native = findAction(*actionClassId);
  Ptr<IAction> ext;
  std::string description;
  if (native) {
    description = native->name();
  } else {
    if (!registry_) return SQ_E_NOTFOUND;
    if (SQ_FAILED(registry_->createInstance(actionClassId, &IAction::iid, ext.putVoid())) || !ext) return SQ_E_NOTFOUND;
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    if (registry_->findClass(actionClassId, &ci) == SQ_OK && ci.name) description = ci.name;
  }
  const bool implicit = levels_.empty();
  if (implicit) beginTransaction(description.c_str());
  const size_t depth = levels_.size();
  const size_t mark = pending_.changes.size();
  levels_.push_back(mark);
  floors_.push_back(levels_.size());
  sq_result r = SQ_E_FAIL;
  try {
    if (native) {
      Editing ed(*this);
      r = native->perform(ed, params, result);
    } else {
      r = ext->perform(static_cast<IDocument*>(this), static_cast<IEditor*>(this), params, result);
    }
  } catch (...) {
    r = SQ_E_FAIL;
  }
  floors_.pop_back();
  if (levels_.size() != depth + 1) {
    // An external action left transactions open: treat as failure. (Closing below its floor is
    // refused by commit/cancel, so levels_ cannot have shrunk.)
    if (SQ_SUCCEEDED(r)) r = SQ_E_FAIL;
    if (levels_.size() < depth + 1) levels_.resize(depth + 1, mark);
  }
  if (SQ_SUCCEEDED(r) && native) ++pending_.actions;
  if (SQ_FAILED(r)) {
    rollbackTo(mark);
    levels_.resize(depth);
    if (implicit) {
      levels_.clear();
      pending_ = Transaction{};
    }
    return r;
  }
  levels_.resize(depth);
  if (implicit) commitTransaction();
  return r;
}

// ---- history ----
sq_result SQ_CALL DocumentImpl::undo() {
  if (closed_) return SQ_E_CLOSED;
  if (!levels_.empty() || inSideEffects_) return SQ_E_BUSY;
  if (!undo_.canUndo()) return SQ_S_FALSE;
  const bool wasModified = modified();
  Transaction t = undo_.popUndo();
  for (size_t i = t.changes.size(); i-- > 0;) store_.apply(t.changes[i], false);
  ++editSerial_;
  Transaction copy;
  copy.description = t.description;
  copy.changes = t.changes;
  undo_.pushRedo(std::move(t));
  emitChanged(copy, "undo");
  emitModifiedIfToggled(wasModified);
  return SQ_OK;
}

sq_result SQ_CALL DocumentImpl::redo() {
  if (closed_) return SQ_E_CLOSED;
  if (!levels_.empty() || inSideEffects_) return SQ_E_BUSY;
  if (!undo_.canRedo()) return SQ_S_FALSE;
  const bool wasModified = modified();
  Transaction t = undo_.popRedo();
  for (const Change& c : t.changes) store_.apply(c, true);
  ++editSerial_;
  Transaction copy;
  copy.description = t.description;
  copy.changes = t.changes;
  undo_.pushUndo(std::move(t));
  emitChanged(copy, "redo");
  emitModifiedIfToggled(wasModified);
  return SQ_OK;
}

sq_result SQ_CALL DocumentImpl::getUndoDescription(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) {
  if (index >= undo_.undoCount()) return SQ_E_NOTFOUND;
  return copyString(undo_.undoAt(index).description, buf, bufSize, needed);
}
sq_result SQ_CALL DocumentImpl::getRedoDescription(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) {
  if (index >= undo_.redoCount()) return SQ_E_NOTFOUND;
  return copyString(undo_.redoAt(index).description, buf, bufSize, needed);
}
sq_result SQ_CALL DocumentImpl::clearHistory() {
  if (!levels_.empty()) return SQ_E_BUSY;
  const bool was = modified();
  undo_.clear();
  savedSerial_ = 0;
  forceModified_ = was;
  return SQ_OK;
}

// ---- ISubject ----
sq_result SQ_CALL DocumentImpl::addObserver(IObserver* observer) {
  if (!observer) return SQ_E_INVALIDARG;
  for (auto& o : observers_)
    if (o.get() == observer) return SQ_S_FALSE;
  observers_.emplace_back(observer);
  return SQ_OK;
}
sq_result SQ_CALL DocumentImpl::removeObserver(IObserver* observer) {
  for (auto it = observers_.begin(); it != observers_.end(); ++it) {
    if (it->get() == observer) {
      observers_.erase(it);
      return SQ_OK;
    }
  }
  return SQ_E_NOTFOUND;
}

}  // namespace sq::edoc
