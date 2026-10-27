// IDiscoveryService front end + the built-in mDNS backend (own responder/browser on UDP 5353).
// Backend choice (Services.hardware-remote.DiscoveryBackend):
//   auto    - the OS DNS-SD API when present (Windows 10 1809+: dnsapi DnsService*), else builtin
//   system  - OS API only;  builtin - own mDNS;  off - every call returns SQ_E_UNSUPPORTED
// Nothing touches the network before the first browse/advertise call.
#include <chrono>
#include <cstring>
#include <set>

#include "services.hpp"
#include "sq/platform.h"
#include "sq/services/hardware_remote/dnssd_wire.hpp"

namespace sq::hwremote {

// ---- builtin mDNS backend ----------------------------------------------------------------
namespace {

using Clock = std::chrono::steady_clock;

class BuiltinDiscovery final : public DiscoveryBackend {
 public:
  ~BuiltinDiscovery() override { shutdown(); }
  const char* name() const override { return "builtin-mdns"; }

  sq_result browse(uint32_t cookie, const char* serviceType, Ptr<IDiscoveryListener> listener) override {
    if (!start()) return SQ_E_IO;
    std::lock_guard<std::mutex> lk(m_);
    Browse b;
    b.type = serviceType;
    b.listener = std::move(listener);
    b.next = Clock::now();
    browses_[cookie] = std::move(b);
    return SQ_OK;
  }

  sq_result advertise(uint32_t cookie, const sq_discovery_record& record) override {
    if (!start()) return SQ_E_IO;
    std::lock_guard<std::mutex> lk(m_);
    Advert a;
    a.rec = record;
    a.announcesLeft = 2;  // RFC 6762 8.3: at least two announcements, one second apart
    a.next = Clock::now();
    adverts_[cookie] = a;
    return SQ_OK;
  }

  sq_result cancel(uint32_t cookie) override {
    std::lock_guard<std::mutex> lk(m_);
    if (browses_.erase(cookie)) return SQ_OK;
    auto it = adverts_.find(cookie);
    if (it == adverts_.end()) return SQ_E_NOTFOUND;
    sendAnnouncement(it->second.rec, 0);  // goodbye
    adverts_.erase(it);
    return SQ_OK;
  }

  void shutdown() override {
    {
      std::lock_guard<std::mutex> lk(m_);
      for (auto& [c, a] : adverts_) sendAnnouncement(a.rec, 0);
      adverts_.clear();
      browses_.clear();
    }
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    sock_.close();
  }

 private:
  struct Browse {
    std::string type;
    Ptr<IDiscoveryListener> listener;
    std::set<std::string> known;
    Clock::time_point next;
    int intervalMs = 1000;
  };
  struct Advert {
    sq_discovery_record rec{};
    int announcesLeft = 0;
    Clock::time_point next;
  };

  bool start() {
    std::lock_guard<std::mutex> lk(m_);
    if (thread_.joinable()) return true;
    if (!sock_.open("0.0.0.0", dnssd::kMdnsPort, true)) return false;
    if (!sock_.joinMulticast(dnssd::kMdnsGroupV4)) {
      sock_.close();
      return false;
    }
    sock_.setMulticastLoopback(true);
    net::resolveIPv4(dnssd::kMdnsGroupV4, dnssd::kMdnsPort, group_);
    uint32_t ip = net::primaryIPv4();
    std::memcpy(ipv4_, &ip, 4);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  // m_ held.
  void sendAnnouncement(const sq_discovery_record& rec, uint32_t ttl) {
    std::vector<uint8_t> pkt;
    bool haveIp = ipv4_[0] || ipv4_[1] || ipv4_[2] || ipv4_[3];
    if (dnssd::encode(dnssd::buildAnnouncement(rec, haveIp ? ipv4_ : nullptr, ttl), pkt))
      sock_.sendTo(group_, pkt.data(), pkt.size());
  }

  void run() {
    sq_thread_set_current_name("sq-mdns");
    std::vector<uint8_t> buf(9000);
    while (!stop_.load()) {
      timers();
      net::Endpoint from;
      int n = sock_.recvFrom(buf.data(), buf.size(), from, 200);
      if (n <= 0) continue;
      dnssd::Message msg;
      if (!dnssd::decode(buf.data(), static_cast<size_t>(n), msg)) continue;
      handle(msg);
    }
  }

  void timers() {
    std::lock_guard<std::mutex> lk(m_);
    auto now = Clock::now();
    for (auto& [c, b] : browses_) {
      if (now < b.next) continue;
      std::vector<uint8_t> pkt;
      if (dnssd::encode(dnssd::buildBrowseQuery(b.type.c_str()), pkt)) sock_.sendTo(group_, pkt.data(), pkt.size());
      b.next = now + std::chrono::milliseconds(b.intervalMs);
      b.intervalMs = b.intervalMs * 2 > 60000 ? 60000 : b.intervalMs * 2;  // RFC 6762 5.2 back-off
    }
    for (auto& [c, a] : adverts_) {
      if (a.announcesLeft <= 0 || now < a.next) continue;
      sendAnnouncement(a.rec, 120);
      --a.announcesLeft;
      a.next = now + std::chrono::seconds(1);
    }
  }

  void handle(const dnssd::Message& msg) {
    struct Event {
      Ptr<IDiscoveryListener> l;
      sq_discovery_record rec;
      bool found;
    };
    std::vector<Event> events;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (!(msg.flags & 0x8000)) {
        for (auto& [c, a] : adverts_)
          if (dnssd::queryMatches(msg, a.rec)) sendAnnouncement(a.rec, 120);
        return;
      }
      for (auto& [c, b] : browses_) {
        std::vector<sq_discovery_record> found, lost;
        dnssd::extractInstances(msg, b.type.c_str(), found, lost);
        for (auto& r : found)
          if (b.known.insert(r.name).second) events.push_back({b.listener, r, true});
        for (auto& r : lost)
          if (b.known.erase(r.name)) events.push_back({b.listener, r, false});
      }
    }
    for (auto& e : events) {  // outside the lock: listeners may call back into the service
      if (e.found)
        e.l->onServiceFound(&e.rec);
      else
        e.l->onServiceLost(&e.rec);
    }
  }

