// SquizzyStudio.exe - thin shell over sq::app::Shell (platform + settings + class registry +
// module loader + engine services + command system) and the skin-driven window.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gui.hpp"
#include "sq/app/shell.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/dummy.h"
#include "sq/platform.hpp"
#include "sq/ui/theme.hpp"
#include "sq/ui/ui.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

const char* kTag = "app";

struct Options {
  bool headless = false;
  bool listClasses = false;
  bool listCommands = false;
  bool selftest = false;
  bool reloadTest = false;
  int reloadCycles = 10;
  int newGuids = 0;
  int quitAfterMs = -1;
  bool help = false;
  bool version = false;
  bool licenses = false;
  std::string servicesDir;
  std::string settingsFile;
  std::string logLevel;
  std::string audioDriver;
  std::vector<std::string> run;  // --run "Category.Name key=value ..."
};

void printUsage() {
  std::printf(
      "Squizzy Studio %s (platform ABI %d.%d)\n"
      "usage: SquizzyStudio [options]\n"
      "  --headless            boot, load services, print a summary, exit\n"
      "  --list-classes        print every registered class and exit\n"
      "  --list-commands       print every command of the command system and exit\n"
      "  --run \"Cat.Name k=v\"  execute a command without a window (repeatable, in order;\n"
      "                        values: numbers, true/false, {GUID}, anything else is text)\n"
      "  --selftest            run built-in self checks (exit code 0 = pass)\n"
      "  --reload-test [N]     load/unload/reload the dummy service N times (default 10)\n"
      "  --services-dir DIR    service module folder (default: <exe dir>/services)\n"
      "  --settings FILE       settings XML (GUI default: <user config>/settings.xml)\n"
      "  --audio-driver D      none|null|offline|wasapi|asio (overrides Devices.Audio.Driver)\n"
      "  --log-level LEVEL     trace|debug|info|warn|error\n"
      "  --quit-after MS       GUI smoke test: close the window after MS milliseconds\n"
      "  --new-guid [N]        print N fresh GUIDs for class/interface IDs\n"
      "  --version             print version + ABI stamp\n"
      "  --licenses            print the third-party license notices (THIRD_PARTY_NOTICES.txt)\n",
      SQ_VERSION_STRING, SQ_PLATFORM_ABI_MAJOR, SQ_PLATFORM_ABI_MINOR);
}

bool parseArgs(int argc, char** argv, Options& o) {
  auto nextInt = [&](int& i, int& out) {
    if (i + 1 < argc && argv[i + 1][0] != '-') out = std::atoi(argv[++i]);
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    if (a == "--headless") o.headless = true;
    else if (a == "--list-classes") o.listClasses = true;
    else if (a == "--list-commands") o.listCommands = true;
    else if (a == "--selftest") o.selftest = true;
    else if (a == "--reload-test") {
      o.reloadTest = true;
      nextInt(i, o.reloadCycles);
    } else if (a == "--new-guid") {
      o.newGuids = 1;
      nextInt(i, o.newGuids);
    } else if (a == "--quit-after") {
      std::string v;
      if (!value(v)) return false;
      o.quitAfterMs = std::atoi(v.c_str());
    } else if (a == "--services-dir") {
      if (!value(o.servicesDir)) return false;
    } else if (a == "--settings") {
      if (!value(o.settingsFile)) return false;
    } else if (a == "--log-level") {
      if (!value(o.logLevel)) return false;
    } else if (a == "--audio-driver") {
      if (!value(o.audioDriver)) return false;
    } else if (a == "--run") {
      std::string v;
      if (!value(v)) return false;
      o.run.push_back(v);
    } else if (a == "--help" || a == "-h" || a == "/?") o.help = true;
    else if (a == "--version") o.version = true;
    else if (a == "--licenses") o.licenses = true;
    else {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      return false;
    }
  }
  return true;
}

std::string guidStr(const sq_guid& g) { return sq::toString(g); }

