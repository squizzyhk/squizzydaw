// pluginhost-vst3: VST3 plugin format adapter (IPluginFormat, category "PluginFormat").
#include <cstring>
#include <string>
#include <vector>

#include "sq/interfaces/pluginhost.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"
#include "vst3_instance.hpp"
#include "vst3_module.hpp"

namespace {

using sqvst3::copyStr;

std::string envVar(const char* name) {
  sq::Buffer b;
  return sq_env_get(name, b.out()) == SQ_OK ? b.str() : std::string();
}

// Standard VST3 install locations for this OS.
std::vector<std::string> searchPaths() {
  std::vector<std::string> out;
#if defined(_WIN32)
  std::string common = envVar("COMMONPROGRAMFILES");
  if (!common.empty()) out.push_back(sq::joinPath(common, "VST3"));
  std::string local = envVar("LOCALAPPDATA");
  if (!local.empty()) out.push_back(sq::joinPath(local, "Programs/Common/VST3"));
#elif defined(__APPLE__)
  std::string home = envVar("HOME");
  if (!home.empty()) out.push_back(sq::joinPath(home, "Library/Audio/Plug-Ins/VST3"));
  out.push_back("/Library/Audio/Plug-Ins/VST3");
#else
  std::string home = envVar("HOME");
  if (!home.empty()) out.push_back(sq::joinPath(home, ".vst3"));
  out.push_back("/usr/lib/vst3");
  out.push_back("/usr/local/lib/vst3");
#endif
  return out;
}

class Vst3Format final : public sq::Object<Vst3Format, sq::IPluginFormat> {
 public:
  sq_result SQ_CALL getFormatInfo(sq_plugin_format_info* out) override {
    if (!out || out->struct_size < sizeof(sq_plugin_format_info)) return SQ_E_INVALIDARG;
    copyStr(out->format, sizeof out->format, "vst3");
    copyStr(out->display_name, sizeof out->display_name, "VST3");
    copyStr(out->file_extension, sizeof out->file_extension, "vst3");
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
    try {
      return scan(path, sink);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
  }

  sq_result SQ_CALL instantiate(const sq_plugin_desc* desc, sq::IPluginHostCallback* host, sq::IPluginInstance** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!desc || desc->struct_size < sizeof(sq_plugin_desc)) return SQ_E_INVALIDARG;
    // The desc may come from a manifest or a document: every string field used must be
    // NUL-terminated inside its array (never read past it).
    auto terminated = [](const char* f, size_t n) { return std::memchr(f, 0, n) != nullptr; };
    if (!terminated(desc->format, sizeof desc->format) || !terminated(desc->id, sizeof desc->id) ||
        !terminated(desc->path, sizeof desc->path) || !terminated(desc->name, sizeof desc->name) ||
        !terminated(desc->vendor, sizeof desc->vendor) || !terminated(desc->version, sizeof desc->version) ||
        !terminated(desc->category, sizeof desc->category))
      return SQ_E_INVALIDARG;
    if (std::strcmp(desc->format, "vst3") != 0 || !desc->path[0]) return SQ_E_INVALIDARG;
    try {
      std::string err;
      auto mod = sqvst3::Module::acquire(desc->path, &err);
      if (!mod) {
        sq::logf(SQ_LOG_WARN, "instantiate %s: %s", desc->path, err.c_str());
        return SQ_E_NOTFOUND;
      }
      return sqvst3::Vst3Instance::create(mod, desc, host, out);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
  }

 private:
  sq_result scan(const char* path, sq::IPluginScanSink* sink) {
    std::string err;
    auto mod = sqvst3::Module::acquire(path, &err);
    if (!mod) {
      sq::logf(SQ_LOG_WARN, "scan %s: %s", path, err.c_str());
      return SQ_E_FAIL;
    }
    int64_t mtimeMs = 0;
    uint64_t size = 0;
    sq_fs_mtime(path, &mtimeMs);
    sq_fs_file_size(mod->binaryPath().c_str(), &size);
    Steinberg::int32 count = mod->factory()->countClasses();
    for (Steinberg::int32 i = 0; i < count; ++i) {
      sq_plugin_desc desc{};
      desc.struct_size = sizeof desc;
      if (sqvst3::describeClass(*mod, i, &desc) != SQ_OK) continue;  // not an audio module / unreadable
      copyStr(desc.path, sizeof desc.path, path);
      desc.sub_index = static_cast<uint32_t>(i);
      desc.file_mtime = mtimeMs / 1000;
      desc.file_size = static_cast<int64_t>(size);
      sink->onPlugin(&desc);
    }
    return SQ_OK;
  }
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Vst3Format>(sq::pluginhost::kVst3FormatClassId, sq::pluginhost::kCategory, "VST3 Plugin Format"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('V', 'S', 'T', '3'),
    .name = "pluginhost-vst3",
    .description = "VST3 plugin host adapter",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
