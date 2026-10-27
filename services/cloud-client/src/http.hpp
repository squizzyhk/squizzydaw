// cloud-client: injectable HTTP transport. The service uses WinHTTP (Windows); tests inject a
// fake, so no test ever reaches a real network. Not RT-safe.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sq::cloud {

struct HttpRequest {
  std::string method = "GET";
  std::string url;  // http(s)://host[:port]/path?query
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int timeoutMs = 15000;
};

struct HttpResponse {
  int status = 0;     // 0 = transport failure (no HTTP response): see error
  std::string body;
  std::string error;
};

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse send(const HttpRequest& req) = 0;
};

inline constexpr size_t kMaxResponseBytes = 1u << 20;

// WinHTTP implementation; nullptr on platforms without it. useSystemProxy=false talks directly
// (loopback tests).
std::unique_ptr<HttpTransport> makeWinHttpTransport(bool useSystemProxy = true);

std::string urlEncode(const std::string& s);  // RFC 3986 unreserved kept, everything else %XX
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields);
// https://... always; http:// only for loopback hosts (127.0.0.1, localhost, [::1]).
bool isAllowedBaseUrl(const std::string& url);

}  // namespace sq::cloud