// "Cat.Name k=v k2=v2" -> command + attributes.
sq::Ptr<sq::IAttributes> parseRun(const std::string& spec, std::string& command) {
  auto args = sq::makeAttributes();
  std::vector<std::string> words;
  std::string cur;
  bool quoted = false;
  for (char c : spec) {
    if (c == '"') quoted = !quoted;
    else if (c == ' ' && !quoted) {
      if (!cur.empty()) words.push_back(cur);
      cur.clear();
    } else cur.push_back(c);
  }
  if (!cur.empty()) words.push_back(cur);
  command = words.empty() ? std::string() : words[0];
  for (size_t i = 1; i < words.size(); ++i) {
    size_t eq = words[i].find('=');
    if (eq == std::string::npos) continue;
    std::string k = words[i].substr(0, eq), v = words[i].substr(eq + 1);
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    sq_guid g{};
    if (!v.empty() && end && *end == '\0') args->setDouble(k.c_str(), d);
    else if (v == "true" || v == "false") args->setBool(k.c_str(), v == "true" ? SQ_TRUE : SQ_FALSE);
    else if (v.size() == 38 && v[0] == '{' && sq::parseGuid(v, g)) args->setGuid(k.c_str(), &g);
    else args->setString(k.c_str(), v.c_str());
  }
  return args;
}

struct App {
  sq::app::Shell shell;

  void printSummary() {
    std::printf("Squizzy Studio %s | platform ABI %d.%d | sqplatform %s\n", SQ_VERSION_STRING, SQ_PLATFORM_ABI_MAJOR, SQ_PLATFORM_ABI_MINOR,
                sq_platform_version_string());
    std::printf("services dir: %s\n", shell.servicesDir().c_str());
    for (const auto& m : shell.loader.modules()) {
      char fc[5];
      std::printf("  module %-24s [%s] v%u.%u.%u  %u class(es)\n", m.name.c_str(), sq::app::ModuleLoader::fourccString(m.fourcc, fc),
                  SQ_VERSION_GET_MAJOR(m.version), SQ_VERSION_GET_MINOR(m.version), SQ_VERSION_GET_PATCH(m.version), m.classCount);
    }
    std::printf("engine: %s | audio: %s\n", shell.hasEngine() ? "documents + edit + audio" : "not built (filtered)", shell.deviceDescription().c_str());
    std::string themeList;
    if (sq::ui::ThemeResolver* th = shell.themes())
      for (const auto& ti : th->themes()) themeList += (themeList.empty() ? "" : ", ") + ti.name;
    std::printf("themes: %s | active: %s\n", themeList.empty() ? "none" : themeList.c_str(),
                shell.themes() && !shell.themes()->activeTheme().empty() ? shell.themes()->activeTheme().c_str() : "-");
    std::printf("%zu module(s), %u class(es), %zu setting(s), %zu command(s)\n", shell.loader.count(), shell.registry.getClassCount(),
                shell.settings.size(), shell.commands().size());
  }

  void listClasses() {
    auto entries = shell.registry.snapshot();
    std::printf("%-40s %-12s %-6s %s\n", "classID", "category", "module", "name");
    for (const auto& e : entries) {
      char fc[5];
      std::printf("%-40s %-12s %-6s %s\n", guidStr(e.classId).c_str(), e.category.c_str(),
                  e.moduleFourcc ? sq::app::ModuleLoader::fourccString(e.moduleFourcc, fc) : "-", e.name.c_str());
    }
    std::printf("%zu class(es)\n", entries.size());
  }

  void listCommands() {
    auto cmds = shell.commands();
    for (const auto& c : cmds) std::printf("%-10s %-28s %-7s %s\n", c.category.c_str(), c.name.c_str(), c.source.c_str(), c.title.c_str());
    std::printf("%zu command(s)\n", cmds.size());
  }

  int runCommands(const std::vector<std::string>& specs) {
    int rc = 0;
    for (const auto& s : specs) {
      std::string cmd;
      auto args = parseRun(s, cmd);
      auto res = sq::makeAttributes();
      sq_result r = shell.execute(cmd, args.get(), res.get());
      shell.idle();
      std::printf("run %s: %s%s%s\n", cmd.c_str(), sq_result_string(r), shell.lastMessage().empty() ? "" : " - ", shell.lastMessage().c_str());
      if (SQ_FAILED(r)) rc = 1;
    }
    return rc;
  }

