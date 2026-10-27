// Module loader: scan, registry integration, refuse-unload-while-busy, unload/reload cycles.
#include <string>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/dummy.h"
#include "sqtest.hpp"
#include "test_util.hpp"

namespace {
struct Rig {
  sq::app::Settings settings;
  sq::app::ClassRegistry registry;
  sq::app::HostContext host{settings, registry};
  sq::app::ModuleLoader loader{host, registry};
};
}  // namespace

SQ_TEST("reload: scan services dir registers the dummy classes") {
  Rig r;
  auto res = r.loader.scan(sqtest::servicesDir());
  CHECK(res.loaded >= 1);
  CHECK_EQ(res.rejected, 0);
  REQUIRE(r.loader.find("dummy"));
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(r.registry.findClass(&sq::dummy::kServiceClassId, &ci));
  CHECK_EQ(std::string(ci.name), std::string("Dummy Service"));
  CHECK_EQ(ci.module_fourcc, SQ_FOURCC('D', 'M', 'M', 'Y'));
  // Loading the same module twice is refused.
  auto again = r.loader.load(sqtest::dummyPath());
  CHECK_EQ(again.code, SQ_E_EXISTS);
  CHECK_EQ(r.loader.count(), size_t(res.loaded));
}

SQ_TEST("reload: unload refused while instances live, then 20 clean reload cycles") {
  Rig r;
  REQUIRE(r.loader.load(sqtest::dummyPath()).ok());
  std::string path = sqtest::dummyPath();
  for (int cycle = 1; cycle <= 20; ++cycle) {
    {
      auto svc = sq::createInstance<sq::IDummyService>(&r.registry, sq::dummy::kServiceClassId);
      REQUIRE(svc);
      CHECK_EQ(svc->add(cycle, 1), cycle + 1);
      // Each cycle inits twice: busy-unload rollback re-INIT + the reload below.
      CHECK_EQ(svc->getInitCount(), 2 * cycle - 1);
      CHECK_EQ(r.loader.unload("dummy"), SQ_E_BUSY);
      CHECK(r.loader.find("dummy"));
      // Rolled back: the module still serves its classes and the live object still works.
      CHECK_EQ(r.registry.findClass(&sq::dummy::kServiceClassId, nullptr), SQ_OK);
      CHECK_EQ(svc->add(2, 2), 4);
    }
    REQUIRE_OK(r.loader.unload("dummy"));
    CHECK(!r.loader.find("dummy"));
    CHECK(!sq_dylib_is_loaded(path.c_str()));
    CHECK_EQ(r.registry.findClass(&sq::dummy::kServiceClassId, nullptr), SQ_E_NOTFOUND);
    void* out = nullptr;
    CHECK_EQ(r.registry.createInstance(&sq::dummy::kServiceClassId, &sq::IUnknown::iid, &out), SQ_E_NOTFOUND);
    REQUIRE(r.loader.load(path).ok());
  }
  CHECK_EQ(r.settings.getInt("Services.dummy.InitCount", 0), 41);
}

SQ_TEST("reload: reload() picks the module up again; unknown names fail") {
  Rig r;
  REQUIRE(r.loader.load(sqtest::dummyPath()).ok());
  auto res = r.loader.reload("dummy");
  CHECK(res.ok());
  CHECK(r.loader.find("dummy"));
  CHECK_EQ(r.loader.reload("nope").code, SQ_E_NOTFOUND);
  CHECK_EQ(r.loader.unload("nope"), SQ_E_NOTFOUND);
  r.loader.unloadAll(false);
  CHECK_EQ(r.loader.count(), size_t(0));
  CHECK_EQ(r.registry.getClassCount(), 0u);
}

SQ_TEST("registry: dynamic (script-style) class registration") {
  Rig r;
  REQUIRE(r.loader.load(sqtest::dummyPath()).ok());
  // Re-serve the dummy counter class under a new GUID via the module's factory, the way the
  // scripting service will register <ScriptClass> entries.
  sq::Ptr<sq::IClassFactory> factory;
  {
    sq_dylib* lib = nullptr;
    REQUIRE_OK(sq_dylib_open(sqtest::dummyPath().c_str(), &lib, nullptr, 0));
    auto gf = reinterpret_cast<sq_GetClassFactoryFn>(sq_dylib_symbol(lib, SQ_EXPORT_GET_CLASS_FACTORY));
    factory = sq::Ptr<sq::IClassFactory>::adopt(gf());
    sq_dylib_close(lib);  // still mapped: the loader holds a reference
  }
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  ci.class_id = sq::guid("{B51CB162-24E7-45F1-936A-CF0915467400}");
  ci.category = "ScriptClass";
  ci.name = "Alias";
  CHECK_OK(r.registry.registerClass(&ci, factory.get()));
  CHECK_EQ(r.registry.registerClass(&ci, factory.get()), SQ_E_EXISTS);
  sq_class_info found{};
  found.struct_size = sizeof found;
  REQUIRE_OK(r.registry.findClass(&ci.class_id, &found));
  CHECK_EQ(found.module_fourcc, 0u);
  // The alias GUID is unknown to the dummy factory -> NOTFOUND from the factory.
  void* out = nullptr;
  CHECK_EQ(r.registry.createInstance(&ci.class_id, &sq::IUnknown::iid, &out), SQ_E_NOTFOUND);
  CHECK_EQ(r.registry.unregisterClass(&sq::dummy::kServiceClassId), SQ_E_ACCESS);
  CHECK_OK(r.registry.unregisterClass(&ci.class_id));
  factory.reset();
}

SQ_TEST("host: service locator register/query/unregister") {
  Rig r;
  auto obj = sq::Ptr<sq::ISettingsObserver>::adopt(new sq::SettingsObserverFn(nullptr));
  CHECK_OK(r.host.registerService(&sq::ISettingsObserver::iid, obj.get()));
  CHECK_EQ(r.host.registerService(&sq::ISettingsObserver::iid, obj.get()), SQ_E_EXISTS);
  sq::Ptr<sq::ISettingsObserver> got;
  CHECK_OK(r.host.queryService(&sq::ISettingsObserver::iid, got.putVoid()));
  CHECK(got.get() == obj.get());
  CHECK_EQ(r.host.queryService(&sq::IDummyService::iid, got.putVoid()), SQ_E_NOTFOUND);
  CHECK_OK(r.host.unregisterService(&sq::ISettingsObserver::iid, obj.get()));
  CHECK_EQ(r.host.serviceCount(), size_t(0));
}
