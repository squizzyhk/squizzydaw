#include "sq/app/module_loader.hpp"

#include <algorithm>
#include <cstdio>

#include "sq/platform.hpp"

namespace sq::app {
namespace {

const char* kTag = "loader";

std::string abiStr(uint32_t v) {
  return std::to_string(SQ_ABI_GET_MAJOR(v)) + "." + std::to_string(SQ_ABI_GET_MINOR(v));
}

LoadResult fail(sq_result code, std::string msg) {
  LoadResult r;
  r.code = code;
  r.message = std::move(msg);
  return r;
}

}  // namespace

const char* ModuleLoader::fourccString(uint32_t f, char out[5]) {
  for (int i = 0; i < 4; ++i) {
    char c = static_cast<char>((f >> (24 - 8 * i)) & 0xFF);
    out[i] = (c >= 32 && c < 127) ? c : '?';
  }
  out[4] = '\0';
  return out;
}

ModuleLoader::ModuleLoader(HostContext& host, ClassRegistry& registry) : host_(host), registry_(registry) {}

ModuleLoader::~ModuleLoader() { unloadAll(true); }

LoadResult ModuleLoader::load(const std::string& path) {
  sq_dylib* lib = nullptr;
  char err[512] = {};
  sq_result r = sq_dylib_open(path.c_str(), &lib, err, sizeof err);
  if (r != SQ_OK) return fail(r, std::string("cannot load library: ") + err);

  auto reject = [&](sq_result code, std::string msg) {
    sq_dylib_close(lib);
    return fail(code, std::move(msg));
  };

  auto getFactory = reinterpret_cast<sq_GetClassFactoryFn>(sq_dylib_symbol(lib, SQ_EXPORT_GET_CLASS_FACTORY));
  auto moduleMain = reinterpret_cast<sq_ModuleMainFn>(sq_dylib_symbol(lib, SQ_EXPORT_MODULE_MAIN));
  if (!getFactory || !moduleMain) return reject(SQ_E_NOINTERFACE, "not a Squizzy module (missing GetClassFactory/ModuleMain export)");

  sq::Ptr<sq::IClassFactory> factory = sq::Ptr<sq::IClassFactory>::adopt(getFactory());
  if (!factory) return reject(SQ_E_FAIL, "GetClassFactory returned null");

  sq_module_info info{};
  info.struct_size = sizeof info;
  if (factory->getModuleInfo(&info) != SQ_OK || !info.name || !*info.name) {
    factory.reset();
    return reject(SQ_E_FAIL, "getModuleInfo failed");
  }
  ModuleRecord rec;
  rec.path = path;
  rec.name = info.name;
  rec.description = info.description ? info.description : "";
  rec.buildStamp = info.build_stamp ? info.build_stamp : "";
  rec.fourcc = info.fourcc;
  rec.version = info.version;
  rec.abiMin = info.abi_min;
  rec.abiMax = info.abi_max;
  rec.sdkAbi = info.sdk_abi;

  // ABI gate: the host's platform ABI must lie inside the module's declared range.
  uint32_t hostAbi = host_.getHostAbi();
  if (!sq_abi_in_range(hostAbi, info.abi_min, info.abi_max)) {
    factory.reset();
    return reject(SQ_E_ABI_MISMATCH, "module '" + rec.name + "' requires platform ABI " + abiStr(info.abi_min) + ".." +
                                         abiStr(info.abi_max) + ", host is " + abiStr(hostAbi));
  }
  for (const auto& m : mods_) {
    if (m.rec.name == rec.name || m.rec.fourcc == rec.fourcc) {
      factory.reset();
      return reject(SQ_E_EXISTS, "module '" + rec.name + "' (or its fourcc) is already loaded from " + m.rec.path);
    }
  }

  r = moduleMain(SQ_MODULE_INIT, &host_);
  if (r != SQ_OK) {
    factory.reset();
    return reject(r, "module '" + rec.name + "' ModuleMain(INIT) failed: " + sq_result_string(r));
  }

  rec.id = nextId_++;
  std::vector<std::string> conflicts;
  rec.classCount = registry_.addModule(rec.id, rec.fourcc, factory.get(), &conflicts);
  for (const auto& c : conflicts) sq_log_printf(SQ_LOG_WARN, kTag, "module '%s': duplicate class %s skipped", rec.name.c_str(), c.c_str());

  Loaded l;
  l.rec = rec;
  l.lib = lib;
  l.factory = std::move(factory);
  l.main = moduleMain;
  mods_.push_back(std::move(l));

  char fc[5];
  sq_log_printf(SQ_LOG_INFO, kTag, "loaded '%s' [%s] v%u.%u.%u abi %s..%s, %u class(es)", rec.name.c_str(), fourccString(rec.fourcc, fc),
                SQ_VERSION_GET_MAJOR(rec.version), SQ_VERSION_GET_MINOR(rec.version), SQ_VERSION_GET_PATCH(rec.version),
                abiStr(rec.abiMin).c_str(), abiStr(rec.abiMax).c_str(), rec.classCount);
  LoadResult ok;
  ok.code = SQ_OK;
  ok.moduleId = rec.id;
  return ok;
}

ModuleLoader::ScanResult ModuleLoader::scan(const std::string& dir) {
  ScanResult res;
  std::vector<std::string> files;
  std::string suffix = sq_dylib_suffix();
  struct Ctx {
    std::vector<std::string>* files;
    const std::string* suffix;
  } ctx{&files, &suffix};
  sq_fs_list_dir(
      dir.c_str(),
      [](void* user, const char* name, sq_bool isDir) -> sq_bool {
        auto* c = static_cast<Ctx*>(user);
        std::string n = name;
        if (!isDir && n.size() > c->suffix->size() && n.compare(n.size() - c->suffix->size(), c->suffix->size(), *c->suffix) == 0)
          c->files->push_back(n);
        return SQ_TRUE;
      },
      &ctx);
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    std::string path = sq::joinPath(dir, f);
    LoadResult r = load(path);
    if (r.ok()) {
      ++res.loaded;
    } else {
      ++res.rejected;
      sq_log_printf(SQ_LOG_WARN, kTag, "rejected %s: %s", path.c_str(), r.message.c_str());
    }
    res.results.emplace_back(path, r);
  }
  return res;
}

