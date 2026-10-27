// ABI version gate: modules outside the host's platform ABI are rejected and leave no trace.
#include "sq/app/module_loader.hpp"
#include "sqtest.hpp"
#include "test_util.hpp"

SQ_TEST("abi: range predicate") {
  uint32_t host = SQ_ABI_MAKE(1, 3);
  CHECK(sq_abi_in_range(host, SQ_ABI_MAKE(1, 0), SQ_ABI_MAKE(1, 0xFFFF)));
  CHECK(sq_abi_in_range(host, SQ_ABI_MAKE(1, 3), SQ_ABI_MAKE(1, 3)));
  CHECK(!sq_abi_in_range(host, SQ_ABI_MAKE(1, 4), SQ_ABI_MAKE(1, 0xFFFF)));  // module needs newer minor
  CHECK(!sq_abi_in_range(host, SQ_ABI_MAKE(2, 0), SQ_ABI_MAKE(2, 0xFFFF)));  // future major
  CHECK(!sq_abi_in_range(host, SQ_ABI_MAKE(0, 0), SQ_ABI_MAKE(0, 0xFFFF)));  // old major
  CHECK(sq_abi_in_range(SQ_PLATFORM_ABI, SQ_ABI_DEFAULT_MIN, SQ_ABI_DEFAULT_MAX));
  CHECK_EQ(SQ_ABI_GET_MAJOR(SQ_PLATFORM_ABI), uint32_t(SQ_PLATFORM_ABI_MAJOR));
  CHECK_EQ(sq_platform_abi_version(), uint32_t(SQ_PLATFORM_ABI));
}

SQ_TEST("abi: loader rejects module requiring a future ABI before INIT") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string path = sqtest::modulePath(sqtest::testModulesDir(), "badabi");
  REQUIRE(sq_fs_exists(path.c_str()));
  auto r = loader.load(path);
  CHECK_EQ(r.code, SQ_E_ABI_MISMATCH);
  CHECK(r.message.find("requires platform ABI") != std::string::npos);
  CHECK_EQ(loader.count(), size_t(0));
  CHECK_EQ(reg.getClassCount(), 0u);
  CHECK(!settings.has("Test.BadAbi.Initialized"));  // ModuleMain(INIT) never ran
  CHECK(!sq_dylib_is_loaded(path.c_str()));         // library closed again
}

SQ_TEST("abi: module-side handshake refuses an incompatible host") {
  // Even if a loader skipped the range check, ModuleMain(INIT) re-checks the host ABI.
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);  // current ABI: outside badabi's range
  sq_dylib* lib = nullptr;
  std::string path = sqtest::modulePath(sqtest::testModulesDir(), "badabi");
  REQUIRE_OK(sq_dylib_open(path.c_str(), &lib, nullptr, 0));
  auto main = reinterpret_cast<sq_ModuleMainFn>(sq_dylib_symbol(lib, SQ_EXPORT_MODULE_MAIN));
  REQUIRE(main != nullptr);
  CHECK_EQ(main(SQ_MODULE_INIT, &host), SQ_E_ABI_MISMATCH);
  CHECK(!settings.has("Test.BadAbi.Initialized"));
  sq_dylib_close(lib);
}

SQ_TEST("abi: loader with an older host ABI rejects the current dummy") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext oldHost(settings, reg, SQ_ABI_MAKE(SQ_PLATFORM_ABI_MAJOR - 1, 0));
  sq::app::ModuleLoader loader(oldHost, reg);
  auto r = loader.load(sqtest::dummyPath());
  CHECK_EQ(r.code, SQ_E_ABI_MISMATCH);
  CHECK_EQ(loader.count(), size_t(0));
  CHECK(!settings.has("Services.dummy.InitCount"));
}

SQ_TEST("abi: non-module libraries and missing files are rejected") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
#ifdef _WIN32
  std::string platformLib = sq::joinPath(sqtest::binDir(), "sqplatform.dll");
#elif defined(__APPLE__)
  std::string platformLib = sq::joinPath(sqtest::binDir(), "libsqplatform.dylib");
#else
  std::string platformLib = sq::joinPath(sqtest::binDir(), "libsqplatform.so");
#endif
  auto r = loader.load(platformLib);
  CHECK_EQ(r.code, SQ_E_NOINTERFACE);
  auto missing = loader.load(sqtest::modulePath(sqtest::binDir(), "does-not-exist"));
  CHECK(!missing.ok());
  CHECK_EQ(loader.count(), size_t(0));
}

SQ_TEST("abi: scanning the test-module folder rejects badabi, loads the rest") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  auto res = loader.scan(sqtest::testModulesDir());
  CHECK(res.rejected >= 1);
  bool sawBad = false;
  for (const auto& [path, r] : res.results) {
    if (path.find("badabi") != std::string::npos) {
      sawBad = true;
      CHECK_EQ(r.code, SQ_E_ABI_MISMATCH);
    }
  }
  CHECK(sawBad);
  CHECK(!loader.find("badabi"));
  CHECK(!settings.has("Test.BadAbi.Initialized"));
  CHECK_EQ(res.loaded + res.rejected, int(res.results.size()));
}
