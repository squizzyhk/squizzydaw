// Squizzy Studio - ASIO driver service: module definition.
//
// Licence seam: the module always builds and loads. Without -DSQ_ASIO_SDK_DIR the driver class
// is registered but reports zero devices, with the reason available through IAsioDriverStatus
// (and logged on the first refresh). With the SDK path the real host is compiled in.
#include "asio_driver.hpp"
#include "sq/sdk.hpp"

namespace {

sq_result createAsioDriver(const sq_guid* riid, void** out) noexcept try {
  sq::asio::DriverConfig cfg;
  cfg.registry = [] { return sq::asio::openSystemRegistry(); };
#if defined(SQ_HAVE_ASIO_SDK) && SQ_HAVE_ASIO_SDK
  cfg.backends = sq::asio::makeSdkBackendFactory();
#else
  cfg.unavailableReason =
      "ASIO support is not compiled into this build: it was configured without SQ_ASIO_SDK_DIR. The ASIO SDK is "
      "licensed separately and is not part of the source tree; rebuild with -DSQ_ASIO_SDK_DIR=<path to the SDK> "
      "to enable ASIO devices.";
#endif
  return sq::asio::createDriverWith(std::move(cfg), riid, out);
} catch (...) {
  if (out) *out = nullptr;
  return SQ_E_OUTOFMEMORY;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::ClassEntry{sq::audiodevice::kAsioDriverClassId, sq::audiodevice::kCategory, "ASIO Audio Driver", 0, &createAsioDriver},
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('A', 'S', 'I', 'O'),
    .name = "audiodriver-asio",
#if defined(SQ_HAVE_ASIO_SDK) && SQ_HAVE_ASIO_SDK
    .description = "ASIO audio driver host",
#else
    .description = "ASIO audio driver host (not compiled in: built without SQ_ASIO_SDK_DIR, reports no devices)",
#endif
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