  // One full cycle against the dummy service: instance -> calls -> release -> unload -> verify gone -> load.
  bool dummyCycle(std::string& why) {
    auto& loader = shell.loader;
    sq::app::ModuleRecord rec;
    if (!loader.find("dummy", &rec)) {
      why = "dummy module not loaded";
      return false;
    }
    {
      auto svc = sq::createInstance<sq::IDummyService>(&shell.registry, sq::dummy::kServiceClassId);
      if (!svc) {
        why = "cannot create Dummy Service";
        return false;
      }
      if (svc->add(2, 3) != 5) {
        why = "IDummyService::add wrong";
        return false;
      }
      auto counter = svc.as<sq::IDummyCounter>();
      if (!counter || counter->increment() != 1) {
        why = "IDummyCounter via queryInterface failed";
        return false;
      }
      if (loader.unload("dummy") != SQ_E_BUSY) {
        why = "unload with live objects was not refused";
        return false;
      }
    }
    if (loader.unload("dummy") != SQ_OK) {
      why = "unload failed";
      return false;
    }
    if (sq_dylib_is_loaded(rec.path.c_str())) {
      why = "library still mapped after unload";
      return false;
    }
    if (shell.registry.findClass(&sq::dummy::kServiceClassId, nullptr) != SQ_E_NOTFOUND) {
      why = "class still registered after unload";
      return false;
    }
    auto r = loader.load(rec.path);
    if (!r.ok()) {
      why = "reload failed: " + r.message;
      return false;
    }
    return true;
  }

  int reloadTest(int cycles) {
    int64_t before = shell.settings.getInt("Services.dummy.InitCount", 0);
    for (int i = 0; i < cycles; ++i) {
      std::string why;
      if (!dummyCycle(why)) {
        std::printf("reload-test: cycle %d FAILED: %s\n", i + 1, why.c_str());
        return 1;
      }
    }
    int64_t after = shell.settings.getInt("Services.dummy.InitCount", 0);
    // Each cycle initialises the module twice: once when the refused (busy) unload rolls back,
    // once when it is loaded again after the real unload.
    if (after - before != 2 * cycles) {
      std::printf("reload-test: expected %d re-inits, saw %lld\n", 2 * cycles, static_cast<long long>(after - before));
      return 1;
    }
    std::printf("reload-test: PASS (%d load/unload/reload cycles)\n", cycles);
    return 0;
  }

