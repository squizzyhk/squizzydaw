// Dummy service: proves module load / unload / reload and multi-interface classes.
// Also the reference example for agents writing a new service (see docs/CONTRACTS.md).
#include <atomic>
#include <cstring>

#include "sq/interfaces/dummy.h"
#include "sq/sdk.hpp"

namespace {

constexpr const char* kInitCountKey = "Services.dummy.InitCount";

class DummyService final : public sq::Object<DummyService, sq::IDummyService, sq::IDummyCounter> {
 public:
  int32_t SQ_CALL add(int32_t a, int32_t b) override { return a + b; }

  sq_result SQ_CALL getGreeting(char* buf, uint32_t size) override {
    static const char kText[] = "Hello from the Squizzy dummy service";
    if (!buf || size < sizeof kText) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, kText, sizeof kText);
    return SQ_OK;
  }

  int64_t SQ_CALL getInitCount() override {
    sq::IHostContext* h = sq::host();
    return h ? h->getSettings()->getInt(kInitCountKey, 0) : 0;
  }

  int32_t SQ_CALL increment() override { return ++count_; }
  int32_t SQ_CALL value() override { return count_.load(); }

 private:
  std::atomic<int32_t> count_{0};
};

class DummyCounter final : public sq::Object<DummyCounter, sq::IDummyCounter> {
 public:
  int32_t SQ_CALL increment() override { return ++count_; }
  int32_t SQ_CALL value() override { return count_.load(); }

 private:
  std::atomic<int32_t> count_{0};
};

sq_result onInit(sq::IHostContext* host) {
  sq::ISettings* s = host->getSettings();
  s->setInt(kInitCountKey, s->getInt(kInitCountKey, 0) + 1);
  sq::logf(SQ_LOG_DEBUG, "initialised (count %lld)", static_cast<long long>(s->getInt(kInitCountKey, 0)));
  return SQ_OK;
}

void onShutdown() { sq::logf(SQ_LOG_DEBUG, "shutdown"); }

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<DummyService>(sq::dummy::kServiceClassId, "Service", "Dummy Service"),
    sq::classEntry<DummyCounter>(sq::dummy::kCounterClassId, "Test", "Dummy Counter"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('D', 'M', 'M', 'Y'),
    .name = "dummy",
    .description = "Dummy service proving load/unload/reload",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
