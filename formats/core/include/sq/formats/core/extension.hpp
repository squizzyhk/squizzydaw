// Squizzy Studio formats - extensions: a directory holding metainfo.xml + installdata.xml plus
// payload files. installdata.xml gates installation on the application id, the platform ABI
// range and the OS/architecture; the gate is enforced by extensionInstall() and extensionScan().
// Schema: SCHEMAS.md "Extensions".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sq/formats/core/metainfo.hpp"

namespace sq::formats {

inline constexpr int kInstallDataFormat = 1;
inline constexpr const char* kInstallDataFile = "installdata.xml";
// Application id this build accepts in installdata.xml <Target app="...">.
inline constexpr const char* kAppId = "squizzy.studio";

struct ExtensionTarget {
  std::string app;          // application id (kAppId for this product)
  uint32_t abiMin = 0;      // SQ_ABI_MAKE(major, minor), inclusive
  uint32_t abiMax = 0;      // inclusive; "1.*" = SQ_ABI_MAKE(1, 0xFFFF)
  std::string os = "*";     // "windows" | "macos" | "linux" | "*"
  std::string arch = "*";   // "x64" | "arm64" | "*"
};

struct InstallData {
  std::vector<ExtensionTarget> targets;  // at least one; the extension installs if any matches
};

struct HostIdentity {
  std::string app = kAppId;
  uint32_t abi = 0;
  std::string os;
  std::string arch;
  static HostIdentity current();  // kAppId, SQ_PLATFORM_ABI, compile-time OS/arch
};

struct Extension {
  std::string dir;  // source directory
  MetaInfo meta;    // Package.Id (safe identifier) and Package.Version required
  InstallData install;
  std::string id() const { return meta.get(meta::kId); }
};

std::string installDataToXml(const InstallData& d);
Status installDataFromXml(std::string_view xml, InstallData& out);

// Reads and validates metainfo.xml + installdata.xml of an extension directory.
Status extensionLoad(const std::string& dir, Extension& out);
// Gate: SQ_OK if a target matches the host. Otherwise SQ_E_ABI_MISMATCH (app ok, ABI out of
// range) or SQ_E_UNSUPPORTED (other application or platform), with a message.
Status extensionCheck(const Extension& ext, const HostIdentity& host);
// Load + gate + copy into <extensionsRoot>/<id>/ (replacing an older install atomically
// enough: staged copy, then swap). Nothing is written when the gate fails.
Status extensionInstall(const std::string& srcDir, const std::string& extensionsRoot, const HostIdentity& host,
                        std::string* installedDir = nullptr);
Status extensionUninstall(const std::string& extensionsRoot, std::string_view id);
// Loads every installed extension and re-applies the gate (an app/ABI upgrade can invalidate
// an installed extension). Rejected ones are reported, not deleted.
struct ScanResult {
  std::vector<Extension> accepted;
  std::vector<std::pair<std::string, Status>> rejected;  // dir name -> reason
};
Status extensionScan(const std::string& extensionsRoot, const HostIdentity& host, ScanResult& out);

}  // namespace sq::formats
