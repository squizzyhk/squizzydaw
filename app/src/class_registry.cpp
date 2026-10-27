#include "sq/app/class_registry.hpp"

#include <mutex>

namespace sq::app {

const ClassRegistry::Entry* ClassRegistry::findLocked(const sq_guid& id) const {
  for (const auto& e : entries_)
    if (e.classId == id) return &e;
  return nullptr;
}

void ClassRegistry::internLocked(Entry& e) {
  e.categoryInterned = strings_.insert(e.category).first->c_str();
  e.nameInterned = strings_.insert(e.name).first->c_str();
}

void ClassRegistry::fill(const Entry& e, sq_class_info* out) {
  out->class_id = e.classId;
  out->category = e.categoryInterned;
  out->name = e.nameInterned;
  out->flags = e.flags;
  out->module_fourcc = e.moduleFourcc;
}

uint32_t ClassRegistry::addModule(uint32_t moduleId, uint32_t fourcc, sq::IClassFactory* factory, std::vector<std::string>* conflicts) {
  if (!factory) return 0;
  // Query the module before taking the lock: never call foreign code while holding it.
  std::vector<Entry> incoming;
  uint32_t n = factory->getClassCount();
  for (uint32_t i = 0; i < n; ++i) {
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    if (factory->getClassInfo(i, &ci) != SQ_OK) continue;
    Entry e;
    e.classId = ci.class_id;
    e.category = ci.category ? ci.category : "";
    e.name = ci.name ? ci.name : "";
    e.flags = ci.flags;
    e.moduleFourcc = fourcc;
    e.moduleId = moduleId;
    e.factory = sq::Ptr<sq::IClassFactory>(factory);
    incoming.push_back(std::move(e));
  }
  std::vector<Entry> rejected;  // their factory refs are released outside the lock
  uint32_t added = 0;
  {
    std::unique_lock<std::shared_mutex> lk(m_);
    for (auto& e : incoming) {
      if (sq_guid_is_null(&e.classId) || findLocked(e.classId)) {
        if (conflicts) conflicts->push_back(sq::toString(e.classId) + " (" + e.name + ")");
        rejected.push_back(std::move(e));
        continue;
      }
      internLocked(e);
      entries_.push_back(std::move(e));
      ++added;
    }
  }
  return added;
}

void ClassRegistry::removeModule(uint32_t moduleId) {
  std::vector<Entry> removed;  // factories released outside the lock
  {
    std::unique_lock<std::shared_mutex> lk(m_);
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->moduleId == moduleId) {
        removed.push_back(std::move(*it));
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

std::vector<ClassRegistry::Entry> ClassRegistry::snapshot() const {
  std::shared_lock<std::shared_mutex> lk(m_);
  return entries_;
}

uint32_t SQ_CALL ClassRegistry::getClassCount() {
  std::shared_lock<std::shared_mutex> lk(m_);
  return static_cast<uint32_t>(entries_.size());
}

sq_result SQ_CALL ClassRegistry::getClassInfo(uint32_t index, sq_class_info* out) {
  if (!out || out->struct_size < sizeof(sq_class_info)) return SQ_E_INVALIDARG;
  std::shared_lock<std::shared_mutex> lk(m_);
  if (index >= entries_.size()) return SQ_E_NOTFOUND;
  fill(entries_[index], out);
  return SQ_OK;
}

sq_result SQ_CALL ClassRegistry::findClass(const sq_guid* classId, sq_class_info* out) {
  if (!classId) return SQ_E_INVALIDARG;
  std::shared_lock<std::shared_mutex> lk(m_);
  const Entry* e = findLocked(*classId);
  if (!e) return SQ_E_NOTFOUND;
  if (out) {
    if (out->struct_size < sizeof(sq_class_info)) return SQ_E_INVALIDARG;
    fill(*e, out);
  }
  return SQ_OK;
}

sq_result SQ_CALL ClassRegistry::createInstance(const sq_guid* classId, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!classId || !riid) return SQ_E_INVALIDARG;
  sq::Ptr<sq::IClassFactory> f;
  {
    std::shared_lock<std::shared_mutex> lk(m_);
    const Entry* e = findLocked(*classId);
    if (!e) return SQ_E_NOTFOUND;
    f = e->factory;
  }
  // Called outside the lock so constructors may use the registry themselves.
  return f->createInstance(classId, riid, out);
}

sq_result SQ_CALL ClassRegistry::registerClass(const sq_class_info* info, sq::IClassFactory* factory) {
  if (!info || !factory || info->struct_size < sizeof(sq_class_info) || sq_guid_is_null(&info->class_id)) return SQ_E_INVALIDARG;
  Entry e;  // built (and, on failure, destroyed) outside the lock: addRef/release are foreign calls
  e.classId = info->class_id;
  e.category = info->category ? info->category : "";
  e.name = info->name ? info->name : "";
  e.flags = info->flags;
  e.factory = sq::Ptr<sq::IClassFactory>(factory);
  std::unique_lock<std::shared_mutex> lk(m_);
  if (findLocked(info->class_id)) {
    lk.unlock();
    return SQ_E_EXISTS;
  }
  internLocked(e);
  entries_.push_back(std::move(e));
  return SQ_OK;
}

sq_result SQ_CALL ClassRegistry::unregisterClass(const sq_guid* classId) {
  if (!classId) return SQ_E_INVALIDARG;
  Entry removed;
  {
    std::unique_lock<std::shared_mutex> lk(m_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
      if (it->classId == *classId) {
        if (it->moduleId != 0) return SQ_E_ACCESS;  // module classes go away with their module
        removed = std::move(*it);
        entries_.erase(it);
        return SQ_OK;
      }
  }
  return SQ_E_NOTFOUND;
}

}  // namespace sq::app
