// pluginscanner: full out-of-process scan of bin/testplugins (gain + a crasher + a hanger) through
// the real loader. The host process must survive, the crasher must be quarantined as CRASHED, the
// hanger as TIMEOUT, and the gain plugin must be instantiable through IPluginRegistry.
#include <cstring>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/pluginhost.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
std::string svc(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}
bool waitScan(sq::IPluginRegistry* r) {
  for (int i = 0; i < 1200; ++i) {
    if (!r->isScanning()) return true;
    sq_thread_sleep_ms(50);
  }
  return false;
}
struct Found {
  int ok = 0, crashed = 0, timeout = 0, blocked = 0;
  sq_plugin_desc gain{};
};
Found survey(sq::IPluginRegistry* r) {
  Found f;
  for (uint32_t i = 0; i < r->getPluginCount(); ++i) {
    sq_plugin_desc d{};
    d.struct_size = sizeof d;
    REQUIRE_OK(r->getPlugin(i, &d));
    if (d.status == SQ_PLUGIN_STATUS_OK) ++f.ok;
    if (d.status == SQ_PLUGIN_STATUS_CRASHED) {
      ++f.crashed;
      CHECK(std::strstr(d.path, "crash") != nullptr);
    }
    if (d.status == SQ_PLUGIN_STATUS_TIMEOUT) {
      ++f.timeout;
      CHECK(std::strstr(d.path, "hang") != nullptr);
    }
    if (d.status == SQ_PLUGIN_STATUS_BLOCKED) ++f.blocked;
    if (std::strcmp(d.id, "studio.squizzy.test.gain") == 0) f.gain = d;
  }
  return f;
}
}  // namespace

SQ_TEST("pluginscanner: crasher and hanger are quarantined, the host survives, gain instantiates") {
  std::string tmp = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-pscan-" + std::to_string(sq_process_id()));
  sq_fs_create_dirs(tmp.c_str());
  std::string manifest = sq::joinPath(tmp, "plugins.json");
  sq::app::Settings settings;
  settings.setString("Services.pluginscanner.Manifest", manifest.c_str());
  settings.setString("Services.pluginscanner.Paths", sq::joinPath(sq::specialDir(SQ_DIR_EXE), "testplugins").c_str());
  settings.setBool("Services.pluginscanner.UseDefaultPaths", false);
  settings.setInt("Services.pluginscanner.TimeoutMs", 3000);
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(svc("pluginhost-clap")).ok());
  REQUIRE(loader.load(svc("pluginscanner")).ok());
  {
    sq::Ptr<sq::IPluginRegistry> r;
    REQUIRE_OK(host.queryService(&sq::IPluginRegistry::iid, r.putVoid()));
    REQUIRE_OK(r->rescan(SQ_TRUE));
    CHECK_EQ(r->rescan(SQ_TRUE), SQ_E_BUSY);
    REQUIRE(waitScan(r.get()));
    Found f = survey(r.get());
    CHECK_EQ(f.ok, 1);
    CHECK_EQ(f.crashed, 1);
    CHECK_EQ(f.timeout, 1);
    REQUIRE(f.gain.status == SQ_PLUGIN_STATUS_OK);
    CHECK(sq_fs_exists(manifest.c_str()));

    // Instantiate through the registry and process one block.
    sq::Ptr<sq::IPluginInstance> inst;
    REQUIRE_OK(r->instantiate(&f.gain, nullptr, inst.put()));
    sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 64, 0};
    REQUIRE_OK(inst->activate(&setup));
    CHECK_EQ(inst->getLatencyFrames(), 64u);
    REQUIRE_OK(inst->deactivate());
    inst = nullptr;

    // Incremental rescan: nothing changed, so the quarantined files are not loaded again.
    uint64_t t0 = sq_time_monotonic_ns();
    REQUIRE_OK(r->rescan(SQ_FALSE));
    REQUIRE(waitScan(r.get()));
    CHECK((sq_time_monotonic_ns() - t0) / 1000000ull < 2500ull);  // no 3 s timeout again
    Found g = survey(r.get());
    CHECK_EQ(g.crashed, 1);
    CHECK_EQ(g.timeout, 1);

    // Blocklist persists and refuses instantiation.
    REQUIRE_OK(r->setBlocked("clap", "studio.squizzy.test.gain", SQ_TRUE));
    Found b = survey(r.get());
    CHECK_EQ(b.blocked, 1);
    CHECK_EQ(r->instantiate(&f.gain, nullptr, inst.put()), SQ_E_ACCESS);
  }
  CHECK(loader.unload("pluginscanner") == SQ_OK);
  // A fresh registry reads the manifest back, blocked entry included.
  REQUIRE(loader.load(svc("pluginscanner")).ok());
  {
    sq::Ptr<sq::IPluginRegistry> r;
    REQUIRE_OK(host.queryService(&sq::IPluginRegistry::iid, r.putVoid()));
    Found f = survey(r.get());
    CHECK_EQ(f.blocked, 1);
    CHECK_EQ(f.crashed, 1);
  }
  loader.unloadAll(true);
  host.clearServices();
  sq_fs_remove_all(tmp.c_str());
}
