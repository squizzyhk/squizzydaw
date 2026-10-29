// System DNS-SD backend: Windows dnsapi DnsServiceBrowse / DnsServiceResolve / DnsServiceRegister
// (Windows 10 1809+). The functions are resolved at runtime, so the module still loads where they
// are missing (makeSystemDiscovery() then returns nullptr and "auto" falls back to builtin-mdns).
//
// The OS delivers results on its own thread-pool threads. Record handling is the portable code in
// dnssd_wire.cpp: OS records are converted into a dnssd::Message and run through
// extractInstances / extractPointers; instances whose SRV was not in the browse answer are
// resolved with DnsServiceResolve and turned into records with dnssd::makeRecord.
//
// Lifetime: every OS request has a context object owned by the backend. A context is freed only
// after the OS reported completion/cancellation for it (or, at shutdown, after a bounded wait;
// a context whose completion never arrived is leaked on purpose rather than freed under a
// callback that might still run). An advertisement gets two completions, the registration's and
// the deregistration's, in that order even when it is cancelled before its registration
// completed. The callbacks are code of this module, and one that has just signalled its event is
// still returning through it; no API reports when it has left. So the first OS request pins the
// module (it stays mapped until the process exits; a later unload / load reuses the image, which
// module.h allows: INIT after SHUTDOWN). A leaked context outlives the backend, so the callbacks
// reach nothing of the backend: each context shares ownership of the DnsApi (a leaked one keeps
// it, and with it dnsapi.dll, alive for the rest of the process) and a browse has its own lock.
#include "services.hpp"

#if defined(_WIN32)

#include <winsock2.h>
#include <windows.h>
#include <windns.h>

#include <cstring>
#include <set>

#include "sq/platform.h"
#include "sq/services/hardware_remote/dnssd_wire.hpp"

namespace sq::hwremote {
namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.resize(static_cast<size_t>(n - 1));
  return w;
}

std::string narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  s.resize(static_cast<size_t>(n - 1));
  return s;
}

