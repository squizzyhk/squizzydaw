// Test helper: a tiny HTTP/1.1 server on 127.0.0.1 (one connection at a time, Connection: close).
// Loopback only: no test ever talks to a real network. Windows only (winsock).
#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct LoopbackRequest {
  std::string method, path, headers, body;
};
struct LoopbackResponse {
  int status = 200;
  std::string body;
  std::string contentType = "application/json";
  std::string location;  // sent as a Location header when not empty
};

class LoopbackHttpServer {
 public:
  using Handler = std::function<LoopbackResponse(const LoopbackRequest&)>;

  explicit LoopbackHttpServer(Handler h) : handler_(std::move(h)) {
    WSADATA d;
    wsa_ = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (s_ == INVALID_SOCKET || bind(s_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || listen(s_, 8) != 0) return;
    int len = sizeof sa;
    getsockname(s_, reinterpret_cast<sockaddr*>(&sa), &len);
    port_ = ntohs(sa.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~LoopbackHttpServer() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (s_ != INVALID_SOCKET) closesocket(s_);
    if (wsa_) WSACleanup();
  }
  uint16_t port() const { return port_; }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::vector<LoopbackRequest> requests() {
    std::lock_guard<std::mutex> lk(m_);
    return log_;
  }

 private:
  void run() {
    while (!stop_.load()) {
      fd_set rd;
      FD_ZERO(&rd);
      FD_SET(s_, &rd);
      timeval tv{0, 50000};
      if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
      SOCKET c = accept(s_, nullptr, nullptr);
      if (c == INVALID_SOCKET) continue;
      serve(c);
      closesocket(c);
    }
  }
  void serve(SOCKET c) {
    std::string data;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
      int n = recv(c, buf, sizeof buf, 0);
      if (n <= 0) return;
      data.append(buf, static_cast<size_t>(n));
      headerEnd = data.find("\r\n\r\n");
    }
    LoopbackRequest req;
    std::string head = data.substr(0, headerEnd);
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    req.method = head.substr(0, sp1);
    req.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
    req.headers = head;
    size_t contentLength = 0;
    std::string lower = head;
    for (auto& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    size_t cl = lower.find("content-length:");
    if (cl != std::string::npos) contentLength = static_cast<size_t>(std::strtoul(head.c_str() + cl + 15, nullptr, 10));
    req.body = data.substr(headerEnd + 4);
    while (req.body.size() < contentLength) {
      int n = recv(c, buf, sizeof buf, 0);
      if (n <= 0) break;
      req.body.append(buf, static_cast<size_t>(n));
    }
    LoopbackResponse res = handler_(req);
    {
      std::lock_guard<std::mutex> lk(m_);
      log_.push_back(req);
    }
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " X\r\nContent-Type: " + res.contentType +
                      "\r\nContent-Length: " + std::to_string(res.body.size()) +
                      (res.location.empty() ? std::string() : "\r\nLocation: " + res.location) +
                      "\r\nConnection: close\r\n\r\n" + res.body;
    send(c, out.data(), static_cast<int>(out.size()), 0);
    shutdown(c, SD_SEND);
  }

  Handler handler_;
  bool wsa_ = false;
  SOCKET s_ = INVALID_SOCKET;
  uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex m_;
  std::vector<LoopbackRequest> log_;
};
#endif
