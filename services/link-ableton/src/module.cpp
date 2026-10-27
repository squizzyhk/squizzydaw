// link-ableton module: ITempoSync (remote.h) + ITempoSyncInfo. Optional package (M8).
// Settings: Services.link-ableton.Backend = "auto" (SDK session when compiled in, else local),
//           "local" (always the own timeline), "sdk" (SDK only; falls back to local with a log
//           line when it is not compiled in). Read when the service object is created.
// The singleton is created idle in onInit: with the SDK backend nothing joins the network until
// enable(SQ_TRUE).
#include <cstring>
#include <memory>
#include <string>

#include "backend.hpp"
#include "sq/platform.h"
#include "sq/sdk.hpp"
#include "sq/services/link_ableton/link_ableton.h"

namespace {

using sq::tempo::SyncBackend;

std::unique_ptr<SyncBackend> chooseBackend() {
  std::string mode = "auto";
  if (sq::IHostContext* h = sq::host()) mode = sq::getString(h->getSettings(), "Services.link-ableton.Backend", "auto");
  if (mode == "sdk" || mode == "auto") {
    if (auto b = sq::tempo::makeSdkBackend(120.0)) return b;
    if (mode == "sdk") sq::logf(SQ_LOG_WARN, "SDK backend requested but not compiled in (SQ_LINK_SDK_DIR); using the local timeline");
  }
  return std::make_unique<sq::tempo::LocalBackend>(sq_time_monotonic_ns(), 120.0);
}

class TempoSyncService final : public sq::Object<TempoSyncService, sq::ITempoSync, sq::ITempoSyncInfo> {
 public:
  TempoSyncService() : backend_(chooseBackend()) {}

  sq_result SQ_CALL enable(sq_bool enabled) override {
    try {
      backend_->enable(enabled != SQ_FALSE);
      enabled_.store(enabled != SQ_FALSE);
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }
  sq_bool SQ_CALL isEnabled() override { return enabled_.load() ? SQ_TRUE : SQ_FALSE; }

  sq_result SQ_CALL captureState(uint64_t hostTimeNs, sq_tempo_sync_state* out) override {
    if (!out) return SQ_E_INVALIDARG;
    return backend_->capture(hostTimeNs, *out) ? SQ_OK : SQ_E_BUSY;
  }

  sq_result SQ_CALL setTempo(double bpm, uint64_t hostTimeNs) override {
    if (!(bpm > 0.0) || bpm > 1.0e6) return SQ_E_INVALIDARG;  // also rejects NaN
    try {
      backend_->setTempo(bpm, hostTimeNs);
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL setPlaying(sq_bool playing, uint64_t hostTimeNs) override {
    try {
      backend_->setPlaying(playing != SQ_FALSE, hostTimeNs);
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL setQuantum(double beats) override {
    if (!(beats > 0.0)) return SQ_E_INVALIDARG;
    try {
      backend_->setQuantum(beats);
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL getBackendName(char* buf, uint32_t bufSize, uint32_t* needed) override {
    const char* n = backend_->name();
    uint32_t len = static_cast<uint32_t>(std::strlen(n)) + 1;
    if (needed) *needed = len;
    if (!buf || bufSize < len) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, n, len);
    return SQ_OK;
  }

  sq_bool SQ_CALL hasNetworkBackend() override { return sq::tempo::sdkBackendCompiled() ? SQ_TRUE : SQ_FALSE; }

  void shutdown() {
    try {
      backend_->enable(false);
    } catch (...) {
    }
  }

 private:
  std::unique_ptr<SyncBackend> backend_;
  std::atomic<bool> enabled_{false};
};

TempoSyncService* g_service = nullptr;

void onShutdown() {
  if (!g_service) return;
  g_service->shutdown();
  if (sq::IHostContext* h = sq::host()) h->unregisterService(&sq::ITempoSync::iid, static_cast<sq::ITempoSync*>(g_service));
  g_service->release();
  g_service = nullptr;
}

sq_result onInit(sq::IHostContext* host) {
  g_service = nullptr;
  try {
    g_service = new TempoSyncService();
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  if (SQ_FAILED(host->registerService(&sq::ITempoSync::iid, static_cast<sq::ITempoSync*>(g_service))))
    sq::logf(SQ_LOG_INFO, "ITempoSync already provided; not registering the singleton");
  return SQ_OK;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<TempoSyncService>(sq::tempo_sync::kTempoSyncClassId, "Service", "Tempo Sync"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('T', 'S', 'Y', 'N'),
    .name = "link-ableton",
    .description = "Network tempo/beat/phase sync seam with a local timeline fallback (optional)",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
