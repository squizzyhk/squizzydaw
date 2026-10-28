// Squizzy Studio - PulseAudio driver service: module definition.
#include "pulse_driver.hpp"
#include "sq/sdk.hpp"

namespace {

constexpr sq::ClassEntry kClasses[] = {
    sq::ClassEntry{sq::pulse::kPulseDriverClassId, sq::audiodevice::kCategory, "PulseAudio Driver", 0, &sq::pulse::createDriver},
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('P', 'U', 'L', 'S'),
    .name = "audiodriver-pulse",
    .description = "PulseAudio / PipeWire-pulse audio driver (libpulse-simple loaded at runtime)",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
