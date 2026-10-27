// Squizzy Studio - WASAPI audio driver service (implements sq/interfaces/audiodevice.h).
//
// Device model
//   Every active endpoint is one device:   "wasapi:out:<endpoint id>"   (render, outputs only)
//                                          "wasapi:in:<endpoint id>"    (capture, inputs only)
//   plus one synthetic duplex device:      "wasapi:default"             (default render + capture)
//   openDevice() also accepts an explicit pair "wasapi:duplex:<render id>|<capture id>".
//
// Streaming
//   Event-driven. A render stream drives the clock; in duplex mode the capture stream is drained
//   on every render wake-up into a pre-allocated planar FIFO. Input-only devices are driven by the
//   capture event. Shared mode uses IAudioClient3 low-latency periods when available (Windows 10+),
//   otherwise the engine period; non-mix sample rates go through the OS resampler (AUTOCONVERTPCM).
//   Exclusive mode negotiates F32 > I32 > I24-in-32 > I24 > I16 and aligns the period to the
//   hardware buffer. The stream thread joins MMCSS "Pro Audio".
//
// Notifications
//   IMMNotificationClient -> IAudioDeviceListener::onDeviceListChanged; endpoint removal/disable
//   of an open device -> onDeviceReset(DEVICE_LOST); default-device change for "wasapi:default"
//   -> onDeviceReset(REQUESTED); a stream invalidated by a format change -> FORMAT_CHANGED.
#include "wasapi_driver.hpp"

#ifdef _WIN32

// clang-format off
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <shellapi.h>
// clang-format on
#ifdef small
#undef small  // rpcndr.h macro; clashes with identifiers in sq/cpp/settings.hpp
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sq/audiodriver/sample_convert.hpp"
#include "sq/platform.h"
#include "sq/sdk.hpp"

namespace sq::wasapi {
namespace {

using audiodrv::SampleKind;
using audiodrv::SampleLayout;

// ---------------------------------------------------------------------------------------------
// small COM helpers

template <class T>
class Com {
 public:
  Com() = default;
  Com(const Com& o) : p_(o.p_) {
    if (p_) p_->AddRef();
  }
  Com& operator=(const Com& o) {
    if (this != &o) {
      if (o.p_) o.p_->AddRef();
      reset();
      p_ = o.p_;
    }
    return *this;
  }
  ~Com() { reset(); }
  void reset() {
    if (p_) {
      T* p = p_;
      p_ = nullptr;
      p->Release();
    }
  }
  T** put() {
    reset();
    return &p_;
  }
  void** putVoid() { return reinterpret_cast<void**>(put()); }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

struct ComScope {
  HRESULT hr;
  ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
};

struct CoTaskString {
  LPWSTR p = nullptr;
  ~CoTaskString() {
    if (p) CoTaskMemFree(p);
  }
};

struct CoTaskFormat {
  WAVEFORMATEX* p = nullptr;
  ~CoTaskFormat() {
    if (p) CoTaskMemFree(p);
  }
};

std::string toUtf8(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  s.resize(static_cast<size_t>(n - 1));
  return s;
}

std::wstring toWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.resize(static_cast<size_t>(n - 1));
  return w;
}

void copyStr(char* dst, size_t cap, const std::string& s) {
  if (!cap) return;
  size_t n = std::min(cap - 1, s.size());
  // do not cut a UTF-8 sequence in half
  while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

sq_result mapHr(HRESULT hr) {
  if (SUCCEEDED(hr)) return SQ_OK;
  if (hr == E_INVALIDARG || hr == E_POINTER) return SQ_E_INVALIDARG;
  if (hr == E_OUTOFMEMORY) return SQ_E_OUTOFMEMORY;
  if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return SQ_E_NOTFOUND;
  if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) return SQ_E_BUSY;
  if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return SQ_E_UNSUPPORTED;
  if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return SQ_E_CLOSED;
  if (hr == E_ACCESSDENIED) return SQ_E_ACCESS;
  return SQ_E_FAIL;
}

constexpr REFERENCE_TIME kHnsPerSec = 10000000;

uint32_t hnsToFrames(REFERENCE_TIME hns, double rate) {
  if (hns <= 0 || rate <= 0) return 0;
  return static_cast<uint32_t>(static_cast<double>(hns) * rate / static_cast<double>(kHnsPerSec) + 0.5);
}
REFERENCE_TIME framesToHns(uint32_t frames, double rate) {
  if (rate <= 0) return 0;
  return static_cast<REFERENCE_TIME>(static_cast<double>(frames) * static_cast<double>(kHnsPerSec) / rate + 0.5);
}

// ---------------------------------------------------------------------------------------------
// format helpers

bool isFloatFormat(const WAVEFORMATEX* f) {
  if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
    auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    return IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
  }
  return false;
}

bool isPcmFormat(const WAVEFORMATEX* f) {
  if (f->wFormatTag == WAVE_FORMAT_PCM) return true;
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
    auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    return IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
  }
  return false;
}

// Map a WAVEFORMATEX to our layout (all WASAPI formats are little endian).
SampleLayout layoutOf(const WAVEFORMATEX* f) {
  SampleLayout L{};
  uint16_t valid = f->wBitsPerSample;
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
    auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    if (x->Samples.wValidBitsPerSample) valid = x->Samples.wValidBitsPerSample;
  }
  if (isFloatFormat(f)) {
    if (f->wBitsPerSample == 32) L.kind = SampleKind::F32;
    else if (f->wBitsPerSample == 64) L.kind = SampleKind::F64;
  } else if (isPcmFormat(f)) {
    if (f->wBitsPerSample == 16) L.kind = SampleKind::I16;
    else if (f->wBitsPerSample == 24) L.kind = SampleKind::I24;
    else if (f->wBitsPerSample == 32) L.kind = (valid == 24) ? SampleKind::I32Hi24 : SampleKind::I32;
  }
  return L;
}

sq_sample_format sqFormatOf(SampleLayout L) {
  switch (L.kind) {
    case SampleKind::F32: return SQ_SAMPLE_F32;
    case SampleKind::F64: return SQ_SAMPLE_F64;
    case SampleKind::I16: return SQ_SAMPLE_I16;
    case SampleKind::I24: return SQ_SAMPLE_I24;
    case SampleKind::I32Hi24: return SQ_SAMPLE_I24_IN32;
    case SampleKind::I32: return SQ_SAMPLE_I32;
    default: return SQ_SAMPLE_UNKNOWN;
  }
}

WAVEFORMATEXTENSIBLE makeFormat(SampleKind kind, uint32_t channels, uint32_t rate, DWORD mask) {
  WAVEFORMATEXTENSIBLE x{};
  WORD bits = 32, valid = 32;
  GUID sub = KSDATAFORMAT_SUBTYPE_PCM;
  switch (kind) {
    case SampleKind::F32: sub = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT; break;
    case SampleKind::I32: break;
    case SampleKind::I32Hi24: valid = 24; break;
    case SampleKind::I24: bits = valid = 24; break;
    case SampleKind::I16: bits = valid = 16; break;
    default: break;
  }
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = static_cast<WORD>(channels);
  x.Format.nSamplesPerSec = rate;
  x.Format.wBitsPerSample = bits;
  x.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
  x.Format.nAvgBytesPerSec = rate * x.Format.nBlockAlign;
  x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  x.Samples.wValidBitsPerSample = valid;
  x.dwChannelMask = mask;
  x.SubFormat = sub;
  return x;
}

