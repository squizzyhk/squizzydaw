// cloud-client through the real module loader:
//   - offline-only (no BaseUrl): no network, signed-out state, core features granted, telemetry
//     opt-in semantics, a pre-seeded signed cache is honoured;
//   - end to end against a loopback server: device-flow sign-in, entitlements, refresh, telemetry
//     upload, sign-out, "account.changed" notifications.
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include "loopback_http.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/platform.hpp"
#include "sq/services/cloud_client/cloud_client.h"
#include "sqtest.hpp"
#include "store.hpp"

namespace {

std::string tempDir(const char* name) {
  std::string d = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-cloud-module"), name);
  sq_fs_remove_all(d.c_str());
  sq_fs_create_dirs(d.c_str());
  return d;
}

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  bool loaded = false;
  explicit Fixture(const std::string& dataDir, const std::string& baseUrl = "") {
    settings.setString("Services.cloud-client.DataDir", dataDir.c_str());
    settings.setString("Services.cloud-client.BaseUrl", baseUrl.c_str());
    settings.setBool("Services.cloud-client.OpenBrowser", SQ_FALSE);
  }
  bool load() {
    std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"),
                                    std::string("cloud-client") + sq_dylib_suffix());
    loaded = loader.load(path).ok();
    return loaded;
  }
  sq::Ptr<sq::ICloudAccount> account() {
    sq::Ptr<sq::ICloudAccount> a;
    host.queryService(&sq::ICloudAccount::iid, a.putVoid());
    return a;
  }
};

class Observer final : public sq::Object<Observer, sq::IObserver> {
 public:
  void SQ_CALL onNotify(sq::IUnknown*, const char* topic, sq::IAttributes* args) override {
    std::lock_guard<std::mutex> lk(m_);
    if (std::strcmp(topic, sq::cloud::kTopicAccountChanged) == 0) {
      ++count_;
      lastState_ = args ? static_cast<int32_t>(args->getInt("state", -1)) : -1;
    }
    cv_.notify_all();
  }
  bool waitState(int32_t state, int ms) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return lastState_ == state; });
  }
  int count() {
    std::lock_guard<std::mutex> lk(m_);
    return count_;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  int count_ = 0;
  int32_t lastState_ = -1;
};

