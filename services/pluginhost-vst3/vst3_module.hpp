// pluginhost-vst3: a loaded VST3 module (binary + entry/exit + plugin factory), shared per path.
#pragma once

#include <pluginterfaces/base/ipluginbase.h>

#include <memory>
#include <string>

#include "vst3_common.hpp"
#include "vst3_hostobjects.hpp"

namespace sqvst3 {

// Binary inside a .vst3 bundle directory for this OS/arch, or `path` itself for a single-file
// module (the legacy Windows layout).
std::string resolveBinaryPath(const std::string& path);

class Module {
 public:
  // Loads (or returns the cached, still-alive) module for `path`. Main thread.
  static std::shared_ptr<Module> acquire(const std::string& path, std::string* err);
  ~Module();

  Steinberg::IPluginFactory* factory() const { return factory_; }
  Steinberg::IPluginFactory2* factory2() const { return factory2_; }
  Steinberg::IPluginFactory3* factory3() const { return factory3_; }
  HostApplication* hostApp() const { return hostApp_; }
  const std::string& path() const { return path_; }
  const std::string& binaryPath() const { return binary_; }
  std::string factoryVendor() const;

 private:
  Module() = default;
  std::string path_, binary_;
  sq_dylib* lib_ = nullptr;
  void* osHandle_ = nullptr;  // Linux: dlopen handle for ModuleEntry; macOS: CFBundleRef
  bool entered_ = false;
  IPtr<Steinberg::IPluginFactory> factory_;
  IPtr<Steinberg::IPluginFactory2> factory2_;
  IPtr<Steinberg::IPluginFactory3> factory3_;
  IPtr<HostApplication> hostApp_;
};

// Class id <-> manifest string (32 hex digits, the canonical VST3 FUID string form).
std::string cidToString(const TUID cid);
bool cidFromString(const char* s, TUID out);

}  // namespace sqvst3