  int selftest() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
      std::printf("[selftest] %s: %s\n", ok ? "ok  " : "FAIL", what);
      if (!ok) ++failures;
    };
    check(sq_platform_abi_version() == SQ_PLATFORM_ABI, "sqplatform ABI matches the app");
    sq_guid g{};
    check(sq_guid_generate(&g) == SQ_OK && !sq_guid_is_null(&g), "GUID generation");
    sq_guid back{};
    check(sq::parseGuid(guidStr(g), back) && back == g, "GUID format/parse roundtrip");
    {
      sq::app::Settings s2;
      s2.setInt("Engine.Test.Value", 42);
      sq::app::Settings s3;
      s3.loadXml(s2.toXml());
      check(s3.getInt("Engine.Test.Value", 0) == 42, "settings XML roundtrip");
    }
    check(sq::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
    check(shell.loader.find("dummy"), "dummy service loaded");
    check(shell.registry.getClassCount() >= 2, "classes registered");
    if (shell.hasEngine()) {
      check(shell.documents() != nullptr, "document manager service");
      check(shell.engine() != nullptr, "audio engine service");
      bool cmd = SQ_SUCCEEDED(shell.execute("File.New")) && SQ_SUCCEEDED(shell.execute("Track.AddAudio")) &&
                 SQ_SUCCEEDED(shell.execute("Edit.Undo")) && SQ_SUCCEEDED(shell.execute("File.Close"));
      check(cmd, "command system: File.New / Track.AddAudio / Edit.Undo / File.Close");
    }
    std::string why;
    bool cyc = dummyCycle(why);
    check(cyc, cyc ? "dummy load/unload/reload cycle" : why.c_str());
    std::printf("[selftest] %s (%d failure(s))\n", failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
  }
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) {
    printUsage();
    return 64;
  }
  if (opt.help) {
    printUsage();
    return 0;
  }
  if (opt.version) {
    std::printf("Squizzy Studio %s\nplatform ABI %d.%d\nbuild %s\nsqplatform ABI %u.%u (%s)\n", SQ_VERSION_STRING, SQ_PLATFORM_ABI_MAJOR,
                SQ_PLATFORM_ABI_MINOR, SQ_BUILD_STAMP, SQ_ABI_GET_MAJOR(sq_platform_abi_version()),
                SQ_ABI_GET_MINOR(sq_platform_abi_version()), sq_platform_version_string());
    return 0;
  }
  if (opt.licenses) {
    // Generated at build time next to the exe from third_party/*/LICENSE* (cmake/SqNotices.cmake).
    const std::string path = sq::joinPath(sq::specialDir(SQ_DIR_EXE), "THIRD_PARTY_NOTICES.txt");
    const auto text = sq::readFile(path);
    if (!text || text->empty()) {
      std::fprintf(stderr, "license notices not found: %s\n", path.c_str());
      return 2;
    }
    std::fwrite(text->data(), 1, text->size(), stdout);
    return 0;
  }
  if (opt.newGuids > 0) {
    for (int i = 0; i < opt.newGuids; ++i) {
      sq_guid g;
      sq_guid_generate(&g);
      std::printf("%s\n", guidStr(g).c_str());
    }
    return 0;
  }
  // The platform DLL must speak the ABI this exe was compiled against.
  if (SQ_ABI_GET_MAJOR(sq_platform_abi_version()) != SQ_PLATFORM_ABI_MAJOR || sq_platform_abi_version() < SQ_PLATFORM_ABI) {
    std::fprintf(stderr, "sqplatform ABI mismatch (dll %08x, exe %08x)\n", sq_platform_abi_version(), static_cast<unsigned>(SQ_PLATFORM_ABI));
    return 3;
  }

  bool gui = !(opt.headless || opt.listClasses || opt.listCommands || opt.selftest || opt.reloadTest || !opt.run.empty());
  App app;
  sq::app::Shell::registerDefaults(app.shell.settings);

  // Settings file: GUI mode uses the per-user file by default; batch modes only if --settings given.
  std::string settingsPath = opt.settingsFile;
  if (settingsPath.empty() && gui) settingsPath = sq::joinPath(sq::specialDir(SQ_DIR_USER_CONFIG), "settings.xml");
  if (!settingsPath.empty() && sq_fs_exists(settingsPath.c_str())) {
    std::string err;
    if (app.shell.settings.loadFile(settingsPath, &err) != SQ_OK) sq_log_printf(SQ_LOG_WARN, kTag, "settings: %s", err.c_str());
  }
  std::string level = opt.logLevel.empty() ? app.shell.settings.get("Platform.Log.Level", "info") : opt.logLevel;
  int32_t lv = sq_log_level_from_name(level.c_str());
  sq_log_set_level(lv >= 0 ? lv : SQ_LOG_INFO);
  app.shell.settings.observe("Platform.Log.Level", [&app](const char*) {
    int32_t l = sq_log_level_from_name(app.shell.settings.get("Platform.Log.Level", "info").c_str());
    if (l >= 0) sq_log_set_level(l);
  });

  sq::app::ShellOptions so;
  so.servicesDir = opt.servicesDir;
  so.audioDriver = opt.audioDriver;
  so.gui = gui;
  app.shell.start(so);

  int rc = 0;
  if (opt.listClasses) app.listClasses();
  if (opt.listCommands) app.listCommands();
  if (opt.headless) app.printSummary();
  if (!opt.run.empty()) rc |= app.runCommands(opt.run);
  if (opt.selftest) rc |= app.selftest();
  if (opt.reloadTest) rc |= app.reloadTest(opt.reloadCycles);
#ifdef _WIN32
  // Console-subsystem exe (so CLI modes behave in terminals/CI). When started by double-click the
  // console belongs to us alone: detach it so only the window remains.
  if (gui) {
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) FreeConsole();
  }
#endif
  if (gui) {
    rc = sq::app::runGui(app.shell, opt.quitAfterMs);
    if (!settingsPath.empty()) {
      sq_result r = app.shell.settings.saveFile(settingsPath);
      sq_log_printf(r == SQ_OK ? SQ_LOG_INFO : SQ_LOG_WARN, kTag, "settings saved to %s: %s", settingsPath.c_str(), sq_result_string(r));
    }
  }
  app.shell.stop();
  return rc;
}