  std::mutex m_;
  std::map<uint32_t, Browse> browses_;
  std::map<uint32_t, Advert> adverts_;
  net::UdpSocket sock_;
  net::Endpoint group_;
  uint8_t ipv4_[4] = {};
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace

std::unique_ptr<DiscoveryBackend> makeBuiltinDiscovery() { return std::make_unique<BuiltinDiscovery>(); }

// ---- service front end -------------------------------------------------------------------
DiscoveryService::DiscoveryService() : mode_(settingString(kSetDiscovery, "auto")) {}
DiscoveryService::~DiscoveryService() { stopAll(); }

DiscoveryBackend* DiscoveryService::backend() {
  if (backendTried_) return backend_.get();
  backendTried_ = true;
  try {
    if (mode_ == "off") return nullptr;
    if (mode_ == "auto" || mode_ == "system") backend_ = makeSystemDiscovery();
    if (!backend_ && (mode_ == "auto" || mode_ == "builtin")) backend_ = makeBuiltinDiscovery();
    if (backend_) sq::logf(SQ_LOG_INFO, "discovery backend: %s", backend_->name());
  } catch (...) {
    backend_.reset();
  }
  return backend_.get();
}

sq_result SQ_CALL DiscoveryService::getBackendName(char* buf, uint32_t bufSize, uint32_t* needed) {
  const char* name = "none";
  try {
    std::lock_guard<std::mutex> lk(m_);
    if (DiscoveryBackend* be = backend()) name = be->name();
  } catch (...) {
    return SQ_E_FAIL;
  }
  uint32_t n = static_cast<uint32_t>(std::strlen(name)) + 1;
  if (needed) *needed = n;
  if (!buf || bufSize < n) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, name, n);
  return SQ_OK;
}

sq_result SQ_CALL DiscoveryService::browse(const char* serviceType, IDiscoveryListener* listener, uint32_t* cookie) {
  if (!serviceType || !*serviceType || !listener || !cookie) return SQ_E_INVALIDARG;
  *cookie = 0;
  try {
    std::lock_guard<std::mutex> lk(m_);
    DiscoveryBackend* b = backend();
    if (!b) return SQ_E_UNSUPPORTED;
    uint32_t c = nextCookie_++;
    sq_result r = b->browse(c, serviceType, Ptr<IDiscoveryListener>(listener));
    if (SQ_SUCCEEDED(r)) *cookie = c;
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL DiscoveryService::advertise(const sq_discovery_record* record, uint32_t* cookie) {
  if (!record || record->struct_size < sizeof(sq_discovery_record) || !cookie || !record->name[0] || !record->type[0] ||
      record->port == 0)
    return SQ_E_INVALIDARG;
  *cookie = 0;
  try {
    std::lock_guard<std::mutex> lk(m_);
    DiscoveryBackend* b = backend();
    if (!b) return SQ_E_UNSUPPORTED;
    uint32_t c = nextCookie_++;
    sq_result r = b->advertise(c, *record);
    if (SQ_SUCCEEDED(r)) *cookie = c;
    return r;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL DiscoveryService::cancel(uint32_t cookie) {
  try {
    std::lock_guard<std::mutex> lk(m_);
    if (!backend_) return SQ_E_NOTFOUND;
    return backend_->cancel(cookie);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

void DiscoveryService::stopAll() {
  std::unique_ptr<DiscoveryBackend> b;
  {
    std::lock_guard<std::mutex> lk(m_);
    b = std::move(backend_);
    backendTried_ = false;
  }
  if (b) b->shutdown();
}

}  // namespace sq::hwremote
