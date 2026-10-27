// cloud-client: OAuth 2.0 Device Authorization Grant (RFC 8628) as a step-driven state machine.
// No threads, no clock: the caller passes "now" (milliseconds, any monotonic origin) and runs
// step() again after the returned delay. Each step makes at most one HTTP request.
//
//   Idle --step--> (device code request) --ok--> Waiting --step after interval--> token poll
//     authorization_pending -> Waiting (same interval)
//     slow_down             -> Waiting (interval + 5 s, RFC 8628 3.5)
//     access_denied         -> Denied
//     expired_token / past expires_in -> Expired
//     200 + access_token    -> Succeeded
//     transport failure     -> Waiting (retried; Failed after kMaxTransportFailures in a row)
//     anything else         -> Failed
#pragma once

#include <cstdint>
#include <string>

#include "http.hpp"

namespace sq::cloud {

struct TokenSet {
  std::string accessToken;
  std::string refreshToken;
  int64_t expiresInSec = 0;  // 0 = not reported
};

struct DeviceFlowConfig {
  std::string deviceEndpoint;  // full URL
  std::string tokenEndpoint;   // full URL
  std::string clientId;
  std::string scope;
};

class DeviceFlow {
 public:
  enum class State { Idle, Waiting, Succeeded, Denied, Expired, Failed, Cancelled };
  static constexpr int kMaxTransportFailures = 5;
  static constexpr const char* kGrantType = "urn:ietf:params:oauth:grant-type:device_code";

  DeviceFlow(HttpTransport& http, DeviceFlowConfig cfg) : http_(http), cfg_(std::move(cfg)) {}

  // Returns the delay in ms until step() should run again, or -1 when the flow has finished.
  int64_t step(int64_t nowMs);
  void cancel() { state_ = State::Cancelled; }

  State state() const { return state_; }
  bool finished() const { return state_ != State::Idle && state_ != State::Waiting; }
  const std::string& userCode() const { return userCode_; }
  const std::string& verificationUri() const { return verificationUri_; }
  const std::string& verificationUriComplete() const { return verificationUriComplete_; }
  int64_t expiresAtMs() const { return expiresAtMs_; }
  int64_t intervalMs() const { return intervalMs_; }
  const std::string& error() const { return error_; }
  const TokenSet& tokens() const { return tokens_; }

 private:
  int64_t requestCode(int64_t nowMs);
  int64_t poll(int64_t nowMs);
  int64_t fail(const std::string& err);

  HttpTransport& http_;
  DeviceFlowConfig cfg_;
  State state_ = State::Idle;
  std::string deviceCode_, userCode_, verificationUri_, verificationUriComplete_, error_;
  int64_t expiresAtMs_ = 0, intervalMs_ = 5000, nextPollMs_ = 0;
  int transportFailures_ = 0;
  TokenSet tokens_;
};

// Token endpoint helpers shared with the refresh path. Parses a token response body.
bool parseTokenResponse(const std::string& body, TokenSet& out, std::string* errorCode);
// Refresh-token grant. status 0 => transport failure (offline), else the HTTP status.
HttpResponse refreshTokens(HttpTransport& http, const std::string& tokenEndpoint, const std::string& clientId,
                           const std::string& refreshToken, TokenSet& out, std::string* errorCode);

}  // namespace sq::cloud
