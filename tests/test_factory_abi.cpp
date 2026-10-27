// Talks to the dummy module through the raw two-export ABI (no app loader involved).
#include <set>
#include <string>

#include "sq/app/host_context.hpp"
#include "sq/interfaces/dummy.h"
#include "sqtest.hpp"
#include "test_util.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct RawModule {
  sq_dylib* lib = nullptr;
  sq_GetClassFactoryFn getFactory = nullptr;
  sq_ModuleMainFn main = nullptr;
  ~RawModule() { sq_dylib_close(lib); }
};

#ifdef _WIN32
std::set<std::string> exportNames(const std::string& path) {
  std::set<std::string> names;
  // UTF-8 -> UTF-16 properly (a byte-wise widen breaks on non-ASCII install paths).
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
  HMODULE h = GetModuleHandleW(w.c_str());
  if (!h) return names;
  auto* base = reinterpret_cast<const unsigned char*>(h);
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!dir.VirtualAddress) return names;
  auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  auto* nameRvas = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
  for (DWORD i = 0; i < exp->NumberOfNames; ++i) names.insert(reinterpret_cast<const char*>(base + nameRvas[i]));
  if (exp->NumberOfFunctions != exp->NumberOfNames) names.insert("<ordinal-only export>");
  return names;
}
#endif

}  // namespace

SQ_TEST("factory ABI: module exports exactly GetClassFactory + ModuleMain") {
  RawModule m;
  char err[256] = {};
  REQUIRE_OK(sq_dylib_open(sqtest::dummyPath().c_str(), &m.lib, err, sizeof err));
  CHECK(sq_dylib_symbol(m.lib, SQ_EXPORT_GET_CLASS_FACTORY) != nullptr);
  CHECK(sq_dylib_symbol(m.lib, SQ_EXPORT_MODULE_MAIN) != nullptr);
#ifdef _WIN32
  std::set<std::string> names = exportNames(sqtest::dummyPath());
  std::set<std::string> expected = {"GetClassFactory", "ModuleMain"};
  CHECK(names == expected);
  CHECK_EQ(names.size(), size_t(2));
#endif
}

SQ_TEST("factory ABI: module info, class table, createInstance, QI, refcount") {
  RawModule m;
  REQUIRE_OK(sq_dylib_open(sqtest::dummyPath().c_str(), &m.lib, nullptr, 0));
  m.getFactory = reinterpret_cast<sq_GetClassFactoryFn>(sq_dylib_symbol(m.lib, SQ_EXPORT_GET_CLASS_FACTORY));
  m.main = reinterpret_cast<sq_ModuleMainFn>(sq_dylib_symbol(m.lib, SQ_EXPORT_MODULE_MAIN));
  REQUIRE(m.getFactory && m.main);

  // Factory is a lazily-built singleton: two calls give the same object.
  auto f = sq::Ptr<sq::IClassFactory>::adopt(m.getFactory());
  auto f2 = sq::Ptr<sq::IClassFactory>::adopt(m.getFactory());
  REQUIRE(f);
  CHECK(f.get() == f2.get());
  auto asUnknown = f.as<sq::IUnknown>();
  CHECK(asUnknown);
  CHECK(!f.as<sq::IDummyService>());

  sq_module_info info{};
  CHECK_EQ(f->getModuleInfo(&info), SQ_E_INVALIDARG);  // struct_size not set
  info.struct_size = sizeof info;
  REQUIRE_OK(f->getModuleInfo(&info));
  CHECK_EQ(std::string(info.name), std::string("dummy"));
  CHECK_EQ(info.fourcc, SQ_FOURCC('D', 'M', 'M', 'Y'));
  CHECK_EQ(info.version, SQ_MAKE_VERSION(1, 0, 0));  // stamped by sq_add_service(VERSION 1.0.0)
  CHECK_EQ(info.sdk_abi, uint32_t(SQ_PLATFORM_ABI));
  CHECK(sq_abi_in_range(SQ_PLATFORM_ABI, info.abi_min, info.abi_max));
  CHECK_EQ(std::string(info.build_stamp), std::string(SQ_BUILD_STAMP));

  REQUIRE_EQ(f->getClassCount(), 2u);
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(f->getClassInfo(0, &ci));
  CHECK(ci.class_id == sq::dummy::kServiceClassId);
  CHECK_EQ(std::string(ci.category), std::string("Service"));
  CHECK_EQ(f->getClassInfo(2, &ci), SQ_E_NOTFOUND);

  // ModuleMain handshake with a real host context.
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  REQUIRE_OK(m.main(SQ_MODULE_INIT, &host));
  CHECK_EQ(settings.getInt("Services.dummy.InitCount", 0), 1);

  CHECK(f->canUnload());
  {
    sq::Ptr<sq::IDummyService> svc;
    REQUIRE_OK(f->createInstance(&sq::dummy::kServiceClassId, &sq::IDummyService::iid, svc.putVoid()));
    CHECK(!f->canUnload());
    CHECK_EQ(svc->add(40, 2), 42);
    char buf[128];
    CHECK_OK(svc->getGreeting(buf, sizeof buf));
    CHECK_EQ(svc->getGreeting(buf, 4), SQ_E_BUFFER_TOO_SMALL);
    CHECK_EQ(svc->getInitCount(), 1);

    // Multi-interface QI + identity rule (IUnknown pointer is stable).
    auto counter = svc.as<sq::IDummyCounter>();
    REQUIRE(counter);
    CHECK_EQ(counter->increment(), 1);
    CHECK_EQ(svc.as<sq::IDummyCounter>()->value(), 1);
    auto u1 = svc.as<sq::IUnknown>();
    auto u2 = counter.as<sq::IUnknown>();
    CHECK(u1.get() == u2.get());

    // Refcount: addRef/release return the new count.
    uint32_t c = svc->addRef();
    CHECK_EQ(svc->release(), c - 1);

    void* none = reinterpret_cast<void*>(1);
    CHECK_EQ(svc->queryInterface(&sq::IClassFactory::iid, &none), SQ_E_NOINTERFACE);
    CHECK(none == nullptr);
  }
  CHECK(f->canUnload());  // every object released

  // Class that only implements IDummyCounter refuses IDummyService.
  void* out = nullptr;
  CHECK_EQ(f->createInstance(&sq::dummy::kCounterClassId, &sq::IDummyService::iid, &out), SQ_E_NOINTERFACE);
  CHECK(out == nullptr);
  CHECK(f->canUnload());
  sq_guid unknownClass = sq::guid("{00000000-0000-0000-0000-000000000001}");
  CHECK_EQ(f->createInstance(&unknownClass, &sq::IUnknown::iid, &out), SQ_E_NOTFOUND);

  CHECK_OK(m.main(SQ_MODULE_SHUTDOWN, &host));
  f.reset();
  f2.reset();
  asUnknown.reset();
}
