// scripting-js tests: loads the real service through the app's ModuleLoader, with a mock
// document model (mock_document.hpp) standing in for the engine and a mock codec.
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "miniz.h"
#include "mock_document.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/scripting.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {

constexpr sq_guid kToneCodecClass = sq::guid("{30D007E9-3E6B-4AC5-8B57-B45CE3E8854A}");
constexpr sq_guid kUserCommand = sq::guid("{91B3835F-F24D-4839-A831-BCABB88E738B}");
constexpr sq_guid kUserAction = sq::guid("{6F667F3D-8637-4495-BFDB-A5B603F4357F}");
constexpr sq_guid kUserObserver = sq::guid("{2DF9AACC-BA9E-4546-9784-D9E831538705}");
constexpr sq_guid kUserBroken = sq::guid("{0018E58A-DEA7-4A28-B4FC-9CFC4D221D83}");
constexpr sq_guid kMacroActionSequence = sq::guid("{5F785F64-52F8-4381-AFF1-036C1C60E9F5}");

std::string str(const sq_guid& g) {
  char b[SQ_GUID_STRING_SIZE];
  sq_guid_format(&g, b);
  return b;
}

std::string modulePath() {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("scripting-js") + sq_dylib_suffix());
}

std::string tempDir(const std::string& name) {
  std::string d = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest_scripting_js"), name);
  sq_fs_remove_all(d.c_str());
  sq_fs_create_dirs(d.c_str());
  return d;
}

void put(const std::string& dir, const std::string& file, const std::string& text) {
  REQUIRE_OK(sq::writeFile(sq::joinPath(dir, file), text));
}

// A host with the real scripting-js module, a mock document manager and the mock codec.
struct Harness {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  sq::Ptr<mock::DocumentManager> docs;
  sq::Ptr<mock::Document> doc;
  sq::Ptr<sq::IScriptEngine> engine;
  sq::Ptr<sq::IObjectRegistry> objects;

  explicit Harness(bool bundled) {
    settings.setBool("Services.scripting-js.LoadBundled", bundled ? SQ_TRUE : SQ_FALSE);
    settings.setBool("Services.scripting-js.LoadUserPackages", SQ_FALSE);
    settings.setInt("Engine.BlockSize", 256);
    docs = sq::Ptr<mock::DocumentManager>::adopt(new mock::DocumentManager());
    doc = sq::Ptr<mock::Document>::adopt(new mock::Document(&reg));
    docs->active = doc;
    host.registerService(&sq::IDocumentManager::iid, docs.get());
    auto factory = sq::Ptr<sq::IClassFactory>::adopt(new mock::SimpleFactory<mock::ToneCodec, sq::ICodecService>(kToneCodecClass));
    sq_class_info ci{sizeof(sq_class_info), kToneCodecClass, "Codec", "Tone test codec", 0, 0};
    reg.registerClass(&ci, factory.get());
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
    reg.unregisterClass(&kToneCodecClass);
  }

  std::string lastError() {
    char buf[8192] = {};
    uint32_t need = 0;
    engine->getLastError(buf, sizeof buf, &need);
    return buf;
  }

  // evaluate() returning the "value" as text ("" if none); REQUIREs success.
  std::string eval(const std::string& src) {
    auto r = sq::makeAttributes();
    sq_result code = engine->evaluate(src.c_str(), "test.js", r.get());
    if (code != SQ_OK) std::printf("evaluate failed (%d): %s\n", code, lastError().c_str());
    REQUIRE_OK(code);
    switch (r->getType("value")) {
      case SQ_VALUE_STRING: return sq::getAttrString(r.get(), "value");
      case SQ_VALUE_DOUBLE: {
        char b[64];
        std::snprintf(b, sizeof b, "%g", r->getDouble("value", 0));
        return b;
      }
      case SQ_VALUE_BOOL: return r->getBool("value", SQ_FALSE) ? "true" : "false";
      default: return "";
    }
  }

  // What the app does for a menu item: find the "Command" class that handles it, check, run.
  sq_result runCommand(const char* category, const char* name, sq::IAttributes* args, bool checkOnly = false) {
    sq_command cmd{sizeof(sq_command), category, name, 0};
    for (uint32_t i = 0; i < reg.getClassCount(); ++i) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      if (reg.getClassInfo(i, &ci) != SQ_OK || std::strcmp(ci.category, sq::scripting::kCommandCategory) != 0) continue;
      auto h = sq::createInstance<sq::ICommandHandler>(&reg, ci.class_id);
      if (!h) continue;
      sq_result r = h->interpretCommand(&cmd, args, SQ_TRUE);
      if (r == SQ_E_NOTFOUND) continue;
      if (r != SQ_OK || checkOnly) return r;
      return h->interpretCommand(&cmd, args, SQ_FALSE);
    }
    return SQ_E_NOTFOUND;
  }

  mock::Model& model() { return doc->model(); }
  sq::IEditor* editor() { return doc->editor(); }
};

