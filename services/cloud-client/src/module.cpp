// cloud-client module: ICloudAccount (+ ISubject "account.changed", ICloudSignIn, ICloudTelemetry).
// Optional package (plan 2.8, M8). Settings (read when the service object is created):
//   Services.cloud-client.BaseUrl          ""  => offline-only: no transport is even created
//   Services.cloud-client.ClientId         "squizzy-studio-desktop"
//   Services.cloud-client.DataDir          ""  => <user data>/cloud
//   Services.cloud-client.OpenBrowser      true: open the verification page during sign-in
//   Services.cloud-client.OfflineGraceDays 30
//   Services.cloud-client.TelemetryOptIn   false
// Network work runs on one lazily started worker thread; nothing runs in onInit except reading
// the local cache.
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "account.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

using namespace sq::cloud;

const char* kPrefix = "Services.cloud-client.";
std::string key(const char* k) { return std::string(kPrefix) + k; }

int64_t unixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// One background thread running posted jobs in order; sleeps are interruptible.
class Worker {
 public:
  ~Worker() { stop(); }
  void post(std::function<void()> job) {
    std::lock_guard<std::mutex> lk(m_);
    if (stopping_) return;
    jobs_.push_back(std::move(job));
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    cv_.notify_all();
  }
  // Sleeps up to ms; false when interrupted (stop or wake()).
  bool sleep(int64_t ms) {
    std::unique_lock<std::mutex> lk(m_);
    bool woken = cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return stopping_ || wake_; });
    wake_ = false;
    return !woken;
  }
  void wake() {
    std::lock_guard<std::mutex> lk(m_);
    wake_ = true;
    cv_.notify_all();
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stopping_ = true;
      jobs_.clear();
      cv_.notify_all();
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  }
  bool stopping() {
    std::lock_guard<std::mutex> lk(m_);
    return stopping_;
  }

 private:
  void run() {
    sq_thread_set_current_name("sq-cloud");
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return stopping_ || !jobs_.empty(); });
        if (stopping_) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      try {
        job();
      } catch (...) {
      }
    }
  }
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  std::thread thread_;
  bool stopping_ = false;
  bool wake_ = false;
};

