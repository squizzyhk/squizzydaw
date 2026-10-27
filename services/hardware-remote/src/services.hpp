// hardware-remote: internal service classes (one DLL, two registered services).
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/services/hardware_remote/hardware_remote.h"
#include "udp.hpp"

namespace sq::hwremote {

// Settings (Services.hardware-remote.*), read when a service object is created.
inline constexpr const char* kSetOscBind = "Services.hardware-remote.OscBindAddress";      // "0.0.0.0"
inline constexpr const char* kSetDiscovery = "Services.hardware-remote.DiscoveryBackend";  // auto|system|builtin|off

std::string settingString(const char* key, const char* def);

// ---- OSC ---------------------------------------------------------------------------------
class OscService final : public Object<OscService, IOscService, IOscBundleSender> {
 public:
  OscService();
  ~OscService() override;

  sq_result SQ_CALL listen(uint16_t port, IOscListener* listener, uint16_t* boundPort, uint32_t* cookie) override;
  sq_result SQ_CALL stopListening(uint32_t cookie) override;
  sq_result SQ_CALL send(const char* host, uint16_t port, const char* address, const sq_osc_arg* args,
                         uint32_t count) override;
  sq_result SQ_CALL sendBundle(const char* host, uint16_t port, uint64_t timeTag, const sq_osc_message* messages,
                               uint32_t count) override;

  sq_result sendPacket(const net::Endpoint& to, const std::vector<uint8_t>& pkt);
  void stopAll();

 private:
  struct Listen {
    net::UdpSocket sock;
    std::thread thread;
    std::atomic<bool> stop{false};
    Ptr<IOscListener> listener;
    Ptr<IOscTimedListener> timed;  // set when the listener answers IOscTimedListener
  };
  static void run(Listen* l);

  std::string bindAddr_;
  std::mutex m_;
  std::map<uint32_t, std::unique_ptr<Listen>> listens_;
  std::vector<std::unique_ptr<Listen>> zombies_;
  uint32_t nextCookie_ = 1;
  std::mutex sendM_;
  net::UdpSocket sendSock_;
};

// ---- Discovery ---------------------------------------------------------------------------
class DiscoveryBackend {
 public:
  virtual ~DiscoveryBackend() = default;
  virtual const char* name() const = 0;
  virtual sq_result browse(uint32_t cookie, const char* serviceType, Ptr<IDiscoveryListener> listener) = 0;
  virtual sq_result advertise(uint32_t cookie, const sq_discovery_record& record) = 0;
  virtual sq_result cancel(uint32_t cookie) = 0;  // SQ_E_NOTFOUND if not owned
  virtual void shutdown() = 0;
};

std::unique_ptr<DiscoveryBackend> makeSystemDiscovery();   // nullptr when the OS API is missing
std::unique_ptr<DiscoveryBackend> makeBuiltinDiscovery();  // own mDNS responder/browser over UDP 5353

class DiscoveryService final : public Object<DiscoveryService, IDiscoveryService, IDiscoveryBackendInfo> {
 public:
  DiscoveryService();
  ~DiscoveryService() override;
  sq_result SQ_CALL browse(const char* serviceType, IDiscoveryListener* listener, uint32_t* cookie) override;
  sq_result SQ_CALL advertise(const sq_discovery_record* record, uint32_t* cookie) override;
  sq_result SQ_CALL cancel(uint32_t cookie) override;
  sq_result SQ_CALL getBackendName(char* buf, uint32_t bufSize, uint32_t* needed) override;
  void stopAll();

 private:
  DiscoveryBackend* backend();  // created lazily: no socket or OS registration before first use
  std::mutex m_;
  std::string mode_;
  std::unique_ptr<DiscoveryBackend> backend_;
  bool backendTried_ = false;
  uint32_t nextCookie_ = 1;
};

}  // namespace sq::hwremote