template <class Pred>
bool waitUntil(Pred p, int ms) {
  for (int i = 0; i < ms / 20; ++i) {
    if (p()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return p();
}

}  // namespace

SQ_TEST("cloud-client: offline-only module never needs the network") {
  std::string dir = tempDir("offline");
  Fixture f(dir);
  REQUIRE(f.load());
  auto acc = f.account();
  REQUIRE(acc);
  sq_account_state st{};
  REQUIRE_OK(acc->getAccountState(&st));
  CHECK_EQ(st.state, SQ_ACCOUNT_SIGNED_OUT);
  CHECK_EQ(acc->hasEntitlement("core.audio"), SQ_TRUE);
  CHECK_EQ(acc->hasEntitlement("sync.presets"), SQ_FALSE);
  CHECK_EQ(acc->beginSignIn(), SQ_E_UNSUPPORTED);
  CHECK_EQ(acc->refreshEntitlements(), SQ_E_UNSUPPORTED);
  CHECK_EQ(acc->getAccountState(nullptr), SQ_E_INVALIDARG);

  auto tel = acc.as<sq::ICloudTelemetry>();
  REQUIRE(tel);
  CHECK_EQ(tel->isOptedIn(), SQ_FALSE);
  CHECK_EQ(tel->track("app.start", nullptr), SQ_S_FALSE);  // dropped: not opted in
  CHECK_EQ(tel->getQueuedCount(), 0u);
  CHECK_EQ(tel->track("Bad Name", nullptr), SQ_E_INVALIDARG);
  CHECK_OK(tel->setOptIn(SQ_TRUE));
  CHECK_EQ(f.settings.getBool("Services.cloud-client.TelemetryOptIn", SQ_FALSE), SQ_TRUE);
  auto props = sq::makeAttributes();
  props->setInt("tracks", 12);
  props->setString("mode", "record");
  CHECK_OK(tel->track("session.open", props.get()));
  CHECK_EQ(tel->getQueuedCount(), 1u);
  CHECK_EQ(tel->flush(), SQ_E_UNSUPPORTED);
  CHECK_OK(tel->setOptIn(SQ_FALSE));
  CHECK_EQ(tel->getQueuedCount(), 0u);

  auto signin = acc.as<sq::ICloudSignIn>();
  REQUIRE(signin);
  sq_cloud_signin_info info{};
  REQUIRE_OK(signin->getSignInInfo(&info));
  CHECK_EQ(info.phase, SQ_SIGNIN_IDLE);
  CHECK_EQ(signin->cancelSignIn(), SQ_S_FALSE);

  acc.reset();
  tel.reset();
  signin.reset();
  CHECK_OK(f.loader.unload("cloud-client"));
  // Nothing but (possibly) the empty data dir was created: no key, no cache.
  CHECK(!sq_fs_exists(sq::joinPath(dir, "device-key.secret").c_str()));
  CHECK(!sq_fs_exists(sq::joinPath(dir, "entitlements.json").c_str()));
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("cloud-client: a signed cache from this device is honoured offline") {
  std::string dir = tempDir("seeded");
  {
    auto store = sq::cloud::makeFileSecretStore(dir);
    sq::cloud::Entitlements e;
    e.displayName = "Offline User";
    e.features = {"sync.presets"};
    e.lastVerified = static_cast<int64_t>(std::time(nullptr)) - 3600;
    REQUIRE_OK(sq::writeFile(sq::joinPath(dir, "entitlements.json"), sq::cloud::signEntitlements(e, sq::cloud::deviceKey(*store))));
  }
  Fixture f(dir);
  REQUIRE(f.load());
  auto acc = f.account();
  REQUIRE(acc);
  sq_account_state st{};
  REQUIRE_OK(acc->getAccountState(&st));
  CHECK_EQ(st.state, SQ_ACCOUNT_OFFLINE);
  CHECK_EQ(std::string(st.display_name), std::string("Offline User"));
  CHECK_EQ(acc->hasEntitlement("sync.presets"), SQ_TRUE);
  CHECK_EQ(acc->hasEntitlement("sync.tones"), SQ_FALSE);
  auto obs = sq::Ptr<Observer>::adopt(new Observer());
  auto subj = acc.as<sq::ISubject>();
  REQUIRE(subj);
  CHECK_OK(subj->addObserver(obs.get()));
  CHECK_EQ(subj->addObserver(obs.get()), SQ_S_FALSE);
  CHECK_OK(acc->signOut());
  CHECK(obs->waitState(SQ_ACCOUNT_SIGNED_OUT, 1000));
  CHECK_EQ(acc->hasEntitlement("sync.presets"), SQ_FALSE);
  CHECK(!sq_fs_exists(sq::joinPath(dir, "entitlements.json").c_str()));
  CHECK_OK(subj->removeObserver(obs.get()));
  CHECK_EQ(subj->removeObserver(obs.get()), SQ_E_NOTFOUND);
  acc.reset();
  subj.reset();
  CHECK_OK(f.loader.unload("cloud-client"));
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("cloud-client: end to end sign-in, refresh and telemetry against a loopback server") {
#if !defined(_WIN32)
  SQ_SKIP("WinHTTP transport is Windows-only");
#else
  std::mutex m;
  int tokenPolls = 0, telemetryPosts = 0;
  LoopbackHttpServer srv([&](const LoopbackRequest& r) {
    std::lock_guard<std::mutex> lk(m);
    LoopbackResponse res;
    if (r.path == "/oauth/device/code") {
      res.body = R"({"device_code":"dc","user_code":"QWER-TYUI","verification_uri":"https://auth.example.test/device","expires_in":120,"interval":1})";
    } else if (r.path == "/oauth/token") {
      if (r.body.find("grant_type=refresh_token") != std::string::npos) {
        res.body = R"({"access_token":"at-2","refresh_token":"rt-2"})";
      } else if (++tokenPolls != 2) {  // approve exactly once; later sign-ins stay pending
        res.status = 400;
        res.body = R"({"error":"authorization_pending"})";
      } else {
        res.body = R"({"access_token":"at-1","refresh_token":"rt-1","expires_in":3600})";
      }
    } else if (r.path == "/v1/entitlements") {
      bool ok = r.headers.find("Bearer at-") != std::string::npos;
      res.status = ok ? 200 : 401;
      res.body = R"({"display_name":"Loop Back","features":["sync.*"]})";
    } else if (r.path == "/v1/telemetry") {
      ++telemetryPosts;
      res.status = 202;
    } else {
      res.status = 404;
    }
    return res;
  });
  REQUIRE(srv.port() != 0);
  std::string dir = tempDir("e2e");
  Fixture f(dir, srv.base());
  f.settings.setBool("Services.cloud-client.TelemetryOptIn", SQ_TRUE);
  REQUIRE(f.load());
  auto acc = f.account();
  REQUIRE(acc);
  auto obs = sq::Ptr<Observer>::adopt(new Observer());
  auto subj = acc.as<sq::ISubject>();
  REQUIRE_OK(subj->addObserver(obs.get()));
  auto signin = acc.as<sq::ICloudSignIn>();
  REQUIRE(signin);

  REQUIRE_OK(acc->beginSignIn());
  CHECK_EQ(acc->beginSignIn(), SQ_E_BUSY);
  REQUIRE(waitUntil(
      [&] {
        sq_cloud_signin_info i{};
        signin->getSignInInfo(&i);
        return i.phase == SQ_SIGNIN_WAITING || i.phase >= SQ_SIGNIN_SUCCEEDED;
      },
      5000));
  sq_cloud_signin_info info{};
  signin->getSignInInfo(&info);
  CHECK_EQ(std::string(info.user_code), std::string("QWER-TYUI"));
  REQUIRE(obs->waitState(SQ_ACCOUNT_SIGNED_IN, 15000));
  signin->getSignInInfo(&info);
  CHECK_EQ(info.phase, SQ_SIGNIN_SUCCEEDED);
  CHECK_EQ(acc->hasEntitlement("sync.presets"), SQ_TRUE);
  CHECK(sq_fs_exists(sq::joinPath(dir, "entitlements.json").c_str()));
  CHECK(sq_fs_exists(sq::joinPath(dir, "refresh-token.secret").c_str()));
  // The refresh token is not stored in clear text (DPAPI).
  auto raw = sq::readFile(sq::joinPath(dir, "refresh-token.secret"));
  REQUIRE(raw.has_value());
  CHECK(raw->find("rt-1") == std::string::npos);

  int before = obs->count();
  REQUIRE_OK(acc->refreshEntitlements());
  CHECK(waitUntil([&] { return obs->count() > before; }, 10000));
  sq_account_state st{};
  REQUIRE_OK(acc->getAccountState(&st));
  CHECK_EQ(st.state, SQ_ACCOUNT_SIGNED_IN);

  auto tel = acc.as<sq::ICloudTelemetry>();
  REQUIRE(tel);
  CHECK_EQ(tel->isOptedIn(), SQ_TRUE);
  CHECK_OK(tel->track("session.open", nullptr));
  CHECK_OK(tel->flush());
  CHECK(waitUntil([&] { return tel->getQueuedCount() == 0; }, 10000));
  {
    std::lock_guard<std::mutex> lk(m);
    CHECK_EQ(telemetryPosts, 1);
    CHECK_EQ(tokenPolls, 2);
  }

  CHECK_OK(acc->signOut());
  REQUIRE_OK(acc->getAccountState(&st));
  CHECK_EQ(st.state, SQ_ACCOUNT_SIGNED_OUT);
  CHECK(!sq_fs_exists(sq::joinPath(dir, "refresh-token.secret").c_str()));

  // A sign-in that is cancelled while waiting for the user.
  REQUIRE_OK(acc->beginSignIn());
  REQUIRE(waitUntil(
      [&] {
        sq_cloud_signin_info i{};
        signin->getSignInInfo(&i);
        return i.phase == SQ_SIGNIN_WAITING;
      },
      5000));
  CHECK_OK(signin->cancelSignIn());
  CHECK(waitUntil(
      [&] {
        sq_cloud_signin_info i{};
        signin->getSignInInfo(&i);
        return i.phase == SQ_SIGNIN_CANCELLED;
      },
      5000));

  subj->removeObserver(obs.get());
  acc.reset();
  subj.reset();
  signin.reset();
  tel.reset();
  CHECK_OK(f.loader.unload("cloud-client"));
  sq_fs_remove_all(dir.c_str());
#endif
}
