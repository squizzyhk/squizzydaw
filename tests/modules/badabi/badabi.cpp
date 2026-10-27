// Test module that demands a FUTURE platform ABI major. The loader must reject it before
// ModuleMain(INIT) runs; if INIT ever runs it leaves a marker in settings.
#include "sq/sdk.hpp"

namespace {

class Nothing final : public sq::Object<Nothing, sq::IUnknown> {};

sq_result onInit(sq::IHostContext* host) {
  host->getSettings()->setBool("Test.BadAbi.Initialized", SQ_TRUE);
  return SQ_OK;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Nothing>("{2708DBBA-359A-42EA-9E06-332F785D256C}", "Test", "Bad ABI Nothing"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('B', 'A', 'D', 'A'),
    .name = "badabi",
    .description = "requires a future platform ABI",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = nullptr,
    .version = SQ_MAKE_VERSION(1, 0, 0),
    .abiMin = SQ_ABI_MAKE(SQ_PLATFORM_ABI_MAJOR + 1, 0),
    .abiMax = SQ_ABI_MAKE(SQ_PLATFORM_ABI_MAJOR + 1, 0xFFFF),
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
