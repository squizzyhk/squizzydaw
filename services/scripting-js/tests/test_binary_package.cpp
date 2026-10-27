// scripting-js: loading script packages from the binary .package container (formats/core:
// header + zlib payload). Bundle payloads are full packages, Xml payloads manifest-only; bare
// JavaScript payloads, skin packages and corrupted containers are refused cleanly.
// Without formats/core in the build the whole binary is reported SKIPPED (exit 77).
#include <cstring>
#include <string>
#include <vector>

#include "mock_document.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/scripting.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"
#if SQJS_HAVE_FORMATS_CORE
#include "sq/formats/core/package.hpp"
#endif

#if SQJS_HAVE_FORMATS_CORE
namespace {

namespace sf = sq::formats;

constexpr sq_guid kPkgCommand = sq::guid("{4C1D7E0B-93A2-4F61-8E2B-5B0F1C7D9A36}");
constexpr sq_guid kPkgHelper = sq::guid("{B7E2A9D4-1F38-4C05-9A6E-2D8C3B4F5E71}");

std::string modulePath() {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("scripting-js") + sq_dylib_suffix());
}
std::string tempDir(const std::string& name) {
  std::string d = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest_scripting_js_binpkg"), name);
  sq_fs_remove_all(d.c_str());
  sq_fs_create_dirs(d.c_str());
  return d;
}

struct Harness {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  sq::Ptr<mock::DocumentManager> docs;
  sq::Ptr<mock::Document> doc;
  sq::Ptr<sq::IScriptEngine> engine;
  sq::Ptr<sq::IObjectRegistry> objects;

  Harness() {
    settings.setBool("Services.scripting-js.LoadBundled", SQ_FALSE);
    settings.setBool("Services.scripting-js.LoadUserPackages", SQ_FALSE);
    docs = sq::Ptr<mock::DocumentManager>::adopt(new mock::DocumentManager());
    doc = sq::Ptr<mock::Document>::adopt(new mock::Document(&reg));
    docs->active = doc;
    host.registerService(&sq::IDocumentManager::iid, docs.get());
    auto lr = loader.load(modulePath());
    if (!lr.ok()) std::printf("load failed: %s\n", lr.message.c_str());
    REQUIRE(lr.ok());
    host.queryService(&sq::IScriptEngine::iid, engine.putVoid());
    host.queryService(&sq::IObjectRegistry::iid, objects.putVoid());
    REQUIRE(engine);
    REQUIRE(objects);
  }
  ~Harness() {
    engine = nullptr;
    objects = nullptr;
    loader.unloadAll(true);
    host.unregisterService(&sq::IDocumentManager::iid, docs.get());
  }
  std::string lastError() {
    char buf[4096] = {};
    uint32_t need = 0;
    engine->getLastError(buf, sizeof buf, &need);
    return buf;
  }
  sq_result runCommand(const char* category, const char* name) {
    sq_command cmd{sizeof(sq_command), category, name, 0};
    for (uint32_t i = 0; i < reg.getClassCount(); ++i) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      if (reg.getClassInfo(i, &ci) != SQ_OK || std::strcmp(ci.category, sq::scripting::kCommandCategory) != 0) continue;
      auto h = sq::createInstance<sq::ICommandHandler>(&reg, ci.class_id);
      if (!h) continue;
      sq_result r = h->interpretCommand(&cmd, nullptr, SQ_TRUE);
      if (r == SQ_E_NOTFOUND) continue;
      if (r != SQ_OK) return r;
      return h->interpretCommand(&cmd, nullptr, SQ_FALSE);
    }
    return SQ_E_NOTFOUND;
  }
};

std::string guidText(const sq_guid& g) {
  char b[SQ_GUID_STRING_SIZE];
  sq_guid_format(&g, b);
  return b;
}

std::string manifest() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<ScriptPackage id=\"test.binary\" name=\"Binary Package\" version=\"2.0.0\" abi=\"1.0\">\n"
         "  <ClassFactory>\n"
         "    <ScriptClass classID=\"" + guidText(kPkgCommand) + "\" category=\"Command\" name=\"Binary: Add Track\"\n"
         "                 sourceFile=\"src/cmd.js\" functionName=\"createCommand\">\n"
         "      <Command category=\"Binary\" name=\"Add Track\" title=\"Add A Track\"/>\n"
         "    </ScriptClass>\n"
         "    <ScriptClass classID=\"" + guidText(kPkgHelper) + "\" name=\"Binary: Helper\"\n"
         "                 sourceFile=\"src/cmd.js\" functionName=\"createHelper\"/>\n"
         "  </ClassFactory>\n"
         "</ScriptPackage>\n";
}

