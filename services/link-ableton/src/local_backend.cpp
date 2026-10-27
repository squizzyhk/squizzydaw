#include "backend.hpp"

namespace sq::tempo {

namespace {
Timeline initialTimeline(uint64_t nowNs, double bpm) {
  Timeline t;
  t.bpm = clampTempo(bpm);
  t.timeOrigin = nowNs;
  t.playTime = nowNs;
  return t;
}
}  // namespace

LocalBackend::LocalBackend(uint64_t nowNs, double bpm) : tl_(initialTimeline(nowNs, bpm)), pub_(tl_) {}

bool LocalBackend::capture(uint64_t hostNs, sq_tempo_sync_state& out) noexcept {
  Timeline t;
  if (!pub_.read(t)) return false;
  project(t, hostNs, 0, out);
  return true;
}

void LocalBackend::setTempo(double bpm, uint64_t hostNs) {
  std::lock_guard<std::mutex> lk(writeM_);
  tl_ = withTempo(tl_, bpm, hostNs);
  pub_.write(tl_);
}

void LocalBackend::setPlaying(bool playing, uint64_t hostNs) {
  std::lock_guard<std::mutex> lk(writeM_);
  tl_ = withPlaying(tl_, playing, hostNs);
  pub_.write(tl_);
}

void LocalBackend::setQuantum(double beats) {
  std::lock_guard<std::mutex> lk(writeM_);
  tl_.quantum = clampQuantum(beats);
  pub_.write(tl_);
}

#if !defined(SQ_HAVE_LINK_SDK)
std::unique_ptr<SyncBackend> makeSdkBackend(double) { return nullptr; }
bool sdkBackendCompiled() { return false; }
#endif

}  // namespace sq::tempo
