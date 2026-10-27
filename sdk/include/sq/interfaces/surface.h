/* Squizzy Studio - MIDI control-surface extension interface beside remote.h.
   OWNER: orchestrator (promoted from services/hardware-surface; same IID, unchanged).
   IMPLEMENTS: services/hardware-surface (QI from its IControlSurfaceService object).
   CONSUMES: app (Shell::idle pumps it), ui, scripting-js.

   The hardware-surface service implements IControlSurfaceService (remote.h) for MIDI control
   surfaces described by .device + .surface.xml maps. It is registered with
   host->registerService(&IControlSurfaceService::iid) and is always creatable from the class
   registry (category "ControlSurface").

   THREADING
   - IControlSurfaceListener::onControlChanged: the service's worker thread (as remote.h says).
     onSurfacesChanged: the thread that called pump() / loadSurfaceMap() (main thread).
   - Bound object:// targets are written ONLY from ISurfacePump::pump() (main thread). Hardware
     input queues up (coalesced per control) until pump() runs; the host calls it from its idle.
   - Feedback via setControlValue() is sent immediately through the RT-safe IMidiOutputPort. */
#ifndef SQ_INTERFACES_SURFACE_H
#define SQ_INTERFACES_SURFACE_H

#include "sq/interfaces/remote.h"

#ifdef __cplusplus
namespace sq {

/* QI from the IControlSurfaceService of the hardware-surface service. */
struct ISurfacePump : IUnknown {
  SQ_INTERFACE(ISurfacePump, IUnknown, "{BC55EA5D-F429-4471-9E5D-F42D81901139}")
  /* Main thread. Applies queued control -> target writes, re-reads bound targets and sends
     feedback for controls whose target changed (motor faders, LEDs), and reconnects/disconnects
     surfaces after MIDI port hot-plug. SQ_OK if anything happened, SQ_S_FALSE if idle. */
  virtual sq_result SQ_CALL pump() = 0;
  /* Validation/parse report of the last failed loadSurfaceMap (one problem per line), or "". */
  virtual sq_result SQ_CALL getLastError(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
};

namespace surface {
inline constexpr const char* kCategory = "ControlSurface";
inline constexpr sq_guid kHardwareSurfaceClassId = sq::guid("{77EB4CFA-E26B-4A00-ABEB-497EADAA2675}");
/* Optional: GUID string of the IMidiDriver class to use. Default: an IMidiDriver registered as a
   host service, else the first class of category "MidiDriver". */
inline constexpr const char* kSettingMidiDriver = "Services.hardware-surface.MidiDriver";
}  // namespace surface

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_SURFACE_H */
