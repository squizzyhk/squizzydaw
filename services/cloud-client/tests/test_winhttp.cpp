// cloud-client: the WinHTTP transport against a loopback (127.0.0.1) server inside this test.
#include <string>

#include "http.hpp"
#include "loopback_http.hpp"
#include "sqtest.hpp"

using namespace sq::cloud;

SQ_TEST("cloud winhttp: GET and POST round-trip through a loopback server") {
#if !defined(_WIN32)
  SQ_SKIP("WinHTTP transport is Windows-only");
#else
  LoopbackHttpServer srv([](const LoopbackRequest& r) {
    LoopbackResponse res;
    if (r.path == "/missing") {
      res.status = 404;
      res.body = "{\"error\":\"nope\"}";
    } else {
      res.body = "{\"echo\":\"" + r.method + " " + r.path + "\",\"len\":" + std::to_string(r.body.size()) + "}";
    }
    return res;
  });
  REQUIRE(srv.port() != 0);
  auto http = makeWinHttpTransport(false);
  REQUIRE(http);

  HttpRequest get;
  get.url = srv.base() + "/v1/entitlements?x=1";
  get.headers = {{"Authorization", "Bearer abc"}};
  HttpResponse r = http->send(get);
  CHECK_EQ(r.status, 200);
  CHECK_EQ(r.body, std::string("{\"echo\":\"GET /v1/entitlements?x=1\",\"len\":0}"));

  HttpRequest post;
  post.method = "POST";
  post.url = srv.base() + "/oauth/token";
  post.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
  post.body = formEncode({{"grant_type", "refresh_token"}, {"refresh_token", "r t"}});
  r = http->send(post);
  CHECK_EQ(r.status, 200);
  CHECK(r.body.find("POST /oauth/token") != std::string::npos);

  HttpRequest miss;
  miss.url = srv.base() + "/missing";
  r = http->send(miss);
  CHECK_EQ(r.status, 404);
  CHECK_EQ(r.body, std::string("{\"error\":\"nope\"}"));

  auto log = srv.requests();
  REQUIRE_EQ(log.size(), size_t(3));
  CHECK(log[0].headers.find("Authorization: Bearer abc") != std::string::npos);
  CHECK_EQ(log[1].body, std::string("grant_type=refresh_token&refresh_token=r%20t"));

  // Nothing listening: a transport failure (status 0), not an exception or a hang.
  uint16_t deadPort = srv.port();
  {
    LoopbackHttpServer tmp([](const LoopbackRequest&) { return LoopbackResponse{}; });
    deadPort = tmp.port();
  }
  HttpRequest dead;
  dead.url = "http://127.0.0.1:" + std::to_string(deadPort) + "/x";
  dead.timeoutMs = 3000;
  r = http->send(dead);
  CHECK_EQ(r.status, 0);
  CHECK(!r.error.empty());

  HttpRequest bad;
  bad.url = "not a url";
  r = http->send(bad);
  CHECK_EQ(r.status, 0);
#endif
}

// Verifier regression: WinHTTP replays added request headers on redirects, so following a 3xx would
// hand the bearer token to whichever host the Location names. Redirects are never followed.
SQ_TEST("cloud winhttp: redirects are not followed (no token replay to another host)") {
#if !defined(_WIN32)
  SQ_SKIP("WinHTTP transport is Windows-only");
#else
  LoopbackHttpServer other([](const LoopbackRequest&) { return LoopbackResponse{}; });
  REQUIRE(other.port() != 0);
  const std::string target = other.base() + "/steal";
  LoopbackHttpServer srv([target](const LoopbackRequest&) {
    LoopbackResponse res;
    res.status = 302;
    res.location = target;
    return res;
  });
  REQUIRE(srv.port() != 0);
  auto http = makeWinHttpTransport(false);
  REQUIRE(http);
  HttpRequest get;
  get.url = srv.base() + "/v1/entitlements";
  get.headers = {{"Authorization", "Bearer secret-token"}};
  get.timeoutMs = 5000;
  HttpResponse r = http->send(get);
  CHECK_EQ(r.status, 302);
  CHECK_EQ(srv.requests().size(), size_t(1));
  CHECK(other.requests().empty());
#endif
}
