// Squizzy Studio app - GUID class registry (implements sq::IClassRegistry). Thread-safe.
// Category/name strings handed out through sq_class_info are interned and never freed, so the
// pointers stay valid for the life of the process (stronger than the documented contract).
#pragma once

#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "sq/sdk.hpp"

namespace sq::app {

class ClassRegistry final : public sq::StaticObject<ClassRegistry, sq::IClassRegistry> {
 public:
  struct Entry {
    sq_guid classId{};
    std::string category;
    std::string name;
    uint32_t flags = 0;
    uint32_t moduleFourcc = 0;
    uint32_t moduleId = 0;  // 0 = dynamically registered
    sq::Ptr<sq::IClassFactory> factory;
    const char* categoryInterned = "";  // stable copies handed out through sq_class_info
    const char* nameInterned = "";
  };

  // Registers every class served by a module factory. Duplicate class IDs are skipped and
  // reported in `conflicts`. Returns the number registered.
  uint32_t addModule(uint32_t moduleId, uint32_t fourcc, sq::IClassFactory* factory, std::vector<std::string>* conflicts);
  void removeModule(uint32_t moduleId);
  std::vector<Entry> snapshot() const;

  // ---- sq::IClassRegistry ----
  uint32_t SQ_CALL getClassCount() override;
  sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) override;
  sq_result SQ_CALL findClass(const sq_guid* classId, sq_class_info* out) override;
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override;
  sq_result SQ_CALL registerClass(const sq_class_info* info, sq::IClassFactory* factory) override;
  sq_result SQ_CALL unregisterClass(const sq_guid* classId) override;

 private:
  static void fill(const Entry& e, sq_class_info* out);
  void internLocked(Entry& e);  // caller holds m_ exclusively
  const Entry* findLocked(const sq_guid& id) const;

  mutable std::shared_mutex m_;
  std::vector<Entry> entries_;
  std::unordered_set<std::string> strings_;  // node-based: element addresses are stable
};

}  // namespace sq::app
