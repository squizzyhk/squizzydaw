// Shared helpers for tests (paths of built artifacts, temp dirs).
#pragma once

#include <string>

#include "sq/platform.hpp"

namespace sqtest {

inline std::string binDir() { return sq::specialDir(SQ_DIR_EXE); }
inline std::string servicesDir() { return sq::joinPath(binDir(), "services"); }
inline std::string testModulesDir() { return sq::joinPath(binDir(), "testmodules"); }
inline std::string modulePath(const std::string& dir, const std::string& name) { return sq::joinPath(dir, name + sq_dylib_suffix()); }
inline std::string dummyPath() { return modulePath(servicesDir(), "dummy"); }

// Unique scratch dir under the system temp folder, removed by the caller.
inline std::string makeTempDir(const char* tag) {
  sq_guid g{};
  sq_guid_generate(&g);
  std::string d = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), std::string("sqtest-") + tag + "-" + std::to_string(g.data1));
  sq_fs_create_dirs(d.c_str());
  return d;
}

}  // namespace sqtest
