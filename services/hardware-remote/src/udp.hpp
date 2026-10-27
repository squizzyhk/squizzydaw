// Minimal blocking-with-timeout IPv4 UDP socket (Winsock / BSD sockets). Not RT-safe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sq::net {

struct Endpoint {
  uint32_t addr = 0;  // IPv4, network byte order
  uint16_t port = 0;  // host byte order
  std::string toString() const;
};

// Resolves "1.2.3.4", "localhost" or a host name (blocking DNS lookup for names).
bool resolveIPv4(const char* host, uint16_t port, Endpoint& out);
// First non-loopback IPv4 address of this machine (network byte order), 0 if none.
uint32_t primaryIPv4();

class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // bindAddr: "0.0.0.0", "127.0.0.1", ... ; port 0 = ephemeral. reuse: SO_REUSEADDR (mDNS).
  bool open(const char* bindAddr, uint16_t port, bool reuse = false);
  bool joinMulticast(const char* group);
  void setMulticastLoopback(bool on);
  void close();
  bool isOpen() const;
  uint16_t localPort() const;

  bool sendTo(const Endpoint& to, const void* data, size_t size);
  // Waits up to timeoutMs. Returns bytes received, 0 on timeout, -1 on error.
  int recvFrom(void* buf, size_t cap, Endpoint& from, int timeoutMs);

 private:
  intptr_t s_;
  bool wsa_;
};

}  // namespace sq::net
