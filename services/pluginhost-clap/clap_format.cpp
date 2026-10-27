// pluginhost-clap: CLAP plugin format adapter (IPluginFormat, category "PluginFormat").
#include <clap/clap.h>

#include <cstring>
#include <string>
#include <vector>

#include "clap_instance.hpp"
#include "clap_library.hpp"
#include "sq/interfaces/pluginhost.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"

namespace {

void copyStr(char* dst, size_t cap, const std::string& s) {
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  std::memcpy(dst, s.data(), n);
  dst[n] = 0;
}

std::string envVar(const char* name) {
  sq::Buffer b;
  return sq_env_get(name, b.out()) == SQ_OK ? b.str() : std::string();
}

std::vector<std::string> searchPaths() {
  std::vector<std::string> out;
  // CLAP_PATH (platform path-list separator) comes first, as the CLAP spec recommends.
#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::string env = envVar("CLAP_PATH");
  size_t start = 0;
  while (start <= env.size() && !env.empty()) {
    size_t e = env.find(sep, start);
    std::string p = env.substr(start, e == std::string::npos ? std::string::npos : e - start);
    if (!p.empty()) out.push_back(p);
    if (e == std::string::npos) break;
    start = e + 1;
  }
#if defined(_WIN32)
  std::string common = envVar("COMMONPROGRAMFILES");
  if (!common.empty()) out.push_back(sq::joinPath(common, "CLAP"));
  std::string local = envVar("LOCALAPPDATA");
  if (!local.empty()) out.push_back(sq::joinPath(local, "Programs/Common/CLAP"));
#elif defined(__APPLE__)
  out.push_back("/Library/Audio/Plug-Ins/CLAP");
  std::string home = envVar("HOME");
  if (!home.empty()) out.push_back(sq::joinPath(home, "Library/Audio/Plug-Ins/CLAP"));
#else
  std::string home = envVar("HOME");
  if (!home.empty()) out.push_back(sq::joinPath(home, ".clap"));
  out.push_back("/usr/lib/clap");
#endif
  return out;
}

// Minimal host for the scanner: no extensions, requests ignored.
const void* CLAP_ABI scanGetExtension(const clap_host_t*, const char*) { return nullptr; }
void CLAP_ABI scanRequest(const clap_host_t*) {}

class ClapFormat final : public sq::Object<ClapFormat, sq::IPluginFormat> {
 public:
  sq_result SQ_CALL getFormatInfo(sq_plugin_format_info* out) override {
    if (!out || out->struct_size < sizeof(sq_plugin_format_info)) return SQ_E_INVALIDARG;
    copyStr(out->format, sizeof out->format, "clap");
    copyStr(out->display_name, sizeof out->display_name, "CLAP");
    copyStr(out->file_extension, sizeof out->file_extension, "clap");
    out->flags = 0;
    return SQ_OK;
  }
  uint32_t SQ_CALL getSearchPathCount() override { return static_cast<uint32_t>(searchPaths().size()); }
  sq_result SQ_CALL getSearchPath(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override {
    auto paths = searchPaths();
    if (index >= paths.size()) return SQ_E_NOTFOUND;
    uint32_t n = static_cast<uint32_t>(paths[index].size()) + 1;
    if (needed) *needed = n;
    if (!buf || bufSize < n) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, paths[index].c_str(), n);
    return SQ_OK;
  }

  sq_result SQ_CALL scanFile(const char* path, sq::IPluginScanSink* sink) override {
    if (!path || !sink) return SQ_E_INVALIDARG;
    std::string err;
    auto lib = sqclap::ClapLibrary::acquire(path, &err);
    if (!lib) {
      sq::logf(SQ_LOG_WARN, "scan %s: %s", path, err.c_str());
      return SQ_E_FAIL;
    }
    int64_t mtimeMs = 0;
    uint64_t size = 0;
    sq_fs_mtime(path, &mtimeMs);
    sq_fs_file_size(sqclap::resolveBinaryPath(path).c_str(), &size);
    clap_host_t host{};
    sqclap::fillHostInfo(host);
    host.get_extension = &scanGetExtension;
    host.request_restart = &scanRequest;
    host.request_process = &scanRequest;
    host.request_callback = &scanRequest;
    const clap_plugin_factory_t* f = lib->factory();
    uint32_t count = f->get_plugin_count(f);
    for (uint32_t i = 0; i < count; ++i) {
      const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
      if (!d || !d->id) continue;
      sq_plugin_desc desc{};
      desc.struct_size = sizeof desc;
      const clap_plugin_t* p = f->create_plugin(f, &host, d->id);
      bool inited = p && p->init(p);
      sqclap::describePlugin(d, inited ? p : nullptr, &desc);
      if (p) p->destroy(p);
      copyStr(desc.path, sizeof desc.path, path);
      desc.sub_index = i;
      desc.status = inited ? SQ_PLUGIN_STATUS_OK : SQ_PLUGIN_STATUS_INVALID;
      desc.file_mtime = mtimeMs / 1000;
      desc.file_size = static_cast<int64_t>(size);
      sink->onPlugin(&desc);
    }
    return SQ_OK;
  }

  sq_result SQ_CALL instantiate(const sq_plugin_desc* desc, sq::IPluginHostCallback* host, sq::IPluginInstance** out) override {
    if (!desc || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (std::strcmp(desc->format, "clap") != 0) return SQ_E_INVALIDARG;
    std::string err;
    auto lib = sqclap::ClapLibrary::acquire(desc->path, &err);
    if (!lib) {
      sq::logf(SQ_LOG_WARN, "instantiate %s: %s", desc->path, err.c_str());
      return SQ_E_NOTFOUND;
    }
    return sqclap::ClapInstance::create(lib, desc, host, out);
  }
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<ClapFormat>(sq::pluginhost::kClapFormatClassId, sq::pluginhost::kCategory, "CLAP Plugin Format"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'L', 'A', 'P'),
    .name = "pluginhost-clap",
    .description = "CLAP plugin host adapter",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
