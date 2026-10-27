// link-ableton: backend seam. The service forwards ITempoSync to one backend: the own local
// timeline (always available) or the SDK session adapter (only when compiled with
// SQ_LINK_SDK_DIR; see sdk_backend.cpp).
#pragma once

#include <memory>
#include <mutex>

#include "sq/interfaces/remote.h"
#include "timeline.hpp"

namespace sq::tempo {

class SyncBackend {
 public:
  virtual ~SyncBackend() = default;
  virtual const char* name() const noexcept = 0;
  virtual void enable(bool on) = 0;
  // RT-safe: no lock, no allocation.
  virtual bool capture(uint64_t hostNs, sq_tempo_sync_state& out) noexcept = 0;
  virtual void setTempo(double bpm, uint64_t hostNs) = 0;
  virtual void setPlaying(bool playing, uint64_t hostNs) = 0;
  virtual void setQuantum(double beats) = 0;
};

// Own local timeline: tempo/beat/phase from the host clock, no network, peers = 0.
class LocalBackend final : public SyncBackend {
 public:
  explicit LocalBackend(uint64_t nowNs, double bpm = 120.0);
  const char* name() const noexcept override { return "local-timeline"; }
  void enable(bool) override {}
  bool capture(uint64_t hostNs, sq_tempo_sync_state& out) noexcept override;
  void setTempo(double bpm, uint64_t hostNs) override;
  void setPlaying(bool playing, uint64_t hostNs) override;
  void setQuantum(double beats) override;

 private:
  std::mutex writeM_;  // serialises writers; readers never take it
  Timeline tl_;        // writer copy (under writeM_)
  StatePublisher pub_;
};

// nullptr when the SDK adapter is not compiled in.
std::unique_ptr<SyncBackend> makeSdkBackend(double bpm);
bool sdkBackendCompiled();

}  // namespace sq::tempo
