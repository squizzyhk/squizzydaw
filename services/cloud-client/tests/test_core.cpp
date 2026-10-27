// cloud-client core: encoding, HMAC, signed cache, device flow, account state machine, telemetry.
// Everything runs against a scripted fake transport and a fake clock: no network at all.
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "account.hpp"
#include "device_flow.hpp"
#include "http.hpp"
#include "sq/platform.hpp"
#include "sqtest.hpp"
#include "store.hpp"
#include "telemetry.hpp"

using namespace sq::cloud;

namespace {

class FakeTransport final : public HttpTransport {
 public:
  using Fn = std::function<HttpResponse(const HttpRequest&)>;
  Fn fn;
  std::vector<HttpRequest> log;
  HttpResponse send(const HttpRequest& r) override {
    log.push_back(r);
    return fn ? fn(r) : HttpResponse{};
  }
};

HttpResponse ok(const std::string& body, int status = 200) {
  HttpResponse r;
  r.status = status;
  r.body = body;
  return r;
}
HttpResponse offline() {
  HttpResponse r;
  r.error = "no route";
  return r;
}
bool endsWith(const std::string& s, const char* suf) {
  std::string t(suf);
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

std::string tempDir(const char* name) {
  std::string d = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-cloud"), name);
  sq_fs_remove_all(d.c_str());
  sq_fs_create_dirs(d.c_str());
  return d;
}

const char* kDeviceCode =
    R"({"device_code":"dev-123","user_code":"WDJB-MJHT","verification_uri":"https://auth.example.test/device",)"
    R"("verification_uri_complete":"https://auth.example.test/device?c=WDJB-MJHT","expires_in":600,"interval":5})";

}  // namespace

SQ_TEST("cloud: url and form encoding, base url policy") {
  CHECK_EQ(urlEncode("a b&c=d/é~"), std::string("a%20b%26c%3Dd%2F%C3%A9~"));
  CHECK_EQ(formEncode({{"grant_type", "x:y"}, {"k", ""}}), std::string("grant_type=x%3Ay&k="));
  CHECK(isAllowedBaseUrl("https://api.example.test"));
  CHECK(isAllowedBaseUrl("http://127.0.0.1:8080/x"));
  CHECK(isAllowedBaseUrl("http://localhost/x"));
  CHECK(isAllowedBaseUrl("http://[::1]:99"));
  CHECK(!isAllowedBaseUrl("http://api.example.test"));
  CHECK(!isAllowedBaseUrl("http://127.0.0.1.evil.test/"));
  CHECK(!isAllowedBaseUrl("ftp://x"));
  CHECK(!isAllowedBaseUrl("https://"));
}

SQ_TEST("cloud: HMAC-SHA256 matches RFC 4231 test cases") {
  CHECK_EQ(hmacSha256Hex("Jefe", "what do ya want for nothing?"),
           std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
  std::string key(131, '\xaa');  // test case 6: key longer than the block size
  CHECK_EQ(hmacSha256Hex(key, "Test Using Larger Than Block-Size Key - Hash Key First"),
           std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
  CHECK(constantTimeEquals("abc", "abc"));
  CHECK(!constantTimeEquals("abc", "abd"));
  CHECK(!constantTimeEquals("abc", "ab"));
  CHECK_EQ(randomBytes(32).size(), size_t(32));
  CHECK(randomBytes(16) != randomBytes(16));
}

SQ_TEST("cloud: signed entitlement cache verifies, rejects tampering and foreign keys") {
  Entitlements e;
  e.displayName = "Ada";
  e.features = {"sync.presets", "assistant.*"};
  e.lastVerified = 1700000000;
  e.validUntil = 1800000000;
  const std::string key(32, 'k');
  std::string doc = signEntitlements(e, key);
  Entitlements back;
  std::string err;
  REQUIRE(verifyEntitlements(doc, key, back, &err));
  CHECK_EQ(back.displayName, std::string("Ada"));
  REQUIRE_EQ(back.features.size(), size_t(2));
  CHECK_EQ(back.lastVerified, int64_t(1700000000));
  CHECK_EQ(back.validUntil, int64_t(1800000000));

  CHECK(!verifyEntitlements(doc, std::string(32, 'x'), back, &err));  // another device's key
  CHECK_EQ(err, std::string("cache: bad signature"));
  std::string tampered = doc;
  size_t at = tampered.find("sync.presets");
  REQUIRE(at != std::string::npos);
  tampered.replace(at, 4, "SYNC");
  CHECK(!verifyEntitlements(tampered, key, back, &err));
  CHECK(!verifyEntitlements("{}", key, back, &err));
  CHECK(!verifyEntitlements("not json", key, back, &err));

  CHECK(featureGranted(e.features, "sync.presets"));
  CHECK(featureGranted(e.features, "assistant.chat"));
  CHECK(!featureGranted(e.features, "assistant"));
  CHECK(!featureGranted(e.features, "sync.tones"));
  CHECK(featureGranted({}, "core.anything"));
  CHECK(!featureGranted(e.features, ""));
  CHECK(!featureGranted(e.features, nullptr));
}

SQ_TEST("cloud: device flow happy path with pending and slow_down") {
  FakeTransport http;
  int polls = 0;
  http.fn = [&](const HttpRequest& r) {
    if (endsWith(r.url, "/device")) return ok(kDeviceCode);
    ++polls;
    if (polls == 1) return ok(R"({"error":"authorization_pending"})", 400);
    if (polls == 2) return ok(R"({"error":"slow_down"})", 400);
    return ok(R"({"access_token":"at-1","refresh_token":"rt-1","expires_in":3600,"token_type":"Bearer"})");
  };
  DeviceFlow f(http, {"https://a.test/device", "https://a.test/token", "client-x", "entitlements"});
  int64_t now = 1000;
  CHECK_EQ(f.step(now), int64_t(5000));
  CHECK(f.state() == DeviceFlow::State::Waiting);
  CHECK_EQ(f.userCode(), std::string("WDJB-MJHT"));
  CHECK_EQ(http.log[0].method, std::string("POST"));
  CHECK(http.log[0].body.find("client_id=client-x") != std::string::npos);
  CHECK(http.log[0].body.find("scope=entitlements") != std::string::npos);
  CHECK_EQ(f.step(now + 1000), int64_t(4000));  // too early: no request
  CHECK_EQ(http.log.size(), size_t(1));
  now += 5000;
  CHECK_EQ(f.step(now), int64_t(5000));  // pending
  CHECK(http.log[1].body.find("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code") != std::string::npos);
  CHECK(http.log[1].body.find("device_code=dev-123") != std::string::npos);
  now += 5000;
  CHECK_EQ(f.step(now), int64_t(10000));  // slow_down: +5 s
  now += 10000;
  CHECK_EQ(f.step(now), int64_t(-1));
  CHECK(f.state() == DeviceFlow::State::Succeeded);
  CHECK_EQ(f.tokens().accessToken, std::string("at-1"));
  CHECK_EQ(f.tokens().refreshToken, std::string("rt-1"));
  CHECK_EQ(f.tokens().expiresInSec, int64_t(3600));
}

SQ_TEST("cloud: device flow terminal states") {
  FakeTransport http;
  std::string pollAnswer;
  http.fn = [&](const HttpRequest& r) {
    if (endsWith(r.url, "/device")) return ok(kDeviceCode);
    return ok(pollAnswer, 400);
  };
  auto run = [&](const std::string& answer) {
    pollAnswer = answer;
    DeviceFlow f(http, {"https://a.test/device", "https://a.test/token", "c", ""});
    f.step(0);
    f.step(5000);
    return f;
  };
  CHECK(run(R"({"error":"access_denied"})").state() == DeviceFlow::State::Denied);
  CHECK(run(R"({"error":"expired_token"})").state() == DeviceFlow::State::Expired);
  DeviceFlow other = run(R"({"error":"invalid_client"})");
  CHECK(other.state() == DeviceFlow::State::Failed);
  CHECK_EQ(other.error(), std::string("invalid_client"));
  CHECK(run("<html>").state() == DeviceFlow::State::Failed);

  // Code lifetime runs out while pending.
  pollAnswer = R"({"error":"authorization_pending"})";
  DeviceFlow f(http, {"https://a.test/device", "https://a.test/token", "c", ""});
  f.step(0);
  CHECK_EQ(f.step(600000), int64_t(-1));
  CHECK(f.state() == DeviceFlow::State::Expired);

  // Bad device-code answers.
  http.fn = [&](const HttpRequest&) { return ok(R"({"device_code":"x"})"); };
  DeviceFlow g(http, {"https://a.test/device", "https://a.test/token", "c", ""});
  CHECK_EQ(g.step(0), int64_t(-1));
  CHECK(g.state() == DeviceFlow::State::Failed);
  http.fn = [&](const HttpRequest&) { return offline(); };
  DeviceFlow h(http, {"https://a.test/device", "https://a.test/token", "c", ""});
  h.step(0);
  CHECK(h.state() == DeviceFlow::State::Failed);
}

SQ_TEST("cloud: device flow retries transport failures while polling, then gives up") {
  FakeTransport http;
  int fails = 0;
  http.fn = [&](const HttpRequest& r) {
    if (endsWith(r.url, "/device")) return ok(R"({"device_code":"d","user_code":"U","verification_uri":"https://v.test","expires_in":900,"interval":1})");
    ++fails;
    return offline();
  };
  DeviceFlow f(http, {"https://a.test/device", "https://a.test/token", "c", ""});
  int64_t now = 0, d = f.step(now);
  CHECK_EQ(d, int64_t(1000));
  for (int i = 0; i < DeviceFlow::kMaxTransportFailures - 1; ++i) {
    now += d;
    d = f.step(now);
    CHECK(f.state() == DeviceFlow::State::Waiting);
  }
  now += d;
  CHECK_EQ(f.step(now), int64_t(-1));
  CHECK(f.state() == DeviceFlow::State::Failed);
  CHECK_EQ(fails, DeviceFlow::kMaxTransportFailures);
}

namespace {
struct Server {
  FakeTransport http;
  bool down = false;
  bool revoked = false;
  int entitlementCalls = 0;
  std::string features = R"(["sync.presets","assistant.*"])";
  Server() {
    http.fn = [this](const HttpRequest& r) {
      if (down) return offline();
      if (endsWith(r.url, "/oauth/device/code"))
        return ok(R"({"device_code":"d1","user_code":"ABCD","verification_uri":"https://auth.test/d","expires_in":300,"interval":2})");
      if (endsWith(r.url, "/oauth/token")) {
        if (r.body.find("grant_type=refresh_token") != std::string::npos) {
          if (revoked) return ok(R"({"error":"invalid_grant"})", 400);
          return ok(R"({"access_token":"at-2","refresh_token":"rt-2"})");
        }
        return ok(R"({"access_token":"at-1","refresh_token":"rt-1"})");
      }
      if (endsWith(r.url, "/v1/entitlements")) {
        ++entitlementCalls;
        bool authed = false;
        for (auto& [k, v] : r.headers)
          if (k == "Authorization" && (v == "Bearer at-1" || v == "Bearer at-2")) authed = true;
        if (!authed) return ok("{}", 401);
        return ok(std::string(R"({"display_name":"Ada","valid_until":0,"features":)") + features + "}");
      }
      if (endsWith(r.url, "/v1/telemetry")) return ok("{}", 202);
      return ok("", 404);
    };
  }
};
}  // namespace

SQ_TEST("cloud: account sign-in, offline restart, grace expiry, refresh and revocation") {
  std::string dir = tempDir("account");
  Server srv;
  MemorySecretStore secrets;
  int64_t nowMs = 1700000000000;
  CloudConfig cfg;
  cfg.baseUrl = "https://api.test/";
  cfg.dataDir = dir;
  cfg.graceSeconds = 7 * 86400;
  auto clock = [&] { return nowMs; };
  {
    AccountCore core(cfg, &srv.http, secrets, clock);
    core.load();
    CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
    CHECK(!core.hasEntitlement("sync.presets"));
    CHECK(core.hasEntitlement("core.audio"));
    REQUIRE_OK(core.beginSignIn());
    CHECK_EQ(core.beginSignIn(), SQ_E_BUSY);
    CHECK_EQ(core.signInInfo().phase, SQ_SIGNIN_REQUESTING);
    int64_t d = core.stepSignIn();
    CHECK_EQ(d, int64_t(2000));
    auto info = core.signInInfo();
    CHECK_EQ(info.phase, SQ_SIGNIN_WAITING);
    CHECK_EQ(std::string(info.user_code), std::string("ABCD"));
    CHECK_EQ(std::string(info.verification_uri), std::string("https://auth.test/d"));
    CHECK_EQ(info.expires_at, nowMs / 1000 + 300);
    nowMs += d;
    CHECK_EQ(core.stepSignIn(), int64_t(-1));
    CHECK_EQ(core.signInInfo().phase, SQ_SIGNIN_SUCCEEDED);
    CHECK(!core.signInRunning());
    auto st = core.state();
    CHECK_EQ(st.state, SQ_ACCOUNT_SIGNED_IN);
    CHECK_EQ(std::string(st.display_name), std::string("Ada"));
    CHECK_EQ(st.last_verified, nowMs / 1000);
    CHECK(core.hasEntitlement("sync.presets"));
    CHECK(core.hasEntitlement("assistant.chat"));
    CHECK(!core.hasEntitlement("sync.tones"));
    std::string rt;
    REQUIRE(secrets.get("refresh-token", rt));
    CHECK_EQ(rt, std::string("rt-1"));
    CHECK(sq_fs_exists(core.cachePath().c_str()));
  }
  // Restart without network: cache only -> OFFLINE, still entitled.
  srv.down = true;
  nowMs += 3 * 86400 * 1000LL;
  {
    AccountCore core(cfg, &srv.http, secrets, clock);
    core.load();
    CHECK_EQ(core.state().state, SQ_ACCOUNT_OFFLINE);
    CHECK(core.hasEntitlement("sync.presets"));
    CHECK_EQ(core.refresh(), SQ_E_IO);  // offline: nothing lost
    CHECK_EQ(core.state().state, SQ_ACCOUNT_OFFLINE);
    // Past the grace period the cached entitlements lapse (core features stay).
    nowMs += 5 * 86400 * 1000LL;
    CHECK_EQ(core.state().state, SQ_ACCOUNT_EXPIRED);
    CHECK(!core.hasEntitlement("sync.presets"));
    CHECK(core.hasEntitlement("core.audio"));
    // Back online: refresh rotates the token and re-verifies.
    srv.down = false;
    REQUIRE_OK(core.refresh());
    CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_IN);
    std::string rt;
    REQUIRE(secrets.get("refresh-token", rt));
    CHECK_EQ(rt, std::string("rt-2"));
    // Server-side entitlement change is picked up.
    srv.features = R"(["sync.tones"])";
    REQUIRE_OK(core.refresh());
    CHECK(!core.hasEntitlement("sync.presets"));
    CHECK(core.hasEntitlement("sync.tones"));
    // Revoked grant: signed out, cache and token wiped.
    srv.revoked = true;
    CHECK_EQ(core.refresh(), SQ_E_ACCESS);
    CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
    CHECK(!secrets.get("refresh-token", rt));
    CHECK(!sq_fs_exists(core.cachePath().c_str()));
    CHECK_EQ(core.refresh(), SQ_E_ACCESS);
  }
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("cloud: tampered or foreign cache reads as signed out; offline-only never sends") {
  std::string dir = tempDir("tamper");
  Server srv;
  MemorySecretStore secrets;
  int64_t nowMs = 1700000000000;
  CloudConfig cfg;
  cfg.baseUrl = "https://api.test";
  cfg.dataDir = dir;
  auto clock = [&] { return nowMs; };
  {
    AccountCore core(cfg, &srv.http, secrets, clock);
    REQUIRE_OK(core.beginSignIn());
    core.stepSignIn();
    nowMs += 2000;
    core.stepSignIn();
    REQUIRE_EQ(core.state().state, SQ_ACCOUNT_SIGNED_IN);
  }
  std::string path = sq::joinPath(dir, "entitlements.json");
  auto doc = sq::readFile(path);
  REQUIRE(doc.has_value());
  std::string edited = *doc;
  size_t at = edited.find("assistant");
  REQUIRE(at != std::string::npos);
  edited.replace(at, 9, "everythin");
  REQUIRE_OK(sq::writeFile(path, edited));
  {
    AccountCore core(cfg, &srv.http, secrets, clock);
    core.load();
    CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
  }
  REQUIRE_OK(sq::writeFile(path, *doc));
  {
    MemorySecretStore otherDevice;  // same file, different device key
    AccountCore core(cfg, &srv.http, otherDevice, clock);
    core.load();
    CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
    CHECK(!otherDevice.get("device-key", path));  // loading never creates a key
  }
  {
    FakeTransport never;
    CloudConfig off = cfg;
    off.baseUrl.clear();
    AccountCore core(off, &never, secrets, clock);
    core.load();
    CHECK_EQ(core.state().state, SQ_ACCOUNT_OFFLINE);  // the original cache is valid again
    CHECK_EQ(core.beginSignIn(), SQ_E_UNSUPPORTED);
    CHECK_EQ(core.refresh(), SQ_E_UNSUPPORTED);
    core.telemetry().setOptIn(true);
    TelemetryEvent e;
    e.name = "app.start";
    CHECK(core.telemetry().track(e));
    CHECK_EQ(core.flushTelemetry(), -1);
    CHECK(never.log.empty());
    core.signOut();
    CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
  }
  sq_fs_remove_all(dir.c_str());
}

SQ_TEST("cloud: sign-in cancel, denial and entitlement failure") {
  Server srv;
  MemorySecretStore secrets;
  int64_t nowMs = 0;
  CloudConfig cfg;
  cfg.baseUrl = "https://api.test";
  AccountCore core(cfg, &srv.http, secrets, [&] { return nowMs; });
  REQUIRE_OK(core.beginSignIn());
  core.stepSignIn();
  CHECK(core.cancelSignIn());
  CHECK_EQ(core.stepSignIn(), int64_t(-1));
  CHECK_EQ(core.signInInfo().phase, SQ_SIGNIN_CANCELLED);
  CHECK(!core.cancelSignIn());

  srv.http.fn = [prev = srv.http.fn](const HttpRequest& r) {
    if (endsWith(r.url, "/v1/entitlements")) return ok("oops", 500);
    return prev(r);
  };
  REQUIRE_OK(core.beginSignIn());
  core.stepSignIn();
  nowMs += 2000;
  core.stepSignIn();
  CHECK_EQ(core.signInInfo().phase, SQ_SIGNIN_FAILED);
  CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
}

SQ_TEST("cloud: telemetry is opt-in, bounded, persisted and flushed in order") {
  std::string dir = tempDir("telemetry");
  std::string path = sq::joinPath(dir, "telemetry.json");
  FakeTransport http;
  int status = 202;
  http.fn = [&](const HttpRequest&) { return ok("", status); };
  {
    TelemetryQueue q(path, 3);
    TelemetryEvent e;
    e.name = "app.start";
    CHECK(!q.track(e));  // opted out by default: dropped
    CHECK_EQ(q.size(), size_t(0));
    CHECK(!sq_fs_exists(path.c_str()));
    q.setOptIn(true);
    CHECK(!TelemetryQueue::validName("Bad Name"));
    CHECK(!TelemetryQueue::validName(""));
    e.name = "Bad";
    CHECK(!q.track(e));
    for (int i = 0; i < 5; ++i) {
      TelemetryEvent ev;
      ev.name = "edit.op" + std::to_string(i);
      ev.timeMs = i;
      TelemetryProp p;
      p.kind = TelemetryProp::Int;
      p.key = "n";
      p.i = i;
      ev.props.push_back(p);
      CHECK(q.track(ev));
    }
    CHECK_EQ(q.size(), size_t(3));  // oldest dropped
    CHECK(sq_fs_exists(path.c_str()));
  }
  {
    TelemetryQueue q(path, 3);
    q.setOptIn(true);
    q.load();
    auto snap = q.snapshot();
    REQUIRE_EQ(snap.size(), size_t(3));
    CHECK_EQ(snap[0].name, std::string("edit.op2"));
    REQUIRE_EQ(snap[0].props.size(), size_t(1));
    CHECK_EQ(snap[0].props[0].i, int64_t(2));
    status = 500;
    CHECK_EQ(q.flush(http, "https://t.test/v1/telemetry", 2), 500);
    CHECK_EQ(q.size(), size_t(3));  // kept on failure
    status = 202;
    CHECK_EQ(q.flush(http, "https://t.test/v1/telemetry", 2), 202);
    CHECK_EQ(q.size(), size_t(1));
    CHECK(http.log.back().body.find("edit.op2") != std::string::npos);
    CHECK(http.log.back().body.find("edit.op4") == std::string::npos);
    CHECK_EQ(q.flush(http, "https://t.test/v1/telemetry"), 202);
    CHECK_EQ(q.size(), size_t(0));
    CHECK_EQ(q.flush(http, "https://t.test/v1/telemetry"), -1);
    CHECK(!sq_fs_exists(path.c_str()));  // empty queue leaves no file
    TelemetryEvent e;
    e.name = "x";
    CHECK(q.track(e));
    q.setOptIn(false);  // opting out wipes memory and disk
    CHECK_EQ(q.size(), size_t(0));
    CHECK(!sq_fs_exists(path.c_str()));
  }
  sq_fs_remove_all(dir.c_str());
}

// ---- verifier regressions ----------------------------------------------------------------------

SQ_TEST("cloud: base url policy rejects userinfo tricks that point http at a remote host") {
  // WinHttpCrackUrl treats "127.0.0.1:1@" as user:password, so these would send bearer tokens
  // over plain http to evil.test.
  CHECK(!isAllowedBaseUrl("http://127.0.0.1:1@evil.test/"));
  CHECK(!isAllowedBaseUrl("http://localhost@evil.test"));
  CHECK(!isAllowedBaseUrl("http://[::1]@evil.test/"));
  CHECK(!isAllowedBaseUrl("http://[::1]x.evil.test/"));
  CHECK(!isAllowedBaseUrl("http://localhost:80x/"));
  CHECK(!isAllowedBaseUrl("https://user@api.example.test"));
  CHECK(!isAllowedBaseUrl("https://api.example.test\\@evil.test"));
  CHECK(isAllowedBaseUrl("http://localhost:8080"));
  CHECK(isAllowedBaseUrl("http://[::1]/x"));
  CHECK(isAllowedBaseUrl("https://api.example.test:8443/v1?x=a@b"));  // '@' outside the authority
}

SQ_TEST("cloud: device flow survives absurd expires_in / interval values") {
  FakeTransport http;
  http.fn = [&](const HttpRequest& r) {
    if (endsWith(r.url, "/device"))
      return ok(R"({"device_code":"d","user_code":"U","verification_uri":"https://v.test",)"
                R"("expires_in":9.2e18,"interval":9.2e18})");
    return ok(R"({"error":"authorization_pending"})", 400);
  };
  DeviceFlow f(http, {"https://a.test/device", "https://a.test/token", "c", ""});
  int64_t now = 1700000000000;
  int64_t d = f.step(now);
  CHECK(f.state() == DeviceFlow::State::Waiting);
  CHECK(d > 0 && d <= 300000);
  CHECK(f.expiresAtMs() > now);  // no signed overflow into the past
  CHECK(f.expiresAtMs() - now <= int64_t(86400) * 1000);
}

namespace {
// A transport that runs a hook just before answering a matching request (simulates another thread
// acting while the request is on the wire).
struct HookTransport final : HttpTransport {
  HttpTransport& inner;
  std::string onUrl;
  std::function<void()> hook;
  explicit HookTransport(HttpTransport& i) : inner(i) {}
  HttpResponse send(const HttpRequest& r) override {
    HttpResponse res = inner.send(r);
    if (hook && endsWith(r.url, onUrl.c_str()) &&
        (onUrl != "/oauth/token" || r.body.find("grant_type=refresh_token") != std::string::npos)) {
      auto h = std::move(hook);
      hook = nullptr;
      h();
    }
    return res;
  }
};
}  // namespace

SQ_TEST("cloud: sign-out while a refresh is in flight stays signed out") {
  for (const char* when : {"/oauth/token", "/v1/entitlements"}) {
    std::string dir = tempDir("signout-race");
    Server srv;
    HookTransport http(srv.http);
    MemorySecretStore secrets;
    int64_t nowMs = 1700000000000;
    CloudConfig cfg;
    cfg.baseUrl = "https://api.test";
    cfg.dataDir = dir;
    AccountCore core(cfg, &http, secrets, [&] { return nowMs; });
    REQUIRE_OK(core.beginSignIn());
    core.stepSignIn();
    nowMs += 2000;
    core.stepSignIn();
    REQUIRE_EQ(core.state().state, SQ_ACCOUNT_SIGNED_IN);
    http.onUrl = when;
    http.hook = [&] { core.signOut(); };  // the user signs out while the refresh is on the wire
    sq_result r = core.refresh();
    CHECK(SQ_FAILED(r));
    CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
    CHECK(!core.hasEntitlement("sync.presets"));
    std::string rt;
    CHECK(!secrets.get("refresh-token", rt));       // no rotated token written back
    CHECK(!sq_fs_exists(core.cachePath().c_str()));  // no cache re-persisted
    sq_fs_remove_all(dir.c_str());
  }
}

SQ_TEST("cloud: sign-out while sign-in fetches entitlements stays signed out") {
  Server srv;
  HookTransport http(srv.http);
  MemorySecretStore secrets;
  int64_t nowMs = 0;
  CloudConfig cfg;
  cfg.baseUrl = "https://api.test";
  AccountCore core(cfg, &http, secrets, [&] { return nowMs; });
  REQUIRE_OK(core.beginSignIn());
  core.stepSignIn();
  http.onUrl = "/v1/entitlements";
  http.hook = [&] { core.signOut(); };
  nowMs += 2000;
  CHECK_EQ(core.stepSignIn(), int64_t(-1));
  CHECK(core.signInInfo().phase != SQ_SIGNIN_SUCCEEDED);
  CHECK_EQ(core.state().state, SQ_ACCOUNT_SIGNED_OUT);
  std::string rt;
  CHECK(!secrets.get("refresh-token", rt));
}
