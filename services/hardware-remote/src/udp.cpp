#include "udp.hpp"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
using sock_t = SOCKET;
static const intptr_t kInvalid = static_cast<intptr_t>(INVALID_SOCKET);
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const intptr_t kInvalid = -1;
#define closesocket ::close
#endif

namespace sq::net {

namespace {
sock_t S(intptr_t v) { return static_cast<sock_t>(v); }

bool wsaStart() {
#ifdef _WIN32
  WSADATA d;
  return WSAStartup(MAKEWORD(2, 2), &d) == 0;
#else
  return true;
#endif
}
void wsaStop() {
#ifdef _WIN32
  WSACleanup();
#endif
}
}  // namespace

std::string Endpoint::toString() const {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
  char s[32];
  std::snprintf(s, sizeof s, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return s;
}

bool resolveIPv4(const char* host, uint16_t port, Endpoint& out) {
  if (!host || !*host) return false;
  bool wsa = wsaStart();
  bool ok = false;
  in_addr a{};
  if (inet_pton(AF_INET, host, &a) == 1) {
    std::memcpy(&out.addr, &a, 4);
    ok = true;
  } else {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res) {
      const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
      std::memcpy(&out.addr, &sin->sin_addr, 4);
      ok = true;
      freeaddrinfo(res);
    }
  }
  out.port = port;
  if (wsa) wsaStop();
  return ok;
}

uint32_t primaryIPv4() {
  uint32_t found = 0;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf(size);
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
  if (r == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    r = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
  }
  if (r != NO_ERROR) return 0;
  for (auto* a = aa; a && !found; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      std::memcpy(&found, &sin->sin_addr, 4);
      break;
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return 0;
  for (ifaddrs* i = list; i && !found; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(i->ifa_addr);
    uint32_t a;
    std::memcpy(&a, &sin->sin_addr, 4);
    if ((ntohl(a) >> 24) == 127) continue;
    found = a;
  }
  freeifaddrs(list);
#endif
  return found;
}

UdpSocket::UdpSocket() : s_(kInvalid), wsa_(wsaStart()) {}
UdpSocket::~UdpSocket() {
  close();
  if (wsa_) wsaStop();
}

bool UdpSocket::open(const char* bindAddr, uint16_t port, bool reuse) {
  close();
  sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (static_cast<intptr_t>(s) == kInvalid) return false;
  if (reuse) {
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof one);
#endif
  }
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (!bindAddr || !*bindAddr || inet_pton(AF_INET, bindAddr, &sa.sin_addr) != 1) sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0) {
    closesocket(s);
    return false;
  }
  s_ = static_cast<intptr_t>(s);
  return true;
}

bool UdpSocket::joinMulticast(const char* group) {
  if (!isOpen()) return false;
  ip_mreq m{};
  if (inet_pton(AF_INET, group, &m.imr_multiaddr) != 1) return false;
  m.imr_interface.s_addr = htonl(INADDR_ANY);
  return setsockopt(S(s_), IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m) == 0;
}

void UdpSocket::setMulticastLoopback(bool on) {
  if (!isOpen()) return;
#ifdef _WIN32
  DWORD v = on ? 1 : 0;
#else
  unsigned char v = on ? 1 : 0;
#endif
  setsockopt(S(s_), IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&v), sizeof v);
}

void UdpSocket::close() {
  if (s_ != kInvalid) {
    closesocket(S(s_));
    s_ = kInvalid;
  }
}

bool UdpSocket::isOpen() const { return s_ != kInvalid; }

uint16_t UdpSocket::localPort() const {
  if (!isOpen()) return 0;
  sockaddr_in sa{};
#ifdef _WIN32
  int len = sizeof sa;
#else
  socklen_t len = sizeof sa;
#endif
  if (getsockname(S(s_), reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
  return ntohs(sa.sin_port);
}

bool UdpSocket::sendTo(const Endpoint& to, const void* data, size_t size) {
  if (!isOpen()) return false;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(to.port);
  std::memcpy(&sa.sin_addr, &to.addr, 4);
  auto n = sendto(S(s_), static_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&sa),
                  sizeof sa);
  return n == static_cast<decltype(n)>(size);
}

int UdpSocket::recvFrom(void* buf, size_t cap, Endpoint& from, int timeoutMs) {
  if (!isOpen()) return -1;
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(S(s_), &rd);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int r = select(static_cast<int>(S(s_)) + 1, &rd, nullptr, nullptr, &tv);
  if (r == 0) return 0;
  if (r < 0) return -1;
  sockaddr_in sa{};
#ifdef _WIN32
  int len = sizeof sa;
#else
  socklen_t len = sizeof sa;
#endif
  auto n = recvfrom(S(s_), static_cast<char*>(buf), static_cast<int>(cap), 0, reinterpret_cast<sockaddr*>(&sa), &len);
  if (n < 0) {
#ifdef _WIN32
    // ICMP port-unreachable from an earlier send surfaces here on Windows: not fatal.
    if (WSAGetLastError() == WSAECONNRESET) return 0;
#endif
    return -1;
  }
  std::memcpy(&from.addr, &sa.sin_addr, 4);
  from.port = ntohs(sa.sin_port);
  return static_cast<int>(n);
}

}  // namespace sq::net
