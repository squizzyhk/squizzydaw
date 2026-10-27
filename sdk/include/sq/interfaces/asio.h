/* Squizzy Studio - ASIO driver status (extension beside audiodevice.h).
   OWNER: orchestrator (promoted from services/audiodriver-asio; same IID, unchanged).
   IMPLEMENTS: services/audiodriver-asio: the ASIO IAudioDriver class
               (sq::audiodevice::kAsioDriverClassId) answers queryInterface for IAsioDriverStatus.
   CONSUMES: app (Audio.ListDevices / device settings), ui.

   The ASIO host is a licence seam. A build configured without -DSQ_ASIO_SDK_DIR still loads and
   registers the driver class but reports zero devices; IAudioDriver cannot say why.
   IAsioDriverStatus carries that reason as text, plus how many ASIO drivers the system registry
   lists.

   THREADING: main/control thread (same as IAudioDriver). Values refer to the last refreshDevices(). */
#ifndef SQ_INTERFACES_ASIO_H
#define SQ_INTERFACES_ASIO_H

#include "sq/module.h"

#ifdef __cplusplus
namespace sq {

struct IAsioDriverStatus : IUnknown {
  SQ_INTERFACE(IAsioDriverStatus, IUnknown, "{CCBE8845-3E77-4C37-B2C9-5E172776A278}")
  /* SQ_TRUE when this build contains the ASIO host (configured with SQ_ASIO_SDK_DIR). */
  virtual sq_bool SQ_CALL isHostCompiled() = 0;
  /* Human-readable status of the last refresh, UTF-8. Never empty. *needed = size incl. NUL;
     SQ_E_BUFFER_TOO_SMALL when bufSize is too small. */
  virtual sq_result SQ_CALL getStatusText(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* Drivers listed in the system's ASIO registry key (valid entries), whether usable or not. */
  virtual uint32_t SQ_CALL getInstalledDriverCount() = 0;
};

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_ASIO_H */