class CloudAccountService final : public sq::Object<CloudAccountService, sq::ICloudAccount, sq::ISubject,
                                                   sq::ICloudSignIn, sq::ICloudTelemetry> {
 public:
  CloudAccountService() {
    sq::ISettings* s = sq::host() ? sq::host()->getSettings() : nullptr;
    CloudConfig cfg;
    std::string base = s ? sq::getString(s, key("BaseUrl").c_str(), "") : "";
    if (!base.empty() && !isAllowedBaseUrl(base)) {
      sq::logf(SQ_LOG_WARN, "BaseUrl '%s' rejected (https required, http only for loopback): offline-only", base.c_str());
      base.clear();
    }
    cfg.baseUrl = base;
    if (s) {
      cfg.clientId = sq::getString(s, key("ClientId").c_str(), cfg.clientId);
      cfg.dataDir = sq::getString(s, key("DataDir").c_str(), "");
      int64_t days = s->getInt(key("OfflineGraceDays").c_str(), 30);
      cfg.graceSeconds = (days < 0 ? 0 : days) * 86400;
      openBrowser_ = s->getBool(key("OpenBrowser").c_str(), SQ_TRUE) != SQ_FALSE;
    }
    if (cfg.dataDir.empty()) cfg.dataDir = sq::joinPath(sq::specialDir(SQ_DIR_USER_DATA), "cloud");
    secrets_ = makeFileSecretStore(cfg.dataDir);
    if (!cfg.baseUrl.empty()) http_ = makeWinHttpTransport(true);
    core_ = std::make_unique<AccountCore>(cfg, http_.get(), *secrets_, &unixMs);
    core_->telemetry().setOptIn(s && s->getBool(key("TelemetryOptIn").c_str(), SQ_FALSE) != SQ_FALSE);
    core_->load();
    core_->telemetry().load();
  }
  ~CloudAccountService() override { shutdown(); }

  void shutdown() {
    if (core_) core_->cancelSignIn();
    worker_.stop();
    std::lock_guard<std::mutex> lk(om_);
    observers_.clear();
  }

  // ---- ICloudAccount ----
  sq_result SQ_CALL getAccountState(sq_account_state* out) override {
    if (!out) return SQ_E_INVALIDARG;
    try {
      *out = core_->state();
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL beginSignIn() override {
    sq_result r = core_->beginSignIn();
    if (SQ_FAILED(r)) return r;
    worker_.post([this] { runSignIn(); });
    notify();
    return SQ_OK;
  }

  sq_result SQ_CALL signOut() override {
    try {
      core_->signOut();
      worker_.wake();
      notify();
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_bool SQ_CALL hasEntitlement(const char* feature) override {
    try {
      return core_->hasEntitlement(feature) ? SQ_TRUE : SQ_FALSE;
    } catch (...) {
      return SQ_FALSE;
    }
  }

  sq_result SQ_CALL refreshEntitlements() override {
    if (!core_->networkConfigured()) return SQ_E_UNSUPPORTED;
    worker_.post([this] {
      sq_result r = core_->refresh();
      if (SQ_FAILED(r)) sq::logf(SQ_LOG_INFO, "entitlement refresh: %s", sq_result_string(r));
      notify();
    });
    return SQ_OK;
  }

  // ---- ISubject ----
  sq_result SQ_CALL addObserver(sq::IObserver* o) override {
    if (!o) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lk(om_);
    for (auto& p : observers_)
      if (p.get() == o) return SQ_S_FALSE;
    observers_.emplace_back(o);
    return SQ_OK;
  }
  sq_result SQ_CALL removeObserver(sq::IObserver* o) override {
    std::lock_guard<std::mutex> lk(om_);
    for (auto it = observers_.begin(); it != observers_.end(); ++it)
      if (it->get() == o) {
        observers_.erase(it);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }

  // ---- ICloudSignIn ----
  sq_result SQ_CALL getSignInInfo(sq_cloud_signin_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = core_->signInInfo();
    return SQ_OK;
  }
  sq_result SQ_CALL cancelSignIn() override {
    if (!core_->cancelSignIn()) return SQ_S_FALSE;
    worker_.wake();
    return SQ_OK;
  }

  // ---- ICloudTelemetry ----
  sq_result SQ_CALL setOptIn(sq_bool optIn) override {
    core_->telemetry().setOptIn(optIn != SQ_FALSE);
    if (sq::IHostContext* h = sq::host()) h->getSettings()->setBool(key("TelemetryOptIn").c_str(), optIn);
    return SQ_OK;
  }
  sq_bool SQ_CALL isOptedIn() override { return core_->telemetry().optedIn() ? SQ_TRUE : SQ_FALSE; }

  sq_result SQ_CALL track(const char* event, sq::IAttributes* props) override {
    if (!TelemetryQueue::validName(event)) return SQ_E_INVALIDARG;
    try {
      TelemetryEvent e;
      e.name = event;
      e.timeMs = unixMs();
      if (props) {
        for (uint32_t i = 0; i < props->getCount() && i < 32; ++i) {
          char k[128];
          if (props->getKey(i, k, sizeof k, nullptr) != SQ_OK) continue;
          TelemetryProp p;
          p.key = k;
          switch (props->getType(k)) {
            case SQ_VALUE_INT: p.kind = TelemetryProp::Int; p.i = props->getInt(k, 0); break;
            case SQ_VALUE_DOUBLE: p.kind = TelemetryProp::Double; p.d = props->getDouble(k, 0); break;
            case SQ_VALUE_BOOL: p.kind = TelemetryProp::Bool; p.b = props->getBool(k, SQ_FALSE) != SQ_FALSE; break;
            case SQ_VALUE_STRING: {
              p.kind = TelemetryProp::String;
              p.s = sq::getAttrString(props, k);
              if (p.s.size() > 256) p.s.resize(256);
              break;
            }
            default: continue;
          }
          e.props.push_back(std::move(p));
        }
      }
      return core_->telemetry().track(std::move(e)) ? SQ_OK : SQ_S_FALSE;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }
  uint32_t SQ_CALL getQueuedCount() override { return static_cast<uint32_t>(core_->telemetry().size()); }
  sq_result SQ_CALL flush() override {
    if (!core_->networkConfigured()) return SQ_E_UNSUPPORTED;
    if (!core_->telemetry().optedIn()) return SQ_S_FALSE;
    worker_.post([this] { core_->flushTelemetry(); });
    return SQ_OK;
  }

 private:
  void runSignIn() {
    bool browserOpened = false;
    for (;;) {
      int64_t delay = core_->stepSignIn();
      sq_cloud_signin_info info = core_->signInInfo();
      if (info.phase == SQ_SIGNIN_WAITING && !browserOpened && openBrowser_) {
        browserOpened = true;
        openVerificationPage(info);
      }
      notify();
      if (delay < 0 || worker_.stopping()) break;
      worker_.sleep(delay);  // cancelSignIn() wakes it; the next step then finishes as cancelled
    }
  }

  static void openVerificationPage(const sq_cloud_signin_info& info) {
    std::string url = info.verification_uri_complete[0] ? info.verification_uri_complete : info.verification_uri;
    if (!isAllowedBaseUrl(url)) return;  // only https (or loopback http) is ever handed to the shell
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, w.data(), n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
  }

  void notify() {
    std::vector<sq::Ptr<sq::IObserver>> obs;
    {
      std::lock_guard<std::mutex> lk(om_);
      obs = observers_;
    }
    if (obs.empty()) return;
    sq_account_state st = core_->state();
    sq_cloud_signin_info info = core_->signInInfo();
    auto args = sq::makeAttributes();
    args->setInt("state", st.state);
    args->setInt("signin_phase", info.phase);
    for (auto& o : obs) o->onNotify(static_cast<sq::ICloudAccount*>(this), sq::cloud::kTopicAccountChanged, args.get());
  }

  std::unique_ptr<SecretStore> secrets_;
  std::unique_ptr<HttpTransport> http_;
  std::unique_ptr<AccountCore> core_;
  bool openBrowser_ = true;
  std::mutex om_;
  std::vector<sq::Ptr<sq::IObserver>> observers_;
  Worker worker_;  // declared last: destroyed (joined) first
};

CloudAccountService* g_service = nullptr;

void onShutdown() {
  if (!g_service) return;
  g_service->shutdown();
  if (sq::IHostContext* h = sq::host()) h->unregisterService(&sq::ICloudAccount::iid, static_cast<sq::ICloudAccount*>(g_service));
  g_service->release();
  g_service = nullptr;
}

sq_result onInit(sq::IHostContext* host) {
  g_service = nullptr;
  try {
    g_service = new CloudAccountService();
  } catch (...) {
    return SQ_E_FAIL;
  }
  if (SQ_FAILED(host->registerService(&sq::ICloudAccount::iid, static_cast<sq::ICloudAccount*>(g_service))))
    sq::logf(SQ_LOG_INFO, "ICloudAccount already provided; not registering the singleton");
  return SQ_OK;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<CloudAccountService>(sq::cloud::kCloudAccountClassId, "Service", "Cloud Account"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'L', 'D', 'A'),
    .name = "cloud-client",
    .description = "Cloud account, entitlements and opt-in telemetry (optional, offline-first)",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