std::string userPackage() {
  std::string dir = tempDir("user_pkg");
  put(dir, "package.xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<ScriptPackage id="test.user" name="User Test" version="1.2.3" abi="1.0">
  <ClassFactory>
    <ScriptClass classID="{91B3835F-F24D-4839-A831-BCABB88E738B}" category="Command" name="User: Add Drum Tracks"
                 sourceFile="cmd.js" functionName="createCommand">
      <Command category="User" name="Add Drums" title="Add Drum Tracks"/>
    </ScriptClass>
    <ScriptClass classID="{6F667F3D-8637-4495-BFDB-A5B603F4357F}" category="Action" name="User: Number Tracks"
                 sourceFile="action.js" functionName="createAction"/>
    <ScriptClass classID="{2DF9AACC-BA9E-4546-9784-D9E831538705}" name="User: Observer"
                 sourceFile="cmd.js" functionName="createObserver"/>
    <ScriptClass classID="{0018E58A-DEA7-4A28-B4FC-9CFC4D221D83}" category="Command" name="User: Broken"
                 sourceFile="broken.js" functionName="createBroken">
      <Command category="User" name="Explode"/>
      <Command category="User" name="Spin"/>
    </ScriptClass>
  </ClassFactory>
</ScriptPackage>
)");
  put(dir, "cmd.js", R"(// user command: adds drum tracks and moves the first clip, as one undo step
function createCommand(info) {
  return {
    interfaces: [Host.Interfaces.ICommandHandler],
    info: info,
    interpretCommand(cmd, args, checkOnly) {
      if (cmd.category !== 'User' || cmd.name !== 'Add Drums') return Host.Results.NOTFOUND;
      const doc = Host.Documents.active();
      if (!doc) return Host.Results.DISABLED;
      if (checkOnly) return Host.Results.OK;
      const count = args && args.count ? args.count : 2;
      doc.edit('Add Drum Tracks', () => {
        for (let i = 0; i < count; ++i) {
          const r = doc.execute(Host.Actions.AddTrack, { kind: 1, name: 'tmp' });
          doc.execute(Host.Actions.SetProperty, { node: r.node, key: 'name', value: 'Drums ' + (i + 1) });
        }
        const first = doc.track(0);
        if (first.clipCount > 0) {
          const c = first.clip(0);
          doc.execute(Host.Actions.MoveClip, { node: c.id, start: Host.Values.double(c.start + 1) });
        }
      });
      return Host.Results.OK;
    },
  };
}

var notifications = [];
function createObserver() {
  return {
    onNotify(subject, topic, args) { notifications.push(topic + ':' + (args ? args.reason : '')); },
    getProperty(name) { return name === 'count' ? notifications.length : undefined; },
  };
}
)");
  put(dir, "action.js", R"(function createAction() {
  return {
    perform(doc, editor, params, result) {
      const n = doc.trackCount;
      for (let i = 0; i < n; ++i)
        editor.execute(Host.Actions.SetProperty, { node: doc.track(i).id, key: 'name', value: params.prefix + (i + 1) });
      result.renamed = n;
    },
  };
}
)");
  put(dir, "broken.js", R"(function createBroken() {
  return Host.Commands.handler([
    { category: 'User', name: 'Explode', run(ctx) { ctx.doc.execute(Host.Actions.AddTrack, { kind: 1, name: 'half' }); throw new Error('boom'); } },
    { category: 'User', name: 'Spin', run(ctx) { ctx.doc.execute(Host.Actions.AddTrack, { kind: 1, name: 'spin' }); for (;;) {} } },
  ]);
}
)");
  return dir;
}

}  // namespace

// =====================================================================================================
SQ_TEST("scripting: service loads, evaluates and reports errors with file:line") {
  Harness h(false);
  char info[256];
  uint32_t need = 0;
  REQUIRE_OK(h.engine->getEngineInfo(info, sizeof info, &need));
  CHECK(std::string(info).find("QuickJS-ng") != std::string::npos);

  CHECK_EQ(h.eval("1 + 2"), std::string("3"));
  CHECK_EQ(h.eval("'abc'.toUpperCase()"), std::string("ABC"));
  CHECK_EQ(h.eval("typeof Host.Classes.createInstance"), std::string("function"));
  CHECK_EQ(h.eval("Host.Interfaces.ICommandHandler"), std::string("{E03E59EA-39AF-45D9-B69F-C14833664CEE}"));

  CHECK_EQ(h.engine->evaluate("\n\nthrow new TypeError('bad thing');", "err.js", nullptr), SQ_E_FAIL);
  std::string e = h.lastError();
  CHECK(e.find("err.js:3") != std::string::npos);
  CHECK(e.find("TypeError: bad thing") != std::string::npos);

  CHECK_EQ(h.engine->evaluate("let x = ;", "syn.js", nullptr), SQ_E_PARSE);
  e = h.lastError();
  CHECK(e.find("SyntaxError") != std::string::npos);
  CHECK(e.find("syn.js:1") != std::string::npos);

  // object results come back as IController wrappers
  auto r = sq::makeAttributes();
  REQUIRE_OK(h.engine->evaluate("({ greeting: 'hi', nested: { n: 5 } })", "obj.js", r.get()));
  sq::Ptr<sq::IController> ctl;
  REQUIRE_OK(r->getObject("value", &sq::IController::iid, ctl.putVoid()));
  auto out = sq::makeAttributes();
  REQUIRE_OK(ctl->getProperty("greeting", out.get()));
  CHECK_EQ(sq::getAttrString(out.get(), "value"), std::string("hi"));
  auto in = sq::makeAttributes();
  in->setInt("value", 9);
  REQUIRE_OK(ctl->setProperty("count", in.get()));
  REQUIRE_OK(ctl->getProperty("count", out.get()));
  CHECK_EQ(out->getInt("value", 0), 9);
  CHECK_EQ(ctl->getProperty("missing", out.get()), SQ_E_NOTFOUND);
  sq::Ptr<sq::IController> nested;
  REQUIRE_OK(ctl->getChild("nested", &sq::IController::iid, nested.putVoid()));
  REQUIRE_OK(nested->getProperty("n", out.get()));
  CHECK_EQ(out->getInt("value", 0), 5);
}

SQ_TEST("scripting: time limit stops runaway scripts and the engine stays usable") {
  Harness h(false);
  h.settings.setInt("Services.scripting-js.TimeLimitMs", 200);
  auto t0 = std::chrono::steady_clock::now();
  CHECK_EQ(h.engine->evaluate("for (;;) {}", "loop.js", nullptr), SQ_E_FAIL);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 5000);
  CHECK(h.lastError().find("time limit") != std::string::npos);
  // try/catch cannot swallow the interrupt
  CHECK_EQ(h.engine->evaluate("try { for (;;) {} } catch (e) { 'caught' }", "loop2.js", nullptr), SQ_E_FAIL);
  CHECK_EQ(h.eval("40 + 2"), std::string("42"));
}

SQ_TEST("scripting: memory limit and stack overflow are contained") {
  Harness h(false);
  h.settings.setInt("Services.scripting-js.MemoryLimitMB", 32);
  CHECK_EQ(h.engine->evaluate("(function(){ const a = []; for (;;) a.push(new Array(100000).fill(1.5)); })()", "oom.js", nullptr), SQ_E_FAIL);
  CHECK(h.lastError().find("memory") != std::string::npos);
  CHECK_EQ(h.eval("[1,2,3].length"), std::string("3"));
  CHECK_EQ(h.engine->evaluate("(function f(){ return f() + 1; })()", "deep.js", nullptr), SQ_E_FAIL);
  CHECK(h.lastError().find("stack") != std::string::npos);
  CHECK_EQ(h.eval("'still ' + 'alive'"), std::string("still alive"));
}