DWORD channelMaskOf(const WAVEFORMATEX* f) {
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->dwChannelMask;
  if (f->nChannels == 1) return SPEAKER_FRONT_CENTER;
  if (f->nChannels == 2) return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  return 0;
}

const char* speakerName(DWORD bit) {
  switch (bit) {
    case SPEAKER_FRONT_LEFT: return "Left";
    case SPEAKER_FRONT_RIGHT: return "Right";
    case SPEAKER_FRONT_CENTER: return "Center";
    case SPEAKER_LOW_FREQUENCY: return "LFE";
    case SPEAKER_BACK_LEFT: return "Rear Left";
    case SPEAKER_BACK_RIGHT: return "Rear Right";
    case SPEAKER_FRONT_LEFT_OF_CENTER: return "Front Left Center";
    case SPEAKER_FRONT_RIGHT_OF_CENTER: return "Front Right Center";
    case SPEAKER_BACK_CENTER: return "Rear Center";
    case SPEAKER_SIDE_LEFT: return "Side Left";
    case SPEAKER_SIDE_RIGHT: return "Side Right";
    case SPEAKER_TOP_CENTER: return "Top Center";
    default: return nullptr;
  }
}

constexpr double kStandardRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
constexpr SampleKind kExclusiveCandidates[] = {SampleKind::F32, SampleKind::I32, SampleKind::I32Hi24, SampleKind::I24,
                                               SampleKind::I16};

// Static facts about one endpoint, gathered without starting a stream.
struct EndpointCaps {
  bool ok = false;
  uint32_t channels = 0;
  double mixRate = 0;
  DWORD mask = 0;
  uint32_t defaultPeriodFrames = 0;   // shared engine period
  uint32_t minSharedFrames = 0;       // IAudioClient3 low-latency minimum (or default period)
  uint32_t maxSharedFrames = 0;
  uint32_t fundamentalFrames = 0;     // IAudioClient3 step (0 = only the default period)
  uint32_t minExclusiveFrames = 0;    // 0 = exclusive mode unsupported
  std::vector<double> exclusiveRates;
};

EndpointCaps probeEndpoint(IMMDevice* dev) {
  EndpointCaps c;
  Com<IAudioClient> client;
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()))) return c;
  CoTaskFormat mix;
  if (FAILED(client->GetMixFormat(&mix.p)) || !mix.p) return c;
  c.channels = mix.p->nChannels;
  c.mixRate = mix.p->nSamplesPerSec;
  c.mask = channelMaskOf(mix.p);
  REFERENCE_TIME def = 0, mn = 0;
  if (SUCCEEDED(client->GetDevicePeriod(&def, &mn))) {
    c.defaultPeriodFrames = hnsToFrames(def, c.mixRate);
    c.minExclusiveFrames = hnsToFrames(mn, c.mixRate);
  }
  c.minSharedFrames = c.maxSharedFrames = c.defaultPeriodFrames;
  Com<IAudioClient3> c3;
  if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), c3.putVoid()))) {
    UINT32 d = 0, f = 0, lo = 0, hi = 0;
    if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix.p, &d, &f, &lo, &hi)) && f > 0) {
      c.fundamentalFrames = f;
      c.minSharedFrames = lo;
      c.maxSharedFrames = hi;
    }
  }
  // Exclusive support: any candidate format at any standard rate (or the mix rate).
  bool anyExclusive = false;
  std::vector<double> rates(std::begin(kStandardRates), std::end(kStandardRates));
  if (std::find(rates.begin(), rates.end(), c.mixRate) == rates.end()) rates.push_back(c.mixRate);
  std::sort(rates.begin(), rates.end());
  for (double r : rates) {
    for (SampleKind k : kExclusiveCandidates) {
      WAVEFORMATEXTENSIBLE x = makeFormat(k, c.channels, static_cast<uint32_t>(r), c.mask);
      if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &x.Format, nullptr) == S_OK) {
        c.exclusiveRates.push_back(r);
        anyExclusive = true;
        break;
      }
    }
  }
  if (!anyExclusive) c.minExclusiveFrames = 0;
  c.ok = true;
  return c;
}

std::string friendlyName(IMMDevice* dev) {
  Com<IPropertyStore> props;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, props.put()))) return {};
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string name;
  if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = toUtf8(v.pwszVal);
  PropVariantClear(&v);
  return name;
}

std::wstring endpointId(IMMDevice* dev) {
  CoTaskString s;
  if (FAILED(dev->GetId(&s.p)) || !s.p) return {};
  return s.p;
}

std::wstring defaultEndpointId(IMMDeviceEnumerator* en, EDataFlow flow) {
  Com<IMMDevice> d;
  if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, d.put()))) return {};
  return endpointId(d.get());
}

// ---------------------------------------------------------------------------------------------
// listener hub (shared by the driver, its notification client and its open devices)

class WasapiDevice;

class Hub {
 public:
  ~Hub() {
    for (IAudioDeviceListener* l : listeners_) l->release();
  }
  sq_result addListener(IAudioDeviceListener* l) {
    if (!l) return SQ_E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end()) return SQ_S_FALSE;
    l->addRef();
    listeners_.push_back(l);
    return SQ_OK;
  }
  sq_result removeListener(IAudioDeviceListener* l) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = std::find(listeners_.begin(), listeners_.end(), l);
    if (it == listeners_.end()) return SQ_E_NOTFOUND;
    listeners_.erase(it);
    l->release();
    return SQ_OK;
  }
  void attach(WasapiDevice* d) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    devices_.push_back(d);
  }
  void detach(WasapiDevice* d) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    devices_.erase(std::remove(devices_.begin(), devices_.end(), d), devices_.end());
  }

  void listChanged() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (IAudioDeviceListener* l : listeners_) l->onDeviceListChanged();
  }
  void notifyReset(IAudioDevice* d, int32_t reason) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (IAudioDeviceListener* l : listeners_) l->onDeviceReset(d, reason);
  }
  // Implemented after WasapiDevice.
  void endpointGone(const std::wstring& id);
  void defaultChanged(EDataFlow flow);

 private:
  std::recursive_mutex mu_;
  std::vector<IAudioDeviceListener*> listeners_;
  std::vector<WasapiDevice*> devices_;
};

