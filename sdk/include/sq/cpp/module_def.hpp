// Squizzy Studio SDK - C++ helpers to declare a module (class table + the two exports).
//
//   #include <sq/sdk.hpp>
//   class Hello final : public sq::Object<Hello, IHello> { ... };
//
//   static constexpr sq::ClassEntry kClasses[] = {
//     sq::classEntry<Hello>("{GUID}", "Service", "Hello Service"),
//   };
//   static constexpr sq::ModuleDesc kModule{
//     .fourcc = SQ_FOURCC('H','E','L','O'),
//     .name = "hello",
//     .description = "says hello",
//     .classes = kClasses,
//     .onInit = nullptr,       // optional: sq_result (*)(sq::IHostContext*)
//     .onShutdown = nullptr,   // optional: void (*)()
//   };
//   SQ_MODULE_EXPORTS(kModule)
//
// Inside the module, sq::host() returns the host context (valid between init and shutdown).
#pragma once

#include <cstdarg>
#include <cstdio>
#include <new>
#include <span>

#include "sq/cpp/object.hpp"
#include "sq/cpp/ptr.hpp"

#ifndef SQ_MODULE_VERSION
#define SQ_MODULE_VERSION SQ_VERSION
#endif

namespace sq {

using CreateFn = sq_result (*)(const sq_guid* riid, void** out);

struct ClassEntry {
  sq_guid classId;
  const char* category;
  const char* name;
  uint32_t flags;
  CreateFn create;
};

template <class T>
sq_result createObject(const sq_guid* riid, void** out) noexcept {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  T* obj = nullptr;
  try {
    obj = new T();
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
  sq_result r = obj->queryInterface(riid, out);
  obj->release();
  return r;
}

template <class T, std::size_t N>
constexpr ClassEntry classEntry(const char (&classId)[N], const char* category, const char* name, uint32_t flags = 0) {
  return ClassEntry{guid(classId), category, name, flags, &createObject<T>};
}
template <class T>
constexpr ClassEntry classEntry(const sq_guid& classId, const char* category, const char* name, uint32_t flags = 0) {
  return ClassEntry{classId, category, name, flags, &createObject<T>};
}

struct ModuleDesc {
  uint32_t fourcc = 0;
  const char* name = "";
  const char* description = "";
  std::span<const ClassEntry> classes{};
  sq_result (*onInit)(IHostContext* host) = nullptr;
  void (*onShutdown)() = nullptr;
  uint32_t version = SQ_MODULE_VERSION;
  uint32_t abiMin = SQ_ABI_DEFAULT_MIN;
  uint32_t abiMax = SQ_ABI_DEFAULT_MAX;
};

namespace detail {

inline IHostContext*& hostSlot() noexcept {
  static IHostContext* h = nullptr;
  return h;
}
inline const char*& moduleNameSlot() noexcept {
  static const char* n = "module";
  return n;
}

class ModuleFactory final : public IClassFactory {
 public:
  explicit constexpr ModuleFactory(const ModuleDesc& d) noexcept : d_(d) {}

  sq_result SQ_CALL queryInterface(const sq_guid* riid, void** out) override {
    if (!out || !riid) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (*riid == IClassFactory::iid || *riid == IUnknown::iid) {
      addRef();
      *out = static_cast<IClassFactory*>(this);
      return SQ_OK;
    }
    return SQ_E_NOINTERFACE;
  }
  // Singleton with static lifetime: never deleted, but the count is exact. The loader uses it to
  // refuse unloading while anyone besides itself still holds the factory (e.g. registerClass).
  uint32_t SQ_CALL addRef() override { return refs_.fetch_add(1) + 1; }
  uint32_t SQ_CALL release() override { return refs_.fetch_sub(1) - 1; }

  sq_result SQ_CALL getModuleInfo(sq_module_info* out) override {
    if (!out || out->struct_size < sizeof(sq_module_info)) return SQ_E_INVALIDARG;
    out->fourcc = d_.fourcc;
    out->name = d_.name;
    out->version = d_.version;
    out->abi_min = d_.abiMin;
    out->abi_max = d_.abiMax;
    out->sdk_abi = SQ_PLATFORM_ABI;
    out->build_stamp = SQ_BUILD_STAMP;
    out->description = d_.description ? d_.description : "";
    return SQ_OK;
  }
  uint32_t SQ_CALL getClassCount() override { return static_cast<uint32_t>(d_.classes.size()); }
  sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) override {
    if (!out || out->struct_size < sizeof(sq_class_info)) return SQ_E_INVALIDARG;
    if (index >= d_.classes.size()) return SQ_E_NOTFOUND;
    const ClassEntry& e = d_.classes[index];
    out->class_id = e.classId;
    out->category = e.category;
    out->name = e.name;
    out->flags = e.flags;
    out->module_fourcc = d_.fourcc;
    return SQ_OK;
  }
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!classId || !riid) return SQ_E_INVALIDARG;
    for (const ClassEntry& e : d_.classes)
      if (e.classId == *classId) return e.create(riid, out);
    return SQ_E_NOTFOUND;
  }
  sq_bool SQ_CALL canUnload() override { return liveObjects().load() == 0 ? SQ_TRUE : SQ_FALSE; }

 private:
  const ModuleDesc& d_;
  std::atomic<int32_t> refs_{0};
};

inline sq_result moduleMain(const ModuleDesc& d, uint32_t reason, IHostContext* host) noexcept {
  try {
    if (reason == SQ_MODULE_INIT) {
      if (!host) return SQ_E_INVALIDARG;
      if (!sq_abi_in_range(host->getHostAbi(), d.abiMin, d.abiMax)) return SQ_E_ABI_MISMATCH;
      hostSlot() = host;
      moduleNameSlot() = d.name;
      if (d.onInit) {
        sq_result r = d.onInit(host);
        if (SQ_FAILED(r)) {
          hostSlot() = nullptr;
          return r;
        }
      }
      return SQ_OK;
    }
    if (reason == SQ_MODULE_SHUTDOWN) {
      if (d.onShutdown) d.onShutdown();
      hostSlot() = nullptr;
      return SQ_OK;
    }
    return SQ_E_INVALIDARG;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

}  // namespace detail

// Host context of the current module; nullptr outside INIT..SHUTDOWN.
inline IHostContext* host() noexcept { return detail::hostSlot(); }

// printf-style logging through the host (tag = module name). No-op when not initialised.
// Not realtime safe.
inline void logf(int32_t level, const char* fmt, ...) noexcept {
  IHostContext* h = host();
  if (!h) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  h->log(level, detail::moduleNameSlot(), buf);
}

}  // namespace sq

// Defines the module's two exports. `desc` must be a namespace-scope sq::ModuleDesc.
#define SQ_MODULE_EXPORTS(desc)                                                                \
  extern "C" SQ_DLL_EXPORT sq_IClassFactory* SQ_CALL GetClassFactory(void) {                   \
    static ::sq::detail::ModuleFactory sq_factory_instance_(desc);                             \
    sq_factory_instance_.addRef();                                                             \
    return &sq_factory_instance_;                                                              \
  }                                                                                            \
  extern "C" SQ_DLL_EXPORT sq_result SQ_CALL ModuleMain(uint32_t reason, sq_IHostContext* host) { \
    return ::sq::detail::moduleMain(desc, reason, host);                                       \
  }