SQ_TEST("scripting: settings mirror object://settings/ (+ object://app/settings/) both ways") {
  Harness h(false);
  CHECK_EQ(h.eval("Host.Settings.set('Test.Script.Value', 42); "
                  "[Host.Settings.get('Test.Script.Value'),"
                  " Host.Objects.getObjectByUrl('object://settings/Engine').get('BlockSize'),"
                  " Host.Objects.getObjectByUrl('object://app/settings/Test.Script.Value').get('value'),"
                  " Host.Settings.get('Test.Nope', 'dflt')].join()"),
           std::string("42,256,42,dflt"));
  CHECK_EQ(h.settings.getInt("Test.Script.Value", 0), 42);

  sq::Ptr<sq::IController> node;
  REQUIRE_OK(h.objects->resolve("object://settings/Test.Script.Value", &sq::IController::iid, node.putVoid()));
  auto a = sq::makeAttributes();
  REQUIRE_OK(node->getProperty("value", a.get()));
  CHECK_EQ(a->getInt("value", 0), 42);
  a->setDouble("value", 1.5);
  REQUIRE_OK(node->setProperty("value", a.get()));
  CHECK(std::fabs(h.settings.getDouble("Test.Script.Value", 0) - 1.5) < 1e-12);

  // observers: callbacks for changes made from native code, stoppable
  h.eval("var seen = []; var stop = Host.Settings.observe('Test.Obs', (k) => seen.push(k));");
  h.settings.setInt("Test.Obs.A", 1);
  CHECK_EQ(h.eval("seen.join()"), std::string("Test.Obs.A"));
  h.eval("stop();");
  h.settings.setInt("Test.Obs.B", 2);
  CHECK_EQ(h.eval("seen.length"), std::string("1"));

  // engine controller
  sq::Ptr<sq::IController> eng;
  REQUIRE_OK(h.objects->resolve("object://scripting", &sq::IController::iid, eng.putVoid()));
  REQUIRE_OK(eng->getProperty("info", a.get()));
  CHECK(sq::getAttrString(a.get(), "value").find("QuickJS-ng") != std::string::npos);
}

SQ_TEST("scripting: M6 exit - a user script adds a menu command that edits the document with full undo") {
  Harness h(false);
  mock::MTrack& bass = h.doc->addTrack("Bass");
  mock::Document::addClip(bass, "Riff", 0.0, 4.0);
  const sq_guid clipId = bass.clips[0].id;

  std::string dir = userPackage();
  sq::Ptr<sq::IScriptPackage> pkg;
  sq_result lr = h.engine->loadPackage(dir.c_str(), pkg.put());
  if (lr != SQ_OK) std::printf("%s\n", h.lastError().c_str());
  REQUIRE_OK(lr);
  CHECK_EQ(pkg->getClassCount(), 4u);
  char id[64];
  uint32_t need = 0;
  REQUIRE_OK(pkg->getId(id, sizeof id, &need));
  CHECK_EQ(std::string(id), std::string("test.user"));

  // Same GUID registry as native classes, with the package's categories.
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(h.reg.findClass(&kUserCommand, &ci));
  CHECK_EQ(std::string(ci.category), std::string("Command"));
  CHECK_EQ(std::string(ci.name), std::string("User: Add Drum Tracks"));
  REQUIRE_OK(h.reg.findClass(&kUserObserver, &ci));
  CHECK_EQ(std::string(ci.category), std::string("ScriptClass"));
  // command catalog for menus
  sq::Ptr<sq::IController> eng;
  REQUIRE_OK(h.objects->resolve("object://scripting", &sq::IController::iid, eng.putVoid()));
  auto a = sq::makeAttributes();
  REQUIRE_OK(eng->getProperty("commands", a.get()));
  CHECK(sq::getAttrString(a.get(), "value").find("User\tAdd Drums\tAdd Drum Tracks\t" + str(kUserCommand)) != std::string::npos);

  // the app's menu dispatch
  CHECK_EQ(h.runCommand("User", "Add Drums", nullptr, true), SQ_OK);
  CHECK_EQ(h.runCommand("User", "No Such", nullptr, true), SQ_E_NOTFOUND);
  auto args = sq::makeAttributes();
  args->setInt("count", 3);
  REQUIRE_OK(h.runCommand("User", "Add Drums", args.get()));

  REQUIRE_EQ(h.model().tracks.size(), size_t(4));
  CHECK_EQ(h.model().tracks[1].name, std::string("Drums 1"));
  CHECK_EQ(h.model().tracks[3].name, std::string("Drums 3"));
  CHECK_EQ(h.model().clip(clipId)->start, 1.0);
  CHECK_EQ(h.editor()->getUndoCount(), 1u);
  char desc[128];
  REQUIRE_OK(h.editor()->getUndoDescription(0, desc, sizeof desc, &need));
  CHECK_EQ(std::string(desc), std::string("Add Drum Tracks"));
  CHECK(!h.editor()->inTransaction());

  REQUIRE_OK(h.editor()->undo());
  REQUIRE_EQ(h.model().tracks.size(), size_t(1));
  CHECK_EQ(h.model().tracks[0].name, std::string("Bass"));
  CHECK_EQ(h.model().clip(clipId)->start, 0.0);
  REQUIRE_OK(h.editor()->redo());
  CHECK_EQ(h.model().tracks.size(), size_t(4));
  CHECK_EQ(h.model().clip(clipId)->start, 1.0);

  // callFactory directly
  sq::Ptr<sq::ICommandHandler> direct;
  REQUIRE_OK(h.engine->callFactory("test.user", "cmd.js", "createCommand", &sq::ICommandHandler::iid, direct.putVoid()));
  CHECK_EQ(h.engine->callFactory("test.user", "cmd.js", "noSuchFn", &sq::ICommandHandler::iid, direct.putVoid()), SQ_E_NOTFOUND);
  // a script object only answers the interfaces it declares
  sq::Ptr<sq::IAction> notAction;
  CHECK_EQ(h.reg.createInstance(&kUserCommand, &sq::IAction::iid, notAction.putVoid()), SQ_E_NOINTERFACE);
}

SQ_TEST("scripting: script exceptions and timeouts are isolated; the transaction is rolled back") {
  Harness h(false);
  h.doc->addTrack("Keep");
  std::string dir = userPackage();
  REQUIRE_OK(h.engine->loadPackage(dir.c_str(), nullptr));

  CHECK_EQ(h.runCommand("User", "Explode", nullptr), SQ_E_FAIL);
  std::string e = h.lastError();
  CHECK(e.find("boom") != std::string::npos);
  CHECK(e.find("package://test.user/broken.js:3") != std::string::npos);
  CHECK_EQ(h.model().tracks.size(), size_t(1));
  CHECK(!h.editor()->inTransaction());
  CHECK_EQ(h.editor()->getUndoCount(), 0u);

  h.settings.setInt("Services.scripting-js.TimeLimitMs", 250);
  CHECK_EQ(h.runCommand("User", "Spin", nullptr), SQ_E_FAIL);
  CHECK(h.lastError().find("time limit") != std::string::npos);
  CHECK_EQ(h.model().tracks.size(), size_t(1));
  CHECK(!h.editor()->inTransaction());
  CHECK_EQ(h.editor()->getUndoCount(), 0u);

  // the package keeps working afterwards
  h.settings.setInt("Services.scripting-js.TimeLimitMs", 10000);
  REQUIRE_OK(h.runCommand("User", "Add Drums", nullptr));
  CHECK_EQ(h.model().tracks.size(), size_t(3));
}

