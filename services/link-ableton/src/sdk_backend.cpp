// link-ableton: adapter onto the separately licensed network tempo-sync SDK. Compiled ONLY when
// the user configures -DSQ_LINK_SDK_DIR=<sdk checkout> (CMakeLists.txt); never vendored.
// Not built in the default configuration. Verified on 2026-10-28 against the SDK's public
// repository head: /W4 /WX clean, CTest link_ableton_sdk passed (session left disabled).
//
// Host time mapping: ITempoSync times are sq_time_monotonic_ns(); the SDK uses its own
// microsecond clock. Each call maps by sampling both clocks (both are QueryPerformanceCounter /
// CLOCK_MONOTONIC reads: RT-safe).
#include "backend.hpp"

#if defined(SQ_HAVE_LINK_SDK)

#include <atomic>
#include <chrono>

#include <ableton/Link.hpp>

#include "sq/platform.h"

namespace sq::tempo {
namespace {

class SdkBackend final : public SyncBackend {
 public:
  explicit SdkBackend(double bpm) : session_(clampTempo(bpm)) {
    session_.setNumPeersCallback([this](std::size_t n) { peers_.store(static_cast<uint32_t>(n)); });
    session_.enableStartStopSync(true);
  }
  ~SdkBackend() override { session_.enable(false); }

  const char* name() const noexcept override { return "sdk-session"; }
  void enable(bool on) override { session_.enable(on); }

  bool capture(uint64_t hostNs, sq_tempo_sync_state& out) noexcept override {
    // Audio-thread capture: RT-safe, single (audio) thread only.
    auto st = session_.captureAudioSessionState();
    const auto t = toSdkTime(hostNs);
    const double q = quantum_.load(std::memory_order_relaxed);
    out.struct_size = sizeof(sq_tempo_sync_state);
    out.bpm = st.tempo();
    out.beat = st.beatAtTime(t, q);
    out.phase = st.phaseAtTime(t, q);
    out.quantum = q;
    out.playing = st.isPlaying() ? SQ_TRUE : SQ_FALSE;
    out.peers = peers_.load(std::memory_order_relaxed);
    out.host_time_ns = hostNs;
    return true;
  }

  void setTempo(double bpm, uint64_t hostNs) override {
    auto st = session_.captureAppSessionState();
    st.setTempo(clampTempo(bpm), toSdkTime(hostNs));
    session_.commitAppSessionState(st);
  }

  void setPlaying(bool playing, uint64_t hostNs) override {
    auto st = session_.captureAppSessionState();
    st.setIsPlaying(playing, toSdkTime(hostNs));
    session_.commitAppSessionState(st);
  }

  void setQuantum(double beats) override { quantum_.store(clampQuantum(beats), std::memory_order_relaxed); }

 private:
  std::chrono::microseconds toSdkTime(uint64_t hostNs) const noexcept {
    const uint64_t nowHost = sq_time_monotonic_ns();
    const auto nowSdk = session_.clock().micros();
    const int64_t deltaNs =
        hostNs >= nowHost ? static_cast<int64_t>(hostNs - nowHost) : -static_cast<int64_t>(nowHost - hostNs);
    return nowSdk + std::chrono::microseconds(deltaNs / 1000);
  }

  // Declared before the session so they outlive its threads during destruction.
  std::atomic<double> quantum_{4.0};
  std::atomic<uint32_t> peers_{0};
  ableton::Link session_;
};

}  // namespace

std::unique_ptr<SyncBackend> makeSdkBackend(double bpm) { return std::make_unique<SdkBackend>(bpm); }
bool sdkBackendCompiled() { return true; }

}  // namespace sq::tempo

#endif  // SQ_HAVE_LINK_SDK