class NotificationClient final : public IMMNotificationClient {
 public:
  explicit NotificationClient(std::shared_ptr<Hub> hub) : hub_(std::move(hub)) {
    sq::detail::liveObjects().fetch_add(1);  // keep the module mapped while the OS holds us
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
    hub_->listChanged();
    if (id && state != DEVICE_STATE_ACTIVE) hub_->endpointGone(id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
    hub_->listChanged();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
    hub_->listChanged();
    if (id) hub_->endpointGone(id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (role != eConsole) return S_OK;  // fires once per role; react once
    hub_->listChanged();
    hub_->defaultChanged(flow);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  ~NotificationClient() { sq::detail::liveObjects().fetch_sub(1); }
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<Hub> hub_;
};

// ---------------------------------------------------------------------------------------------
// device description (enumeration result)

struct DeviceEntry {
  sq_audio_device_info info{};
  std::wstring renderId;   // empty = no outputs
  std::wstring captureId;  // empty = no inputs
  bool followDefault = false;
};

// ---------------------------------------------------------------------------------------------
// one direction of an initialised stream

struct Stream {
  Com<IMMDevice> device;
  Com<IAudioClient> client;
  Com<IAudioRenderClient> render;
  Com<IAudioCaptureClient> capture;
  HANDLE event = nullptr;  // only for the stream that drives the clock
  SampleLayout layout{};
  uint32_t channels = 0;       // device channels
  uint32_t bytesPerFrame = 0;
  uint32_t bufferFrames = 0;
  uint32_t periodFrames = 0;
  uint32_t streamLatencyFrames = 0;
  bool exclusive = false;
  bool lowLatencyShared = false;
  DWORD mask = 0;

  void release() {
    render.reset();
    capture.reset();
    client.reset();
    if (event) {
      CloseHandle(event);
      event = nullptr;
    }
  }
};

// ---------------------------------------------------------------------------------------------
// the device

class WasapiDevice final : public sq::Object<WasapiDevice, IAudioDevice> {
 public:
  WasapiDevice(std::shared_ptr<Hub> hub, DeviceEntry entry, Com<IMMDevice> render, Com<IMMDevice> capture)
      : hub_(std::move(hub)), entry_(std::move(entry)) {
    out_.device = render;
    in_.device = capture;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (out_.device) outCaps_ = probeEndpoint(out_.device.get());
    if (in_.device) inCaps_ = probeEndpoint(in_.device.get());
    hub_->attach(this);
  }

  ~WasapiDevice() override {
    ComScope com;
    stop();
    releaseStreams();
    hub_->detach(this);
    if (stopEvent_) CloseHandle(stopEvent_);
  }

  bool valid() const { return stopEvent_ && (outCaps_.ok || inCaps_.ok); }
  bool usesEndpoint(const std::wstring& id) const { return id == entry_.renderId || id == entry_.captureId; }
  bool followsDefault(EDataFlow flow) const {
    return entry_.followDefault && ((flow == eRender && out_.device) || (flow == eCapture && in_.device) || flow == eAll);
  }
  // Returns true the first time only (one DEVICE_LOST notification per device).
  bool markLost() {
    lost_.store(true);
    return !lostNotified_.exchange(true);
  }

  // ---- IAudioDevice

  sq_result SQ_CALL getInfo(sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    *out = entry_.info;
    out->struct_size = sizeof(sq_audio_device_info);
    return SQ_OK;
  }

  sq_result SQ_CALL getSupportedSampleRates(double* rates, uint32_t capacity, uint32_t* count) override {
    // Shared mode accepts any standard rate through the OS resampler; the mix rate comes first.
    std::vector<double> v;
    const EndpointCaps& c = primaryCaps();
    if (c.mixRate > 0) v.push_back(c.mixRate);
    for (double r : kStandardRates)
      if (std::find(v.begin(), v.end(), r) == v.end()) v.push_back(r);
    for (double r : c.exclusiveRates)
      if (std::find(v.begin(), v.end(), r) == v.end()) v.push_back(r);
    return fillList(v, rates, capacity, count);
  }

  sq_result SQ_CALL getSupportedBlockSizes(uint32_t* sizes, uint32_t capacity, uint32_t* count) override {
    std::vector<uint32_t> v;
    const EndpointCaps& c = primaryCaps();
    if (c.defaultPeriodFrames) v.push_back(c.defaultPeriodFrames);
    if (c.fundamentalFrames) {
      for (uint32_t f = c.minSharedFrames; f <= c.maxSharedFrames && v.size() < 64; f += c.fundamentalFrames) v.push_back(f);
    }
    if (c.minExclusiveFrames) {
      v.push_back(c.minExclusiveFrames);
      for (uint32_t f = 32; f <= 4096; f *= 2)
        if (f >= c.minExclusiveFrames) v.push_back(f);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return fillList(v, sizes, capacity, count);
  }

  sq_result SQ_CALL getChannelName(sq_bool input, uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override {
    const EndpointCaps& c = input ? inCaps_ : outCaps_;
    if (!c.ok || index >= c.channels) return SQ_E_NOTFOUND;
    std::string name = (input ? "In " : "Out ") + std::to_string(index + 1);
    // Speaker position from the channel mask: the n-th set bit belongs to channel n.
    uint32_t n = 0;
    for (DWORD bit = 1; bit && bit <= SPEAKER_TOP_CENTER; bit <<= 1) {
      if (!(c.mask & bit)) continue;
      if (n++ == index) {
        if (const char* s = speakerName(bit)) name += std::string(" (") + s + ")";
        break;
      }
    }
    const uint32_t need = static_cast<uint32_t>(name.size() + 1);
    if (needed) *needed = need;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, name.c_str(), need);
    return SQ_OK;
  }

  sq_result SQ_CALL configure(const sq_audio_device_config* req, sq_audio_device_config* actual) override {
    if (!req || !actual || req->struct_size < sizeof(sq_audio_device_config) ||
        actual->struct_size < sizeof(sq_audio_device_config))
      return SQ_E_INVALIDARG;
    if (running_.load()) return SQ_E_BUSY;
    ComScope com;
    return configureImpl(*req, actual);
  }

  sq_result SQ_CALL getStreamFormat(sq_stream_format* input, sq_stream_format* output) override {
    if ((input && input->struct_size < sizeof(sq_stream_format)) || (output && output->struct_size < sizeof(sq_stream_format)))
      return SQ_E_INVALIDARG;
    if (!configured_) {
      // Not configured yet: report what a default configure() would grant.
      const EndpointCaps& c = primaryCaps();
      if (input) *input = sq_stream_format{sizeof(sq_stream_format), c.mixRate, inCaps_.ok ? inCaps_.channels : 0, c.defaultPeriodFrames, SQ_SAMPLE_F32};
      if (output) *output = sq_stream_format{sizeof(sq_stream_format), c.mixRate, outCaps_.ok ? outCaps_.channels : 0, c.defaultPeriodFrames, SQ_SAMPLE_F32};
      return SQ_S_FALSE;
    }
    if (input) *input = sq_stream_format{sizeof(sq_stream_format), rate_, static_cast<uint32_t>(openIn_.size()), block_, SQ_SAMPLE_F32};
    if (output) *output = sq_stream_format{sizeof(sq_stream_format), rate_, static_cast<uint32_t>(openOut_.size()), block_, SQ_SAMPLE_F32};
    return SQ_OK;
  }

  sq_result SQ_CALL getLatency(sq_latency_info* out) override {
    if (!out || out->struct_size < sizeof(sq_latency_info)) return SQ_E_INVALIDARG;
    out->input_frames = configured_ ? latencyIn_ : 0;
    out->output_frames = configured_ ? latencyOut_ : 0;
    out->process_frames = 0;
    return configured_ ? SQ_OK : SQ_S_FALSE;
  }

  sq_result SQ_CALL start(IAudioCallback* cb) override {
    if (!cb) return SQ_E_INVALIDARG;
    if (running_.load()) return SQ_E_BUSY;
    if (lost_.load()) return SQ_E_CLOSED;
    ComScope com;
    if (!configured_) {
      // Convenience: all channels at the default rate and block size.
      std::vector<uint32_t> ins, outs;
      for (uint32_t i = 0; outCaps_.ok && i < outCaps_.channels; ++i) outs.push_back(i);
      if (outs.empty())
        for (uint32_t i = 0; inCaps_.ok && i < inCaps_.channels; ++i) ins.push_back(i);
      sq_audio_device_config rq{};
      rq.struct_size = sizeof rq;
      rq.input_channels = ins.data();
      rq.input_channel_count = static_cast<uint32_t>(ins.size());
      rq.output_channels = outs.data();
      rq.output_channel_count = static_cast<uint32_t>(outs.size());
      sq_audio_device_config got{};
      got.struct_size = sizeof got;
      sq_result r = configureImpl(rq, &got);
      if (SQ_FAILED(r)) return r;
    }
    sq_stream_format inF{}, outF{};
    inF.struct_size = outF.struct_size = sizeof(sq_stream_format);
    getStreamFormat(&inF, &outF);
    sq_latency_info lat{sizeof(sq_latency_info), latencyIn_, latencyOut_, 0};
    sq_result r = cb->onStart(&inF, &outF, &lat);
    if (SQ_FAILED(r)) return r;

    cb->addRef();
    callback_ = cb;
    deviceTime_ = 0;
    pendingXrun_ = false;
    captureSeen_ = false;
    fifoRead_ = fifoWrite_ = fifoCount_ = 0;
    ResetEvent(stopEvent_);

    HRESULT hr = S_OK;
    if (out_.client) {
      // Exclusive event mode requires a full buffer before Start(); prefill silence in both modes
      // only for exclusive (shared mode starts empty to keep latency at the minimum).
      if (out_.exclusive) {
        BYTE* data = nullptr;
        hr = out_.render->GetBuffer(out_.bufferFrames, &data);
        if (SUCCEEDED(hr)) hr = out_.render->ReleaseBuffer(out_.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
      }
    }
    if (SUCCEEDED(hr)) {
      running_.store(true);
      try {
        thread_ = std::thread([this] { streamMain(); });
      } catch (...) {
        hr = E_OUTOFMEMORY;
        running_.store(false);
      }
    }
    if (SUCCEEDED(hr) && in_.client) hr = in_.client->Start();
    if (SUCCEEDED(hr) && out_.client) hr = out_.client->Start();
    if (FAILED(hr)) {
      sq::logf(SQ_LOG_ERROR, "start failed: 0x%08lx", static_cast<unsigned long>(hr));
      stopImpl();
      return mapHr(hr);
    }
    return SQ_OK;
  }

  sq_result SQ_CALL stop() override {
    if (thread_.joinable() && std::this_thread::get_id() == thread_.get_id()) return SQ_E_BUSY;
    if (!callback_) return SQ_S_FALSE;
    ComScope com;
    stopImpl();
    return SQ_OK;
  }

  sq_bool SQ_CALL isRunning() override { return (running_.load() && !lost_.load()) ? SQ_TRUE : SQ_FALSE; }
  uint64_t SQ_CALL getXrunCount() override { return xruns_.load(); }

  sq_result SQ_CALL showControlPanel(void* parentWindow) override {
    HINSTANCE h = ShellExecuteW(static_cast<HWND>(parentWindow), L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32 ? SQ_OK : SQ_E_FAIL;
  }

 private:
  const EndpointCaps& primaryCaps() const { return outCaps_.ok ? outCaps_ : inCaps_; }

  template <class T>
  static sq_result fillList(const std::vector<T>& v, T* out, uint32_t capacity, uint32_t* count) {
    if (count) *count = static_cast<uint32_t>(v.size());
    if (!out) return capacity ? SQ_E_INVALIDARG : SQ_OK;
    const size_t n = std::min<size_t>(capacity, v.size());
    std::copy(v.begin(), v.begin() + static_cast<ptrdiff_t>(n), out);
    return n < v.size() ? SQ_E_BUFFER_TOO_SMALL : SQ_OK;
  }

  void releaseStreams() {
    out_.release();
    in_.release();
    configured_ = false;
  }

  // Initialise one direction. `drive` = this stream owns the clock (event-driven).
  sq_result openStream(Stream& s, const EndpointCaps& caps, bool isRender, double rate, uint32_t reqBlock, bool exclusive,
                       bool drive) {
    s.render.reset();
    s.capture.reset();
    s.client.reset();
    s.exclusive = exclusive;
    s.lowLatencyShared = false;
    s.mask = caps.mask;
    HRESULT hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, s.client.putVoid());
    if (FAILED(hr)) return mapHr(hr);
    CoTaskFormat mix;
    hr = s.client->GetMixFormat(&mix.p);
    if (FAILED(hr) || !mix.p) return mapHr(hr);
    const uint32_t ch = mix.p->nChannels;
    const uint32_t irate = static_cast<uint32_t>(rate + 0.5);
    DWORD streamFlags = drive ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;

    if (exclusive) {
      WAVEFORMATEXTENSIBLE fmt{};
      bool found = false;
      for (SampleKind k : kExclusiveCandidates) {
        fmt = makeFormat(k, ch, irate, caps.mask);
        if (s.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
          found = true;
          break;
        }
      }
      if (!found) return SQ_E_UNSUPPORTED;
      REFERENCE_TIME def = 0, mn = 0;
      s.client->GetDevicePeriod(&def, &mn);
      REFERENCE_TIME period = reqBlock ? framesToHns(reqBlock, rate) : def;
      if (period < mn) period = mn;
      // Event mode: buffer == period. Polled (duplex capture side): a few periods of slack.
      REFERENCE_TIME buffer = drive ? period : std::max<REFERENCE_TIME>(period * 4, kHnsPerSec / 50);
      hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, streamFlags, buffer, drive ? period : 0, &fmt.Format, nullptr);
      if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        s.client->GetBufferSize(&aligned);
        period = framesToHns(aligned, rate);
        buffer = drive ? period : std::max<REFERENCE_TIME>(period * 4, kHnsPerSec / 50);
        s.client.reset();
        hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, s.client.putVoid());
        if (SUCCEEDED(hr))
          hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, streamFlags, buffer, drive ? period : 0, &fmt.Format, nullptr);
      }
      if (FAILED(hr)) return mapHr(hr);
      s.layout = layoutOf(&fmt.Format);
      s.bytesPerFrame = fmt.Format.nBlockAlign;
      UINT32 frames = 0;
      s.client->GetBufferSize(&frames);
      s.bufferFrames = frames;
      s.periodFrames = drive ? frames : hnsToFrames(period, rate);
    } else {
      // Shared mode: float32 at the mix channel count; the OS converts rate/format if needed.
      WAVEFORMATEXTENSIBLE fmt = makeFormat(SampleKind::F32, ch, irate, caps.mask);
      const bool nativeRate = irate == mix.p->nSamplesPerSec;
      const bool mixIsF32 = isFloatFormat(mix.p) && mix.p->wBitsPerSample == 32;
      bool done = false;
      Com<IAudioClient3> c3;
      if (drive && nativeRate && mixIsF32 && caps.fundamentalFrames &&
          SUCCEEDED(s.client->QueryInterface(__uuidof(IAudioClient3), c3.putVoid()))) {
        uint32_t want = reqBlock ? reqBlock : caps.defaultPeriodFrames;
        uint32_t f = caps.fundamentalFrames;
        uint32_t p = caps.minSharedFrames + ((want > caps.minSharedFrames ? want - caps.minSharedFrames : 0) + f / 2) / f * f;
        p = std::clamp(p, caps.minSharedFrames, caps.maxSharedFrames);
        hr = c3->InitializeSharedAudioStream(streamFlags, p, mix.p, nullptr);
        if (SUCCEEDED(hr)) {
          done = true;
          s.lowLatencyShared = true;
          s.periodFrames = p;
          s.layout = layoutOf(mix.p);
          s.bytesPerFrame = mix.p->nBlockAlign;
        } else {
          // The client may be half-initialised: start over with a fresh one.
          c3.reset();
          s.client.reset();
          hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, s.client.putVoid());
          if (FAILED(hr)) return mapHr(hr);
        }
      }
      if (!done) {
        DWORD flags = streamFlags;
        if (!nativeRate || !mixIsF32) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        REFERENCE_TIME def = 0, mn = 0;
        s.client->GetDevicePeriod(&def, &mn);
        // Buffer: the engine wants ~2 periods; ask for the requested block if larger.
        REFERENCE_TIME buffer = std::max<REFERENCE_TIME>(def * 2, reqBlock ? framesToHns(reqBlock, rate) * 2 : 0);
        if (!drive) buffer = std::max<REFERENCE_TIME>(buffer, kHnsPerSec / 10);
        hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0, &fmt.Format, nullptr);
        if (FAILED(hr)) return mapHr(hr);
        s.layout = layoutOf(&fmt.Format);
        s.bytesPerFrame = fmt.Format.nBlockAlign;
        s.periodFrames = hnsToFrames(def, rate);
      }
      UINT32 frames = 0;
      s.client->GetBufferSize(&frames);
      s.bufferFrames = frames;
    }
    s.channels = ch;
    REFERENCE_TIME lat = 0;
    s.client->GetStreamLatency(&lat);
    s.streamLatencyFrames = hnsToFrames(lat, rate);
    if (s.layout.kind == SampleKind::Unknown || s.periodFrames == 0) return SQ_E_UNSUPPORTED;