SQ_TEST("scripting: script edit task (IAction) runs through IEditor::execute, undoable") {
  Harness h(false);
  h.doc->addTrack("a");
  h.doc->addTrack("b");
  REQUIRE_OK(h.engine->loadPackage(userPackage().c_str(), nullptr));
  auto p = sq::makeAttributes();
  p->setString("prefix", "Trk ");
  auto res = sq::makeAttributes();
  REQUIRE_OK(h.editor()->execute(&kUserAction, p.get(), res.get()));
  CHECK_EQ(res->getInt("renamed", 0), 2);
  CHECK_EQ(h.model().tracks[0].name, std::string("Trk 1"));
  CHECK_EQ(h.model().tracks[1].name, std::string("Trk 2"));
  CHECK_EQ(h.editor()->getUndoCount(), 1u);
  REQUIRE_OK(h.editor()->undo());
  CHECK_EQ(h.model().tracks[0].name, std::string("a"));
}

SQ_TEST("scripting: observers, native classes from script, package:// and unload rules") {
  Harness h(false);
  std::string dir = userPackage();
  sq::Ptr<sq::IScriptPackage> pkg;
  REQUIRE_OK(h.engine->loadPackage(dir.c_str(), pkg.put()));
  CHECK_EQ(h.engine->loadPackage(dir.c_str(), nullptr), SQ_E_EXISTS);

  // IObserver implemented in script
  sq::Ptr<sq::IObserver> obs = sq::createInstance<sq::IObserver>(&h.reg, kUserObserver);
  REQUIRE(obs);
  auto args = sq::makeAttributes();
  args->setString("reason", "commit");
  obs->onNotify(nullptr, "document.changed", args.get());
  obs->onNotify(nullptr, "document.saved", nullptr);
  auto ctl = obs.as<sq::IController>();
  REQUIRE(ctl);
  auto a = sq::makeAttributes();
  REQUIRE_OK(ctl->getProperty("count", a.get()));
  CHECK_EQ(a->getInt("value", 0), 2);

  // native classes from script
  CHECK_EQ(h.eval("Host.Classes.createInstance('" + str(kToneCodecClass) + "').supports(Host.Interfaces.ICodecService)"),
           std::string("true"));
  CHECK_EQ(h.eval("Host.Classes.list('Codec').length >= 1"), std::string("true"));
  CHECK_EQ(h.eval("Host.Documents.active().name"), std::string("Mock Song"));

  // package:// resources
  sq::Ptr<sq::IByteStream> s;
  REQUIRE_OK(h.objects->openStream("package://test.user/action.js", s.put()));
  char buf[16] = {};
  uint32_t got = 0;
  REQUIRE_OK(s->read(buf, 8, &got));
  CHECK_EQ(std::string(buf, got), std::string("function"));
  CHECK_EQ(h.objects->openStream("package://test.user/../secret.txt", s.put()), SQ_E_ACCESS);
  CHECK_EQ(h.eval("Host.Objects.readText('package://test.user/action.js').startsWith('function')"), std::string("true"));

  // unload refuses while a script object is alive, then removes the classes
  CHECK_EQ(pkg->unload(), SQ_E_BUSY);
  obs = nullptr;
  ctl = nullptr;
  REQUIRE_OK(pkg->unload());
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  CHECK_EQ(h.reg.findClass(&kUserCommand, &ci), SQ_E_NOTFOUND);
  CHECK_EQ(h.reg.findClass(&kUserBroken, &ci), SQ_E_NOTFOUND);
  // and it can be loaded again
  REQUIRE_OK(h.engine->loadPackage(dir.c_str(), nullptr));
}

SQ_TEST("scripting: packages can be ZIP archives (.package)") {
  Harness h(false);
  h.doc->addTrack("x");
  std::string src = userPackage();
  std::string zipPath = sq::joinPath(tempDir("zip"), "user.package");
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  REQUIRE(mz_zip_writer_init_file(&zip, zipPath.c_str(), 0));
  for (const char* f : {"package.xml", "cmd.js", "action.js", "broken.js"}) {
    auto data = sq::readFile(sq::joinPath(src, f));
    REQUIRE(data.has_value());
    std::string name = std::string("user/") + f;  // wrapped in a top-level folder
    REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data->data(), data->size(), static_cast<mz_uint>(MZ_BEST_SPEED)));
  }
  REQUIRE(mz_zip_writer_finalize_archive(&zip));
  mz_zip_writer_end(&zip);
  REQUIRE_OK(h.engine->loadPackage(zipPath.c_str(), nullptr));
  REQUIRE_OK(h.runCommand("User", "Add Drums", nullptr));
  CHECK_EQ(h.model().tracks.size(), size_t(3));
  std::string notZip = sq::joinPath(tempDir("notzip"), "junk.package");
  REQUIRE_OK(sq::writeFile(notZip, "PACKAGE\x01not really"));
  CHECK_EQ(h.engine->loadPackage(notZip.c_str(), nullptr), SQ_E_UNSUPPORTED);
}

