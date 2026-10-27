// pluginhost-clap: refcounted CLAP binary (one clap_entry init/deinit pair per loaded file).
#pragma once

#include <clap/clap.h>

#include <memory>
#include <string>

struct sq_dylib;

namespace sqclap {

class ClapLibrary {
 public:
  ~ClapLibrary();
  ClapLibrary(const ClapLibrary&) = delete;
  ClapLibrary& operator=(const ClapLibrary&) = delete;

  // Returns the shared library for `path` (loads + clap_entry.init on first use). Main thread.
  static std::shared_ptr<ClapLibrary> acquire(const std::string& path, std::string* err);

  const std::string& path() const { return path_; }
  const clap_plugin_entry_t* entry() const { return entry_; }
  const clap_plugin_factory_t* factory() const { return factory_; }

 private:
  ClapLibrary() = default;
  std::string path_;
  sq_dylib* lib_ = nullptr;
  const clap_plugin_entry_t* entry_ = nullptr;
  const clap_plugin_factory_t* factory_ = nullptr;
  bool inited_ = false;
};

// Resolves the binary inside a bundle (macOS .clap bundles); files are returned unchanged.
std::string resolveBinaryPath(const std::string& path);

}  // namespace sqclap