    if (drive) {
      if (!s.event) s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!s.event) return SQ_E_FAIL;
      hr = s.client->SetEventHandle(s.event);
      if (FAILED(hr)) return mapHr(hr);
    }
    if (isRender) hr = s.client->GetService(__uuidof(IAudioRenderClient), s.render.putVoid());
    else hr = s.client->GetService(__uuidof(IAudioCaptureClient), s.capture.putVoid());
    return mapHr(hr);
  }

  sq_result configureImpl(const sq_audio_device_config& req, sq_audio_device_config* actual) {
    releaseStreams();
    if (lost_.load()) {
      // An endpoint that is back can be reconfigured; a missing one fails below on Activate.
      lost_.store(false);
      lostNotified_.store(false);
    }
    const bool wantOut = req.output_channel_count > 0;
    const bool wantIn = req.input_channel_count > 0;
    if (!wantOut && !wantIn) return SQ_E_INVALIDARG;
    if ((wantOut && (!out_.device || !outCaps_.ok)) || (wantIn && (!in_.device || !inCaps_.ok))) return SQ_E_INVALIDARG;
    if ((wantOut && !req.output_channels) || (wantIn && !req.input_channels)) return SQ_E_INVALIDARG;

    bool differs = false;
    std::vector<uint32_t> outs, ins;
    for (uint32_t i = 0; i < req.output_channel_count; ++i) {
      uint32_t c = req.output_channels[i];
      if (c < outCaps_.channels && std::find(outs.begin(), outs.end(), c) == outs.end()) outs.push_back(c);
      else differs = true;
    }
    for (uint32_t i = 0; i < req.input_channel_count; ++i) {
      uint32_t c = req.input_channels[i];
      if (c < inCaps_.channels && std::find(ins.begin(), ins.end(), c) == ins.end()) ins.push_back(c);
      else differs = true;
    }
    if (outs.empty() && ins.empty()) return SQ_E_INVALIDARG;

    const EndpointCaps& caps = outs.empty() ? inCaps_ : outCaps_;
    const double rate = req.sample_rate > 0 ? req.sample_rate : caps.mixRate;
    const bool exclusive = (req.flags & SQ_AUDIO_CONFIG_EXCLUSIVE) != 0;

    sq_result r = SQ_OK;
    if (!outs.empty()) {
      r = openStream(out_, outCaps_, true, rate, req.block_frames, exclusive, true);
      if (SQ_FAILED(r)) {
        releaseStreams();
        return r;
      }
    }
    if (!ins.empty()) {
      const uint32_t blk = outs.empty() ? req.block_frames : out_.periodFrames;
      r = openStream(in_, inCaps_, false, rate, blk, exclusive, outs.empty());
      if (SQ_FAILED(r)) {
        releaseStreams();
        return r;
      }
    }

    rate_ = rate;
    block_ = outs.empty() ? in_.periodFrames : out_.periodFrames;
    openOut_ = std::move(outs);
    openIn_ = std::move(ins);

    // Latency: frames queued in the endpoint buffer + the stream latency the OS reports. Input
    // additionally waits up to one block in our FIFO before process() sees it.
    latencyOut_ = 0;
    latencyIn_ = 0;
    if (out_.client) latencyOut_ = out_.bufferFrames + out_.streamLatencyFrames;
    if (in_.client) latencyIn_ = in_.periodFrames + in_.streamLatencyFrames + (out_.client ? block_ : 0);

    // Pre-allocate everything the stream thread touches.
    try {
      outBufs_.assign(openOut_.size(), std::vector<float>(block_, 0.0f));
      inBufs_.assign(openIn_.size(), std::vector<float>(block_, 0.0f));
      outPtrs_.clear();
      inPtrs_.clear();
      for (auto& b : outBufs_) outPtrs_.push_back(b.data());
      for (auto& b : inBufs_) inPtrs_.push_back(b.data());
      fifoCap_ = 0;
      fifo_.clear();
      if (in_.client) {
        fifoCap_ = std::max(in_.bufferFrames, block_) * 4 + block_;
        fifo_.assign(openIn_.size(), std::vector<float>(fifoCap_, 0.0f));
      }
    } catch (...) {
      releaseStreams();
      return SQ_E_OUTOFMEMORY;
    }

    configured_ = true;
    *actual = req;
    actual->struct_size = sizeof(sq_audio_device_config);
    actual->sample_rate = rate_;
    actual->block_frames = block_;
    actual->input_channels = openIn_.empty() ? nullptr : openIn_.data();
    actual->input_channel_count = static_cast<uint32_t>(openIn_.size());
    actual->output_channels = openOut_.empty() ? nullptr : openOut_.data();
    actual->output_channel_count = static_cast<uint32_t>(openOut_.size());
    actual->device_format = sqFormatOf(out_.client ? out_.layout : in_.layout);
    if (req.sample_rate > 0 && req.sample_rate != rate_) differs = true;
    if (req.block_frames && req.block_frames != block_) differs = true;
    sq::logf(SQ_LOG_INFO, "configured %s: %.0f Hz, %u frames, %s %s, out %u ch, in %u ch, latency in/out %u/%u",
             entry_.info.name, rate_, block_, exclusive ? "exclusive" : (out_.lowLatencyShared ? "shared-lowlat" : "shared"),
             audiodrv::kindName((out_.client ? out_.layout : in_.layout).kind), static_cast<unsigned>(openOut_.size()),
             static_cast<unsigned>(openIn_.size()), latencyIn_, latencyOut_);
    return differs ? SQ_S_FALSE : SQ_OK;
  }

  void stopImpl() {
    if (thread_.joinable()) {
      SetEvent(stopEvent_);
      thread_.join();
    }
    running_.store(false);
    if (out_.client) {
      out_.client->Stop();
      out_.client->Reset();
    }
    if (in_.client) {
      in_.client->Stop();
      in_.client->Reset();
    }
    if (callback_) {
      callback_->onStop();
      callback_->release();
      callback_ = nullptr;
    }
  }

  // ---- stream thread (everything below runs on it; RT rules apply inside the loop)

  void fifoPush(const BYTE* data, uint32_t frames, bool silent) {
    if (fifoCap_ == 0) return;
    if (frames > fifoCap_) {
      data += size_t(frames - fifoCap_) * in_.bytesPerFrame;
      frames = fifoCap_;
    }
    if (fifoCount_ + frames > fifoCap_) {  // consumer fell behind (clock drift): drop oldest
      const uint32_t drop = fifoCount_ + frames - fifoCap_;
      fifoRead_ = (fifoRead_ + drop) % fifoCap_;
      fifoCount_ -= drop;
      xruns_.fetch_add(1, std::memory_order_relaxed);
      pendingXrun_ = true;
    }
    const uint32_t bps = audiodrv::bytesPerSample(in_.layout.kind);
    const uint32_t first = std::min(frames, fifoCap_ - fifoWrite_);
    for (size_t k = 0; k < openIn_.size(); ++k) {
      float* ring = fifo_[k].data();
      if (silent || !data) {
        std::memset(ring + fifoWrite_, 0, first * sizeof(float));
        if (frames > first) std::memset(ring, 0, (frames - first) * sizeof(float));
      } else {
        const BYTE* src = data + size_t(openIn_[k]) * bps;
        audiodrv::toFloat(in_.layout, src, in_.bytesPerFrame, ring + fifoWrite_, first);
        if (frames > first)
          audiodrv::toFloat(in_.layout, src + size_t(first) * in_.bytesPerFrame, in_.bytesPerFrame, ring, frames - first);
      }
    }
    fifoWrite_ = (fifoWrite_ + frames) % fifoCap_;
    fifoCount_ += frames;
  }

  void fifoPop(uint32_t frames) {
    const uint32_t first = std::min(frames, fifoCap_ - fifoRead_);
    for (size_t k = 0; k < openIn_.size(); ++k) {
      std::memcpy(inBufs_[k].data(), fifo_[k].data() + fifoRead_, first * sizeof(float));
      if (frames > first) std::memcpy(inBufs_[k].data() + first, fifo_[k].data(), (frames - first) * sizeof(float));
    }
    fifoRead_ = (fifoRead_ + frames) % fifoCap_;
    fifoCount_ -= frames;
  }

  HRESULT drainCapture() {
    for (;;) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      HRESULT hr = in_.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;
      if (FAILED(hr)) return hr;
      if (frames) {
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && captureSeen_) {
          xruns_.fetch_add(1, std::memory_order_relaxed);
          pendingXrun_ = true;
        }
        fifoPush(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        captureSeen_ = true;
      }
      hr = in_.capture->ReleaseBuffer(frames);
      if (FAILED(hr)) return hr;
      if (!in_.exclusive && frames == 0) return S_OK;
    }
  }

  void runProcess(BYTE* renderData) {
    const uint32_t n = block_;
    sq_audio_block inBlock{inPtrs_.data(), static_cast<uint32_t>(inPtrs_.size()), n, deviceTime_};
    sq_audio_block outBlock{outPtrs_.data(), static_cast<uint32_t>(outPtrs_.size()), n, deviceTime_};
    if (!openIn_.empty()) {
      if (fifoCount_ >= n) {
        fifoPop(n);
      } else {
        for (auto& b : inBufs_) std::memset(b.data(), 0, n * sizeof(float));
        if (captureSeen_) {
          xruns_.fetch_add(1, std::memory_order_relaxed);
          pendingXrun_ = true;
        }
      }
    }
    for (auto& b : outBufs_) std::memset(b.data(), 0, n * sizeof(float));
    sq_audio_io io{};
    io.struct_size = sizeof io;
    io.input = openIn_.empty() ? nullptr : &inBlock;
    io.output = &outBlock;
    io.device_time = deviceTime_;
    io.host_time_ns = sq_time_monotonic_ns();
    io.flags = pendingXrun_ ? SQ_AUDIO_IO_XRUN : 0u;
    pendingXrun_ = false;
    callback_->process(&io);
    if (renderData) {
      if (openOut_.size() < out_.channels) std::memset(renderData, 0, size_t(n) * out_.bytesPerFrame);
      const uint32_t bps = audiodrv::bytesPerSample(out_.layout.kind);
      for (size_t k = 0; k < openOut_.size(); ++k)
        audiodrv::fromFloat(out_.layout, outBufs_[k].data(), renderData + size_t(openOut_[k]) * bps, out_.bytesPerFrame, n);
    }
    deviceTime_ += n;
  }

  void streamMain() {
    ComScope com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    else sq_thread_set_current_priority(SQ_PRIORITY_REALTIME);

    const bool render = static_cast<bool>(out_.client);
    HANDLE waits[2] = {stopEvent_, render ? out_.event : in_.event};
    // Generous timeout: a missing wake-up for 4 periods (min 200 ms) is an xrun, not an error.
    const DWORD timeoutMs = std::max<DWORD>(200, static_cast<DWORD>(4000.0 * block_ / rate_));
    HRESULT hr = S_OK;
    bool first = true;
    while (running_.load(std::memory_order_relaxed)) {
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
      if (w == WAIT_OBJECT_0) break;
      if (w == WAIT_TIMEOUT) {
        xruns_.fetch_add(1, std::memory_order_relaxed);
        pendingXrun_ = true;
        continue;
      }
      if (w != WAIT_OBJECT_0 + 1) {
        hr = E_FAIL;
        break;
      }
      if (in_.capture) {
        hr = drainCapture();
        if (FAILED(hr)) break;
      }
      if (render) {
        UINT32 avail = out_.bufferFrames;
        if (!out_.exclusive) {
          UINT32 padding = 0;
          hr = out_.client->GetCurrentPadding(&padding);
          if (FAILED(hr)) break;
          if (padding == 0 && !first) {  // the endpoint ran dry
            xruns_.fetch_add(1, std::memory_order_relaxed);
            pendingXrun_ = true;
          }
          avail = out_.bufferFrames - padding;
        }
        while (avail >= block_) {
          BYTE* data = nullptr;
          hr = out_.render->GetBuffer(block_, &data);
          if (FAILED(hr)) break;
          runProcess(data);
          hr = out_.render->ReleaseBuffer(block_, 0);
          if (FAILED(hr)) break;
          avail -= block_;
          if (out_.exclusive) break;  // exclusive event mode: exactly one buffer per event
        }
        if (FAILED(hr)) break;
      } else {
        while (fifoCount_ >= block_) runProcess(nullptr);
      }
      first = false;
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    if (FAILED(hr)) {
      // Leaves the RT loop for good: report once, on this thread (not inside process()).
      int32_t reason = SQ_AUDIO_RESET_FORMAT_CHANGED;
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        DWORD state = 0;
        Stream& s = render ? out_ : in_;
        if (!s.device || FAILED(s.device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) reason = SQ_AUDIO_RESET_DEVICE_LOST;
      } else {
        reason = SQ_AUDIO_RESET_DEVICE_LOST;
      }
      sq::logf(SQ_LOG_WARN, "stream of %s stopped: 0x%08lx", entry_.info.name, static_cast<unsigned long>(hr));
      if (markLost()) hub_->notifyReset(this, reason);
    }
  }

  std::shared_ptr<Hub> hub_;
  DeviceEntry entry_;
  EndpointCaps outCaps_, inCaps_;
  Stream out_, in_;
  HANDLE stopEvent_ = nullptr;

  bool configured_ = false;
  double rate_ = 0;
  uint32_t block_ = 0;
  uint32_t latencyIn_ = 0, latencyOut_ = 0;
  std::vector<uint32_t> openOut_, openIn_;

  std::vector<std::vector<float>> outBufs_, inBufs_, fifo_;
  std::vector<float*> outPtrs_, inPtrs_;
  uint32_t fifoCap_ = 0, fifoRead_ = 0, fifoWrite_ = 0, fifoCount_ = 0;
  bool pendingXrun_ = false;
  bool captureSeen_ = false;
  sq_sample_time deviceTime_ = 0;

  IAudioCallback* callback_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> lost_{false};
  std::atomic<bool> lostNotified_{false};
  std::atomic<uint64_t> xruns_{0};

  friend class Hub;
};