SQ_TEST("scripting: bad packages are rejected cleanly") {
  Harness h(false);
  std::string d1 = tempDir("bad_abi");
  put(d1, "package.xml",
      R"(<ScriptPackage id="t.abi" abi="99.0"><ClassFactory><ScriptClass classID="{065E8DB3-789E-495A-8829-2FF292BA9548}" sourceFile="a.js" functionName="f"/></ClassFactory></ScriptPackage>)");
  CHECK_EQ(h.engine->loadPackage(d1.c_str(), nullptr), SQ_E_ABI_MISMATCH);
  std::string d2 = tempDir("bad_xml");
  put(d2, "package.xml", "<ScriptPackage id=\"t.x\"><ClassFactory>");
  CHECK_EQ(h.engine->loadPackage(d2.c_str(), nullptr), SQ_E_PARSE);
  std::string d3 = tempDir("dup_class");  // class id already taken by a native class
  put(d3, "package.xml",
      "<ScriptPackage id=\"t.dup\"><ClassFactory><ScriptClass classID=\"" + str(kToneCodecClass) +
          "\" sourceFile=\"a.js\" functionName=\"f\"/></ClassFactory></ScriptPackage>");
  CHECK_EQ(h.engine->loadPackage(d3.c_str(), nullptr), SQ_E_PARSE);  // a.js missing: refused before registering
  put(d3, "a.js", "function f() { return {}; }");
  CHECK_EQ(h.engine->loadPackage(d3.c_str(), nullptr), SQ_E_EXISTS);
  CHECK_EQ(h.engine->loadPackage("Z:/definitely/not/here", nullptr), SQ_E_NOTFOUND);
  std::string d4 = tempDir("bad_path");
  put(d4, "package.xml",
      R"(<ScriptPackage id="t.path"><ClassFactory><ScriptClass classID="{52B7F17E-B0D3-4B54-A2F8-BB6460251A82}" sourceFile="../x.js" functionName="f"/></ClassFactory></ScriptPackage>)");
  CHECK_EQ(h.engine->loadPackage(d4.c_str(), nullptr), SQ_E_PARSE);
}

