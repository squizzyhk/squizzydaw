// IOscService: OSC 1.0 over UDP (IPv4). One receive thread per listening socket; callbacks run
// on that thread (remote.h: post to the main thread yourself).
#include <vector>

#include "services.hpp"
#include "sq/platform.h"
#include "sq/services/hardware_remote/osc.hpp"

namespace sq::hwremote {

std::string settingString(const char* key, const char* def) {
  IHostContext* h = host();
  if (!h) return def;
  return sq::getString(h->getSettings(), key, def);
}

OscService::OscService() : bindAddr_(settingString(kSetOscBind, "0.0.0.0")) {}

OscService::~OscService() { stopAll(); }

void OscService::run(Listen* l) {
  sq_thread_set_current_name("sq-osc-recv");
  std::vector<uint8_t> buf(65536);
  while (!l->stop.load(std::memory_order_acquire)) {
    net::Endpoint from;
    int n = l->sock.recvFrom(buf.data(), buf.size(), from, 100);
    if (n <= 0) {
      if (n < 0) sq_thread_sleep_ms(20);  // socket error: avoid spinning until stop
      continue;
    }
    std::string fromHost = from.toString();
    osc::decodePacket(buf.data(), static_cast<size_t>(n),
                      [&](const char* addr, const sq_osc_arg* args, uint32_t count, uint64_t timeTag) {
                        if (l->timed)
                          l->timed->onOscTimedMessage(addr, args, count, timeTag, fromHost.c_str(), from.port);
                        else
                          l->listener->onOscMessage(addr, args, count, fromHost.c_str(), from.port);
                      });
  }
}

sq_result SQ_CALL OscService::listen(uint16_t port, IOscListener* listener, uint16_t* boundPort, uint32_t* cookie) {
  if (!listener || !cookie) return SQ_E_INVALIDARG;
  *cookie = 0;
  try {
    auto l = std::make_unique<Listen>();
    if (!l->sock.open(bindAddr_.c_str(), port, false)) return SQ_E_IO;
    l->listener = Ptr<IOscListener>(listener);
    l->timed = l->listener.as<IOscTimedListener>();
    if (boundPort) *boundPort = l->sock.localPort();
    Listen* raw = l.get();
    std::lock_guard<std::mutex> lk(m_);
    uint32_t c = nextCookie_++;
    l->thread = std::thread(&OscService::run, raw);
    listens_[c] = std::move(l);
    *cookie = c;
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL OscService::stopListening(uint32_t cookie) {
  std::unique_ptr<Listen> l;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = listens_.find(cookie);
    if (it == listens_.end()) return SQ_E_NOTFOUND;
    l = std::move(it->second);
    listens_.erase(it);
  }
  l->stop.store(true, std::memory_order_release);
  if (l->thread.joinable()) {
    if (l->thread.get_id() == std::this_thread::get_id()) {
      // Called from inside its own callback: the thread exits after the callback returns.
      // Park it; stopAll() joins it from another thread.
      std::lock_guard<std::mutex> lk(m_);
      zombies_.push_back(std::move(l));
      return SQ_OK;
    }
    l->thread.join();
  }
  return SQ_OK;
}

sq_result OscService::sendPacket(const net::Endpoint& to, const std::vector<uint8_t>& pkt) {
  std::lock_guard<std::mutex> lk(sendM_);
  if (!sendSock_.isOpen() && !sendSock_.open(nullptr, 0, false)) return SQ_E_IO;
  return sendSock_.sendTo(to, pkt.data(), pkt.size()) ? SQ_OK : SQ_E_IO;
}

sq_result SQ_CALL OscService::send(const char* hostName, uint16_t port, const char* address, const sq_osc_arg* args,
                                   uint32_t count) {
  if (!hostName || !address || port == 0) return SQ_E_INVALIDARG;
  try {
    net::Endpoint to;
    if (!net::resolveIPv4(hostName, port, to)) return SQ_E_NOTFOUND;
    std::vector<uint8_t> pkt;
    if (!osc::encodeMessage(address, args, count, pkt)) return SQ_E_INVALIDARG;
    return sendPacket(to, pkt);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL OscService::sendBundle(const char* hostName, uint16_t port, uint64_t timeTag,
                                         const sq_osc_message* messages, uint32_t count) {
  if (!hostName || port == 0 || (count && !messages)) return SQ_E_INVALIDARG;
  try {
    std::vector<std::vector<uint8_t>> elements(count);
    for (uint32_t k = 0; k < count; ++k) {
      const sq_osc_message& m = messages[k];
      if (m.struct_size < sizeof(sq_osc_message)) return SQ_E_INVALIDARG;
      if (!osc::encodeMessage(m.address, m.args, m.count, elements[k])) return SQ_E_INVALIDARG;
    }
    std::vector<uint8_t> pkt;
    osc::encodeBundle(timeTag, elements, pkt);
    if (pkt.size() > 65507) return SQ_E_INVALIDARG;  // larger than one UDP datagram
    net::Endpoint to;
    if (!net::resolveIPv4(hostName, port, to)) return SQ_E_NOTFOUND;
    return sendPacket(to, pkt);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

void OscService::stopAll() {
  std::vector<uint32_t> cookies;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& [c, l] : listens_) cookies.push_back(c);
  }
  for (uint32_t c : cookies) stopListening(c);
  std::vector<std::unique_ptr<Listen>> zombies;
  {
    std::lock_guard<std::mutex> lk(m_);
    zombies.swap(zombies_);
  }
  for (auto& z : zombies) {
    if (!z->thread.joinable()) continue;
    if (z->thread.get_id() == std::this_thread::get_id()) {
      z->thread.detach();
      (void)z.release();  // still running this very callback: leak rather than free under it
    } else {
      z->thread.join();
    }
  }
  std::lock_guard<std::mutex> lk(sendM_);
  sendSock_.close();
}

}  // namespace sq::hwremote
