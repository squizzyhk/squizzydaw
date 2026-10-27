// Contract check over EVERY module in bin/services (and bin/testmodules): each exports exactly
// GetClassFactory + ModuleMain, reports module info whose name matches its file name, and
// declares an ABI range containing the host's. New services are covered automatically.
#include <set>
#include <string>
#include <vector>

#include "sq/sdk.hpp"
#include "sqtest.hpp"
#include "test_util.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::vector<std::string> modulesIn(const std::string& dir) {
  std::vector<std::string> names;
  std::string suffix = sq_dylib_suffix();
  struct Ctx {
    std::vector<std::string>* names;
    const std::string* suffix;
  } ctx{&names, &suffix};
  sq_fs_list_dir(
      dir.c_str(),
      [](void* user, const char* name, sq_bool isDir) -> sq_bool {
        auto* c = static_cast<Ctx*>(user);
        std::string n = name;
        if (!isDir && n.size() > c->suffix->size() && n.compare(n.size() - c->suffix->size(), c->suffix->size(), *c->suffix) == 0)
          c->names->push_back(n.substr(0, n.size() - c->suffix->size()));
        return SQ_TRUE;
      },
      &ctx);
  return names;
}

#ifdef _WIN32
// Named exports of a loaded image; "<ordinal-only export>" if any export has no name.
std::set<std::string> exportNames(const std::string& utf8Path) {
  std::set<std::string> names;
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, w.data(), n);
  for (auto& ch : w)
    if (ch == L'/') ch = 0x5C;  // backslash
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

void checkModule(const std::string& dir, const std::string& stem, bool expectInRange) {
  std::string path = sqtest::modulePath(dir, stem);
  sq_dylib* lib = nullptr;
  char err[512] = {};
  REQUIRE_OK(sq_dylib_open(path.c_str(), &lib, err, sizeof err));
  auto gf = reinterpret_cast<sq_GetClassFactoryFn>(sq_dylib_symbol(lib, SQ_EXPORT_GET_CLASS_FACTORY));
  auto mm = reinterpret_cast<sq_ModuleMainFn>(sq_dylib_symbol(lib, SQ_EXPORT_MODULE_MAIN));
  CHECK(gf != nullptr);
  CHECK(mm != nullptr);
#ifdef _WIN32
  std::set<std::string> expected = {"GetClassFactory", "ModuleMain"};
  std::set<std::string> got = exportNames(path);
  if (got != expected) {
    std::string list;
    for (auto& g : got) list += g + " ";
    std::printf("  %s exports: %s\n", stem.c_str(), list.c_str());
  }
  CHECK(got == expected);
#endif
  if (gf) {
    auto f = sq::Ptr<sq::IClassFactory>::adopt(gf());
    REQUIRE(f);
    sq_module_info info{};
    info.struct_size = sizeof info;
    REQUIRE_OK(f->getModuleInfo(&info));
    CHECK_EQ(std::string(info.name ? info.name : ""), stem);  // module name == file name == service dir
    CHECK(info.fourcc != 0);
    CHECK_EQ(info.sdk_abi, uint32_t(SQ_PLATFORM_ABI));
    CHECK_EQ(bool(sq_abi_in_range(SQ_PLATFORM_ABI, info.abi_min, info.abi_max)), expectInRange);
    for (uint32_t i = 0; i < f->getClassCount(); ++i) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      CHECK_OK(f->getClassInfo(i, &ci));
      CHECK(!sq_guid_is_null(&ci.class_id));
      CHECK(ci.category && *ci.category);
      CHECK(ci.name && *ci.name);
    }
    CHECK(f->canUnload());
  }
  sq_dylib_close(lib);
}

}  // namespace

SQ_TEST("contract: every service module exports exactly the two ABI symbols") {
  auto mods = modulesIn(sqtest::servicesDir());
  REQUIRE(!mods.empty());
  for (const auto& m : mods) {
    std::printf("  checking services/%s\n", m.c_str());
    checkModule(sqtest::servicesDir(), m, true);
  }
}

SQ_TEST("contract: test modules obey the same export rule") {
  for (const auto& m : modulesIn(sqtest::testModulesDir())) checkModule(sqtest::testModulesDir(), m, m != "badabi");
}