// OS names are unescaped text with '.' separators; the instance label may contain dots only when
// the OS escaped them ("\."), which windns keeps verbatim. Pass them through unchanged.
std::string osName(const wchar_t* w) {
  std::string s = narrow(w);
  while (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

struct DnsApi {
  HMODULE dll = nullptr;
  decltype(&DnsServiceBrowse) browse = nullptr;
  decltype(&DnsServiceBrowseCancel) browseCancel = nullptr;
  decltype(&DnsServiceResolve) resolve = nullptr;
  decltype(&DnsServiceResolveCancel) resolveCancel = nullptr;
  decltype(&DnsServiceConstructInstance) construct = nullptr;
  decltype(&DnsServiceFreeInstance) freeInstance = nullptr;
  decltype(&DnsServiceRegister) reg = nullptr;
  decltype(&DnsServiceDeRegister) dereg = nullptr;
  decltype(&DnsFree) dnsFree = nullptr;

  bool load() {
    dll = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return false;
    auto get = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
      return fn != nullptr;
    };
    bool ok = get(browse, "DnsServiceBrowse") && get(browseCancel, "DnsServiceBrowseCancel") &&
              get(resolve, "DnsServiceResolve") && get(resolveCancel, "DnsServiceResolveCancel") &&
              get(construct, "DnsServiceConstructInstance") && get(freeInstance, "DnsServiceFreeInstance") &&
              get(reg, "DnsServiceRegister") && get(dereg, "DnsServiceDeRegister") && get(dnsFree, "DnsFree");
    if (!ok) {
      FreeLibrary(dll);
      dll = nullptr;
    }
    return ok;
  }
  ~DnsApi() {
    if (dll) FreeLibrary(dll);
  }
};

// Converts an OS record list into the portable message form.
dnssd::Message toMessage(const DNS_RECORD* list) {
  dnssd::Message m;
  m.flags = dnssd::kFlagResponse;
  for (const DNS_RECORD* r = list; r; r = r->pNext) {
    dnssd::Record d;
    d.name = osName(r->pName);
    d.type = r->wType;
    d.ttl = r->dwTtl;
    switch (r->wType) {
      case DNS_TYPE_PTR:
        d.target = osName(r->Data.PTR.pNameHost);
        break;
      case DNS_TYPE_SRV:
        d.target = osName(r->Data.SRV.pNameTarget);
        d.port = r->Data.SRV.wPort;
        d.priority = r->Data.SRV.wPriority;
        d.weight = r->Data.SRV.wWeight;
        break;
      case DNS_TYPE_TEXT:
        for (DWORD i = 0; i < r->Data.TXT.dwStringCount; ++i) {
          std::string t = narrow(r->Data.TXT.pStringArray[i]);
          if (!t.empty()) d.txt.push_back(t);
        }
        break;
      case DNS_TYPE_A:
        std::memcpy(d.addr, &r->Data.A.IpAddress, 4);
        break;
      default:
        continue;
    }
    m.answers.push_back(std::move(d));
  }
  return m;
}

class SystemDiscovery final : public DiscoveryBackend {
 public:
  explicit SystemDiscovery(std::shared_ptr<DnsApi> api) : api_(std::move(api)) {}
  ~SystemDiscovery() override { shutdown(); }
  const char* name() const override { return "system-dnssd"; }

  sq_result browse(uint32_t cookie, const char* serviceType, Ptr<IDiscoveryListener> listener) override {
    pinModule();
    auto b = std::make_unique<Browse>();
    b->api = api_;
    b->cookie = cookie;
    b->type = dnssd::joinName(dnssd::splitName(serviceType));
    b->query = widen(dnssd::serviceDomain(serviceType));
    b->listener = std::move(listener);
    b->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!b->done) return SQ_E_FAIL;
    DNS_SERVICE_BROWSE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.QueryName = b->query.c_str();
    req.pBrowseCallback = &SystemDiscovery::onBrowse;
    req.pQueryContext = b.get();
    std::lock_guard<std::recursive_mutex> lk(m_);
    DNS_STATUS st = api_->browse(&req, &b->cancel);
    if (st != DNS_REQUEST_PENDING) {
      sq::logf(SQ_LOG_WARN, "DnsServiceBrowse(%s) failed: %lu", serviceType, static_cast<unsigned long>(st));
      CloseHandle(b->done);
      return SQ_E_IO;
    }
    browses_[cookie] = std::move(b);
    return SQ_OK;
  }

  sq_result advertise(uint32_t cookie, const sq_discovery_record& record) override {
    pinModule();
    auto a = std::make_unique<Advert>();
    a->rec = record;
    a->api = api_;
    std::string host = record.host;
    bool literal = !host.empty() && host.find_first_not_of("0123456789.") == std::string::npos;
    if (host.empty() || literal) host = localHostName();
    a->instanceName = widen(dnssd::instanceFullName(record));
    a->hostName = widen(dnssd::hostTarget(host.c_str()));
    for (const auto& [k, v] : dnssd::txtPairs(record.txt)) {
      a->keys.push_back(widen(k));
      a->values.push_back(widen(v));
    }
    std::vector<PCWSTR> keys, values;
    for (size_t i = 0; i < a->keys.size(); ++i) {
      keys.push_back(a->keys[i].c_str());
      values.push_back(a->values[i].c_str());
    }
    uint32_t ip = net::primaryIPv4();
    IP4_ADDRESS ip4 = ip;
    a->instance = api_->construct(a->instanceName.c_str(), a->hostName.c_str(), ip ? &ip4 : nullptr, nullptr, record.port,
                                  0, 0, static_cast<DWORD>(keys.size()), keys.empty() ? nullptr : keys.data(),
                                  values.empty() ? nullptr : values.data());
    if (!a->instance) return SQ_E_FAIL;
    a->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!a->done) {
      api_->freeInstance(a->instance);
      return SQ_E_FAIL;
    }
    a->req.Version = DNS_QUERY_REQUEST_VERSION1;
    a->req.InterfaceIndex = 0;
    a->req.pServiceInstance = a->instance;
    a->req.pRegisterCompletionCallback = &SystemDiscovery::onRegister;
    a->req.pQueryContext = a.get();
    a->req.hCredentials = nullptr;
    a->req.unicastEnabled = FALSE;
    std::lock_guard<std::recursive_mutex> lk(m_);
    DWORD st = api_->reg(&a->req, nullptr);
    if (st != DNS_REQUEST_PENDING) {
      sq::logf(SQ_LOG_WARN, "DnsServiceRegister failed: %lu", static_cast<unsigned long>(st));
      CloseHandle(a->done);
      api_->freeInstance(a->instance);
      return SQ_E_IO;
    }
    adverts_[cookie] = std::move(a);
    return SQ_OK;
  }

  sq_result cancel(uint32_t cookie) override {
    std::unique_ptr<Browse> b;
    std::unique_ptr<Advert> a;
    {
      std::lock_guard<std::recursive_mutex> lk(m_);
      if (auto it = browses_.find(cookie); it != browses_.end()) {
        b = std::move(it->second);
        browses_.erase(it);
      } else if (auto at = adverts_.find(cookie); at != adverts_.end()) {
        a = std::move(at->second);
        adverts_.erase(at);
      } else {
        return SQ_E_NOTFOUND;
      }
    }
    if (b) stopBrowse(std::move(b));
    if (a) stopAdvert(std::move(a));
    return SQ_OK;
  }

  void shutdown() override {
    std::map<uint32_t, std::unique_ptr<Browse>> browses;
    std::map<uint32_t, std::unique_ptr<Advert>> adverts;
    {
      std::lock_guard<std::recursive_mutex> lk(m_);
      browses.swap(browses_);
      adverts.swap(adverts_);
    }
    for (auto& [c, b] : browses) stopBrowse(std::move(b));
    for (auto& [c, a] : adverts) stopAdvert(std::move(a));
  }

 private:
  struct Browse;
  struct Resolve {
    Browse* parent = nullptr;
    std::wstring name;
    DNS_SERVICE_CANCEL cancel{};
    HANDLE done = nullptr;
    std::atomic<bool> finished{false};
  };
  struct Browse {
    std::shared_ptr<DnsApi> api;    // shared, not the backend's: a leaked browse outlives it
    uint32_t cookie = 0;
    std::string type;
    std::wstring query;
    Ptr<IDiscoveryListener> listener;
    DNS_SERVICE_CANCEL cancel{};
    HANDLE done = nullptr;          // set when the OS reports the browse cancelled
    std::atomic<bool> stopping{false};
    std::atomic<int> inFlight{0};   // callbacks currently running for this browse
    std::recursive_mutex m;         // guards known and resolves
    std::set<std::string> known;    // lower-case instance labels reported as found
    std::map<std::string, std::unique_ptr<Resolve>> resolves;  // by lower-case label
  };
  struct Advert {
    sq_discovery_record rec{};
    std::wstring instanceName, hostName;
    std::vector<std::wstring> keys, values;
    PDNS_SERVICE_INSTANCE instance = nullptr;
    DNS_SERVICE_REGISTER_REQUEST req{};
    HANDLE done = nullptr;
    std::atomic<int> completions{0};
    std::shared_ptr<DnsApi> api;    // shared, not the backend's: a leaked advert outlives it
  };
  struct Event {
    Ptr<IDiscoveryListener> l;
    sq_discovery_record rec;
    bool found;
  };

  // Keeps this module mapped for the rest of the process (see the Lifetime note at the top): called
  // before the first OS request, whose callbacks run this module's code on OS threads.
  static void pinModule() {
    static std::once_flag once;
    std::call_once(once, [] {
      HMODULE self = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&once), &self))
        sq::logf(SQ_LOG_WARN, "could not pin the module (%lu): unloading it may crash a late DNS-SD callback",
                 static_cast<unsigned long>(GetLastError()));
    });
  }

  static std::string lower(std::string s) {
    for (auto& c : s)
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
  }

  static std::string localHostName() {
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n) && n) return narrow(buf);
    return "squizzy";
  }

  static void fire(std::vector<Event>& events) {
    for (auto& e : events) {
      if (e.found)
        e.l->onServiceFound(&e.rec);
      else
        e.l->onServiceLost(&e.rec);
    }
  }

  // The browse / resolve callbacks and what they run use only the Browse (and its Resolves),
  // never the backend: the context may have been leaked by stopBrowse and the backend be gone.
  static VOID WINAPI onBrowse(DWORD status, PVOID ctx, PDNS_RECORD records) {
    auto* b = static_cast<Browse*>(ctx);
    if (status == ERROR_CANCELLED || !b) {
      if (records && b) b->api->dnsFree(records, DnsFreeRecordList);
      if (b) SetEvent(b->done);
      return;
    }
    b->inFlight.fetch_add(1);
    try {
      if (status == ERROR_SUCCESS && records && !b->stopping.load()) handleBrowse(*b, records);
    } catch (...) {
    }
    if (records) b->api->dnsFree(records, DnsFreeRecordList);
    b->inFlight.fetch_sub(1);
  }

  static void handleBrowse(Browse& b, const DNS_RECORD* records) {
    dnssd::Message msg = toMessage(records);
    std::vector<sq_discovery_record> found, lost;
    dnssd::extractInstances(msg, b.type.c_str(), found, lost);
    std::set<std::string> complete;
    for (auto& r : found) complete.insert(lower(r.name));
    for (auto& r : lost) complete.insert(lower(r.name));
    std::vector<Event> events;
    {
      std::lock_guard<std::recursive_mutex> lk(b.m);
      for (auto& r : found) {
        if (r.type[0] == 0) std::snprintf(r.type, sizeof r.type, "%s", b.type.c_str());
        if (b.known.insert(lower(r.name)).second) events.push_back({b.listener, r, true});
      }
      for (auto& r : lost)
        if (b.known.erase(lower(r.name))) events.push_back({b.listener, r, false});
      // PTRs without SRV in this answer: resolve them. Not once stopping is set: stopBrowse may
      // already have taken the resolves (under b.m) to cancel them and would miss a new one.
      if (!b.stopping.load()) {
        for (const auto& [label, ttl] : dnssd::extractPointers(msg, b.type.c_str())) {
          std::string key = lower(label);
          if (ttl == 0 || complete.count(key) || b.known.count(key) || b.resolves.count(key)) continue;
          startResolve(b, label);
        }
      }
    }
    fire(events);
  }

  // b.m held.
  static void startResolve(Browse& b, const std::string& label) {
    sq_discovery_record tmp{};
    std::snprintf(tmp.name, sizeof tmp.name, "%s", label.c_str());
    std::snprintf(tmp.type, sizeof tmp.type, "%s", b.type.c_str());
    auto r = std::make_unique<Resolve>();
    r->parent = &b;
    r->name = widen(dnssd::instanceFullName(tmp));
    r->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!r->done) return;
    DNS_SERVICE_RESOLVE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.QueryName = r->name.data();
    req.pResolveCompletionCallback = &SystemDiscovery::onResolve;
    req.pQueryContext = r.get();
    if (b.api->resolve(&req, &r->cancel) != DNS_REQUEST_PENDING) {
      CloseHandle(r->done);
      return;
    }
    b.resolves[lower(label)] = std::move(r);
  }

  static VOID WINAPI onResolve(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    auto* r = static_cast<Resolve*>(ctx);
    Browse* b = r ? r->parent : nullptr;
    if (b) b->inFlight.fetch_add(1);
    try {
      if (b && status == ERROR_SUCCESS && inst && !b->stopping.load()) handleResolve(*b, inst);
    } catch (...) {
    }
    if (inst && b) b->api->freeInstance(inst);
    if (r) {
      r->finished.store(true);
      SetEvent(r->done);
    }
    if (b) b->inFlight.fetch_sub(1);
  }

  static void handleResolve(Browse& b, const DNS_SERVICE_INSTANCE* inst) {
    std::vector<dnssd::TxtPair> txt;
    for (DWORD i = 0; i < inst->dwPropertyCount; ++i)
      txt.emplace_back(narrow(inst->keys ? inst->keys[i] : nullptr), narrow(inst->values ? inst->values[i] : nullptr));
    std::string host = osName(inst->pszHostName);
    if (inst->ip4Address && *inst->ip4Address) {
      const auto* p = reinterpret_cast<const uint8_t*>(inst->ip4Address);
      char ip[32];
      std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
      host = ip;
    }
    sq_discovery_record rec{};
    if (!dnssd::makeRecord(osName(inst->pszInstanceName), host, inst->wPort, txt, rec)) return;
    std::vector<Event> events;
    {
      std::lock_guard<std::recursive_mutex> lk(b.m);
      if (b.known.insert(lower(rec.name)).second) events.push_back({b.listener, rec, true});
    }
    fire(events);
  }

  static VOID WINAPI onRegister(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    auto* a = static_cast<Advert*>(ctx);
    // The OS hands back its own copy of the instance (ours stays owned by the advert): free it
    // before counting, the advert may be destroyed as soon as the count reaches what stopAdvert
    // waits for. The count is the last access (the event only wakes the waiter early).
    if (inst && a) a->api->freeInstance(inst);
    if (a) {
      if (status != ERROR_SUCCESS && a->completions.load() == 0)
        sq_log_printf(SQ_LOG_WARN, "hardware-remote", "DNS-SD registration of '%s' failed: %lu", a->rec.name,
                      static_cast<unsigned long>(status));
      SetEvent(a->done);
      a->completions.fetch_add(1);
    }
  }

  void stopBrowse(std::unique_ptr<Browse> b) {
    b->stopping.store(true);
    api_->browseCancel(&b->cancel);
    bool cancelled = WaitForSingleObject(b->done, 2000) == WAIT_OBJECT_0;
    std::map<std::string, std::unique_ptr<Resolve>> resolves;
    {
      std::lock_guard<std::recursive_mutex> lk(b->m);
      resolves.swap(b->resolves);
    }
    bool resolvesDone = true;
    for (auto& [k, r] : resolves) {
      if (!r->finished.load()) api_->resolveCancel(&r->cancel);
      if (WaitForSingleObject(r->done, 2000) != WAIT_OBJECT_0) resolvesDone = false;
    }
    for (int i = 0; i < 200 && b->inFlight.load() > 0; ++i) Sleep(10);
    if (!cancelled || !resolvesDone || b->inFlight.load() > 0) {
      // The OS may still call back into these objects: keep them alive (bounded leak).
      sq::logf(SQ_LOG_WARN, "DNS-SD browse %u did not confirm cancellation; leaking its context", b->cookie);
      for (auto& [k, r] : resolves) (void)r.release();
      (void)b.release();
      return;
    }
    for (auto& [k, r] : resolves) CloseHandle(r->done);
    CloseHandle(b->done);
  }

  void stopAdvert(std::unique_ptr<Advert> a) {
    // Every completion still to come must have arrived: the registration's (pending when the
    // advertisement is cancelled right after advertise(); it arrives even then) and, if the OS
    // took the request, the deregistration's. Waiting for "one more than now" took a late
    // registration completion for the deregistration's and freed the context under the
    // deregistration callback.
    DWORD st = api_->dereg(&a->req, nullptr);
    const int expected = st == DNS_REQUEST_PENDING ? 2 : 1;
    for (int i = 0; i < 40 && a->completions.load() < expected; ++i) WaitForSingleObject(a->done, 50);
    const bool confirmed = a->completions.load() >= expected;
    if (!confirmed) {
      sq::logf(SQ_LOG_WARN, "DNS-SD deregistration of '%s' not confirmed; leaking its context", a->rec.name);
      (void)a.release();
      return;
    }
    CloseHandle(a->done);
    api_->freeInstance(a->instance);
  }

  std::shared_ptr<DnsApi> api_;  // shared with every context (see the Lifetime note at the top)
  std::recursive_mutex m_;        // guards browses_ and adverts_
  std::map<uint32_t, std::unique_ptr<Browse>> browses_;
  std::map<uint32_t, std::unique_ptr<Advert>> adverts_;
};

}  // namespace

std::unique_ptr<DiscoveryBackend> makeSystemDiscovery() {
  auto api = std::make_shared<DnsApi>();
  if (!api->load()) return nullptr;
  return std::make_unique<SystemDiscovery>(std::move(api));
}

}  // namespace sq::hwremote

#else  // !_WIN32

namespace sq::hwremote {
std::unique_ptr<DiscoveryBackend> makeSystemDiscovery() { return nullptr; }
}  // namespace sq::hwremote

#endif
