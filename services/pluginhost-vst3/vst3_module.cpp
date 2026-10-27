#include "vst3_module.hpp"

#include <map>
#include <mutex>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace sqvst3 {

using namespace Steinberg;

namespace {

std::mutex& cacheMutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::weak_ptr<Module>>& cache() {
  static std::map<std::string, std::weak_ptr<Module>> c;
  return c;
}

std::string stem(const std::string& p) {
  std::string s = p;
  while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  size_t sl = s.find_last_of("/\\");
  std::string n = sl == std::string::npos ? s : s.substr(sl + 1);
  size_t d = n.find_last_of('.');
  return d == std::string::npos ? n : n.substr(0, d);
}

// Architecture folder names of the VST3 bundle layout ("Contents/<arch>/").
const char* const* archDirs() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(_M_ARM64EC)
  static const char* const k[] = {"arm64-win", "arm64x-win", "arm64ec-win", nullptr};
#elif defined(_M_X64)
  static const char* const k[] = {"x86_64-win", nullptr};
#else
  static const char* const k[] = {"x86-win", nullptr};
#endif
#elif defined(__APPLE__)
  static const char* const k[] = {"MacOS", nullptr};
#else
#if defined(__aarch64__)
  static const char* const k[] = {"aarch64-linux", nullptr};
#elif defined(__x86_64__)
  static const char* const k[] = {"x86_64-linux", nullptr};
#else
  static const char* const k[] = {"i386-linux", nullptr};
#endif
#endif
  return k;
}

// Module entry/exit signatures (cdecl, no PLUGIN_API, as defined by the VST3 module format).
using InitDllProc = bool (*)();
using ExitDllProc = bool (*)();
using ModuleEntryProc = bool (*)(void*);
using ModuleExitProc = bool (*)();

}  // namespace

std::string resolveBinaryPath(const std::string& path) {
  if (!sq_fs_is_dir(path.c_str())) return path;
  std::string contents = sq::joinPath(path, "Contents");
  std::string name = stem(path);
#if defined(_WIN32)
  const char* ext = ".vst3";
#elif defined(__APPLE__)
  const char* ext = "";
#else
  const char* ext = ".so";
#endif
  for (const char* const* a = archDirs(); *a; ++a) {
    std::string bin = sq::joinPath(sq::joinPath(contents, *a), name + ext);
    if (sq_fs_exists(bin.c_str())) return bin;
  }
  return sq::joinPath(sq::joinPath(contents, archDirs()[0]), name + ext);
}

std::string cidToString(const TUID cid) {
  char buf[64] = {};
  FUID::fromTUID(cid).toString(buf);
  return buf;
}

bool cidFromString(const char* s, TUID out) {
  if (!s || std::strlen(s) != 32) return false;
  FUID f;
  if (!f.fromString(s)) return false;
  f.toTUID(out);
  return true;
}

std::string Module::factoryVendor() const {
  PFactoryInfo fi{};
  if (factory_ && factory_->getFactoryInfo(&fi) == kResultOk) return fieldStr(fi.vendor);
  return {};
}

Module::~Module() {
  // Release the factory before the module exit call, then unload the binary.
  factory3_ = nullptr;
  factory2_ = nullptr;
  factory_ = nullptr;
  if (lib_ && entered_) {
#if defined(_WIN32)
    if (auto ex = reinterpret_cast<ExitDllProc>(sq_dylib_symbol(lib_, "ExitDll"))) ex();
#elif defined(__APPLE__)
    if (auto ex = reinterpret_cast<ModuleExitProc>(sq_dylib_symbol(lib_, "bundleExit"))) ex();
#else
    if (auto ex = reinterpret_cast<ModuleExitProc>(sq_dylib_symbol(lib_, "ModuleExit"))) ex();
#endif
  }
#if defined(__APPLE__)
  if (osHandle_) CFRelease(static_cast<CFBundleRef>(osHandle_));
#elif !defined(_WIN32)
  if (osHandle_) dlclose(osHandle_);
#endif
  if (lib_) sq_dylib_close(lib_);
  hostApp_ = nullptr;
}

std::shared_ptr<Module> Module::acquire(const std::string& path, std::string* err) {
  std::lock_guard<std::mutex> lock(cacheMutex());
  auto& c = cache();
  for (auto it = c.begin(); it != c.end();) it = it->second.expired() ? c.erase(it) : std::next(it);
  if (auto it = c.find(path); it != c.end())
    if (auto sp = it->second.lock()) return sp;

  auto fail = [err](const std::string& why) -> std::shared_ptr<Module> {
    if (err) *err = why;
    return nullptr;
  };
  std::shared_ptr<Module> m(new (std::nothrow) Module());
  if (!m) return fail("out of memory");
  m->path_ = path;
  m->binary_ = resolveBinaryPath(path);
  if (!sq_fs_exists(m->binary_.c_str())) return fail("no module binary at " + m->binary_);
  char e[512] = {};
  if (sq_dylib_open(m->binary_.c_str(), &m->lib_, e, sizeof e) != SQ_OK) return fail(std::string("cannot load: ") + e);

  // Module entry (optional on Windows, mandatory on Linux/macOS).
#if defined(_WIN32)
  if (auto init = reinterpret_cast<InitDllProc>(sq_dylib_symbol(m->lib_, "InitDll"))) {
    if (!init()) return fail("InitDll failed");
  }
  m->entered_ = true;
#elif defined(__APPLE__)
  {
    auto entry = reinterpret_cast<ModuleEntryProc>(sq_dylib_symbol(m->lib_, "bundleEntry"));
    if (!entry) return fail("no bundleEntry symbol");
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), true);
    CFBundleRef bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
    if (url) CFRelease(url);
    m->osHandle_ = bundle;
    if (!entry(bundle)) return fail("bundleEntry failed");
    m->entered_ = true;
  }
#else
  {
    auto entry = reinterpret_cast<ModuleEntryProc>(sq_dylib_symbol(m->lib_, "ModuleEntry"));
    if (!entry) return fail("no ModuleEntry symbol");
    m->osHandle_ = dlopen(m->binary_.c_str(), RTLD_LAZY | RTLD_NOLOAD);  // same image, +1 ref
    if (!entry(m->osHandle_)) return fail("ModuleEntry failed");
    m->entered_ = true;
  }
#endif

  auto getFactory = reinterpret_cast<GetFactoryProc>(sq_dylib_symbol(m->lib_, "GetPluginFactory"));
  if (!getFactory) return fail("no GetPluginFactory symbol");
  IPluginFactory* f = getFactory();
  if (!f) return fail("GetPluginFactory returned null");
  m->factory_ = owned(f);
  m->factory2_ = queryAs<IPluginFactory2>(f);
  m->factory3_ = queryAs<IPluginFactory3>(f);
  m->hostApp_ = owned(new HostApplication());
  if (m->factory3_) m->factory3_->setHostContext(static_cast<V::IHostApplication*>(m->hostApp_.get()));
  c[path] = m;
  return m;
}

}  // namespace sqvst3
