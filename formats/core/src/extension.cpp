#include "sq/formats/core/extension.hpp"

#include <algorithm>

#include "sq/formats/core/xmlutil.hpp"
#include "sq/platform.hpp"

namespace sq::formats {

HostIdentity HostIdentity::current() {
  HostIdentity h;
  h.app = kAppId;
  h.abi = SQ_PLATFORM_ABI;
#if defined(SQ_OS_WINDOWS)
  h.os = "windows";
#elif defined(SQ_OS_MACOS)
  h.os = "macos";
#elif defined(SQ_OS_LINUX)
  h.os = "linux";
#else
  h.os = "unknown";
#endif
#if defined(_M_X64) || defined(__x86_64__)
  h.arch = "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  h.arch = "arm64";
#else
  h.arch = "unknown";
#endif
  return h;
}

std::string installDataToXml(const InstallData& d) {
  auto doc = xmlCreate("InstallData");
  XmlNode root = xmlRoot(doc);
  root.setAttr("format", std::to_string(kInstallDataFormat));
  for (const auto& t : d.targets) {
    XmlNode n = root.append("Target");
    n.setAttr("app", t.app);
    n.setAttr("abiMin", formatAbi(t.abiMin));
    n.setAttr("abiMax", SQ_ABI_GET_MINOR(t.abiMax) == 0xFFFF ? std::to_string(SQ_ABI_GET_MAJOR(t.abiMax)) + ".*" : formatAbi(t.abiMax));
    if (!t.os.empty() && t.os != "*") n.setAttr("os", t.os);
    if (!t.arch.empty() && t.arch != "*") n.setAttr("arch", t.arch);
  }
  return xmlWrite(doc);
}

Status installDataFromXml(std::string_view xml, InstallData& out) {
  out = InstallData{};
  XmlDocPtr doc;
  XmlNode root;
  SQF_TRY(parseVersionedXml(xml, "InstallData", kInstallDataFormat, doc, root));
  for (XmlNode n : root.children("Target")) {
    ExtensionTarget t;
    t.app = n.attr("app");
    if (t.app.empty()) return Status::error(SQ_E_PARSE, "installdata: <Target> without app");
    if (!parseAbi(n.attr("abiMin"), t.abiMin)) return Status::error(SQ_E_PARSE, "installdata: bad abiMin '" + n.attr("abiMin") + "'");
    std::string mx = n.attr("abiMax");
    if (mx.empty()) {
      t.abiMax = SQ_ABI_MAKE(SQ_ABI_GET_MAJOR(t.abiMin), 0xFFFF);  // same major, any minor
    } else if (!parseAbi(mx, t.abiMax)) {
      return Status::error(SQ_E_PARSE, "installdata: bad abiMax '" + mx + "'");
    }
    if (t.abiMax < t.abiMin) return Status::error(SQ_E_PARSE, "installdata: abiMax < abiMin");
    t.os = n.attr("os", "*");
    t.arch = n.attr("arch", "*");
    out.targets.push_back(std::move(t));
  }
  if (out.targets.empty()) return Status::error(SQ_E_PARSE, "installdata: at least one <Target> is required");
  return {};
}

Status extensionLoad(const std::string& dir, Extension& out) {
  out = Extension{};
  out.dir = dir;
  std::string mi, idata;
  Status s = readFileBytes(joinPath(dir, kMetaInfoFile), mi);
  if (!s) return Status::error(SQ_E_NOTFOUND, "extension: missing metainfo.xml in '" + dir + "'");
  s = readFileBytes(joinPath(dir, kInstallDataFile), idata);
  if (!s) return Status::error(SQ_E_NOTFOUND, "extension: missing installdata.xml in '" + dir + "'");
  SQF_TRY(metaInfoFromXml(mi, out.meta));
  if (!isSafeIdentifier(out.id())) return Status::error(SQ_E_PARSE, "extension: missing or invalid Package.Id");
  if (out.meta.get(meta::kVersion).empty()) return Status::error(SQ_E_PARSE, "extension: missing Package.Version");
  return installDataFromXml(idata, out.install);
}

Status extensionCheck(const Extension& ext, const HostIdentity& host) {
  bool appSeen = false, abiOk = false;
  for (const auto& t : ext.install.targets) {
    if (t.app != host.app) continue;
    appSeen = true;
    if (!sq_abi_in_range(host.abi, t.abiMin, t.abiMax)) continue;
    abiOk = true;
    bool osOk = t.os == "*" || t.os.empty() || t.os == host.os;
    bool archOk = t.arch == "*" || t.arch.empty() || t.arch == host.arch;
    if (osOk && archOk) return {};
  }
  if (!appSeen)
    return Status::error(SQ_E_UNSUPPORTED, "extension '" + ext.id() + "' is not built for application '" + host.app + "'");
  if (!abiOk)
    return Status::error(SQ_E_ABI_MISMATCH, "extension '" + ext.id() + "' requires a platform ABI range that excludes " + formatAbi(host.abi));
  return Status::error(SQ_E_UNSUPPORTED, "extension '" + ext.id() + "' does not support " + host.os + "-" + host.arch);
}

Status extensionInstall(const std::string& srcDir, const std::string& extensionsRoot, const HostIdentity& host,
                        std::string* installedDir) {
  Extension ext;
  SQF_TRY(extensionLoad(srcDir, ext));
  SQF_TRY(extensionCheck(ext, host));
  const std::string id = ext.id();
  const std::string finalDir = joinPath(extensionsRoot, id);
  const std::string staging = joinPath(extensionsRoot, "." + id + ".staging");
  const std::string old = joinPath(extensionsRoot, "." + id + ".old");
  if (sq_fs_create_dirs(extensionsRoot.c_str()) != SQ_OK) return Status::error(SQ_E_IO, "cannot create '" + extensionsRoot + "'");
  sq_fs_remove_all(staging.c_str());
  sq_fs_remove_all(old.c_str());
  Status s = copyTree(srcDir, staging);
  if (!s) {
    sq_fs_remove_all(staging.c_str());
    return s;
  }
  if (sq_fs_exists(finalDir.c_str()) && sq_fs_rename(finalDir.c_str(), old.c_str()) != SQ_OK) {
    sq_fs_remove_all(staging.c_str());
    return Status::error(SQ_E_IO, "cannot move the previous install of '" + id + "' aside");
  }
  if (sq_fs_rename(staging.c_str(), finalDir.c_str()) != SQ_OK) {
    if (sq_fs_exists(old.c_str())) sq_fs_rename(old.c_str(), finalDir.c_str());
    sq_fs_remove_all(staging.c_str());
    return Status::error(SQ_E_IO, "cannot move '" + id + "' into place");
  }
  sq_fs_remove_all(old.c_str());
  if (installedDir) *installedDir = finalDir;
  return {};
}

Status extensionUninstall(const std::string& extensionsRoot, std::string_view id) {
  if (!isSafeIdentifier(id)) return Status::error(SQ_E_INVALIDARG, "invalid extension id");
  std::string dir = joinPath(extensionsRoot, id);
  if (!sq_fs_is_dir(dir.c_str())) return Status::error(SQ_E_NOTFOUND, "extension '" + std::string(id) + "' is not installed");
  if (sq_fs_remove_all(dir.c_str()) != SQ_OK) return Status::error(SQ_E_IO, "cannot remove '" + dir + "'");
  return {};
}

namespace {
sq_bool collectDirs(void* user, const char* name, sq_bool isDir) {
  if (isDir && name[0] != '.') static_cast<std::vector<std::string>*>(user)->push_back(name);
  return SQ_TRUE;
}
}  // namespace

Status extensionScan(const std::string& extensionsRoot, const HostIdentity& host, ScanResult& out) {
  out = ScanResult{};
  if (!sq_fs_is_dir(extensionsRoot.c_str())) return {};  // nothing installed
  std::vector<std::string> dirs;
  if (sq_fs_list_dir(extensionsRoot.c_str(), &collectDirs, &dirs) != SQ_OK)
    return Status::error(SQ_E_IO, "cannot list '" + extensionsRoot + "'");
  std::sort(dirs.begin(), dirs.end());
  for (const auto& d : dirs) {
    Extension ext;
    Status s = extensionLoad(joinPath(extensionsRoot, d), ext);
    if (s && ext.id() != d) s = Status::error(SQ_E_PARSE, "extension directory name does not match Package.Id");
    if (s) s = extensionCheck(ext, host);
    if (s) out.accepted.push_back(std::move(ext));
    else out.rejected.emplace_back(d, std::move(s));
  }
  return {};
}

}  // namespace sq::formats
