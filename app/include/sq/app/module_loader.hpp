// Squizzy Studio app - module loader: scan, ABI gate, load/unload/reload at runtime.
// Not thread-safe: call from the main thread only (see docs/CONTRACTS.md).
#pragma once

#include <string>
#include <vector>

#include "sq/app/host_context.hpp"

struct sq_dylib;

namespace sq::app {

struct ModuleRecord {
  uint32_t id = 0;
  std::string path;
  std::string name;
  std::string description;
  std::string buildStamp;
  uint32_t fourcc = 0;
  uint32_t version = 0;
  uint32_t abiMin = 0, abiMax = 0, sdkAbi = 0;
  uint32_t classCount = 0;
};

struct LoadResult {
  sq_result code = SQ_E_FAIL;
  std::string message;  // human readable reason on failure
  uint32_t moduleId = 0;
  bool ok() const { return code == SQ_OK; }
};

class ModuleLoader {
 public:
  ModuleLoader(HostContext& host, ClassRegistry& registry);
  ~ModuleLoader();
  ModuleLoader(const ModuleLoader&) = delete;
  ModuleLoader& operator=(const ModuleLoader&) = delete;

  // Loads one module file. Rejections (not a module, ABI out of range, duplicate, init failure)
  // leave no trace: the library is closed again.
  LoadResult load(const std::string& path);

  struct ScanResult {
    int loaded = 0;
    int rejected = 0;
    std::vector<std::pair<std::string, LoadResult>> results;  // path -> result
  };
  // Loads every <dir>/*<sq_dylib_suffix()> in name order.
  ScanResult scan(const std::string& dir);

  // Unregisters the module's classes, calls ModuleMain(SHUTDOWN) (where the module drops its
  // host registrations), then unmaps it. If objects of the module are still alive afterwards
  // (or its factory is still referenced) the unload is rolled back: ModuleMain(INIT) runs again,
  // classes are re-registered and SQ_E_BUSY is returned. With force the image is left mapped
  // instead. SQ_E_NOTFOUND if unknown.
  sq_result unload(const std::string& name, bool force = false);
  // Unload + load the same file again (picks up a rebuilt DLL).
  LoadResult reload(const std::string& name);
  void unloadAll(bool force);

  std::vector<ModuleRecord> modules() const;
  bool find(const std::string& name, ModuleRecord* out = nullptr) const;
  size_t count() const { return mods_.size(); }

  static const char* fourccString(uint32_t fourcc, char out[5]);

 private:
  struct Loaded {
    ModuleRecord rec;
    sq_dylib* lib = nullptr;
    sq::Ptr<sq::IClassFactory> factory;
    sq_ModuleMainFn main = nullptr;
  };
  sq_result unloadAt(size_t index, bool force);

  HostContext& host_;
  ClassRegistry& registry_;
  std::vector<Loaded> mods_;
  uint32_t nextId_ = 1;
};

}  // namespace sq::app
