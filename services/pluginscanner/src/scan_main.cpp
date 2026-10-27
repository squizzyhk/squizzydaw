// sq-pluginscan: the out-of-process plugin scanner (one plugin file per process). See manifest.hpp
// for the protocol. A plugin that crashes or hangs here takes only this process down; the
// pluginscanner service records it as CRASHED / TIMEOUT and never loads it in the host.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "manifest.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/interfaces/pluginhost.h"

namespace {

class Sink final : public sq::Object<Sink, sq::IPluginScanSink> {
 public:
  std::vector<sq_plugin_desc> found;
  void SQ_CALL onPlugin(const sq_plugin_desc* d) override {
    if (d && found.size() < 4096) found.push_back(*d);
  }
};

int usage() {
  std::fprintf(stderr, "usage: sq-pluginscan --module <format module> --class <format class id> --out <result.json> <plugin file>\n");
  return 64;
}

}  // namespace

int main(int argc, char** argv) {
  std::string module, classId, out, file;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    if (a == "--module") {
      if (!next(module)) return usage();
    } else if (a == "--class") {
      if (!next(classId)) return usage();
    } else if (a == "--out") {
      if (!next(out)) return usage();
    } else if (!a.empty() && a[0] != '-' && file.empty()) {
      file = a;
    } else {
      return usage();
    }
  }
  sq_guid cls{};
  if (module.empty() || out.empty() || file.empty() || !sq::parseGuid(classId, cls)) return usage();
  sq_log_set_level(SQ_LOG_WARN);

  int rc = 0;
  {
    sq::app::Settings settings;
    sq::app::ClassRegistry reg;
    sq::app::HostContext host(settings, reg);
    sq::app::ModuleLoader loader(host, reg);
    auto lr = loader.load(module);
    if (!lr.ok()) {
      std::fprintf(stderr, "sq-pluginscan: cannot load %s: %s\n", module.c_str(), lr.message.c_str());
      return 2;
    }
    {
      auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, cls);
      if (!fmt) {
        std::fprintf(stderr, "sq-pluginscan: no IPluginFormat %s\n", classId.c_str());
        rc = 3;
      } else {
        auto sink = sq::Ptr<Sink>::adopt(new Sink());
        sq_result r = fmt->scanFile(file.c_str(), sink.get());
        if (SQ_FAILED(r)) {
          std::fprintf(stderr, "sq-pluginscan: scan of %s failed: %s\n", file.c_str(), sq_result_string(r));
          rc = 4;
        } else if (sq::writeFile(out, sq::pscan::writeManifest(sink->found, {})) != SQ_OK) {
          std::fprintf(stderr, "sq-pluginscan: cannot write %s\n", out.c_str());
          rc = 5;
        }
      }
    }
    loader.unloadAll(true);
    host.clearServices();
  }
  return rc;
}
