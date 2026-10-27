// formats/core: extension metainfo/installdata gating, install, scan, uninstall.
#include "sq/formats/core/all.hpp"
#include "sq/platform.hpp"
#include "sqtest.hpp"

using namespace sq::formats;

namespace {
std::string tempDir(const char* tag) {
  sq_guid g{};
  sq_guid_generate(&g);
  std::string d = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), std::string("sqfmt-") + tag + "-" + std::to_string(g.data1));
  sq_fs_create_dirs(d.c_str());
  return d;
}

void makeExt(const std::string& dir, const std::string& id, const std::string& abiMin, const std::string& abiMax,
             const std::string& app = kAppId) {
  MetaInfo m;
  m.set(meta::kId, id);
  m.set(meta::kVersion, "1.0.0");
  m.set(meta::kName, "Test " + id);
  REQUIRE(writeFileBytes(sq::joinPath(dir, kMetaInfoFile), metaInfoToXml(m)).ok());
  std::string inst = "<InstallData format=\"1\"><Target app=\"" + app + "\" abiMin=\"" + abiMin + "\" abiMax=\"" + abiMax +
                     "\" os=\"*\" arch=\"*\"/></InstallData>";
  REQUIRE(writeFileBytes(sq::joinPath(dir, kInstallDataFile), inst).ok());
  REQUIRE(writeFileBytes(sq::joinPath(dir, "scripts/main.js"), "// payload").ok());
}
}  // namespace

SQ_TEST("formats: installdata xml round trip") {
  InstallData d;
  d.targets.push_back(ExtensionTarget{kAppId, SQ_ABI_MAKE(1, 0), SQ_ABI_MAKE(1, 0xFFFF), "windows", "x64"});
  InstallData back;
  REQUIRE(installDataFromXml(installDataToXml(d), back).ok());
  REQUIRE_EQ(back.targets.size(), size_t{1});
  CHECK_EQ(back.targets[0].abiMax, uint32_t{SQ_ABI_MAKE(1, 0xFFFF)});
  CHECK_EQ(back.targets[0].os, std::string("windows"));
  CHECK(!installDataFromXml("<InstallData format=\"1\"/>", back).ok());  // no targets
  uint32_t abi = 0;
  CHECK(parseAbi("1.*", abi));
  CHECK_EQ(abi, uint32_t{SQ_ABI_MAKE(1, 0xFFFF)});
  CHECK(!parseAbi("x.y", abi));
}

SQ_TEST("formats: extension gate, install, scan, uninstall") {
  std::string root = tempDir("ext");
  std::string src = sq::joinPath(root, "src");
  std::string dst = sq::joinPath(root, "installed");
  HostIdentity host = HostIdentity::current();

  std::string good = sq::joinPath(src, "good");
  makeExt(good, "good.ext", "1.0", "1.*");
  std::string installed;
  REQUIRE(extensionInstall(good, dst, host, &installed).ok());
  CHECK(sq_fs_exists(sq::joinPath(installed, "scripts/main.js").c_str()));

  std::string future = sq::joinPath(src, "future");
  makeExt(future, "future.ext", "9.0", "9.*");
  Status s = extensionInstall(future, dst, host);
  CHECK_EQ(s.code, SQ_E_ABI_MISMATCH);
  CHECK(!sq_fs_exists(sq::joinPath(dst, "future.ext").c_str()));

  std::string other = sq::joinPath(src, "other");
  makeExt(other, "other.ext", "1.0", "1.*", "some.other.app");
  CHECK_EQ(extensionInstall(other, dst, host).code, SQ_E_UNSUPPORTED);

  std::string evil = sq::joinPath(src, "evil");
  makeExt(evil, "../evil", "1.0", "1.*");
  CHECK(!extensionInstall(evil, dst, host).ok());

  ScanResult scan;
  REQUIRE(extensionScan(dst, host, scan).ok());
  CHECK_EQ(scan.accepted.size(), size_t{1});
  // A host upgrade past the range invalidates the install on the next scan.
  HostIdentity newer = host;
  newer.abi = SQ_ABI_MAKE(2, 0);
  ScanResult scan2;
  REQUIRE(extensionScan(dst, newer, scan2).ok());
  CHECK_EQ(scan2.accepted.size(), size_t{0});
  CHECK_EQ(scan2.rejected.size(), size_t{1});

  REQUIRE(extensionUninstall(dst, "good.ext").ok());
  CHECK(!sq_fs_exists(installed.c_str()));
  CHECK(isExtensionDirectory(good));
  CHECK(!isExtensionDirectory(root));
  sq_fs_remove_all(root.c_str());
}
