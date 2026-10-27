// cloud-client: account logic, independent of threads and of the module ABI (unit-tested with a
// fake transport and a fake clock).
//
// Endpoints (relative to CloudConfig::baseUrl):
//   POST /oauth/device/code   RFC 8628 device authorization
//   POST /oauth/token         device_code and refresh_token grants
//   GET  /v1/entitlements     Bearer -> {"display_name","features":[...],"valid_until"}
//   POST /v1/telemetry        {"events":[...]}
//
// Account state (sq_account_state.state), computed from the signed cache and the clock:
//   no valid cache                                   -> SIGNED_OUT
//   cache older than the offline grace, or past its server valid_until -> EXPIRED
//   verified online during this session              -> SIGNED_IN
//   otherwise (cache only)                           -> OFFLINE
// hasEntitlement(): "core.*" always; else only in SIGNED_IN / OFFLINE, from the cache. It takes a
// short lock and never waits for the network: HTTP calls never run under that lock.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "device_flow.hpp"
#include "http.hpp"
#include "sq/services/cloud_client/cloud_client.h"
#include "store.hpp"
#include "telemetry.hpp"

namespace sq::cloud {

struct CloudConfig {
  std::string baseUrl;  // empty => offline-only, no network ever
  std::string clientId = "squizzy-studio-desktop";
  std::string scope = "entitlements offline_access";
  std::string dataDir;  // entitlements.json, telemetry.json
  int64_t graceSeconds = 30 * 86400;
};

class AccountCore {
 public:
  using Clock = std::function<int64_t()>;  // unix milliseconds

  // http may be null (offline-only). secrets must outlive the core.
  AccountCore(CloudConfig cfg, HttpTransport* http, SecretStore& secrets, Clock clock);

  bool networkConfigured() const { return http_ != nullptr && !cfg_.baseUrl.empty(); }
  void load();  // reads the signed cache; never creates files (telemetry().load() is separate)

  sq_account_state state() const;
  bool hasEntitlement(const char* feature) const;

  // ---- sign-in (beginSignIn on any thread, stepSignIn on ONE worker thread) ----
  sq_result beginSignIn();  // SQ_E_UNSUPPORTED offline-only, SQ_E_BUSY while one runs
  int64_t stepSignIn();     // one step; delay in ms until the next, or -1 when finished
  bool signInRunning() const { return signInRunning_.load(); }
  sq_cloud_signin_info signInInfo() const;
  bool cancelSignIn();      // false if none running

  // ---- online refresh (worker thread) ----
  // SQ_OK, SQ_E_UNSUPPORTED (offline-only), SQ_E_ACCESS (not signed in / grant revoked: signed
  // out), SQ_E_IO (network down: state stays OFFLINE), SQ_E_FAIL (server error).
  sq_result refresh();
  void signOut();

  TelemetryQueue& telemetry() { return telemetry_; }
  int flushTelemetry();  // -1 nothing sent / offline-only, else HTTP status (0 = transport failure)

  std::string cachePath() const;

 private:
  std::string url(const char* path) const;
  // Both commit only if no signOut() happened since `gen` was taken (sessionGen()): a sign-out
  // racing an in-flight refresh or sign-in must not be undone by its late result.
  sq_result fetchEntitlements(const std::string& accessToken, uint64_t gen);  // persists on success
  bool storeRefreshToken(const std::string& token, uint64_t gen);
  uint64_t sessionGen();
  void persistLocked();
  void setInfoLocked(int32_t phase, const std::string& error);

  CloudConfig cfg_;
  HttpTransport* http_;
  SecretStore& secrets_;
  Clock clock_;

  mutable std::mutex m_;
  Entitlements ent_;
  bool haveCache_ = false;
  bool verifiedThisSession_ = false;
  sq_cloud_signin_info info_{};

  std::mutex commitM_;    // serialises signOut() against committing network results (before m_)
  uint64_t gen_ = 0;      // bumped by signOut(), under commitM_
  uint64_t signInGen_ = 0;

  std::unique_ptr<DeviceFlow> flow_;  // touched only by beginSignIn (before the worker) + stepSignIn
  std::atomic<bool> signInRunning_{false};
  std::atomic<bool> cancelRequested_{false};

  TelemetryQueue telemetry_;
};

}  // namespace sq::cloud
