/* Squizzy Studio - optional connectivity seams (plan 2.8, 2.9, M8): hardware control surfaces,
   OSC, mDNS/DNS-SD discovery, network tempo sync, cloud account/entitlements.
   OWNER: orchestrator (frozen).
   IMPLEMENTS: services/hardware-remote (IOscService, IDiscoveryService);
               services/hardware-surface (IControlSurfaceService, MIDI .device/.surface.xml maps;
               extension ISurfacePump in surface.h);
               services/link-ableton (ITempoSync; a vendor SDK is optional and only compiled in when
               its path is supplied via a CMake cache var; without it the service runs a
               local-only clock); services/cloud-client (ICloudAccount).
               All registered via host->registerService; every one is OPTIONAL.
   CONSUMES: engine (tempo sync), app/ui (surfaces, account UI), scripting-js.
   OFFLINE-FIRST RULE: core features never depend on these services. When ICloudAccount is absent
   every entitlement check counts as granted for core features. hasEntitlement() answers from a
   local cache and never blocks on the network.
   THREADING: listener callbacks arrive on the service's network thread: post to the main thread.
   ITempoSync::captureState is RT-safe (wait-free); everything else is main/worker thread. */
#ifndef SQ_INTERFACES_REMOTE_H
#define SQ_INTERFACES_REMOTE_H

#include "sq/module.h"
#include "sq/interfaces/common.h"
#include "sq/interfaces/ui.h"

/* sq_surface_info.transport */
#define SQ_SURFACE_MIDI_USB     1
#define SQ_SURFACE_MIDI_NETWORK 2
#define SQ_SURFACE_BLE          3
#define SQ_SURFACE_OSC          4
#define SQ_SURFACE_CONNECTED    0x01u /* sq_surface_info.flags */

/* sq_account_state.state */
#define SQ_ACCOUNT_SIGNED_OUT 0
#define SQ_ACCOUNT_SIGNED_IN  1
#define SQ_ACCOUNT_OFFLINE    2 /* signed in, using cached entitlements */
#define SQ_ACCOUNT_EXPIRED    3

typedef struct sq_discovery_record {
  uint32_t struct_size;
  char name[128];          /* instance name */
  char type[64];           /* "_osc._udp", "_midi._udp", ... */
  char host[256];
  uint16_t port;
  char txt[512];           /* "key=value;key=value" */
} sq_discovery_record;

/* OSC argument. type: 'i' int32, 'h' int64, 'f' float32, 'd' double, 's' string, 'b' blob,
   'T' true, 'F' false, 'N' nil. Pointers valid during the call. */
typedef struct sq_osc_arg {
  char type;
  int64_t i;
  double d;
  const char* s;
  const void* blob;
  uint32_t blob_size;
} sq_osc_arg;

typedef struct sq_surface_info {
  uint32_t struct_size;
  char id[128];
  char name[128];
  int32_t transport;       /* SQ_SURFACE_* */
  uint32_t flags;
} sq_surface_info;

typedef struct sq_tempo_sync_state {
  uint32_t struct_size;
  double bpm;
  double beat;             /* session beat at host_time_ns */
  double phase;            /* beat modulo quantum */
  double quantum;
  sq_bool playing;
  uint32_t peers;
  uint64_t host_time_ns;
} sq_tempo_sync_state;

typedef struct sq_account_state {
  uint32_t struct_size;
  int32_t state;           /* SQ_ACCOUNT_* */
  char display_name[128];
  int64_t last_verified;   /* unix seconds of the last successful online check */
} sq_account_state;

