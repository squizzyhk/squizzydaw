#include "device_flow.hpp"

#include "json_util.hpp"

namespace sq::cloud {

namespace {
HttpRequest formPost(const std::string& url, const std::vector<std::pair<std::string, std::string>>& fields) {
  HttpRequest r;
  r.method = "POST";
  r.url = url;
  r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
  r.body = formEncode(fields);
  return r;
}
}  // namespace

bool parseTokenResponse(const std::string& body, TokenSet& out, std::string* errorCode) {
  JsonPtr j = jsonParse(body);
  if (!j || sq_json_type_of(j.get()) != SQ_JSON_OBJECT) {
    if (errorCode) *errorCode = "invalid_response";
    return false;
  }
  std::string err = jsonString(j.get(), "error");
  if (!err.empty()) {
    if (errorCode) *errorCode = err;
    return false;
  }
  out.accessToken = jsonString(j.get(), "access_token");
  out.refreshToken = jsonString(j.get(), "refresh_token");
  out.expiresInSec = jsonInt(j.get(), "expires_in", 0);
  if (out.accessToken.empty()) {
    if (errorCode) *errorCode = "invalid_response";
    return false;
  }
  return true;
}

HttpResponse refreshTokens(HttpTransport& http, const std::string& tokenEndpoint, const std::string& clientId,
                           const std::string& refreshToken, TokenSet& out, std::string* errorCode) {
  HttpResponse res = http.send(formPost(
      tokenEndpoint, {{"grant_type", "refresh_token"}, {"refresh_token", refreshToken}, {"client_id", clientId}}));
  if (res.status == 0) {
    if (errorCode) *errorCode = res.error;
    return res;
  }
  TokenSet t;
  std::string code;
  if (!parseTokenResponse(res.body, t, &code) || res.status < 200 || res.status >= 300) {
    if (errorCode) *errorCode = code.empty() ? "http_" + std::to_string(res.status) : code;
    if (res.status >= 200 && res.status < 300) res.status = 502;  // 2xx without a token: treat as server error
    return res;
  }
  if (t.refreshToken.empty()) t.refreshToken = refreshToken;  // rotation is optional (RFC 6749 6)
  out = t;
  return res;
}

int64_t DeviceFlow::fail(const std::string& err) {
  error_ = err;
  state_ = State::Failed;
  return -1;
}

int64_t DeviceFlow::step(int64_t nowMs) {
  switch (state_) {
    case State::Idle:
      return requestCode(nowMs);
    case State::Waiting:
      if (nowMs >= expiresAtMs_) {
        state_ = State::Expired;
        error_ = "expired_token";
        return -1;
      }
      if (nowMs < nextPollMs_) return nextPollMs_ - nowMs;
      return poll(nowMs);
    default:
      return -1;
  }
}

int64_t DeviceFlow::requestCode(int64_t nowMs) {
  std::vector<std::pair<std::string, std::string>> f = {{"client_id", cfg_.clientId}};
  if (!cfg_.scope.empty()) f.emplace_back("scope", cfg_.scope);
  HttpResponse res = http_.send(formPost(cfg_.deviceEndpoint, f));
  if (res.status == 0) return fail(res.error.empty() ? "transport_error" : res.error);
  JsonPtr j = jsonParse(res.body);
  if (res.status < 200 || res.status >= 300 || !j || sq_json_type_of(j.get()) != SQ_JSON_OBJECT) {
    std::string e = j ? jsonString(j.get(), "error") : std::string();
    return fail(e.empty() ? "http_" + std::to_string(res.status) : e);
  }
  deviceCode_ = jsonString(j.get(), "device_code");
  userCode_ = jsonString(j.get(), "user_code");
  verificationUri_ = jsonString(j.get(), "verification_uri");
  verificationUriComplete_ = jsonString(j.get(), "verification_uri_complete");
  int64_t expiresIn = jsonInt(j.get(), "expires_in", 0);
  int64_t interval = jsonInt(j.get(), "interval", 5);  // RFC 8628 3.2: default 5 s
  if (deviceCode_.empty() || userCode_.empty() || verificationUri_.empty() || expiresIn <= 0)
    return fail("invalid_response");
  if (interval < 1) interval = 1;
  if (interval > 300) interval = 300;
  if (expiresIn > 86400) expiresIn = 86400;  // hostile/garbled values: no signed overflow below
  intervalMs_ = interval * 1000;
  expiresAtMs_ = nowMs + expiresIn * 1000;
  nextPollMs_ = nowMs + intervalMs_;
  state_ = State::Waiting;
  return intervalMs_;
}

int64_t DeviceFlow::poll(int64_t nowMs) {
  HttpResponse res = http_.send(
      formPost(cfg_.tokenEndpoint, {{"grant_type", kGrantType}, {"device_code", deviceCode_}, {"client_id", cfg_.clientId}}));
  if (res.status == 0) {
    if (++transportFailures_ >= kMaxTransportFailures) return fail(res.error.empty() ? "transport_error" : res.error);
    nextPollMs_ = nowMs + intervalMs_;
    return intervalMs_;
  }
  transportFailures_ = 0;
  TokenSet t;
  std::string code;
  if (res.status >= 200 && res.status < 300 && parseTokenResponse(res.body, t, &code)) {
    tokens_ = t;
    state_ = State::Succeeded;
    return -1;
  }
  if (code.empty()) {  // non-2xx: read the error code from the body
    TokenSet ignore;
    parseTokenResponse(res.body, ignore, &code);
  }
  if (code == "authorization_pending") {
    nextPollMs_ = nowMs + intervalMs_;
    return intervalMs_;
  }
  if (code == "slow_down") {
    if (intervalMs_ < 300000) intervalMs_ += 5000;
    nextPollMs_ = nowMs + intervalMs_;
    return intervalMs_;
  }
  if (code == "access_denied") {
    state_ = State::Denied;
    error_ = code;
    return -1;
  }
  if (code == "expired_token") {
    state_ = State::Expired;
    error_ = code;
    return -1;
  }
  return fail(code.empty() || code == "invalid_response" ? "http_" + std::to_string(res.status) : code);
}

}  // namespace sq::cloud