const char* kCmdJs = R"(function createCommand(info) {
  return {
    interfaces: [Host.Interfaces.ICommandHandler],
    interpretCommand(cmd, args, checkOnly) {
      if (cmd.category !== 'Binary' || cmd.name !== 'Add Track') return Host.Results.NOTFOUND;
      const doc = Host.Documents.active();
      if (!doc) return Host.Results.DISABLED;
      if (checkOnly) return Host.Results.OK;
      const label = Host.Package.readText('data/label.txt').trim();
      doc.edit('Add Track', () => { doc.execute(Host.Actions.AddTrack, { kind: 1, name: label }); });
      return Host.Results.OK;
    },
  };
}
function createHelper() { return { getProperty(n) { return n === 'answer' ? 42 : undefined; } }; }
)";

std::vector<sf::FileEntry> bundleFiles(const std::string& prefix = "") {
  return {{prefix + "package.xml", manifest()}, {prefix + "src/cmd.js", kCmdJs}, {prefix + "data/label.txt", "From Binary\n"}};
}

std::string writePackage(const std::string& dir, const std::string& file, const sf::ScriptPackage& p) {
  std::string bytes;
  sf::Status st = sf::packageWrite(p, bytes);
  if (!st.ok()) std::printf("packageWrite: %s\n", st.message.c_str());
  REQUIRE(st.ok());
  std::string path = sq::joinPath(dir, file);
  REQUIRE_OK(sq::writeFile(path, bytes));
  return path;
}

}  // namespace

SQ_TEST("scripting binary .package: a bundle loads, registers classes and its command edits the document") {
  Harness h;
  sf::ScriptPackage p;
  p.kind = sf::PackageKind::Script;
  p.content = sf::PackageContent::Bundle;
  p.files = bundleFiles();
  std::string path = writePackage(tempDir("bundle"), "binary.package", p);
  sq::Ptr<sq::IScriptPackage> pkg;
  sq_result r = h.engine->loadPackage(path.c_str(), pkg.put());
  if (SQ_FAILED(r)) std::printf("loadPackage: %s\n", h.lastError().c_str());
  REQUIRE_OK(r);
  REQUIRE(pkg);
  char id[64] = {};
  uint32_t need = 0;
  CHECK_OK(pkg->getId(id, sizeof id, &need));
  CHECK_EQ(std::string(id), std::string("test.binary"));
  CHECK_EQ(pkg->getClassCount(), 2u);
  REQUIRE_OK(h.runCommand("Binary", "Add Track"));
  REQUIRE_EQ(h.doc->model().tracks.size(), size_t(1));
  CHECK_EQ(h.doc->model().tracks[0].name, std::string("From Binary"));
  auto helper = sq::createInstance<sq::IController>(&h.reg, kPkgHelper);
  REQUIRE(helper);
  auto out = sq::makeAttributes();
  CHECK_OK(helper->getProperty("answer", out.get()));
  CHECK_EQ(out->getInt("value", 0), int64_t(42));
  // package:// reaches files inside the container
  sq::Ptr<sq::IByteStream> s;
  CHECK_OK(h.objects->openStream("package://test.binary/data/label.txt", s.put()));
  REQUIRE(s);
  char buf[64] = {};
  uint32_t got = 0;
  CHECK_OK(s->read(buf, sizeof buf - 1, &got));
  CHECK_EQ(std::string(buf, got), std::string("From Binary\n"));
  s = nullptr;
  // the catalog lists the command; the same package cannot load twice
  CHECK_EQ(h.engine->loadPackage(path.c_str(), nullptr), SQ_E_EXISTS);
  helper = nullptr;
  CHECK_OK(pkg->unload());
}

SQ_TEST("scripting binary .package: one top-level folder in the bundle is tolerated") {
  Harness h;
  sf::ScriptPackage p;
  p.content = sf::PackageContent::Bundle;
  p.files = bundleFiles("wrapped/");
  std::string path = writePackage(tempDir("wrapped"), "wrapped.package", p);
  REQUIRE_OK(h.engine->loadPackage(path.c_str(), nullptr));
  REQUIRE_OK(h.runCommand("Binary", "Add Track"));
  CHECK_EQ(h.doc->model().tracks.size(), size_t(1));
}

SQ_TEST("scripting binary .package: Xml payload is a manifest-only package") {
  Harness h;
  std::string dir = tempDir("xml");
  // ScriptClasses whose sourceFile is not in the container are refused at load (the class could
  // never be instantiated), and nothing is registered.
  sf::ScriptPackage p;
  p.content = sf::PackageContent::Xml;
  p.text = manifest();
  std::string path = writePackage(dir, "manifest.package", p);
  const uint32_t before = h.reg.getClassCount();
  CHECK_EQ(h.engine->loadPackage(path.c_str(), nullptr), SQ_E_PARSE);
  CHECK(h.lastError().find("src/cmd.js") != std::string::npos);
  CHECK_EQ(h.reg.getClassCount(), before);
  // a class-less manifest loads
  sf::ScriptPackage empty;
  empty.content = sf::PackageContent::Xml;
  empty.text = "<ScriptPackage id=\"test.empty\" abi=\"1.0\"><ClassFactory/></ScriptPackage>";
  std::string emptyPath = writePackage(dir, "empty.package", empty);
  sq::Ptr<sq::IScriptPackage> pkg;
  REQUIRE_OK(h.engine->loadPackage(emptyPath.c_str(), pkg.put()));
  CHECK_EQ(pkg->getClassCount(), 0u);
}

