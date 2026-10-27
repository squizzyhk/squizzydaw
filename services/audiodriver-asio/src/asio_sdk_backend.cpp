// Squizzy Studio - ASIO driver service: the real host, compiled only with -DSQ_ASIO_SDK_DIR.
//
// Uses the driver interface declared by the user-supplied ASIO SDK (common/iasiodrv.h, asio.h).
// Nothing from the SDK is vendored; this file is the only one that includes it.
//
// Lifecycle (control thread): COM init (apartment) -> CoCreateInstance(CLSID from the registry,
// the driver's CLSID doubles as its interface id) -> init(window) -> ... -> Release.
// The driver's callbacks carry no context pointer, so one backend may exist per process; the
// active sink and driver are kept in process globals.
#if !defined(_WIN32)
#error "The ASIO host is Windows-only"
#endif

#ifndef WIN32
#define WIN32 1  // some SDK headers key their platform setup on WIN32
#endif

// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "asio_backend.hpp"

namespace sq::asio {

// The SDK-independent constants must match the SDK exactly.
static_assert(ASIOSTInt16MSB == static_cast<int>(SampleType::Int16MSB));
static_assert(ASIOSTInt24MSB == static_cast<int>(SampleType::Int24MSB));
static_assert(ASIOSTInt32MSB == static_cast<int>(SampleType::Int32MSB));
static_assert(ASIOSTFloat32MSB == static_cast<int>(SampleType::Float32MSB));
static_assert(ASIOSTFloat64MSB == static_cast<int>(SampleType::Float64MSB));
static_assert(ASIOSTInt32MSB16 == static_cast<int>(SampleType::Int32MSB16));
static_assert(ASIOSTInt32MSB18 == static_cast<int>(SampleType::Int32MSB18));
static_assert(ASIOSTInt32MSB20 == static_cast<int>(SampleType::Int32MSB20));
static_assert(ASIOSTInt32MSB24 == static_cast<int>(SampleType::Int32MSB24));
static_assert(ASIOSTInt16LSB == static_cast<int>(SampleType::Int16LSB));
static_assert(ASIOSTInt24LSB == static_cast<int>(SampleType::Int24LSB));
static_assert(ASIOSTInt32LSB == static_cast<int>(SampleType::Int32LSB));
static_assert(ASIOSTFloat32LSB == static_cast<int>(SampleType::Float32LSB));
static_assert(ASIOSTFloat64LSB == static_cast<int>(SampleType::Float64LSB));
static_assert(ASIOSTInt32LSB16 == static_cast<int>(SampleType::Int32LSB16));
static_assert(ASIOSTInt32LSB18 == static_cast<int>(SampleType::Int32LSB18));
static_assert(ASIOSTInt32LSB20 == static_cast<int>(SampleType::Int32LSB20));
static_assert(ASIOSTInt32LSB24 == static_cast<int>(SampleType::Int32LSB24));
static_assert(kAsioSelectorSupported == msg::kSelectorSupported);
static_assert(kAsioEngineVersion == msg::kEngineVersion);
static_assert(kAsioResetRequest == msg::kResetRequest);
static_assert(kAsioBufferSizeChange == msg::kBufferSizeChange);
static_assert(kAsioResyncRequest == msg::kResyncRequest);
static_assert(kAsioLatenciesChanged == msg::kLatenciesChanged);
static_assert(kAsioSupportsTimeInfo == msg::kSupportsTimeInfo);
static_assert(kAsioSupportsTimeCode == msg::kSupportsTimeCode);
static_assert(kAsioOverload == msg::kOverload);

namespace {

std::atomic<BackendSink*> g_sink{nullptr};
std::atomic<IASIO*> g_driver{nullptr};  // for getSamplePosition from the plain buffer-switch callback
std::atomic<bool> g_backendAlive{false};

// 64-bit sample positions / time stamps are either native integers or {hi, lo} pairs per SDK config.
template <class T>
int64_t toI64(const T& v) {
  if constexpr (std::is_arithmetic_v<T>) {
    return static_cast<int64_t>(v);
  } else {
    return static_cast<int64_t>((static_cast<uint64_t>(v.hi) << 32) | static_cast<uint32_t>(v.lo));
  }
}

sq_result mapError(ASIOError e) {
  switch (e) {
    case ASE_OK:
    case ASE_SUCCESS: return SQ_OK;
    case ASE_NotPresent: return SQ_E_NOTFOUND;
    case ASE_HWMalfunction: return SQ_E_IO;
    case ASE_InvalidParameter: return SQ_E_INVALIDARG;
    case ASE_InvalidMode: return SQ_E_UNSUPPORTED;
    case ASE_NoClock: return SQ_E_UNSUPPORTED;
    case ASE_NoMemory: return SQ_E_OUTOFMEMORY;
    default: return SQ_E_FAIL;
  }
}

// ---- C callbacks handed to the driver ------------------------------------------------------

void cbBufferSwitch(long index, ASIOBool directProcess) {
  BackendSink* s = g_sink.load(std::memory_order_acquire);
  if (!s) return;
  ASIOSamples pos{};
  ASIOTimeStamp ts{};
  IASIO* d = g_driver.load(std::memory_order_acquire);
  const bool ok = d && d->getSamplePosition(&pos, &ts) == ASE_OK;
  s->bufferSwitch(static_cast<int32_t>(index), directProcess != ASIOFalse, ok, ok ? toI64(pos) : 0, ok,
                  ok ? toI64(ts) : 0);
}

ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool directProcess) {
  BackendSink* s = g_sink.load(std::memory_order_acquire);
  if (!s) return nullptr;
  bool posValid = false, timeValid = false;
  int64_t pos = 0, time = 0;
  if (params) {
    const auto& ti = params->timeInfo;
    posValid = (ti.flags & kSamplePositionValid) != 0;
    timeValid = (ti.flags & kSystemTimeValid) != 0;
    if (posValid) pos = toI64(ti.samplePosition);
    if (timeValid) time = toI64(ti.systemTime);
  }
  s->bufferSwitch(static_cast<int32_t>(index), directProcess != ASIOFalse, posValid, pos, timeValid, time);
  return nullptr;
}

void cbSampleRateDidChange(ASIOSampleRate rate) {
  if (BackendSink* s = g_sink.load(std::memory_order_acquire)) s->sampleRateChanged(static_cast<double>(rate));
}

long cbAsioMessage(long selector, long value, void*, double*) {
  BackendSink* s = g_sink.load(std::memory_order_acquire);
  if (!s) return 0;
  return static_cast<long>(s->message(static_cast<int32_t>(selector), static_cast<int32_t>(value)));
}

// ---- backend --------------------------------------------------------------------------------

class SdkBackend final : public Backend {
 public:
  SdkBackend(IASIO* drv, bool comInit) : drv_(drv), comInit_(comInit), thread_(GetCurrentThreadId()) {
    g_driver.store(drv_, std::memory_order_release);
  }
  ~SdkBackend() override {
    if (started_) drv_->stop();
    if (buffers_) disposeBuffers();
    g_driver.store(nullptr, std::memory_order_release);
    drv_->Release();
    if (comInit_ && GetCurrentThreadId() == thread_) CoUninitialize();
    g_backendAlive.store(false);
  }
  SdkBackend(const SdkBackend&) = delete;
  SdkBackend& operator=(const SdkBackend&) = delete;

