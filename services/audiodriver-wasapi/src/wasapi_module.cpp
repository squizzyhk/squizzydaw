// Squizzy Studio - WASAPI audio driver service: module definition.
#include "sq/sdk.hpp"
#include "wasapi_driver.hpp"

namespace {

constexpr sq::ClassEntry kClasses[] = {
    sq::ClassEntry{sq::audiodevice::kWasapiDriverClassId, sq::audiodevice::kCategory, "WASAPI Audio Driver", 0,
                   &sq::wasapi::createDriver},
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('W', 'A', 'S', 'P'),
    .name = "audiodriver-wasapi",
    .description = "Windows Audio Session API driver (shared + exclusive, event-driven)",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