void Hub::endpointGone(const std::wstring& id) {
  std::lock_guard<std::recursive_mutex> g(mu_);
  for (WasapiDevice* d : devices_)
    if (d->usesEndpoint(id) && d->markLost())
      for (IAudioDeviceListener* l : listeners_) l->onDeviceReset(d, SQ_AUDIO_RESET_DEVICE_LOST);
}

void Hub::defaultChanged(EDataFlow flow) {
  std::lock_guard<std::recursive_mutex> g(mu_);
  for (WasapiDevice* d : devices_)
    if (d->followsDefault(flow))
      for (IAudioDeviceListener* l : listeners_) l->onDeviceReset(d, SQ_AUDIO_RESET_REQUESTED);
}

// ---------------------------------------------------------------------------------------------
// the driver

constexpr const char kPrefixOut[] = "wasapi:out:";
constexpr const char kPrefixIn[] = "wasapi:in:";
constexpr const char kPrefixDuplex[] = "wasapi:duplex:";
constexpr const char kDefaultId[] = "wasapi:default";

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

class WasapiDriver final : public sq::Object<WasapiDriver, IAudioDriver> {
 public:
  WasapiDriver() : hub_(std::make_shared<Hub>()) {
    ComScope com;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                enumerator_.putVoid()))) {
      enumerator_.reset();
      return;
    }
    notify_ = new (std::nothrow) NotificationClient(hub_);
    if (notify_ && FAILED(enumerator_->RegisterEndpointNotificationCallback(notify_))) {
      notify_->Release();
      notify_ = nullptr;
    }
  }

  ~WasapiDriver() override {
    ComScope com;
    if (notify_) {
      if (enumerator_) enumerator_->UnregisterEndpointNotificationCallback(notify_);
      notify_->Release();
    }
    enumerator_.reset();
  }

  sq_result SQ_CALL getDriverInfo(sq_audio_driver_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_driver_info)) return SQ_E_INVALIDARG;
    std::memset(out->name, 0, sizeof out->name);
    copyStr(out->name, sizeof out->name, "WASAPI");
    out->flags = 0;
    return SQ_OK;
  }

  sq_result SQ_CALL refreshDevices() override {
    if (!enumerator_) return SQ_E_UNSUPPORTED;
    ComScope com;
    std::vector<DeviceEntry> list;
    const std::wstring defOut = defaultEndpointId(enumerator_.get(), eRender);
    const std::wstring defIn = defaultEndpointId(enumerator_.get(), eCapture);
    const EndpointCaps* defOutCaps = nullptr;
    const EndpointCaps* defInCaps = nullptr;
    std::vector<EndpointCaps> capsStore;
    capsStore.reserve(64);
    std::vector<std::wstring> capsIds;

    for (EDataFlow flow : {eRender, eCapture}) {
      Com<IMMDeviceCollection> coll;
      if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.put()))) continue;
      UINT n = 0;
      coll->GetCount(&n);
      for (UINT i = 0; i < n && capsStore.size() < 64; ++i) {
        Com<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.put()))) continue;
        std::wstring id = endpointId(dev.get());
        if (id.empty()) continue;
        EndpointCaps caps = probeEndpoint(dev.get());
        if (!caps.ok) continue;
        DeviceEntry e;
        e.info.struct_size = sizeof(sq_audio_device_info);
        const bool isOut = flow == eRender;
        copyStr(e.info.id, sizeof e.info.id, std::string(isOut ? kPrefixOut : kPrefixIn) + toUtf8(id.c_str()));
        std::string name = friendlyName(dev.get());
        copyStr(e.info.name, sizeof e.info.name, name.empty() ? "Audio endpoint" : name);
        fillCaps(e.info, caps, isOut, true);
        if (isOut) {
          e.renderId = id;
          if (id == defOut) e.info.flags |= SQ_AUDIO_DEVICE_DEFAULT_OUTPUT;
        } else {
          e.captureId = id;
          if (id == defIn) e.info.flags |= SQ_AUDIO_DEVICE_DEFAULT_INPUT;
        }
        capsStore.push_back(caps);
        capsIds.push_back(id);
        if (id == defOut && isOut) defOutCaps = &capsStore.back();
        if (id == defIn && !isOut) defInCaps = &capsStore.back();
        list.push_back(std::move(e));
      }
    }

    if (defOutCaps || defInCaps) {
      DeviceEntry d;
      d.followDefault = true;
      d.info.struct_size = sizeof(sq_audio_device_info);
      copyStr(d.info.id, sizeof d.info.id, kDefaultId);
      copyStr(d.info.name, sizeof d.info.name, "System default (WASAPI)");
      if (defOutCaps) {
        fillCaps(d.info, *defOutCaps, true, true);
        d.renderId = defOut;
        d.info.flags |= SQ_AUDIO_DEVICE_DEFAULT_OUTPUT;
      }
      if (defInCaps) {
        fillCaps(d.info, *defInCaps, false, !defOutCaps);  // timing follows the render side
        d.captureId = defIn;
        d.info.flags |= SQ_AUDIO_DEVICE_DEFAULT_INPUT;
      }
      list.insert(list.begin(), std::move(d));
    }

    std::lock_guard<std::mutex> g(mu_);
    devices_ = std::move(list);
    return SQ_OK;
  }

  uint32_t SQ_CALL getDeviceCount() override {
    std::lock_guard<std::mutex> g(mu_);
    return static_cast<uint32_t>(devices_.size());
  }

  sq_result SQ_CALL getDeviceInfo(uint32_t index, sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> g(mu_);
    if (index >= devices_.size()) return SQ_E_NOTFOUND;
    *out = devices_[index].info;
    return SQ_OK;
  }

  sq_result SQ_CALL openDevice(const char* deviceId, IAudioDevice** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!deviceId || !*deviceId) return SQ_E_INVALIDARG;
    if (!enumerator_) return SQ_E_UNSUPPORTED;
    ComScope com;
    const std::string id = deviceId;
    DeviceEntry e;
    {
      std::lock_guard<std::mutex> g(mu_);
      for (const DeviceEntry& d : devices_)
        if (id == d.info.id) e = d;
    }
    if (!e.info.struct_size) {
      // Not in the last enumeration (or never enumerated): resolve the id directly.
      e.info.struct_size = sizeof(sq_audio_device_info);
      copyStr(e.info.id, sizeof e.info.id, id);
      if (id == kDefaultId) {
        e.followDefault = true;
        e.renderId = defaultEndpointId(enumerator_.get(), eRender);
        e.captureId = defaultEndpointId(enumerator_.get(), eCapture);
        copyStr(e.info.name, sizeof e.info.name, "System default (WASAPI)");
      } else if (startsWith(id, kPrefixOut)) {
        e.renderId = toWide(id.substr(sizeof kPrefixOut - 1));
      } else if (startsWith(id, kPrefixIn)) {
        e.captureId = toWide(id.substr(sizeof kPrefixIn - 1));
      } else if (startsWith(id, kPrefixDuplex)) {
        std::string rest = id.substr(sizeof kPrefixDuplex - 1);
        size_t bar = rest.find('|');
        if (bar == std::string::npos) return SQ_E_INVALIDARG;
        e.renderId = toWide(rest.substr(0, bar));
        e.captureId = toWide(rest.substr(bar + 1));
      } else {
        return SQ_E_NOTFOUND;
      }
    }
    Com<IMMDevice> render, capture;
    if (!e.renderId.empty() && FAILED(enumerator_->GetDevice(e.renderId.c_str(), render.put()))) return SQ_E_NOTFOUND;
    if (!e.captureId.empty() && FAILED(enumerator_->GetDevice(e.captureId.c_str(), capture.put()))) {
      if (!e.followDefault) return SQ_E_NOTFOUND;
      e.captureId.clear();
    }
    if (!render && !capture) return SQ_E_NOTFOUND;
    if (!e.info.name[0]) copyStr(e.info.name, sizeof e.info.name, friendlyName(render ? render.get() : capture.get()));
    if (!e.info.default_sample_rate) {
      if (render) fillCaps(e.info, probeEndpoint(render.get()), true, true);
      if (capture) fillCaps(e.info, probeEndpoint(capture.get()), false, !render);
    }
    WasapiDevice* dev = new (std::nothrow) WasapiDevice(hub_, std::move(e), render, capture);
    if (!dev) return SQ_E_OUTOFMEMORY;
    if (!dev->valid()) {
      dev->release();
      return SQ_E_FAIL;
    }
    *out = dev;
    return SQ_OK;
  }

  sq_result SQ_CALL addListener(IAudioDeviceListener* l) override { return hub_->addListener(l); }
  sq_result SQ_CALL removeListener(IAudioDeviceListener* l) override { return hub_->removeListener(l); }

 private:
  // Merge one endpoint's caps into `info`; `timing` = also take rate/block sizes from it.
  static void fillCaps(sq_audio_device_info& info, const EndpointCaps& c, bool isOut, bool timing) {
    if (!c.ok) return;
    if (isOut) {
      info.max_output_channels = c.channels;
      info.flags |= SQ_AUDIO_DEVICE_OUTPUT;
    } else {
      info.max_input_channels = c.channels;
      info.flags |= SQ_AUDIO_DEVICE_INPUT;
    }
    info.flags |= SQ_AUDIO_DEVICE_CONTROL_PANEL;
    if (!timing) return;
    if (c.minExclusiveFrames) info.flags |= SQ_AUDIO_DEVICE_EXCLUSIVE;
    info.default_sample_rate = c.mixRate;
    info.default_block_frames = c.defaultPeriodFrames;
    uint32_t mn = c.minSharedFrames ? c.minSharedFrames : c.defaultPeriodFrames;
    if (c.minExclusiveFrames) mn = std::min(mn, c.minExclusiveFrames);
    info.min_block_frames = mn;
    info.max_block_frames = c.minExclusiveFrames ? std::max<uint32_t>(4096, c.maxSharedFrames) : c.maxSharedFrames;
    info.block_granularity = 0;
  }

  // Keeps COM initialised on the owning (main) thread for the driver's lifetime: a balanced
  // CoInitialize/CoUninitialize pair inside the constructor would tear the apartment down again
  // when nobody else initialised it, leaving enumerator_ dangling.
  ComScope com_;
  std::shared_ptr<Hub> hub_;
  Com<IMMDeviceEnumerator> enumerator_;
  NotificationClient* notify_ = nullptr;
  std::mutex mu_;
  std::vector<DeviceEntry> devices_;
};

}  // namespace

