/* Squizzy Studio - hardware-remote service: class IDs and extension interfaces.
   OWNER: services/hardware-remote.
   The frozen interfaces this service implements live in sdk/include/sq/interfaces/remote.h
   (IOscService, IDiscoveryService). This header only adds what remote.h does not cover, as NEW
   interfaces with their own IIDs, so remote.h stays untouched.
   IControlSurfaceService (remote.h) is NOT implemented yet: no class is registered for it.
   THREADING: main/worker thread (not RT). Listener callbacks arrive on the service's network
   thread (remote.h): post to the main thread yourself. */
#ifndef SQ_SERVICES_HARDWARE_REMOTE_H
#define SQ_SERVICES_HARDWARE_REMOTE_H

#include "sq/interfaces/remote.h"

/* OSC time tag (NTP format: seconds since 1900 in the high 32 bits, fraction in the low 32 bits).
   The value 1 means "immediately" (OSC 1.0). */
#define SQ_OSC_TIMETAG_IMMEDIATELY ((uint64_t)1)

/* One message inside a bundle (IOscBundleSender::sendBundle). */
typedef struct sq_osc_message {
  uint32_t struct_size;
  const char* address;
  const sq_osc_arg* args;
  uint32_t count;
} sq_osc_message;

#ifdef __cplusplus
namespace sq {

/* QI from IOscService: bundles with a time tag. */
struct IOscBundleSender : IUnknown {
  SQ_INTERFACE(IOscBundleSender, IUnknown, "{7A7B8E19-862B-4932-B357-3559C691572D}")
  virtual sq_result SQ_CALL sendBundle(const char* host, uint16_t port, uint64_t timeTag, const sq_osc_message* messages,
                                       uint32_t count) = 0;
};

/* Optional extension of the listener passed to IOscService::listen. When the listener answers
   this IID, the service calls onOscTimedMessage INSTEAD of IOscListener::onOscMessage, with the
   time tag of the enclosing bundle (SQ_OSC_TIMETAG_IMMEDIATELY for a bare message). */
struct IOscTimedListener : IUnknown {
  SQ_INTERFACE(IOscTimedListener, IUnknown, "{BBD519AE-642F-4DD5-8075-3B7C5FED9587}")
  virtual void SQ_CALL onOscTimedMessage(const char* address, const sq_osc_arg* args, uint32_t count, uint64_t timeTag,
                                         const char* fromHost, uint16_t fromPort) = 0;
};

/* QI from IDiscoveryService: which backend serves the calls. Name: "system-dnssd" (OS DNS-SD API),
   "builtin-mdns" (own responder/browser on UDP 5353) or "none". Selecting the backend may
   create it (no traffic is sent until the first browse/advertise). */
struct IDiscoveryBackendInfo : IUnknown {
  SQ_INTERFACE(IDiscoveryBackendInfo, IUnknown, "{0FEB4AC4-5C42-4F89-B059-96020E3D99B8}")
  virtual sq_result SQ_CALL getBackendName(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
};

namespace hardware_remote {
inline constexpr sq_guid kOscServiceClassId = sq::guid("{657E1EBC-01C5-407A-AAF6-0EC661B54914}");
inline constexpr sq_guid kDiscoveryServiceClassId = sq::guid("{5B33A72D-661E-414A-B9B4-516C6A1D2DF1}");
}  // namespace hardware_remote

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_SERVICES_HARDWARE_REMOTE_H */