SQ_TEST("scripting binary .package: a bundle missing a sourceFile is refused at load") {
  Harness h;
  sf::ScriptPackage p;
  p.content = sf::PackageContent::Bundle;
  p.files = {{"package.xml", manifest()}, {"data/label.txt", "x"}};
  std::string path = writePackage(tempDir("nosrc"), "nosrc.package", p);
  const uint32_t before = h.reg.getClassCount();
  CHECK_EQ(h.engine->loadPackage(path.c_str(), nullptr), SQ_E_PARSE);
  CHECK_EQ(h.reg.getClassCount(), before);
}

SQ_TEST("scripting binary .package: script-only, skin, corrupted and truncated containers are refused") {
  Harness h;
  std::string dir = tempDir("bad");
  sf::ScriptPackage js;
  js.content = sf::PackageContent::JavaScript;
  js.text = kCmdJs;
  std::string jsPath = writePackage(dir, "bare.package", js);
  CHECK_EQ(h.engine->loadPackage(jsPath.c_str(), nullptr), SQ_E_UNSUPPORTED);
  CHECK(h.lastError().find("package.xml") != std::string::npos);

  sf::ScriptPackage skin;
  skin.kind = sf::PackageKind::Skin;
  skin.content = sf::PackageContent::Bundle;
  skin.files = bundleFiles();
  std::string skinPath = writePackage(dir, "skin.package", skin);
  CHECK_EQ(h.engine->loadPackage(skinPath.c_str(), nullptr), SQ_E_UNSUPPORTED);
  CHECK(h.lastError().find("skin") != std::string::npos);

  sf::ScriptPackage good;
  good.content = sf::PackageContent::Bundle;
  good.files = bundleFiles();
  std::string bytes;
  REQUIRE(sf::packageWrite(good, bytes).ok());
  std::string flipped = bytes;
  flipped[flipped.size() / 2] = static_cast<char>(flipped[flipped.size() / 2] ^ 0x5A);
  std::string flipPath = sq::joinPath(dir, "flipped.package");
  REQUIRE_OK(sq::writeFile(flipPath, flipped));
  CHECK(SQ_FAILED(h.engine->loadPackage(flipPath.c_str(), nullptr)));
  std::string cut = sq::joinPath(dir, "cut.package");
  REQUIRE_OK(sq::writeFile(cut, bytes.substr(0, bytes.size() - 7)));
  CHECK(SQ_FAILED(h.engine->loadPackage(cut.c_str(), nullptr)));
  std::string header = sq::joinPath(dir, "header.package");
  REQUIRE_OK(sq::writeFile(header, bytes.substr(0, 12)));
  CHECK(SQ_FAILED(h.engine->loadPackage(header.c_str(), nullptr)));
  CHECK_EQ(h.doc->model().tracks.size(), size_t(0));
}

SQ_TEST("scripting binary .package: picked up from Services.scripting-js.PackagePath at start-up") {
  std::string dir = tempDir("pathroot");
  sf::ScriptPackage p;
  p.content = sf::PackageContent::Bundle;
  p.files = bundleFiles();
  writePackage(dir, "auto.package", p);
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  settings.setBool("Services.scripting-js.LoadBundled", SQ_TRUE);  // start-up scan (also <exe>/scripts)
  settings.setBool("Services.scripting-js.LoadUserPackages", SQ_FALSE);
  settings.setString("Services.scripting-js.PackagePath", dir.c_str());
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  REQUIRE(loader.load(modulePath()).ok());
  sq::Ptr<sq::IScriptEngine> engine;
  host.queryService(&sq::IScriptEngine::iid, engine.putVoid());
  REQUIRE(engine);
  // the package registered its classes at start-up
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  bool found = false;
  for (uint32_t i = 0; i < reg.getClassCount(); ++i)
    if (reg.getClassInfo(i, &ci) == SQ_OK && sq_guid_equal(&ci.class_id, &kPkgCommand)) found = true;
  CHECK(found);
  engine = nullptr;
  loader.unloadAll(true);
}

#else

SQ_TEST("scripting binary .package: formats/core not in this build") {
  SQ_SKIP("scripting-js built without formats/core: binary .package containers are unsupported in this build");
}

#endif