SQ_TEST("scripting: bundled packages load and their commands edit the document undoably") {
  Harness h(true);
  sq::Ptr<sq::IController> eng;
  REQUIRE_OK(h.objects->resolve("object://scripting", &sq::IController::iid, eng.putVoid()));
  auto a = sq::makeAttributes();
  REQUIRE_OK(eng->getProperty("packages", a.get()));
  const std::string pkgs = sq::getAttrString(a.get(), "value");
  for (const char* p : {"squizzy.audioedit", "squizzy.musicedit", "squizzy.projectedit", "squizzy.trackedit", "squizzy.toolset",
                        "squizzy.macrohandler"})
    CHECK(pkgs.find(p) != std::string::npos);

  mock::MTrack& gtr = h.doc->addTrack("Gtr");
  mock::MClip& audio = mock::Document::addClip(gtr, "Take", 0.0, 4.0);
  audio.source = "C:/media/take.tone";
  const sq_guid gtrId = gtr.id, audioId = audio.id;
  mock::MTrack& keys = h.doc->addTrack("Keys", SQ_TRACK_INSTRUMENT);
  mock::MClip& midi = mock::Document::addClip(keys, "Chords", 0.0, 4.0, SQ_CLIP_MIDI);
  for (auto [pos, key] : std::vector<std::pair<double, int>>{{0.1, 60}, {1.05, 64}, {2.0, 67}}) {
    sq_clip_event ev{};
    ev.struct_size = sizeof ev;
    ev.position = pos;
    ev.length = 0.25;
    ev.bytes[0] = 0x90;
    ev.bytes[1] = static_cast<uint8_t>(key);
    ev.bytes[2] = 100;
    ev.size = 3;
    midi.events.push_back(ev);
  }
  const sq_guid keysId = keys.id, midiId = midi.id;
  h.doc->addTrack("Empty");
  auto& m = h.model();
  auto sel = [](const sq_guid& g) {
    auto x = sq::makeAttributes();
    x->setString("selection", str(g).c_str());
    return x;
  };
  uint32_t undo = 0;
  auto stepped = [&]() {
    uint32_t n = h.editor()->getUndoCount();
    bool one = n == undo + 1;
    undo = n;
    return one;
  };

  // trackedit
  auto args = sq::makeAttributes();
  args->setString("pattern", "T{nn} {name}");
  REQUIRE_OK(h.runCommand("Track", "Rename With Pattern", args.get()));
  CHECK_EQ(m.tracks[0].name, std::string("T01 Gtr"));
  CHECK_EQ(m.tracks[2].name, std::string("T03 Empty"));
  CHECK(stepped());
  REQUIRE_OK(h.editor()->undo());
  CHECK_EQ(m.tracks[0].name, std::string("Gtr"));
  undo = h.editor()->getUndoCount();

  CHECK_EQ(h.runCommand("Track", "Duplicate", nullptr, true), SQ_COMMAND_DISABLED);  // nothing selected
  REQUIRE_OK(h.runCommand("Track", "Duplicate", sel(gtrId).get()));
  REQUIRE_EQ(m.tracks.size(), size_t(4));
  CHECK_EQ(m.tracks[1].name, std::string("Gtr copy"));
  REQUIRE_EQ(m.tracks[1].clips.size(), size_t(1));
  CHECK_EQ(m.tracks[1].clips[0].source, std::string("C:/media/take.tone"));
  CHECK_EQ(m.tracks[1].clips[0].name, std::string("Take"));
  CHECK(stepped());
  REQUIRE_OK(h.editor()->undo());
  CHECK_EQ(m.tracks.size(), size_t(3));
  undo = h.editor()->getUndoCount();

  REQUIRE_OK(h.runCommand("Track", "Remove Empty", nullptr));
  CHECK_EQ(m.tracks.size(), size_t(2));
  CHECK(stepped());

  // toolset
  args = sel(audioId);
  args->setDouble("amount", 0.5);
  REQUIRE_OK(h.runCommand("Edit", "Nudge Right", args.get()));
  CHECK_EQ(m.clip(audioId)->start, 0.5);
  CHECK(stepped());
  REQUIRE_OK(h.runCommand("Edit", "Nudge Left", args.get()));
  CHECK_EQ(m.clip(audioId)->start, 0.0);
  CHECK(stepped());
  CHECK_EQ(h.runCommand("Edit", "Nudge Left", args.get()), SQ_S_FALSE);  // already at 0: no undo step
  CHECK_EQ(h.editor()->getUndoCount(), undo);
  CHECK_EQ(h.runCommand("Edit", "Split At Position", sel(audioId).get(), true), SQ_COMMAND_DISABLED);  // no position
  args = sel(audioId);
  args->setDouble("position", 2.0);
  REQUIRE_OK(h.runCommand("Edit", "Split At Position", args.get()));
  REQUIRE_EQ(m.track(gtrId)->clips.size(), size_t(2));
  CHECK_EQ(m.track(gtrId)->clips[1].start, 2.0);
  CHECK(stepped());
  REQUIRE_OK(h.editor()->undo());
  undo = h.editor()->getUndoCount();
  REQUIRE_OK(h.runCommand("Edit", "Toggle Clip Mute", sel(audioId).get()));
  CHECK((m.clip(audioId)->flags & SQ_CLIP_MUTED) != 0);
  REQUIRE_OK(h.runCommand("Edit", "Toggle Clip Mute", sel(audioId).get()));
  CHECK((m.clip(audioId)->flags & SQ_CLIP_MUTED) == 0);
  undo = h.editor()->getUndoCount();

  // audioedit: the peak comes from the (mock) codec service: 0.5 = -6.02 dBFS
  args = sel(audioId);
  args->setDouble("target", -1.0);
  REQUIRE_OK(h.runCommand("Audio", "Normalize", args.get()));
  CHECK(std::fabs(m.clip(audioId)->gainDb - 5.02) < 0.011);
  CHECK(stepped());
  REQUIRE_OK(h.runCommand("Audio", "Gain Up", sel(audioId).get()));
  CHECK(std::fabs(m.clip(audioId)->gainDb - 6.02) < 0.011);
  REQUIRE_OK(h.runCommand("Audio", "Reset Gain", sel(audioId).get()));
  CHECK_EQ(m.clip(audioId)->gainDb, 0.0);
  REQUIRE_OK(h.runCommand("Audio", "Apply Fades", sel(audioId).get()));
  CHECK(std::fabs(m.clip(audioId)->fadeIn - 0.005) < 1e-12);
  CHECK(std::fabs(m.clip(audioId)->fadeOut - 0.005) < 1e-12);
  CHECK_EQ(h.runCommand("Audio", "Normalize", sel(midiId).get(), true), SQ_COMMAND_DISABLED);  // MIDI only
  undo = h.editor()->getUndoCount();

  // musicedit
  args = sel(midiId);
  args->setInt("semitones", 2);
  REQUIRE_OK(h.runCommand("Music", "Transpose", args.get()));
  CHECK_EQ(int(m.clip(midiId)->events[0].bytes[1]), 62);
  CHECK_EQ(int(m.clip(midiId)->events[2].bytes[1]), 69);
  CHECK(stepped());
  args = sel(keysId);  // a selected track selects its clips
  args->setDouble("grid", 0.5);
  REQUIRE_OK(h.runCommand("Music", "Quantize", args.get()));
  CHECK(std::fabs(m.clip(midiId)->events[0].position - 0.0) < 1e-9);
  CHECK(std::fabs(m.clip(midiId)->events[1].position - 1.0) < 1e-9);
  CHECK(std::fabs(m.clip(midiId)->events[2].position - 2.0) < 1e-9);
  REQUIRE_OK(h.runCommand("Music", "Legato", sel(midiId).get()));
  CHECK(std::fabs(m.clip(midiId)->events[0].length - 1.0) < 1e-9);
  CHECK(std::fabs(m.clip(midiId)->events[1].length - 1.0) < 1e-9);
  CHECK(std::fabs(m.clip(midiId)->events[2].length - 0.25) < 1e-9);
  REQUIRE_OK(h.runCommand("Music", "Octave Down", sel(midiId).get()));
  CHECK_EQ(int(m.clip(midiId)->events[0].bytes[1]), 50);
  undo = h.editor()->getUndoCount();

  // projectedit
  args = sq::makeAttributes();
  args->setDouble("position", 8.0);
  args->setString("name", "Chorus");
  REQUIRE_OK(h.runCommand("Project", "Insert Marker", args.get()));
  REQUIRE_EQ(m.markers.size(), size_t(1));
  CHECK_EQ(std::string(m.markers[0].name), std::string("Chorus"));
  REQUIRE_OK(h.runCommand("Project", "Markers From Clips", sel(audioId).get()));
  CHECK_EQ(m.markers.size(), size_t(2));
  CHECK_EQ(std::string(m.markers[0].name), std::string("Take"));
  args = sq::makeAttributes();
  args->setDouble("bpm", 100.0);
  REQUIRE_OK(h.runCommand("Project", "Set Tempo", args.get()));
  CHECK_EQ(m.tempos[0].bpm, 100.0);
  args = sq::makeAttributes();
  args->setDouble("factor", 1.5);
  REQUIRE_OK(h.runCommand("Project", "Scale Tempo", args.get()));
  CHECK_EQ(m.tempos[0].bpm, 150.0);
  undo = h.editor()->getUndoCount();

  // macrohandler: two commands from two packages = one undo step
  args = sel(audioId);
  args->setString("steps",
                  R"([{"category":"Edit","name":"Nudge Right","args":{"amount":1}},)"
                  R"( {"category":"Track","name":"Rename With Pattern","args":{"pattern":"M{n}"}}])");
  REQUIRE_OK(h.runCommand("Macro", "Run", args.get()));
  CHECK_EQ(m.clip(audioId)->start, 1.0);
  CHECK_EQ(m.tracks[0].name, std::string("M1"));
  CHECK(stepped());
  char desc[128];
  uint32_t need = 0;
  REQUIRE_OK(h.editor()->getUndoDescription(0, desc, sizeof desc, &need));
  CHECK_EQ(std::string(desc), std::string("Run Macro"));
  REQUIRE_OK(h.editor()->undo());
  CHECK_EQ(m.clip(audioId)->start, 0.0);
  CHECK_EQ(m.tracks[0].name, std::string("Gtr"));
  undo = h.editor()->getUndoCount();
  // a failing step cancels the whole macro
  args = sel(audioId);
  args->setString("steps", R"([{"category":"Edit","name":"Nudge Right"},{"category":"Nope","name":"Nothing"}])");
  CHECK_EQ(h.runCommand("Macro", "Run", args.get()), SQ_E_FAIL);
  CHECK(h.lastError().find("macro step 2") != std::string::npos);
  CHECK_EQ(m.clip(audioId)->start, 0.0);
  CHECK_EQ(h.editor()->getUndoCount(), undo);
  CHECK(!h.editor()->inTransaction());
  // saved macro from settings
  h.settings.setString("Services.scripting-js.Macros.Bump", R"([{"category":"Edit","name":"Nudge Right","args":{"amount":2}}])");
  args = sel(audioId);
  args->setString("name", "Bump");
  REQUIRE_OK(h.runCommand("Macro", "Run Saved", args.get()));
  CHECK_EQ(m.clip(audioId)->start, 2.0);

  // macrohandler edit task: action sequence through IEditor::execute
  auto p = sq::makeAttributes();
  p->setString("steps", ("[{\"action\":\"" + str(sq::doc::kAddTrack) + "\",\"params\":{\"kind\":1,\"name\":\"Seq\"}}]").c_str());
  auto res = sq::makeAttributes();
  const size_t before = m.tracks.size();
  REQUIRE_OK(h.editor()->execute(&kMacroActionSequence, p.get(), res.get()));
  CHECK_EQ(m.tracks.size(), before + 1);
  CHECK_EQ(m.tracks.back().name, std::string("Seq"));
  CHECK_EQ(res->getInt("count", 0), 1);
  sq_guid made{};
  CHECK_OK(res->getGuid("node", &made));
}

