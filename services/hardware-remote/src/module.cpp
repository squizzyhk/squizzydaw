// hardware-remote module: OSC 1.0 over UDP (IOscService) and DNS-SD discovery (IDiscoveryService).
// Optional package (plan 2.9 / M8): nothing here is needed by the offline core. onInit only creates
// two idle objects and registers them as host services; no socket is opened and nothing is
// registered with the OS before the first listen/send/browse/advertise call.
#include "services.hpp"

namespace {

sq::hwremote::OscService* g_osc = nullptr;
sq::hwremote::DiscoveryService* g_discovery = nullptr;

void onShutdown() {
  sq::IHostContext* h = sq::host();
  if (g_osc) {
    g_osc->stopAll();
    if (h) h->unregisterService(&sq::IOscService::iid, static_cast<sq::IOscService*>(g_osc));
    g_osc->release();
    g_osc = nullptr;
  }
  if (g_discovery) {
    g_discovery->stopAll();
    if (h) h->unregisterService(&sq::IDiscoveryService::iid, static_cast<sq::IDiscoveryService*>(g_discovery));
    g_discovery->release();
    g_discovery = nullptr;
  }
}

sq_result onInit(sq::IHostContext* host) {
  g_osc = nullptr;
  g_discovery = nullptr;
  try {
    g_osc = new sq::hwremote::OscService();
    g_discovery = new sq::hwremote::DiscoveryService();
  } catch (...) {
    onShutdown();
    return SQ_E_OUTOFMEMORY;
  }
  // A failed registration (another provider already serves the IID) is not fatal: the classes
  // stay creatable through the registry.
  if (SQ_FAILED(host->registerService(&sq::IOscService::iid, static_cast<sq::IOscService*>(g_osc))))
    sq::logf(SQ_LOG_INFO, "IOscService already provided; not registering the singleton");
  if (SQ_FAILED(host->registerService(&sq::IDiscoveryService::iid, static_cast<sq::IDiscoveryService*>(g_discovery))))
    sq::logf(SQ_LOG_INFO, "IDiscoveryService already provided; not registering the singleton");
  return SQ_OK;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<sq::hwremote::OscService>(sq::hardware_remote::kOscServiceClassId, "Service", "OSC Service (UDP)"),
    sq::classEntry<sq::hwremote::DiscoveryService>(sq::hardware_remote::kDiscoveryServiceClassId, "Service",
                                                   "DNS-SD Discovery Service"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('H', 'W', 'R', 'M'),
    .name = "hardware-remote",
    .description = "OSC 1.0 over UDP and mDNS/DNS-SD discovery (optional)",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