sq_result createDriver(const sq_guid* riid, void** out) { return sq::createObject<WasapiDriver>(riid, out); }

}  // namespace sq::wasapi

#else  // !_WIN32 --------------------------------------------------------------------------------

#include <cstring>

#include "sq/sdk.hpp"

namespace sq::wasapi {
namespace {
// WASAPI exists only on Windows: elsewhere the module loads and reports no devices.
class NullDriver final : public sq::Object<NullDriver, IAudioDriver> {
 public:
  sq_result SQ_CALL getDriverInfo(sq_audio_driver_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_driver_info)) return SQ_E_INVALIDARG;
    std::memset(out->name, 0, sizeof out->name);
    std::strcpy(out->name, "WASAPI");
    out->flags = 0;
    return SQ_OK;
  }
  sq_result SQ_CALL refreshDevices() override { return SQ_OK; }
  uint32_t SQ_CALL getDeviceCount() override { return 0; }
  sq_result SQ_CALL getDeviceInfo(uint32_t, sq_audio_device_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL openDevice(const char*, IAudioDevice** out) override {
    if (out) *out = nullptr;
    return SQ_E_NOTFOUND;
  }
  sq_result SQ_CALL addListener(IAudioDeviceListener* l) override { return l ? SQ_OK : SQ_E_INVALIDARG; }
  sq_result SQ_CALL removeListener(IAudioDeviceListener*) override { return SQ_OK; }
};
}  // namespace
sq_result createDriver(const sq_guid* riid, void** out) { return sq::createObject<NullDriver>(riid, out); }
}  // namespace sq::wasapi

#endif