// References to the module's factory held by anyone other than the loader. A module (or the
// scripting service) may have handed its factory to IClassRegistry::registerClass; unmapping the
// DLL while that reference lives would leave a dangling vtable. addRef/release return the new
// count (SDK factories track it exactly).
static uint32_t foreignFactoryRefs(sq::IClassFactory* f) {
  uint32_t c = f->addRef();
  f->release();
  return c >= 2 ? c - 2 : 0;  // minus the probe and the loader's own reference
}

sq_result ModuleLoader::unloadAt(size_t index, bool force) {
  Loaded& m = mods_[index];
  // Phase 1: stop serving classes and let the module drop everything it registered with the
  // host in INIT (services, settings observers, dynamic classes, log sinks). Those objects live
  // in the module, so canUnload() can only become true after SHUTDOWN.
  registry_.removeModule(m.rec.id);
  m.main(SQ_MODULE_SHUTDOWN, &host_);
  bool busy = !m.factory->canUnload() || foreignFactoryRefs(m.factory.get()) > 0;

  if (busy && !force) {
    // Phase 2a: someone still holds objects of this module. Roll back: re-initialise and
    // re-register so the module keeps working, and report BUSY.
    sq_result r = m.main(SQ_MODULE_INIT, &host_);
    if (r == SQ_OK) {
      std::vector<std::string> conflicts;
      registry_.addModule(m.rec.id, m.rec.fourcc, m.factory.get(), &conflicts);
      return SQ_E_BUSY;
    }
    // Could not come back: drop the record but keep the image mapped (live objects point into it).
    sq_log_printf(SQ_LOG_ERROR, kTag, "module '%s' busy and re-INIT failed (%s): disabled, library left mapped", m.rec.name.c_str(),
                  sq_result_string(r));
    m.factory.detach();
    mods_.erase(mods_.begin() + static_cast<std::ptrdiff_t>(index));
    return SQ_E_BUSY;
  }

  // Phase 2b: unmap (or, when forced with live objects, keep the image mapped rather than crash later).
  if (busy) {
    sq_log_printf(SQ_LOG_WARN, kTag, "force-unloading '%s' with live objects: library left mapped", m.rec.name.c_str());
    m.factory.detach();  // the factory may still be referenced; never call into it again
  } else {
    m.factory.reset();
    sq_dylib_close(m.lib);
  }
  sq_log_printf(SQ_LOG_INFO, kTag, "unloaded '%s'", m.rec.name.c_str());
  mods_.erase(mods_.begin() + static_cast<std::ptrdiff_t>(index));
  return SQ_OK;
}

sq_result ModuleLoader::unload(const std::string& name, bool force) {
  for (size_t i = 0; i < mods_.size(); ++i)
    if (mods_[i].rec.name == name) return unloadAt(i, force);
  return SQ_E_NOTFOUND;
}

LoadResult ModuleLoader::reload(const std::string& name) {
  ModuleRecord rec;
  if (!find(name, &rec)) return fail(SQ_E_NOTFOUND, "module '" + name + "' is not loaded");
  sq_result r = unload(name, false);
  if (r != SQ_OK) return fail(r, "cannot unload '" + name + "': " + sq_result_string(r));
  return load(rec.path);
}

void ModuleLoader::unloadAll(bool force) {
  // Reverse load order so later modules (which may depend on earlier ones) go first.
  for (size_t i = mods_.size(); i-- > 0;) {
    std::string name = mods_[i].rec.name;
    if (unloadAt(i, force) != SQ_OK) sq_log_printf(SQ_LOG_WARN, kTag, "module '%s' busy, kept loaded", name.c_str());
  }
}

std::vector<ModuleRecord> ModuleLoader::modules() const {
  std::vector<ModuleRecord> v;
  for (const auto& m : mods_) v.push_back(m.rec);
  return v;
}

bool ModuleLoader::find(const std::string& name, ModuleRecord* out) const {
  for (const auto& m : mods_)
    if (m.rec.name == name) {
      if (out) *out = m.rec;
      return true;
    }
  return false;
}

}  // namespace sq::app
