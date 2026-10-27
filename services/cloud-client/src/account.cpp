#include "account.hpp"

#include <cstdio>
#include <cstring>

#include "sq/platform.hpp"

namespace sq::cloud {

namespace {
void copyStr(char* dst, size_t cap, const std::string& s) {
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  std::memcpy(dst, s.data(), n);
  dst[n] = 0;
}
constexpr const char* kRefreshSecret = "refresh-token";
}  // namespace

AccountCore::AccountCore(CloudConfig cfg, HttpTransport* http, SecretStore& secrets, Clock clock)
    : cfg_(std::move(cfg)),
      http_(http),
      secrets_(secrets),
      clock_(std::move(clock)),
      telemetry_(cfg_.dataDir.empty() ? std::string() : sq::joinPath(cfg_.dataDir, "telemetry.json")) {
  while (!cfg_.baseUrl.empty() && cfg_.baseUrl.back() == '/') cfg_.baseUrl.pop_back();
  info_.struct_size = sizeof info_;
}

std::string AccountCore::cachePath() const {
  return cfg_.dataDir.empty() ? std::string() : sq::joinPath(cfg_.dataDir, "entitlements.json");
}

std::string AccountCore::url(const char* path) const { return cfg_.baseUrl + path; }

void AccountCore::load() {
  std::lock_guard<std::mutex> lk(m_);
  haveCache_ = false;
  std::string path = cachePath();
  if (path.empty() || !sq_fs_exists(path.c_str())) return;  // no key is created without a cache
  auto doc = sq::readFile(path);
  if (!doc) return;
  std::string key;
  if (!secrets_.get("device-key", key) || key.size() != 32) return;  // cache from another device
  Entitlements e;
  std::string err;
  if (!verifyEntitlements(*doc, key, e, &err)) {
    sq_log_printf(SQ_LOG_WARN, "cloud-client", "ignoring entitlement cache: %s", err.c_str());
    return;
  }
  ent_ = std::move(e);
  haveCache_ = true;
}

sq_account_state AccountCore::state() const {
  sq_account_state s{};
  s.struct_size = sizeof s;
  std::lock_guard<std::mutex> lk(m_);
  if (!haveCache_) {
    s.state = SQ_ACCOUNT_SIGNED_OUT;
    return s;
  }
  copyStr(s.display_name, sizeof s.display_name, ent_.displayName);
  s.last_verified = ent_.lastVerified;
  const int64_t now = clock_() / 1000;
  if (now - ent_.lastVerified > cfg_.graceSeconds || (ent_.validUntil > 0 && now > ent_.validUntil))
    s.state = SQ_ACCOUNT_EXPIRED;
  else
    s.state = verifiedThisSession_ ? SQ_ACCOUNT_SIGNED_IN : SQ_ACCOUNT_OFFLINE;
  return s;
}

bool AccountCore::hasEntitlement(const char* feature) const {
  if (!feature || !*feature) return false;
  if (std::strncmp(feature, "core.", 5) == 0) return true;
  int32_t st = state().state;
  if (st != SQ_ACCOUNT_SIGNED_IN && st != SQ_ACCOUNT_OFFLINE) return false;
  std::lock_guard<std::mutex> lk(m_);
  return featureGranted(ent_.features, feature);
}

void AccountCore::setInfoLocked(int32_t phase, const std::string& error) {
  info_.phase = phase;
  copyStr(info_.error, sizeof info_.error, error);
}

sq_result AccountCore::beginSignIn() {
  if (!networkConfigured()) return SQ_E_UNSUPPORTED;
  bool expected = false;
  if (!signInRunning_.compare_exchange_strong(expected, true)) return SQ_E_BUSY;
  cancelRequested_.store(false);
  signInGen_ = sessionGen();
  DeviceFlowConfig fc{url("/oauth/device/code"), url("/oauth/token"), cfg_.clientId, cfg_.scope};
  flow_ = std::make_unique<DeviceFlow>(*http_, std::move(fc));
  std::lock_guard<std::mutex> lk(m_);
  info_ = sq_cloud_signin_info{};
  info_.struct_size = sizeof info_;
  setInfoLocked(SQ_SIGNIN_REQUESTING, "");
  return SQ_OK;
}

int64_t AccountCore::stepSignIn() {
  if (!flow_ || !signInRunning_.load()) return -1;
  auto finish = [&](int32_t phase, const std::string& err) {
    {
      std::lock_guard<std::mutex> lk(m_);
      setInfoLocked(phase, err);
    }
    flow_.reset();
    signInRunning_.store(false);
    return int64_t(-1);
  };
  if (cancelRequested_.load()) return finish(SQ_SIGNIN_CANCELLED, "cancelled");
  int64_t delay = flow_->step(clock_());  // HTTP, outside the lock
  if (cancelRequested_.load()) return finish(SQ_SIGNIN_CANCELLED, "cancelled");
  switch (flow_->state()) {
    case DeviceFlow::State::Waiting: {
      std::lock_guard<std::mutex> lk(m_);
      copyStr(info_.user_code, sizeof info_.user_code, flow_->userCode());
      copyStr(info_.verification_uri, sizeof info_.verification_uri, flow_->verificationUri());
      copyStr(info_.verification_uri_complete, sizeof info_.verification_uri_complete, flow_->verificationUriComplete());
      info_.expires_at = flow_->expiresAtMs() / 1000;
      setInfoLocked(SQ_SIGNIN_WAITING, "");
      return delay;
    }
    case DeviceFlow::State::Succeeded: {
      TokenSet t = flow_->tokens();
      if (!t.refreshToken.empty() && !storeRefreshToken(t.refreshToken, signInGen_))
        return finish(SQ_SIGNIN_CANCELLED, "cancelled");
      sq_result r = fetchEntitlements(t.accessToken, signInGen_);
      if (r == SQ_E_CLOSED) return finish(SQ_SIGNIN_CANCELLED, "cancelled");
      if (SQ_FAILED(r)) return finish(SQ_SIGNIN_FAILED, "entitlements_unavailable");
      return finish(SQ_SIGNIN_SUCCEEDED, "");
    }
    case DeviceFlow::State::Denied:
      return finish(SQ_SIGNIN_DENIED, flow_->error());
    case DeviceFlow::State::Expired:
      return finish(SQ_SIGNIN_EXPIRED, flow_->error());
    case DeviceFlow::State::Cancelled:
      return finish(SQ_SIGNIN_CANCELLED, "cancelled");
    case DeviceFlow::State::Failed:
    default:
      return finish(SQ_SIGNIN_FAILED, flow_->error());
  }
}

sq_cloud_signin_info AccountCore::signInInfo() const {
  std::lock_guard<std::mutex> lk(m_);
  return info_;
}

bool AccountCore::cancelSignIn() {
  if (!signInRunning_.load()) return false;
  cancelRequested_.store(true);
  return true;
}

uint64_t AccountCore::sessionGen() {
  std::lock_guard<std::mutex> lk(commitM_);
  return gen_;
}

bool AccountCore::storeRefreshToken(const std::string& token, uint64_t gen) {
  std::lock_guard<std::mutex> lk(commitM_);
  if (gen != gen_) return false;  // signed out meanwhile
  secrets_.put(kRefreshSecret, token);
  return true;
}

sq_result AccountCore::fetchEntitlements(const std::string& accessToken, uint64_t gen) {
  HttpRequest req;
  req.url = url("/v1/entitlements");
  req.headers = {{"Authorization", "Bearer " + accessToken}, {"Accept", "application/json"}};
  HttpResponse res = http_->send(req);
  if (res.status == 0) return SQ_E_IO;
  if (res.status == 401 || res.status == 403) return SQ_E_ACCESS;
  Entitlements e;
  if (res.status < 200 || res.status >= 300 || !parseEntitlementResponse(res.body, e)) return SQ_E_FAIL;
  e.lastVerified = clock_() / 1000;
  std::lock_guard<std::mutex> ck(commitM_);
  if (gen != gen_) return SQ_E_CLOSED;  // signed out while the request was on the wire
  std::lock_guard<std::mutex> lk(m_);
  ent_ = std::move(e);
  haveCache_ = true;
  verifiedThisSession_ = true;
  persistLocked();
  return SQ_OK;
}

void AccountCore::persistLocked() {
  std::string path = cachePath();
  if (path.empty()) return;
  if (sq_fs_create_dirs(cfg_.dataDir.c_str()) != SQ_OK) return;
  std::string key = deviceKey(secrets_);
  sq::writeFile(path, signEntitlements(ent_, key));
}

sq_result AccountCore::refresh() {
  if (!networkConfigured()) return SQ_E_UNSUPPORTED;
  const uint64_t gen = sessionGen();
  std::string refreshToken;
  if (!secrets_.get(kRefreshSecret, refreshToken) || refreshToken.empty()) return SQ_E_ACCESS;
  TokenSet t;
  std::string code;
  HttpResponse res = refreshTokens(*http_, url("/oauth/token"), cfg_.clientId, refreshToken, t, &code);
  if (res.status == 0) return SQ_E_IO;  // offline: keep the cache, state stays OFFLINE
  if (code == "invalid_grant" || res.status == 401) {
    if (sessionGen() != gen) return SQ_E_ACCESS;  // already signed out (maybe signed in again)
    signOut();  // revoked or expired grant: the cached entitlements are no longer backed
    return SQ_E_ACCESS;
  }
  if (res.status < 200 || res.status >= 300) return SQ_E_FAIL;
  if (t.refreshToken != refreshToken && !storeRefreshToken(t.refreshToken, gen)) return SQ_E_ACCESS;
  sq_result r = fetchEntitlements(t.accessToken, gen);
  return r == SQ_E_CLOSED ? SQ_E_ACCESS : r;
}

void AccountCore::signOut() {
  cancelSignIn();
  std::lock_guard<std::mutex> ck(commitM_);
  ++gen_;
  secrets_.erase(kRefreshSecret);
  std::lock_guard<std::mutex> lk(m_);
  ent_ = Entitlements{};
  haveCache_ = false;
  verifiedThisSession_ = false;
  std::string path = cachePath();
  if (!path.empty()) sq_fs_remove(path.c_str());
}

int AccountCore::flushTelemetry() {
  if (!networkConfigured()) return -1;
  return telemetry_.flush(*http_, url("/v1/telemetry"));
}

}  // namespace sq::cloud
