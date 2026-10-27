// cloud-client: URL/form encoding helpers + the WinHTTP transport.
#include "http.hpp"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace sq::cloud {

std::string urlEncode(const std::string& s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                c == '_' || c == '~';
    if (keep) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
  return out;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (const auto& [k, v] : fields) {
    if (!out.empty()) out.push_back('&');
    out += urlEncode(k);
    out.push_back('=');
    out += urlEncode(v);
  }
  return out;
}

bool isAllowedBaseUrl(const std::string& url) {
  auto starts = [&](const char* p) { return url.rfind(p, 0) == 0; };
  const bool https = starts("https://");
  if (!https && !starts("http://")) return false;
  const std::string rest = url.substr(https ? 8 : 7);
  const std::string authority = rest.substr(0, rest.find_first_of("/?#"));
  if (authority.empty()) return false;
  // No userinfo ("user@host") and no backslashes: URL parsers disagree about them, and WinHTTP
  // reads "http://127.0.0.1:1@evil.test" as a request to evil.test.
  for (char c : authority)
    if (c == '@' || c == '\\' || static_cast<unsigned char>(c) <= 0x20) return false;
  if (https) return true;
  std::string host = authority, port;
  if (host[0] == '[') {
    size_t close = host.find(']');
    if (close == std::string::npos) return false;
    port = host.substr(close + 1);
    host = host.substr(0, close + 1);
  } else if (size_t colon = host.find(':'); colon != std::string::npos) {
    port = host.substr(colon);
    host = host.substr(0, colon);
  }
  if (!port.empty()) {
    if (port[0] != ':' || port.size() < 2 || port.size() > 6) return false;
    for (size_t i = 1; i < port.size(); ++i)
      if (port[i] < '0' || port[i] > '9') return false;
  }
  return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

#if defined(_WIN32)
namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

struct Handle {
  HINTERNET h = nullptr;
  Handle() = default;
  explicit Handle(HINTERNET x) : h(x) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
  explicit operator bool() const { return h != nullptr; }
};

std::string lastError(const char* what) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "%s failed (%lu)", what, static_cast<unsigned long>(GetLastError()));
  return buf;
}

class WinHttpTransport final : public HttpTransport {
 public:
  explicit WinHttpTransport(bool useSystemProxy) {
    DWORD access = useSystemProxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
    session_.h = WinHttpOpen(L"SquizzyStudio-CloudClient/0.1", access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_ && useSystemProxy)  // older systems: no automatic proxy support
      session_.h = WinHttpOpen(L"SquizzyStudio-CloudClient/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
      DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
      protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
      if (!WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
      }
    }
  }

  HttpResponse send(const HttpRequest& req) override {
    HttpResponse res;
    if (!session_) {
      res.error = "WinHttpOpen failed";
      return res;
    }
    std::wstring url = widen(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
      res.error = lastError("WinHttpCrackUrl");
      return res;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    WinHttpSetTimeouts(session_.h, req.timeoutMs, req.timeoutMs, req.timeoutMs, req.timeoutMs);
    Handle conn(WinHttpConnect(session_.h, host, uc.nPort, 0));
    if (!conn) {
      res.error = lastError("WinHttpConnect");
      return res;
    }
    std::wstring object = std::wstring(path) + extra;
    if (object.empty()) object = L"/";
    std::wstring method = widen(req.method);
    Handle request(WinHttpOpenRequest(conn.h, method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
      res.error = lastError("WinHttpOpenRequest");
      return res;
    }
    // Never follow redirects: WinHTTP would replay our added headers (the bearer token) to
    // whatever host the Location names. The API has no legitimate redirects; a 3xx is returned
    // to the caller as a plain status.
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
    std::wstring headers;
    for (const auto& [k, v] : req.headers) headers += widen(k) + L": " + widen(v) + L"\r\n";
    BOOL ok = WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                 headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                 req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
                                 static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0);
    if (!ok || !WinHttpReceiveResponse(request.h, nullptr)) {
      res.error = lastError("WinHttpSendRequest");
      return res;
    }
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX)) {
      res.error = lastError("WinHttpQueryHeaders");
      return res;
    }
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(request.h, &avail)) {
        res.error = lastError("WinHttpQueryDataAvailable");
        return res;
      }
      if (avail == 0) break;
      if (res.body.size() + avail > kMaxResponseBytes) {
        res.error = "response too large";
        res.body.clear();
        return res;
      }
      size_t at = res.body.size();
      res.body.resize(at + avail);
      DWORD got = 0;
      if (!WinHttpReadData(request.h, res.body.data() + at, avail, &got)) {
        res.error = lastError("WinHttpReadData");
        res.body.clear();
        return res;
      }
      res.body.resize(at + got);
    }
    res.status = static_cast<int>(status);
    return res;
  }

 private:
  Handle session_;
};

}  // namespace

std::unique_ptr<HttpTransport> makeWinHttpTransport(bool useSystemProxy) {
  return std::make_unique<WinHttpTransport>(useSystemProxy);
}

#else

std::unique_ptr<HttpTransport> makeWinHttpTransport(bool) { return nullptr; }

#endif

}  // namespace sq::cloud
