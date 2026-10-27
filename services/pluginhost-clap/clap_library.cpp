#include "clap_library.hpp"

#include <map>
#include <mutex>

#include "sq/platform.hpp"

namespace sqclap {

namespace {
std::mutex& cacheMutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::weak_ptr<ClapLibrary>>& cache() {
  static std::map<std::string, std::weak_ptr<ClapLibrary>> c;
  return c;
}
std::string stem(const std::string& p) {
  size_t s = p.find_last_of("/\\");
  std::string n = s == std::string::npos ? p : p.substr(s + 1);
  size_t d = n.find_last_of('.');
  return d == std::string::npos ? n : n.substr(0, d);
}
}  // namespace

std::string resolveBinaryPath(const std::string& path) {
  if (!sq_fs_is_dir(path.c_str())) return path;
  // macOS bundle layout: <name>.clap/Contents/MacOS/<name>
  std::string p = sq::joinPath(sq::joinPath(sq::joinPath(path, "Contents"), "MacOS"), stem(path));
  return p;
}

ClapLibrary::~ClapLibrary() {
  if (entry_ && inited_) entry_->deinit();
  if (lib_) sq_dylib_close(lib_);
}

std::shared_ptr<ClapLibrary> ClapLibrary::acquire(const std::string& path, std::string* err) {
  std::lock_guard<std::mutex> lock(cacheMutex());
  auto& c = cache();
  // Drop expired entries while we are here.
  for (auto it = c.begin(); it != c.end();) it = it->second.expired() ? c.erase(it) : std::next(it);
  if (auto it = c.find(path); it != c.end())
    if (auto sp = it->second.lock()) return sp;

  std::shared_ptr<ClapLibrary> lib(new ClapLibrary());
  lib->path_ = path;
  char e[512] = {};
  std::string bin = resolveBinaryPath(path);
  if (sq_dylib_open(bin.c_str(), &lib->lib_, e, sizeof e) != SQ_OK) {
    if (err) *err = std::string("cannot load: ") + e;
    return nullptr;
  }
  lib->entry_ = static_cast<const clap_plugin_entry_t*>(sq_dylib_symbol(lib->lib_, "clap_entry"));
  if (!lib->entry_) {
    if (err) *err = "no clap_entry symbol";
    return nullptr;
  }
  if (!clap_version_is_compatible(lib->entry_->clap_version)) {
    if (err) *err = "incompatible CLAP version";
    return nullptr;
  }
  if (!lib->entry_->init || !lib->entry_->init(path.c_str())) {
    if (err) *err = "clap_entry.init failed";
    return nullptr;
  }
  lib->inited_ = true;
  lib->factory_ = static_cast<const clap_plugin_factory_t*>(lib->entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!lib->factory_) {
    if (err) *err = "no plugin factory";
    return nullptr;
  }
  c[path] = lib;
  return lib;
}

}  // namespace sqclap
