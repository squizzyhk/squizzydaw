// hardware-remote: the system DNS-SD backend (src/discovery_win.cpp) over a fake dnsapi table, for
// what the OS service cannot be made to do on demand: a completion that arrives only after
// cancel()'s bounded wait gave up on it (so the context was leaked) and the backend is gone.
//
// The backend is internal to the module (anonymous namespace), so its source is compiled here.
#include "../src/discovery_win.cpp"

#include <string>
#include <vector>

#include "sqtest.hpp"

namespace {

using sq::hwremote::DnsApi;
using sq::hwremote::SystemDiscovery;

// What the fake OS was asked. Cancellations / deregistrations are answered on the spot unless
// answerCancels is false; then they are not answered at all until the test delivers them.
struct FakeOs {
  struct Resolve {
    PDNS_SERVICE_CANCEL cancel;
    PDNS_SERVICE_RESOLVE_COMPLETE cb;
    PVOID ctx;
  };
  bool answerCancels = true;
  PDNS_SERVICE_REGISTER_COMPLETE regCb = nullptr;
  PVOID regCtx = nullptr;
  PDNS_SERVICE_BROWSE_CALLBACK browseCb = nullptr;
  PVOID browseCtx = nullptr;
  std::vector<Resolve> resolves;
  int instancesFreed = 0;
  int recordListsFreed = 0;
};
FakeOs g_os;

PDNS_SERVICE_INSTANCE WINAPI fakeConstruct(PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD, DWORD,
                                           PCWSTR*, PCWSTR*) {
  return new DNS_SERVICE_INSTANCE{};
}

VOID WINAPI fakeFreeInstance(PDNS_SERVICE_INSTANCE inst) {
  ++g_os.instancesFreed;
  delete inst;
}

VOID WINAPI fakeDnsFree(PVOID, DNS_FREE_TYPE) { ++g_os.recordListsFreed; }  // the lists are the test's

DWORD WINAPI fakeRegister(PDNS_SERVICE_REGISTER_REQUEST req, PDNS_SERVICE_CANCEL) {
  g_os.regCb = req->pRegisterCompletionCallback;
  g_os.regCtx = req->pQueryContext;
  return DNS_REQUEST_PENDING;
}

DWORD WINAPI fakeDeRegister(PDNS_SERVICE_REGISTER_REQUEST req, PDNS_SERVICE_CANCEL) {
  if (g_os.answerCancels) req->pRegisterCompletionCallback(ERROR_SUCCESS, req->pQueryContext, new DNS_SERVICE_INSTANCE{});
  return DNS_REQUEST_PENDING;
}

DNS_STATUS WINAPI fakeBrowse(PDNS_SERVICE_BROWSE_REQUEST req, PDNS_SERVICE_CANCEL) {
  g_os.browseCb = req->pBrowseCallback;
  g_os.browseCtx = req->pQueryContext;
  return DNS_REQUEST_PENDING;
}

DNS_STATUS WINAPI fakeBrowseCancel(PDNS_SERVICE_CANCEL) {
  if (g_os.answerCancels) g_os.browseCb(ERROR_CANCELLED, g_os.browseCtx, nullptr);
  return ERROR_SUCCESS;
}

DNS_STATUS WINAPI fakeResolve(PDNS_SERVICE_RESOLVE_REQUEST req, PDNS_SERVICE_CANCEL cancel) {
  g_os.resolves.push_back({cancel, req->pResolveCompletionCallback, req->pQueryContext});
  return DNS_REQUEST_PENDING;
}

DNS_STATUS WINAPI fakeResolveCancel(PDNS_SERVICE_CANCEL cancel) {
  for (const auto& r : g_os.resolves)
    if (r.cancel == cancel && g_os.answerCancels) r.cb(ERROR_CANCELLED, r.ctx, nullptr);
  return ERROR_SUCCESS;
}

// The DnsApi over the fakes. It holds a reference on `marker`, a system DLL this process does not
// load otherwise: ~DnsApi frees it, so the DLL being mapped tells whether the DnsApi is alive.
std::unique_ptr<DnsApi> fakeApi(const wchar_t* marker) {
  auto api = std::make_unique<DnsApi>();
  api->dll = LoadLibraryExW(marker, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  api->browse = &fakeBrowse;
  api->browseCancel = &fakeBrowseCancel;
  api->resolve = &fakeResolve;
  api->resolveCancel = &fakeResolveCancel;
  api->construct = &fakeConstruct;
  api->freeInstance = &fakeFreeInstance;
  api->reg = &fakeRegister;
  api->dereg = &fakeDeRegister;
  api->dnsFree = &fakeDnsFree;
  return api;
}

bool mapped(const wchar_t* dll) { return GetModuleHandleW(dll) != nullptr; }

sq_discovery_record record() {
  sq_discovery_record r{};
  r.struct_size = sizeof r;
  std::snprintf(r.name, sizeof r.name, "sqtest");
  std::snprintf(r.type, sizeof r.type, "_sqtest._udp");
  r.port = 4000;
  std::snprintf(r.txt, sizeof r.txt, "ver=1");
  return r;
}

// A browse answer, owned by the test (the fake DnsFree only counts it): PTR type -> instance and,
// when port is set, the instance's SRV, which makes it complete (reported without a resolve).
struct Answer {
  std::wstring domain = L"_sqtest._udp.local";
  std::wstring instance;
  std::wstring host = L"box.local";
  DNS_RECORD ptr{}, srv{};
  Answer(const wchar_t* label, WORD port) : instance(std::wstring(label) + L"._sqtest._udp.local") {
    ptr.pName = domain.data();
    ptr.wType = DNS_TYPE_PTR;
    ptr.dwTtl = 120;
    ptr.Data.PTR.pNameHost = instance.data();
    if (!port) return;
    ptr.pNext = &srv;
    srv.pName = instance.data();
    srv.wType = DNS_TYPE_SRV;
    srv.dwTtl = 120;
    srv.Data.SRV.pNameTarget = host.data();
    srv.Data.SRV.wPort = port;
  }
  Answer(const Answer&) = delete;
  Answer& operator=(const Answer&) = delete;
  PDNS_RECORD list() { return &ptr; }
};

// A resolve result as the OS hands it over (the fake DnsServiceFreeInstance deletes the struct,
// the strings stay the test's).
struct Resolved {
  std::wstring name;
  std::wstring host = L"box.local";
  explicit Resolved(const wchar_t* label) : name(std::wstring(label) + L"._sqtest._udp.local") {}
  PDNS_SERVICE_INSTANCE make(WORD port) {
    auto* i = new DNS_SERVICE_INSTANCE{};
    i->pszInstanceName = name.data();
    i->pszHostName = host.data();
    i->wPort = port;
    return i;
  }
};

class Counter final : public sq::Object<Counter, sq::IDiscoveryListener> {
 public:
  void SQ_CALL onServiceFound(const sq_discovery_record*) override { ++found; }
  void SQ_CALL onServiceLost(const sq_discovery_record*) override { ++lost; }
  int found = 0;
  int lost = 0;
};

constexpr const wchar_t* kMarkerAnswered = L"msimg32.dll";
constexpr const wchar_t* kMarkerLeaked = L"wtsapi32.dll";

}  // namespace

SQ_TEST("system-dnssd [fake]: answered completions free every context, and the API with the backend") {
  REQUIRE(!mapped(kMarkerAnswered));
  g_os = FakeOs{};
  auto listener = sq::Ptr<Counter>::adopt(new Counter());
  {
    auto be = std::make_unique<SystemDiscovery>(fakeApi(kMarkerAnswered));
    REQUIRE(mapped(kMarkerAnswered));
    REQUIRE_OK(be->advertise(1, record()));
    REQUIRE(g_os.regCb);
    g_os.regCb(ERROR_SUCCESS, g_os.regCtx, new DNS_SERVICE_INSTANCE{});  // registered
    REQUIRE_OK(be->browse(2, "_sqtest._udp", sq::Ptr<sq::IDiscoveryListener>(listener.get())));
    REQUIRE(g_os.browseCb);
    Answer one(L"one", 4001), two(L"two", 0), three(L"three", 0);
    g_os.browseCb(ERROR_SUCCESS, g_os.browseCtx, one.list());
    CHECK_EQ(listener->found, 1);
    g_os.browseCb(ERROR_SUCCESS, g_os.browseCtx, two.list());  // PTR only: resolved
    g_os.browseCb(ERROR_SUCCESS, g_os.browseCtx, three.list());
    REQUIRE_EQ(g_os.resolves.size(), size_t(2));
    Resolved twoResolved(L"two");
    g_os.resolves[0].cb(ERROR_SUCCESS, g_os.resolves[0].ctx, twoResolved.make(4002));
    CHECK_EQ(listener->found, 2);
    // "three" is still resolving; cancel() cancels it and the OS answers every cancellation.
    CHECK_OK(be->cancel(1));
    CHECK_OK(be->cancel(2));
    CHECK_EQ(g_os.recordListsFreed, 3);
    CHECK(mapped(kMarkerAnswered));  // still the backend's
  }
  CHECK(!mapped(kMarkerAnswered));  // no context kept the API: it went with the backend
  // The advert's own instance, the OS copies from both registration completions, the resolve result.
  CHECK_EQ(g_os.instancesFreed, 4);
  CHECK_EQ(listener->found, 2);
}

SQ_TEST("system-dnssd [fake]: contexts leaked by cancel() keep the API alive; late completions stay inside them") {
  REQUIRE(!mapped(kMarkerLeaked));
  g_os = FakeOs{};
  g_os.answerCancels = false;  // no completion within cancel()'s bounded waits
  auto listener = sq::Ptr<Counter>::adopt(new Counter());
  Answer bare(L"late", 0), complete(L"later", 4003);
  Resolved lateResolved(L"late");
  {
    auto be = std::make_unique<SystemDiscovery>(fakeApi(kMarkerLeaked));
    REQUIRE(mapped(kMarkerLeaked));
    REQUIRE_OK(be->advertise(1, record()));
    REQUIRE(g_os.regCb);
    g_os.regCb(ERROR_SUCCESS, g_os.regCtx, new DNS_SERVICE_INSTANCE{});  // registered
    REQUIRE_OK(be->browse(2, "_sqtest._udp", sq::Ptr<sq::IDiscoveryListener>(listener.get())));
    REQUIRE(g_os.browseCb);
    g_os.browseCb(ERROR_SUCCESS, g_os.browseCtx, bare.list());  // PTR only: resolving
    REQUIRE_EQ(g_os.resolves.size(), size_t(1));
    CHECK_OK(be->cancel(1));  // deregistration not confirmed: the advert is leaked
    CHECK_OK(be->cancel(2));  // browse / resolve cancellation not confirmed: both leaked
  }
  // The backend is gone. The leaked contexts still own the API, so dnsapi stays loaded ...
  CHECK(mapped(kMarkerLeaked));
  // ... and the completions arriving now use only them (and report nothing after cancel()).
  const int instances = g_os.instancesFreed, lists = g_os.recordListsFreed;
  g_os.regCb(ERROR_SUCCESS, g_os.regCtx, new DNS_SERVICE_INSTANCE{});  // the deregistration
  CHECK_EQ(g_os.instancesFreed, instances + 1);
  g_os.resolves[0].cb(ERROR_SUCCESS, g_os.resolves[0].ctx, lateResolved.make(4004));
  CHECK_EQ(g_os.instancesFreed, instances + 2);
  g_os.browseCb(ERROR_SUCCESS, g_os.browseCtx, complete.list());
  CHECK_EQ(g_os.recordListsFreed, lists + 1);
  g_os.browseCb(ERROR_CANCELLED, g_os.browseCtx, nullptr);  // the browse cancellation
  CHECK_EQ(listener->found, 0);
  CHECK_EQ(listener->lost, 0);
}
