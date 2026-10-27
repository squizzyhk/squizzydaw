// Module lifecycle with host registrations: a module that registers services, settings
// observers and dynamic classes in INIT (and removes them in SHUTDOWN) must still hot-reload;
// a module that leaks one must be refused and keep working (rollback), never unmapped.
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
constexpr sq_guid kDynamicClassId = sq::guid("{7B6A74B2-4871-46E9-8852-20A82554838E}");
std::string selfregPath() { return sqtest::modulePath(sqtest::testModulesDir(), "selfreg"); }
}  // namespace

SQ_TEST("lifecycle: module with host registrations reloads cleanly") {
  Rig r;
  std::string path = selfregPath();
  REQUIRE(r.loader.load(path).ok());
  for (int cycle = 1; cycle <= 10; ++cycle) {
    {
      sq::Ptr<sq::IDummyCounter> svc;
      REQUIRE_OK(r.host.queryService(&sq::IDummyCounter::iid, svc.putVoid()));
      CHECK_EQ(svc->increment(), 1);  // a fresh service object every init
      auto dyn = sq::createInstance<sq::IDummyCounter>(&r.registry, kDynamicClassId);
      REQUIRE(dyn);
      CHECK_EQ(dyn->increment(), 1);
    }
    REQUIRE_OK(r.loader.unload("selfreg"));
    CHECK(!sq_dylib_is_loaded(path.c_str()));
    CHECK_EQ(r.host.serviceCount(), size_t(0));
    CHECK_EQ(r.registry.findClass(&kDynamicClassId, nullptr), SQ_E_NOTFOUND);
    CHECK_EQ(r.registry.getClassCount(), 0u);
    REQUIRE(r.loader.load(path).ok());
  }
  CHECK_EQ(r.settings.getInt("Test.SelfReg.InitCount", 0), 11);
}

SQ_TEST("lifecycle: busy unload rolls back and the module keeps working") {
  Rig r;
  std::string path = selfregPath();
  REQUIRE(r.loader.load(path).ok());
  {
    auto obj = sq::createInstance<sq::IDummyCounter>(&r.registry, sq::guid("{535301FF-C7C0-493C-8B26-52EB544160B7}"));
    REQUIRE(obj);
    CHECK_EQ(r.loader.unload("selfreg"), SQ_E_BUSY);
    CHECK(sq_dylib_is_loaded(path.c_str()));
    CHECK(r.loader.find("selfreg"));
    // Everything INIT registered is back.
    CHECK_EQ(r.host.serviceCount(), size_t(1));
    CHECK_EQ(r.registry.findClass(&kDynamicClassId, nullptr), SQ_OK);
    CHECK_EQ(obj->increment(), 1);
  }
  REQUIRE_OK(r.loader.unload("selfreg"));
  CHECK(!sq_dylib_is_loaded(path.c_str()));
}

SQ_TEST("lifecycle: a module leaking a host registration is never unmapped") {
  Rig r;
  std::string path = selfregPath();
  r.settings.setBool("Test.SelfReg.LeakObserver", SQ_TRUE);
  REQUIRE(r.loader.load(path).ok());
  CHECK_EQ(r.loader.unload("selfreg"), SQ_E_BUSY);  // the leaked observer lives in the module
  CHECK(sq_dylib_is_loaded(path.c_str()));
  CHECK(r.loader.find("selfreg"));
  sq::Ptr<sq::IDummyCounter> svc;
  CHECK_OK(r.host.queryService(&sq::IDummyCounter::iid, svc.putVoid()));
  svc = nullptr;
  // Observers still fire into mapped code without crashing.
  CHECK_OK(r.settings.setInt("Test.SelfReg.Ping", 1));
  // The rig's loader destructor force-unloads: the image must stay mapped (no crash at exit).
}

SQ_TEST("lifecycle: module factory handed to the registry blocks unload") {
  Rig r;
  REQUIRE(r.loader.load(sqtest::dummyPath()).ok());
  sq::Ptr<sq::IClassFactory> factory;
  {
    sq_dylib* lib = nullptr;
    REQUIRE_OK(sq_dylib_open(sqtest::dummyPath().c_str(), &lib, nullptr, 0));
    auto gf = reinterpret_cast<sq_GetClassFactoryFn>(sq_dylib_symbol(lib, SQ_EXPORT_GET_CLASS_FACTORY));
    factory = sq::Ptr<sq::IClassFactory>::adopt(gf());
    sq_dylib_close(lib);
  }
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  ci.class_id = sq::guid("{8E569209-0813-4572-B558-039BF8A39B71}");
  ci.category = "ScriptClass";
  ci.name = "Alias";
  REQUIRE_OK(r.registry.registerClass(&ci, factory.get()));
  factory = nullptr;  // only the registry holds the module's factory now
  CHECK_EQ(r.loader.unload("dummy"), SQ_E_BUSY);
  CHECK(sq_dylib_is_loaded(sqtest::dummyPath().c_str()));
  CHECK_OK(r.registry.unregisterClass(&ci.class_id));
  CHECK_OK(r.loader.unload("dummy"));
  CHECK(!sq_dylib_is_loaded(sqtest::dummyPath().c_str()));
}

SQ_TEST("registry: class info strings stay valid across registry mutations") {
  Rig r;
  REQUIRE(r.loader.load(sqtest::dummyPath()).ok());
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(r.registry.findClass(&sq::dummy::kServiceClassId, &ci));
  const char* name = ci.name;
  REQUIRE_OK(r.loader.unload("dummy"));
  REQUIRE(r.loader.load(sqtest::dummyPath()).ok());
  CHECK_EQ(std::string(name), std::string("Dummy Service"));
}
