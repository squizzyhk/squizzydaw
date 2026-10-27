// scripting-js: module entry. Registers the IScriptEngine and IObjectRegistry services, the
// object://settings/ and package:// resolvers, object://scripting, and loads the bundled script
// packages (<exe>/scripts, the user's scripts folder and Services.scripting-js.PackagePath).
#include "engine.hpp"

namespace {

struct ModuleState {
  sq::Ptr<sqjs::ObjectRegistry> objects;
  sq::Ptr<sqjs::Engine> engine;
  sq::Ptr<sqjs::SettingsResolver> settings;
  sq::Ptr<sqjs::PackageResolver> packages;
  bool objectsService = false;
  bool engineService = false;
};

ModuleState* g_state = nullptr;

void setDefaultInt(sq::ISettings* s, const char* key, int64_t v) {
  if (!s->has(key)) s->setInt(key, v);
}
void setDefaultBool(sq::ISettings* s, const char* key, bool v) {
  if (!s->has(key)) s->setBool(key, v ? SQ_TRUE : SQ_FALSE);
}
void setDefaultString(sq::ISettings* s, const char* key, const char* v) {
  if (!s->has(key)) s->setString(key, v);
}

void onShutdown();

sq_result onInit(sq::IHostContext* host) {
  try {
    sq::ISettings* s = host->getSettings();
    setDefaultInt(s, "Services.scripting-js.TimeLimitMs", 10000);
    setDefaultInt(s, "Services.scripting-js.MemoryLimitMB", 256);
    setDefaultInt(s, "Services.scripting-js.StackSizeKB", 512);
    setDefaultBool(s, "Services.scripting-js.LoadBundled", true);
    setDefaultBool(s, "Services.scripting-js.LoadUserPackages", true);
    setDefaultString(s, "Services.scripting-js.PackagePath", "");

    g_state = new ModuleState();
    g_state->objects = sq::Ptr<sqjs::ObjectRegistry>::adopt(new sqjs::ObjectRegistry());
    g_state->engine = sq::Ptr<sqjs::Engine>::adopt(new sqjs::Engine(host, g_state->objects.get()));
    sq_result r = g_state->engine->start();
    if (SQ_FAILED(r)) {
      onShutdown();
      return r;
    }
    if (host->registerService(&sq::IObjectRegistry::iid, static_cast<sq::IObjectRegistry*>(g_state->objects.get())) == SQ_OK)
      g_state->objectsService = true;
    else
      sq::logf(SQ_LOG_WARN, "another IObjectRegistry is already registered; the scripting one stays private");
    r = host->registerService(&sq::IScriptEngine::iid, static_cast<sq::IScriptEngine*>(g_state->engine.get()));
    if (SQ_FAILED(r)) {
      onShutdown();
      return r;
    }
    g_state->engineService = true;

    g_state->settings = sq::Ptr<sqjs::SettingsResolver>::adopt(new sqjs::SettingsResolver(s));
    g_state->packages = sq::Ptr<sqjs::PackageResolver>::adopt(new sqjs::PackageResolver(g_state->engine.get()));
    g_state->objects->registerResolver(sqjs::kSettingsPrefix, g_state->settings.get());
    g_state->objects->registerResolver(sqjs::kPackagePrefix, g_state->packages.get());
    g_state->objects->registerObject(sqjs::kEngineUrl, static_cast<sq::IController*>(g_state->engine.get()));

    if (s->getBool("Services.scripting-js.LoadBundled", SQ_TRUE)) g_state->engine->loadBundledPackages();
    sq::logf(SQ_LOG_INFO, "%s ready", g_state->engine->engineInfo().c_str());
    return SQ_OK;
  } catch (...) {
    onShutdown();
    return SQ_E_FAIL;
  }
}

void onShutdown() {
  ModuleState* st = g_state;
  if (!st) return;
  g_state = nullptr;
  sq::IHostContext* host = sq::host();
  if (st->engine) st->engine->stop();
  if (st->packages) st->packages->detach();
  if (host && st->engineService) host->unregisterService(&sq::IScriptEngine::iid, static_cast<sq::IScriptEngine*>(st->engine.get()));
  if (host && st->objectsService)
    host->unregisterService(&sq::IObjectRegistry::iid, static_cast<sq::IObjectRegistry*>(st->objects.get()));
  if (st->objects) st->objects->clear();
  delete st;
}

constexpr sq::ModuleDesc kModule{
    .fourcc = sqjs::kModuleFourcc,
    .name = sqjs::kModuleName,
    .description = "JavaScript scripting engine (QuickJS-ng), host object registry and script packages",
    .classes = {},
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
