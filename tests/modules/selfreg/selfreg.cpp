// Test module that behaves like a real service: in onInit it registers a host service, a
// settings observer and a dynamic (script-style) class, and removes them all in onShutdown.
// Proves such a module can still be unloaded/reloaded at runtime. With the setting
// Test.SelfReg.LeakObserver = true it "forgets" its observer, so unload must be refused (BUSY)
// and rolled back instead of unmapping code the host still references.
#include <atomic>

#include "sq/interfaces/dummy.h"
#include "sq/sdk.hpp"

namespace {

constexpr sq_guid kSelfRegClassId = sq::guid("{535301FF-C7C0-493C-8B26-52EB544160B7}");
constexpr sq_guid kDynamicClassId = sq::guid("{7B6A74B2-4871-46E9-8852-20A82554838E}");

class Counter final : public sq::Object<Counter, sq::IDummyCounter> {
 public:
  int32_t SQ_CALL increment() override { return ++n_; }
  int32_t SQ_CALL value() override { return n_.load(); }

 private:
  std::atomic<int32_t> n_{0};
};

// Factory served for the dynamically registered class (what the scripting service will do).
class DynFactory final : public sq::Object<DynFactory, sq::IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 0; }
  sq_result SQ_CALL getClassInfo(uint32_t, sq_class_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!classId || *classId != kDynamicClassId) return SQ_E_NOTFOUND;
    return sq::createObject<Counter>(riid, out);
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};

sq::Ptr<sq::IDummyCounter> gService;
sq::Ptr<sq::SettingsObserverFn> gObserver;
uint32_t gObserverCookie = 0;

sq_result onInit(sq::IHostContext* host) {
  gService = sq::Ptr<sq::IDummyCounter>::adopt(new Counter());
  sq_result r = host->registerService(&sq::IDummyCounter::iid, gService.get());
  if (SQ_FAILED(r)) return r;

  gObserver = sq::Ptr<sq::SettingsObserverFn>::adopt(new sq::SettingsObserverFn([](const char*) {}));
  r = host->getSettings()->addObserver("Test.SelfReg", gObserver.get(), &gObserverCookie);
  if (SQ_FAILED(r)) return r;

  auto factory = sq::Ptr<DynFactory>::adopt(new DynFactory());
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  ci.class_id = kDynamicClassId;
  ci.category = "ScriptClass";
  ci.name = "SelfReg Dynamic";
  r = host->getClassRegistry()->registerClass(&ci, factory.get());
  if (SQ_FAILED(r)) return r;

  sq::ISettings* s = host->getSettings();
  s->setInt("Test.SelfReg.InitCount", s->getInt("Test.SelfReg.InitCount", 0) + 1);
  return SQ_OK;
}

void onShutdown() {
  sq::IHostContext* host = sq::host();
  host->getClassRegistry()->unregisterClass(&kDynamicClassId);
  if (!host->getSettings()->getBool("Test.SelfReg.LeakObserver", SQ_FALSE)) host->getSettings()->removeObserver(gObserverCookie);
  gObserver = nullptr;  // the settings registry may still hold its own reference
  host->unregisterService(&sq::IDummyCounter::iid, gService.get());
  gService = nullptr;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Counter>(kSelfRegClassId, "Test", "SelfReg Counter"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('S', 'R', 'E', 'G'),
    .name = "selfreg",
    .description = "registers host objects in INIT, removes them in SHUTDOWN",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