SQ_TEST("scripting: timers and promises run from idle()") {
  Harness h(false);
  h.eval("var ticks = 0; var p = 0; Host.setTimeout(() => { ticks++; }, 0); Promise.resolve().then(() => { p = 1; });");
  CHECK_EQ(h.eval("p"), std::string("1"));  // evaluate() drains promise jobs
  REQUIRE_OK(h.engine->idle());
  CHECK_EQ(h.eval("ticks"), std::string("1"));
  h.eval("var id = Host.setInterval(() => { ticks++; }, 0);");
  sq_thread_sleep_ms(5);
  REQUIRE_OK(h.engine->idle());
  sq_thread_sleep_ms(5);
  REQUIRE_OK(h.engine->idle());
  h.eval("Host.clearInterval(id);");
  sq_thread_sleep_ms(5);
  REQUIRE_OK(h.engine->idle());
  CHECK_EQ(h.eval("ticks"), std::string("3"));
}

SQ_TEST("scripting: wrong-thread calls are refused") {
  Harness h(false);
  sq_result r = SQ_OK;
  struct Ctx {
    sq::IScriptEngine* e;
    sq_result* r;
  } c{h.engine.get(), &r};
  sq_thread* t = nullptr;
  REQUIRE_OK(sq_thread_create(
      [](void* u) {
        auto* x = static_cast<Ctx*>(u);
        *x->r = x->e->evaluate("1", "thread.js", nullptr);
      },
      &c, "sqtest-js", &t));
  sq_thread_join(t);
  CHECK_EQ(r, SQ_E_ACCESS);
}

SQ_TEST("scripting: module unloads cleanly once scripts objects are released") {
  sq::app::Settings settings;
  settings.setBool("Services.scripting-js.LoadBundled", SQ_TRUE);
  settings.setBool("Services.scripting-js.LoadUserPackages", SQ_FALSE);
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath()).ok());
  const uint32_t withScripts = reg.getClassCount();
  CHECK(withScripts >= 16u);
  {
    sq::Ptr<sq::IScriptEngine> e;
    host.queryService(&sq::IScriptEngine::iid, e.putVoid());
    REQUIRE(e);
    auto r = sq::makeAttributes();
    REQUIRE_OK(e->evaluate("({ a: 1 })", "x.js", r.get()));
    sq::Ptr<sq::IUnknown> held;
    REQUIRE_OK(r->getObject("value", &sq::IUnknown::iid, held.putVoid()));
    r = nullptr;
    e = nullptr;
    CHECK_EQ(loader.unload("scripting-js"), SQ_E_BUSY);  // a script object is still alive: rolled back
    CHECK_EQ(reg.getClassCount(), withScripts);         // ... and everything re-registered
  }
  CHECK_OK(loader.unload("scripting-js"));
  CHECK_EQ(reg.getClassCount(), 0u);
  sq::Ptr<sq::IScriptEngine> gone;
  CHECK_EQ(host.queryService(&sq::IScriptEngine::iid, gone.putVoid()), SQ_E_NOTFOUND);
  // and reloads
  REQUIRE(loader.load(modulePath()).ok());
  CHECK_EQ(reg.getClassCount(), withScripts);
  CHECK_OK(loader.unload("scripting-js"));
}

// =====================================================================================================
// Verifier regression tests (adversarial review): each one exposed a defect before the fix.
namespace {

constexpr sq_guid kNativeUnloadCommand = sq::guid("{CA05F041-AF05-422C-A7E2-A43469C594D8}");
constexpr sq_guid kTimerPackageClass = sq::guid("{BA8C1A2A-E8C3-4BA7-B3F1-9B731771DF18}");

sq::IScriptPackage* g_unloadTarget = nullptr;
sq_result g_unloadResult = SQ_E_FAIL;
int g_unloadCalls = 0;

// A native "Command" handler that unloads a script package when invoked (from inside a script).
class NativeUnloader final : public sq::Object<NativeUnloader, sq::ICommandHandler> {
 public:
  sq_result SQ_CALL interpretCommand(const sq_command* c, sq::IAttributes*, sq_bool checkOnly) override {
    if (!c || std::strcmp(c->category, "Native") != 0 || std::strcmp(c->name, "Unload") != 0) return SQ_E_NOTFOUND;
    if (checkOnly) return SQ_OK;
    ++g_unloadCalls;
    g_unloadResult = g_unloadTarget ? g_unloadTarget->unload() : SQ_E_FAIL;
    return SQ_OK;
  }
};

}  // namespace

SQ_TEST("scripting: a zero or oversized stack setting cannot crash the host on deep recursion") {
  Harness h(false);
  // 0 used to disable QuickJS' stack check entirely (native stack overflow = process crash);
  // a value larger than the thread's stack did the same.
  for (int64_t kb : {int64_t(0), int64_t(1) << 20, int64_t(-5)}) {
    h.settings.setInt("Services.scripting-js.StackSizeKB", kb);
    CHECK_EQ(h.engine->evaluate("(function f(){ return f() + 1; })()", "deep.js", nullptr), SQ_E_FAIL);
    CHECK(h.lastError().find("stack") != std::string::npos);
    CHECK_EQ(h.eval("'ok'"), std::string("ok"));
  }
}

SQ_TEST("scripting: packEvents never emits an event size beyond the 4 payload bytes") {
  Harness h(false);
  // sq_clip_event: struct_size, (pad), position, length, bytes[4], size -> size is the 8th uint32.
  static_assert(sizeof(sq_clip_event) == 32, "layout assumed by this test");
  CHECK_EQ(h.eval("const b = new Uint32Array(Host.Midi.packEvents([{ size: 100 }, { size: -5 }, { size: NaN }, {}]));"
                  "[b[7], b[15], b[23], b[31]].join()"),
           std::string("4,0,0,3"));
}

