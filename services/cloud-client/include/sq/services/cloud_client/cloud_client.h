/* Squizzy Studio - cloud-client service: ICloudAccount (remote.h) + extension interfaces.
   OWNER: services/cloud-client.  Optional package (plan 2.8, M8).
   OFFLINE-FIRST: nothing here is needed by the core. With no Services.cloud-client.BaseUrl
   configured the service never touches the network: sign-in/refresh/flush return
   SQ_E_UNSUPPORTED and hasEntitlement answers from the signed local cache only.
   Sign-in is the OAuth 2.0 device authorization grant (RFC 8628): the user enters a short code
   on a web page (opened in the system browser when Services.cloud-client.OpenBrowser is true).
   Telemetry is OPT-IN (Services.cloud-client.TelemetryOptIn, default false): while opted out
   track() drops events and nothing is stored or sent.
   THREADING: callable from main/worker threads (not RT). "account.changed" observers are called
   on the service's worker thread (or on the calling thread for signOut/cancelSignIn). */
#ifndef SQ_SERVICES_CLOUD_CLIENT_H
#define SQ_SERVICES_CLOUD_CLIENT_H

#include "sq/interfaces/common.h"
#include "sq/interfaces/remote.h"

/* sq_cloud_signin_info.phase */
#define SQ_SIGNIN_IDLE       0
#define SQ_SIGNIN_REQUESTING 1 /* asking the server for a device code */
#define SQ_SIGNIN_WAITING    2 /* user_code is valid: waiting for the user to approve */
#define SQ_SIGNIN_SUCCEEDED  3
#define SQ_SIGNIN_DENIED     4
#define SQ_SIGNIN_EXPIRED    5
#define SQ_SIGNIN_FAILED     6
#define SQ_SIGNIN_CANCELLED  7

typedef struct sq_cloud_signin_info {
  uint32_t struct_size;
  int32_t phase;                      /* SQ_SIGNIN_* */
  char user_code[32];
  char verification_uri[512];
  char verification_uri_complete[512]; /* may be empty */
  int64_t expires_at;                  /* unix seconds, 0 if unknown */
  char error[128];                     /* server error code or transport error text */
} sq_cloud_signin_info;

#ifdef __cplusplus
namespace sq {

/* QI from ICloudAccount: state of the interactive sign-in started with beginSignIn(). */
struct ICloudSignIn : IUnknown {
  SQ_INTERFACE(ICloudSignIn, IUnknown, "{C1E33329-65D7-4BB1-8518-984011D7A333}")
  virtual sq_result SQ_CALL getSignInInfo(sq_cloud_signin_info* out) = 0;
  /* SQ_S_FALSE when no sign-in is running. */
  virtual sq_result SQ_CALL cancelSignIn() = 0;
};

/* QI from ICloudAccount: opt-in usage telemetry with a bounded, persisted queue. */
struct ICloudTelemetry : IUnknown {
  SQ_INTERFACE(ICloudTelemetry, IUnknown, "{71BF1BA5-45A4-4925-B378-8FB4504E6EA4}")
  /* Opting out clears the queue (memory and disk). Persisted in the settings. */
  virtual sq_result SQ_CALL setOptIn(sq_bool optIn) = 0;
  virtual sq_bool SQ_CALL isOptedIn() = 0;
  /* event: [a-z0-9_.]{1,64}. props (nullable, borrowed): int/double/bool/string values are kept,
     other types ignored. SQ_S_FALSE when dropped because the user has not opted in. */
  virtual sq_result SQ_CALL track(const char* event, IAttributes* props) = 0;
  virtual uint32_t SQ_CALL getQueuedCount() = 0;
  /* Background upload of queued events; result via "account.changed" is NOT sent (fire and forget). */
  virtual sq_result SQ_CALL flush() = 0;
};

namespace cloud {
inline constexpr sq_guid kCloudAccountClassId = sq::guid("{72EE097F-DF57-490C-BBA3-DF151E4E6A57}");
inline constexpr const char* kTopicAccountChanged = "account.changed";
}  // namespace cloud

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_SERVICES_CLOUD_CLIENT_H */