#ifdef __cplusplus
namespace sq {

struct IDiscoveryListener : IUnknown {
  SQ_INTERFACE(IDiscoveryListener, IUnknown, "{B9E34C3D-6183-48FB-8271-B7BC8CFAE5D7}")
  virtual void SQ_CALL onServiceFound(const sq_discovery_record* record) = 0;
  virtual void SQ_CALL onServiceLost(const sq_discovery_record* record) = 0;
};

struct IDiscoveryService : IUnknown {
  SQ_INTERFACE(IDiscoveryService, IUnknown, "{C3D7A1BF-6F19-41BD-9474-0A0398DDB4B2}")
  virtual sq_result SQ_CALL browse(const char* serviceType, IDiscoveryListener* listener, uint32_t* cookie) = 0;
  virtual sq_result SQ_CALL advertise(const sq_discovery_record* record, uint32_t* cookie) = 0;
  /* Ends a browse or an advertisement. */
  virtual sq_result SQ_CALL cancel(uint32_t cookie) = 0;
};

struct IOscListener : IUnknown {
  SQ_INTERFACE(IOscListener, IUnknown, "{1145C07B-740A-4DFF-8DD4-49B3940EE4C9}")
  virtual void SQ_CALL onOscMessage(const char* address, const sq_osc_arg* args, uint32_t count,
                                    const char* fromHost, uint16_t fromPort) = 0;
};

struct IOscService : IUnknown {
  SQ_INTERFACE(IOscService, IUnknown, "{7B538881-A082-4811-B79F-591D7A9BFAE7}")
  /* Listens on UDP port (0 = any; *boundPort receives it). */
  virtual sq_result SQ_CALL listen(uint16_t port, IOscListener* listener, uint16_t* boundPort, uint32_t* cookie) = 0;
  virtual sq_result SQ_CALL stopListening(uint32_t cookie) = 0;
  virtual sq_result SQ_CALL send(const char* host, uint16_t port, const char* address, const sq_osc_arg* args, uint32_t count) = 0;
};

struct IControlSurfaceListener : IUnknown {
  SQ_INTERFACE(IControlSurfaceListener, IUnknown, "{A941A089-B1F0-4E42-86EA-991F4C4FEF75}")
  virtual void SQ_CALL onSurfacesChanged() = 0;
  /* value normalized 0..1 (encoders: relative delta in -1..1). */
  virtual void SQ_CALL onControlChanged(const char* surfaceId, const char* controlId, double value) = 0;
};

/* Hardware control surfaces driven by .surface.xml maps + remote pages (ui.h). */
struct IControlSurfaceService : IUnknown {
  SQ_INTERFACE(IControlSurfaceService, IUnknown, "{72D6976E-09CA-4154-A64E-E56F9AD66800}")
  virtual uint32_t SQ_CALL getSurfaceCount() = 0;
  virtual sq_result SQ_CALL getSurfaceInfo(uint32_t index, sq_surface_info* out) = 0;
  /* Loads a control-surface map (.device / .surface.xml) and binds it to a connection. */
  virtual sq_result SQ_CALL loadSurfaceMap(const char* path, const char* connectionId, char* surfaceIdBuf,
                                           uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL setRemotePages(const char* surfaceId, IRemotePageSet* pages) = 0;
  virtual sq_result SQ_CALL selectPage(const char* surfaceId, uint32_t pageIndex) = 0;
  /* Feedback to the hardware (motor fader, LED ring, display text; text nullable). */
  virtual sq_result SQ_CALL setControlValue(const char* surfaceId, const char* controlId, double value, const char* text) = 0;
  virtual sq_result SQ_CALL addListener(IControlSurfaceListener* listener) = 0;
  virtual sq_result SQ_CALL removeListener(IControlSurfaceListener* listener) = 0;
};

/* Network tempo/beat/phase sync seam. */
struct ITempoSync : IUnknown {
  SQ_INTERFACE(ITempoSync, IUnknown, "{2B09D4E2-C7EC-41B2-8E45-193F6E4524DA}")
  virtual sq_result SQ_CALL enable(sq_bool enabled) = 0;
  virtual sq_bool SQ_CALL isEnabled() = 0;
  /* RT-safe, wait-free: session state projected to host_time_ns. */
  virtual sq_result SQ_CALL captureState(uint64_t hostTimeNs, sq_tempo_sync_state* out) = 0;
  virtual sq_result SQ_CALL setTempo(double bpm, uint64_t hostTimeNs) = 0;
  virtual sq_result SQ_CALL setPlaying(sq_bool playing, uint64_t hostTimeNs) = 0;
  virtual sq_result SQ_CALL setQuantum(double beats) = 0;
};

/* Cloud account / entitlement seam (optional). QI: ISubject, topic "account.changed". */
struct ICloudAccount : IUnknown {
  SQ_INTERFACE(ICloudAccount, IUnknown, "{FEEE7AE5-AF15-499C-BF06-76B379E6C7DB}")
  virtual sq_result SQ_CALL getAccountState(sq_account_state* out) = 0;
  /* Starts interactive sign-in (system browser); completion via "account.changed". */
  virtual sq_result SQ_CALL beginSignIn() = 0;
  virtual sq_result SQ_CALL signOut() = 0;
  /* Cache-only, never blocks. feature = dotted id, e.g. "sync.presets". */
  virtual sq_bool SQ_CALL hasEntitlement(const char* feature) = 0;
  /* Background refresh; result via "account.changed". */
  virtual sq_result SQ_CALL refreshEntitlements() = 0;
};

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_REMOTE_H */