  std::string driverName() override {
    char buf[256] = {};
    drv_->getDriverName(buf);
    buf[sizeof buf - 1] = '\0';
    return buf;
  }
  sq_result getChannels(int32_t& inputs, int32_t& outputs) override {
    long i = 0, o = 0;
    const sq_result r = mapError(drv_->getChannels(&i, &o));
    inputs = static_cast<int32_t>(i);
    outputs = static_cast<int32_t>(o);
    return r;
  }
  sq_result getLatencies(int32_t& input, int32_t& output) override {
    long i = 0, o = 0;
    const sq_result r = mapError(drv_->getLatencies(&i, &o));
    input = static_cast<int32_t>(i);
    output = static_cast<int32_t>(o);
    return r;
  }
  sq_result getBufferSize(BufferSizes& out) override {
    long mn = 0, mx = 0, pf = 0, gr = 0;
    const sq_result r = mapError(drv_->getBufferSize(&mn, &mx, &pf, &gr));
    out.minFrames = static_cast<int32_t>(mn);
    out.maxFrames = static_cast<int32_t>(mx);
    out.preferredFrames = static_cast<int32_t>(pf);
    out.granularity = static_cast<int32_t>(gr);
    return r;
  }
  sq_result canSampleRate(double rate) override { return mapError(drv_->canSampleRate(static_cast<ASIOSampleRate>(rate))); }
  sq_result getSampleRate(double& rate) override {
    ASIOSampleRate r{};
    const sq_result res = mapError(drv_->getSampleRate(&r));
    rate = SQ_SUCCEEDED(res) ? static_cast<double>(r) : 0.0;
    return res;
  }
  sq_result setSampleRate(double rate) override { return mapError(drv_->setSampleRate(static_cast<ASIOSampleRate>(rate))); }
  sq_result getChannelInfo(bool input, int32_t channel, ChannelInfo& out) override {
    ASIOChannelInfo ci{};
    ci.channel = channel;
    ci.isInput = input ? ASIOTrue : ASIOFalse;
    const sq_result r = mapError(drv_->getChannelInfo(&ci));
    if (SQ_FAILED(r)) return r;
    out.type = static_cast<SampleType>(ci.type);
    out.group = static_cast<int32_t>(ci.channelGroup);
    out.active = ci.isActive != ASIOFalse;
    static_assert(sizeof(out.name) <= sizeof(ci.name));
    std::memcpy(out.name, ci.name, sizeof out.name);
    out.name[sizeof out.name - 1] = '\0';
    return SQ_OK;
  }
  sq_result createBuffers(const ChannelRequest* reqs, int32_t count, int32_t blockFrames, ChannelBuffers* out,
                          BackendSink* sink) override {
    if (buffers_) disposeBuffers();
    if (!reqs || !out || count <= 0) return SQ_E_INVALIDARG;
    infos_.assign(static_cast<size_t>(count), ASIOBufferInfo{});
    for (int32_t k = 0; k < count; ++k) {
      infos_[k].isInput = reqs[k].input ? ASIOTrue : ASIOFalse;
      infos_[k].channelNum = reqs[k].channel;
    }
    callbacks_.bufferSwitch = &cbBufferSwitch;
    callbacks_.sampleRateDidChange = &cbSampleRateDidChange;
    callbacks_.asioMessage = &cbAsioMessage;
    callbacks_.bufferSwitchTimeInfo = &cbBufferSwitchTimeInfo;
    g_sink.store(sink, std::memory_order_release);  // drivers may send messages during createBuffers
    const sq_result r = mapError(drv_->createBuffers(infos_.data(), count, blockFrames, &callbacks_));
    if (SQ_FAILED(r)) {
      g_sink.store(nullptr, std::memory_order_release);
      return r;
    }
    buffers_ = true;
    for (int32_t k = 0; k < count; ++k) {
      out[k].half[0] = infos_[k].buffers[0];
      out[k].half[1] = infos_[k].buffers[1];
    }
    outputReadyProbed_ = false;
    return SQ_OK;
  }
  sq_result disposeBuffers() override {
    if (!buffers_) return SQ_S_FALSE;
    if (started_) stop();
    const sq_result r = mapError(drv_->disposeBuffers());
    buffers_ = false;
    g_sink.store(nullptr, std::memory_order_release);
    return r;
  }
  sq_result start() override {
    const sq_result r = mapError(drv_->start());
    started_ = SQ_SUCCEEDED(r);
    return r;
  }
  sq_result stop() override {
    if (!started_) return SQ_S_FALSE;
    started_ = false;
    return mapError(drv_->stop());
  }
  bool supportsOutputReady() override {
    if (!outputReadyProbed_) {
      outputReady_ = drv_->outputReady() == ASE_OK;
      outputReadyProbed_ = true;
    }
    return outputReady_;
  }
  void outputReady() noexcept override { drv_->outputReady(); }
  sq_result controlPanel() override { return mapError(drv_->controlPanel()); }
  std::string lastError() override {
    char buf[256] = {};
    drv_->getErrorMessage(buf);
    buf[sizeof buf - 1] = '\0';
    return buf;
  }