SQ_TEST("scripting: malformed numeric input to host helpers is handled without UB") {
  Harness h(false);
  // huge / non-finite ranges must not overflow the frame arithmetic
  CHECK_EQ(h.eval("const r = Host.Audio.peak('C:/media/x.tone', 1e300, 1e300); r === null || r.frames >= 0"), std::string("true"));
  CHECK_EQ(h.eval("const q = Host.Audio.peak('C:/media/x.tone', NaN, Infinity); q === null || q.frames >= 0"), std::string("true"));
  CHECK_EQ(h.eval("Host.Audio.peak('', -1, -1)"), std::string(""));
}

SQ_TEST("scripting: nudge and split leave locked clips alone") {
  Harness h(true);
  mock::MTrack& t = h.doc->addTrack("Locked");
  mock::MClip& c = mock::Document::addClip(t, "Fixed", 1.0, 4.0);
  c.flags = SQ_CLIP_LOCKED;
  const sq_guid id = c.id, tid = t.id;
  auto args = sq::makeAttributes();
  args->setString("selection", str(id).c_str());
  args->setDouble("amount", 0.5);
  CHECK_EQ(h.runCommand("Edit", "Nudge Right", args.get(), true), SQ_COMMAND_DISABLED);
  CHECK_EQ(h.model().clip(id)->start, 1.0);
  args->setDouble("position", 2.0);
  CHECK_EQ(h.runCommand("Edit", "Split At Position", args.get(), true), SQ_COMMAND_DISABLED);
  CHECK_EQ(h.model().track(tid)->clips.size(), size_t(1));
  CHECK_EQ(h.editor()->getUndoCount(), 0u);
}

SQ_TEST("scripting: a package cannot be unloaded from under its own running script") {
  Harness h(false);
  auto factory = sq::Ptr<sq::IClassFactory>::adopt(new mock::SimpleFactory<NativeUnloader, sq::ICommandHandler>(kNativeUnloadCommand));
  sq_class_info ci{sizeof(sq_class_info), kNativeUnloadCommand, "Command", "Native unloader", 0, 0};
  REQUIRE_OK(h.reg.registerClass(&ci, factory.get()));
  std::string dir = tempDir("self_unload");
  put(dir, "package.xml",
      "<ScriptPackage id=\"t.selfunload\"><ClassFactory><ScriptClass classID=\"" + str(kTimerPackageClass) +
          "\" sourceFile=\"t.js\" functionName=\"arm\"/></ClassFactory></ScriptPackage>");
  put(dir, "t.js", R"(var after = 'not reached';
function arm() {
  Host.setTimeout(() => {
    Host.Commands.invoke('Native', 'Unload');
    after = Host.Package.id + ' still runs';   // touches the package context after the native call
    Host.setTimeout(() => {}, 0);
  }, 0);
  return { getProperty(n) { return n; } };
})");
  sq::Ptr<sq::IScriptPackage> pkg;
  REQUIRE_OK(h.engine->loadPackage(dir.c_str(), pkg.put()));
  g_unloadTarget = pkg.get();
  g_unloadCalls = 0;
  {
    sq::Ptr<sq::IController> armed;
    REQUIRE_OK(h.engine->callFactory("t.selfunload", "t.js", "arm", &sq::IController::iid, armed.putVoid()));
  }
  REQUIRE_OK(h.engine->idle());
  CHECK_EQ(g_unloadCalls, 1);
  CHECK_EQ(g_unloadResult, SQ_E_BUSY);  // refused while the package's code is on the stack
  REQUIRE_OK(h.engine->idle());
  CHECK_OK(pkg->unload());              // fine once nothing of it is running
  g_unloadTarget = nullptr;
  h.reg.unregisterClass(&kNativeUnloadCommand);
}

SQ_TEST("scripting: script objects released on other threads while the main thread re-wraps them") {
  Harness h(false);
  h.eval("var shared = { getProperty(n) { return 1; } };");
  for (int round = 0; round < 200; ++round) {
    std::vector<sq::IController*> batch;
    for (int i = 0; i < 8; ++i) {
      auto r = sq::makeAttributes();
      REQUIRE_OK(h.engine->evaluate("shared", "s.js", r.get()));
      sq::Ptr<sq::IController> c;
      REQUIRE_OK(r->getObject("value", &sq::IController::iid, c.putVoid()));
      batch.push_back(c.detach());
    }
    struct Job {
      std::vector<sq::IController*>* v;
    } job{&batch};
    sq_thread* t = nullptr;
    REQUIRE_OK(sq_thread_create(
        [](void* u) {
          for (sq::IController* c : *static_cast<Job*>(u)->v) c->release();
        },
        &job, "sqtest-rel", &t));
    for (int i = 0; i < 8; ++i) {
      auto r = sq::makeAttributes();
      REQUIRE_OK(h.engine->evaluate("shared", "s.js", r.get()));
    }
    sq_thread_join(t);
  }
  CHECK_EQ(h.eval("'alive'"), std::string("alive"));
  REQUIRE_OK(h.engine->idle());
}

SQ_TEST("scripting: last-error and catalog access from other threads is refused, not racy") {
  Harness h(false);
  sq_result r1 = SQ_OK, r2 = SQ_OK;
  struct Ctx {
    sq::IScriptEngine* e;
    sq::IObjectRegistry* o;
    sq_result *r1, *r2;
  } c{h.engine.get(), h.objects.get(), &r1, &r2};
  sq_thread* t = nullptr;
  REQUIRE_OK(sq_thread_create(
      [](void* u) {
        auto* x = static_cast<Ctx*>(u);
        char buf[64];
        uint32_t need = 0;
        *x->r1 = x->e->getLastError(buf, sizeof buf, &need);
        sq::Ptr<sq::IController> ctl;
        if (x->o->resolve("object://scripting", &sq::IController::iid, ctl.putVoid()) == SQ_OK) {
          auto a = sq::makeAttributes();
          *x->r2 = ctl->getProperty("lastError", a.get());
        }
      },
      &c, "sqtest-js2", &t));
  sq_thread_join(t);
  CHECK_EQ(r1, SQ_E_ACCESS);
  CHECK_EQ(r2, SQ_E_ACCESS);
}

SQ_TEST("scripting: package ids must start with a letter or digit") {
  Harness h(false);
  for (const char* id : {"..", ".", "-x"}) {
    std::string d = tempDir("bad_id");
    put(d, "package.xml",
        std::string("<ScriptPackage id=\"") + id +
            R"("><ClassFactory><ScriptClass classID="{85CD318C-DA64-4F27-BDC1-91319CDD247D}" sourceFile="a.js" functionName="f"/></ClassFactory></ScriptPackage>)");
    CHECK_EQ(h.engine->loadPackage(d.c_str(), nullptr), SQ_E_PARSE);
  }
}
