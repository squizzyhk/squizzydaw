/* Squizzy Studio - link-ableton service: network tempo/beat/phase sync (ITempoSync, remote.h).
   OWNER: services/link-ableton.
   Without the separately licensed network-sync SDK (CMake cache var SQ_LINK_SDK_DIR) the service
   runs an own local timeline: tempo, beat and phase advance from the host clock, peers = 0.
   With the SDK the same interface is backed by the SDK session (Services.link-ableton.Backend =
   "sdk" (default when compiled in) or "local").
   Host time: every hostTimeNs is in the sq_time_monotonic_ns() domain.
   THREADING: captureState is RT-safe (no lock, no allocation, bounded): call it from the audio
   thread. With the SDK backend it must only be called from ONE thread (the audio thread).
   Everything else: main/worker threads. */
#ifndef SQ_SERVICES_LINK_ABLETON_H
#define SQ_SERVICES_LINK_ABLETON_H

#include "sq/interfaces/remote.h"

#ifdef __cplusplus
namespace sq {

/* QI from ITempoSync. */
struct ITempoSyncInfo : IUnknown {
  SQ_INTERFACE(ITempoSyncInfo, IUnknown, "{77FFFE35-AE25-4BC6-8047-151D9EACCC61}")
  /* "local-timeline" or "sdk-session". */
  virtual sq_result SQ_CALL getBackendName(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* SQ_TRUE when the SDK adapter was compiled in (SQ_LINK_SDK_DIR). */
  virtual sq_bool SQ_CALL hasNetworkBackend() = 0;
};

namespace tempo_sync {
inline constexpr sq_guid kTempoSyncClassId = sq::guid("{04C7C36D-6A7E-4187-A6A9-ADB198A517F6}");
inline constexpr double kMinTempo = 20.0;
inline constexpr double kMaxTempo = 999.0;
}  // namespace tempo_sync

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_SERVICES_LINK_ABLETON_H */
