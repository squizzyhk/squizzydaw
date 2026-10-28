// Squizzy Studio - ALSA audio driver service: module definition.
#include "alsa_driver.hpp"
#include "sq/sdk.hpp"

namespace {

constexpr sq::ClassEntry kClasses[] = {
    sq::ClassEntry{sq::alsa::kAlsaDriverClassId, sq::audiodevice::kCategory, "ALSA Audio Driver", 0, &sq::alsa::createDriver},
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('A', 'L', 'S', 'A'),
    .name = "audiodriver-alsa",
    .description = "Linux ALSA PCM audio driver (libasound loaded at runtime)",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