 private:
  IASIO* drv_;
  bool comInit_;
  DWORD thread_;
  bool buffers_ = false;
  bool started_ = false;
  bool outputReady_ = false;
  bool outputReadyProbed_ = false;
  std::vector<ASIOBufferInfo> infos_;
  ASIOCallbacks callbacks_{};
};

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::unique_ptr<Backend> loadDriver(const DriverEntry& e, std::string* err) {
  auto fail = [&](std::string m) -> std::unique_ptr<Backend> {
    if (err) *err = std::move(m);
    return nullptr;
  };
  bool expected = false;
  if (!g_backendAlive.compare_exchange_strong(expected, true)) return fail("another ASIO driver is already loaded");
  CLSID clsid{};
  const std::wstring ws = widen(e.clsid);
  if (FAILED(CLSIDFromString(ws.c_str(), &clsid))) {
    g_backendAlive.store(false);
    return fail("malformed CLSID " + e.clsid);
  }
  const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool comInit = SUCCEEDED(hrInit);  // RPC_E_CHANGED_MODE: an MTA thread; proceed without owning COM
  IASIO* drv = nullptr;
  // ASIO drivers use their CLSID as the interface id.
  const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&drv));
  if (FAILED(hr) || !drv) {
    if (comInit) CoUninitialize();
    g_backendAlive.store(false);
    char buf[64];
    std::snprintf(buf, sizeof buf, "CoCreateInstance failed (0x%08lX)", static_cast<unsigned long>(hr));
    return fail(buf);
  }
  HWND wnd = GetActiveWindow();
  if (!wnd) wnd = GetDesktopWindow();
  if (drv->init(reinterpret_cast<void*>(wnd)) == ASIOFalse) {
    char msg[256] = {};
    drv->getErrorMessage(msg);
    msg[sizeof msg - 1] = '\0';
    drv->Release();
    if (comInit) CoUninitialize();
    g_backendAlive.store(false);
    return fail(std::string("driver init failed: ") + (msg[0] ? msg : "no reason given"));
  }
  try {
    return std::make_unique<SdkBackend>(drv, comInit);
  } catch (...) {
    drv->Release();
    if (comInit) CoUninitialize();
    g_backendAlive.store(false);
    return fail("out of memory");
  }
}

}  // namespace

BackendFactory makeSdkBackendFactory() { return &loadDriver; }

}  // namespace sq::asio
