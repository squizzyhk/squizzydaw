// Squizzy Studio - ASIO driver service: device + driver objects (see asio_driver.hpp).
//
// Device model
//   One device per installed driver, id "asio:{CLSID}". Opening a device loads the driver on the
//   calling (control) thread; all IAudioDevice calls must come from that thread (ASIO drivers are
//   apartment-bound). Only one device may be open per process.
//
// Streaming
//   configure() creates the driver's double buffers for the requested channels and pre-allocates
//   planar float scratch. The driver's buffer-switch callback (realtime thread) converts the input
//   half to float, calls IAudioCallback::process, converts the output back into the same half and
//   signals outputReady() when the driver supports it. Before start() and after stop() the
//   callback writes silence. stop() waits until no callback is inside process().
//
// Notifications
//   Driver messages are routed by routeMessage(): reset request and resync -> onDeviceReset
//   (REQUESTED; resync also counts a dropout), buffer-size change, latency change and a sample
//   rate change -> onDeviceReset(FORMAT_CHANGED). Callbacks only set bits in an atomic mask; a
//   per-device notifier thread delivers them to the listeners (never from the realtime thread).
//   The notifier's state is shared, not owned by the device: a listener may drop the last device
//   reference inside onDeviceReset; delivery then stops and the dead device is never handed out.
//
// Driver-reported buffer sizes are sanitised (sanitizeSizes): blocks above kMaxBlockFrames are
// never requested, so a driver reporting absurd sizes cannot make the host allocate gigabytes.
#include "asio_driver.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "asio_convert.hpp"
#include "sq/audiodriver-asio/asio_status.h"
#include "sq/platform.h"
#include "sq/sdk.hpp"

namespace sq::asio {

// ---------------------------------------------------------------------------------------------
// pure helpers

MessageEffects routeMessage(int32_t selector, int32_t value) noexcept {
  MessageEffects e;
  switch (selector) {
    case msg::kSelectorSupported:
      switch (value) {
        case msg::kEngineVersion:
        case msg::kResetRequest:
        case msg::kBufferSizeChange:
        case msg::kResyncRequest:
        case msg::kLatenciesChanged:
        case msg::kSupportsTimeInfo:
        case msg::kOverload:
          e.result = 1;
          break;
        default:
          e.result = 0;
      }
      break;
    case msg::kEngineVersion:
      e.result = kHostEngineVersion;
      break;
    case msg::kResetRequest:
      e.result = 1;
      e.resetReason = SQ_AUDIO_RESET_REQUESTED;
      break;
    case msg::kResyncRequest:
      e.result = 1;
      e.resetReason = SQ_AUDIO_RESET_REQUESTED;
      e.xrun = true;
      break;
    case msg::kBufferSizeChange:
    case msg::kLatenciesChanged:
      e.result = 1;
      e.resetReason = SQ_AUDIO_RESET_FORMAT_CHANGED;
      break;
    case msg::kSupportsTimeInfo:
      e.result = 1;
      break;
    case msg::kOverload:
      e.result = 1;
      e.xrun = true;
      break;
    default:
      e.result = 0;  // time code and everything else: not supported
  }
  return e;
}

namespace {

bool validSizes(const BufferSizes& s) { return s.minFrames > 0 && s.maxFrames >= s.minFrames; }

// Largest block the host will ever request (frames per channel and buffer half).
constexpr int32_t kMaxBlockFrames = 65536;

// Clamps a driver's reported sizes into what the host accepts (see kMaxBlockFrames).
BufferSizes sanitizeSizes(BufferSizes s) noexcept {
  if (s.maxFrames > kMaxBlockFrames) s.maxFrames = kMaxBlockFrames;
  if (s.preferredFrames > kMaxBlockFrames) s.preferredFrames = validSizes(s) ? s.maxFrames : 0;
  if (s.preferredFrames < 0) s.preferredFrames = 0;
  return s;
}

uint32_t fallbackPreferred(const BufferSizes& s) {
  if (s.preferredFrames >= s.minFrames && s.preferredFrames <= s.maxFrames && s.preferredFrames > 0)
    return static_cast<uint32_t>(s.preferredFrames);
  return static_cast<uint32_t>(s.minFrames);
}

}  // namespace

uint32_t chooseBlockSize(const BufferSizes& s, uint32_t requested) noexcept {
  if (!validSizes(s)) return s.preferredFrames > 0 ? static_cast<uint32_t>(s.preferredFrames) : 0;
  const uint32_t pref = fallbackPreferred(s);
  const uint32_t lo = static_cast<uint32_t>(s.minFrames), hi = static_cast<uint32_t>(s.maxFrames);
  if (requested == 0) return pref;
  if (lo == hi) return lo;
  const uint32_t r = std::clamp(requested, lo, hi);
  if (s.granularity == 0) return r;  // no step reported: any size in [min, max]
  if (s.granularity < 0) {
    // powers of two within [lo, hi]; nearest, ties to the larger
    uint32_t best = 0;
    uint64_t bestDist = UINT64_MAX;
    for (uint64_t p = 1; p <= hi; p <<= 1) {
      if (p < lo) continue;
      const uint64_t d = p > r ? p - r : r - p;
      if (d <= bestDist) {
        best = static_cast<uint32_t>(p);
        bestDist = d;
      }
    }
    return best ? best : pref;
  }
  const uint32_t g = static_cast<uint32_t>(s.granularity);
  const uint32_t steps = (r - lo + g / 2) / g;
  uint64_t v = static_cast<uint64_t>(lo) + static_cast<uint64_t>(steps) * g;
  while (v > hi && v >= static_cast<uint64_t>(lo) + g) v -= g;
  return static_cast<uint32_t>(v);
}

std::vector<uint32_t> supportedBlockSizes(const BufferSizes& s) {
  std::vector<uint32_t> out;
  if (!validSizes(s)) {
    if (s.preferredFrames > 0) out.push_back(static_cast<uint32_t>(s.preferredFrames));
    return out;
  }
  const uint32_t lo = static_cast<uint32_t>(s.minFrames), hi = static_cast<uint32_t>(s.maxFrames);
  if (s.granularity == 0 || lo == hi) {
    out = {lo, fallbackPreferred(s), hi};
  } else if (s.granularity < 0) {
    for (uint64_t p = 1; p <= hi; p <<= 1)
      if (p >= lo) out.push_back(static_cast<uint32_t>(p));
    if (out.empty()) out.push_back(fallbackPreferred(s));
  } else {
    const uint32_t g = static_cast<uint32_t>(s.granularity);
    if ((hi - lo) / g <= 64) {
      for (uint64_t v = lo; v <= hi; v += g) out.push_back(static_cast<uint32_t>(v));
    } else {
      // too many steps to list: the common sizes the driver accepts, plus min/preferred/max
      static constexpr uint32_t kCommon[] = {16, 24, 32, 48, 64, 96, 128, 160, 192, 256, 320, 384, 448, 480,
                                             512, 640, 768, 960, 1024, 1536, 2048, 3072, 4096, 8192};
      out.push_back(lo);
      out.push_back(fallbackPreferred(s));
      out.push_back(hi);
      for (uint32_t c : kCommon)
        if (c >= lo && c <= hi && (c - lo) % g == 0) out.push_back(c);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

const std::vector<double>& candidateSampleRates() {
  static const std::vector<double> rates = {8000,  11025, 16000,  22050,  32000,  44100,  48000,
                                            88200, 96000, 176400, 192000, 352800, 384000};
  return rates;
}

namespace {

std::atomic<uint32_t> g_openDevices{0};

void copyStr(char* dst, size_t cap, const std::string& s) {
  if (!cap) return;
  size_t n = std::min(cap - 1, s.size());
  while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;  // keep UTF-8 whole
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

sq_result copyOut(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed) {
  const uint32_t need = static_cast<uint32_t>(s.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, s.c_str(), need);
  return SQ_OK;
}

// Shared between the driver and its open device: listeners + info captured from opened drivers.
struct Hub {
  std::mutex m;
  std::vector<sq::Ptr<IAudioDeviceListener>> listeners;
  std::map<std::string, sq_audio_device_info> probed;

  std::vector<sq::Ptr<IAudioDeviceListener>> snapshot() {
    std::lock_guard<std::mutex> lk(m);
    return listeners;
  }
  void notifyReset(IAudioDevice* dev, int32_t reason) {
    for (auto& l : snapshot()) l->onDeviceReset(dev, reason);
  }
  void notifyListChanged() {
    for (auto& l : snapshot()) l->onDeviceListChanged();
  }
  void storeProbe(const sq_audio_device_info& info) {
    std::lock_guard<std::mutex> lk(m);
    probed[info.id] = info;
  }
  bool findProbe(const std::string& id, sq_audio_device_info& out) {
    std::lock_guard<std::mutex> lk(m);
    auto it = probed.find(id);
    if (it == probed.end()) return false;
    out = it->second;
    return true;
  }
};

uint32_t reasonBit(int32_t reason) { return (reason > 0 && reason < 32) ? (1u << reason) : 0u; }

// ---------------------------------------------------------------------------------------------
// device

class AsioDevice final : public sq::Object<AsioDevice, IAudioDevice>, private BackendSink {
 public:
  AsioDevice(std::unique_ptr<Backend> backend, DriverEntry entry, std::shared_ptr<Hub> hub)
      : be_(std::move(backend)), entry_(std::move(entry)), hub_(std::move(hub)), nt_(std::make_shared<Notifier>()) {
    double r = 0;
    if (SQ_SUCCEEDED(be_->getSampleRate(r)) && r > 0) rate_.store(r);
    sq_audio_device_info info{};
    info.struct_size = sizeof info;
    if (SQ_SUCCEEDED(getInfo(&info))) hub_->storeProbe(info);
    // The thread holds copies of the shared state, never `this` members (see notifierLoop).
    notifier_ = std::thread(
        [nt = nt_, hub = hub_, self = static_cast<IAudioDevice*>(this)] { notifierLoop(nt, hub, self); });
    g_openDevices.fetch_add(1);  // last: a throwing constructor must not leak the single-open slot
  }

  ~AsioDevice() override {
    // Stop notifications first: no listener is handed this device once its teardown has begun.
    {
      std::lock_guard<std::mutex> lk(nt_->m);
      nt_->stop = true;
    }
    nt_->cv.notify_all();
    if (notifier_.joinable()) {
      if (notifier_.get_id() == std::this_thread::get_id())
        notifier_.detach();  // last reference dropped inside a listener: the loop sees `stop`, returns
      else
        notifier_.join();
    }
    if (running_.load()) stop();
    if (buffersCreated_) be_->disposeBuffers();
    be_.reset();  // unloads the driver
    g_openDevices.fetch_sub(1);
  }

  // ---- IAudioDevice ------------------------------------------------------------------------

  sq_result SQ_CALL getInfo(sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    try {
      return getInfoImpl(out);
    } catch (...) {
      return SQ_E_FAIL;  // no exception crosses the ABI
    }
  }

  sq_result getInfoImpl(sq_audio_device_info* out) {
    sq_audio_device_info i{};
    i.struct_size = sizeof i;
    copyStr(i.id, sizeof i.id, entry_.id);
    std::string n = be_->driverName();
    copyStr(i.name, sizeof i.name, entry_.name.empty() ? n : entry_.name);
    int32_t ins = 0, outs = 0;
    if (SQ_SUCCEEDED(be_->getChannels(ins, outs))) {
      i.max_input_channels = static_cast<uint32_t>(std::max(0, ins));
      i.max_output_channels = static_cast<uint32_t>(std::max(0, outs));
    }
    i.flags = SQ_AUDIO_DEVICE_EXCLUSIVE | SQ_AUDIO_DEVICE_CONTROL_PANEL;
    if (i.max_input_channels) i.flags |= SQ_AUDIO_DEVICE_INPUT;
    if (i.max_output_channels) i.flags |= SQ_AUDIO_DEVICE_OUTPUT;
    i.default_sample_rate = rate_.load();
    BufferSizes s;
    if (SQ_SUCCEEDED(be_->getBufferSize(s))) {
      s = sanitizeSizes(s);
      i.min_block_frames = static_cast<uint32_t>(std::max(0, s.minFrames));
      i.max_block_frames = static_cast<uint32_t>(std::max(0, s.maxFrames));
      i.default_block_frames = chooseBlockSize(s, 0);
      // 0 for fixed sizes and power-of-two drivers (use getSupportedBlockSizes for the list)
      i.block_granularity = s.granularity > 0 ? static_cast<uint32_t>(s.granularity) : 0;
    }
    std::memcpy(out, &i, sizeof i);
    return SQ_OK;
  }

  sq_result SQ_CALL getSupportedSampleRates(double* rates, uint32_t capacity, uint32_t* count) override try {
    if (!count) return SQ_E_INVALIDARG;
    std::vector<double> ok;
    for (double r : candidateSampleRates())
      if (be_->canSampleRate(r) == SQ_OK) ok.push_back(r);
    const double cur = rate_.load();
    if (cur > 0 && std::find(ok.begin(), ok.end(), cur) == ok.end()) {
      ok.push_back(cur);
      std::sort(ok.begin(), ok.end());
    }
    *count = static_cast<uint32_t>(ok.size());
    for (uint32_t k = 0; rates && k < capacity && k < ok.size(); ++k) rates[k] = ok[k];
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }

  sq_result SQ_CALL getSupportedBlockSizes(uint32_t* sizes, uint32_t capacity, uint32_t* count) override try {
    if (!count) return SQ_E_INVALIDARG;
    BufferSizes s;
    sq_result r = be_->getBufferSize(s);
    if (SQ_FAILED(r)) return r;
    const std::vector<uint32_t> list = supportedBlockSizes(sanitizeSizes(s));
    *count = static_cast<uint32_t>(list.size());
    for (uint32_t k = 0; sizes && k < capacity && k < list.size(); ++k) sizes[k] = list[k];
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }

  sq_result SQ_CALL getChannelName(sq_bool input, uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override try {
    int32_t ins = 0, outs = 0;
    if (SQ_FAILED(be_->getChannels(ins, outs))) return SQ_E_FAIL;
    if (index >= static_cast<uint32_t>(input ? ins : outs)) return SQ_E_NOTFOUND;
    ChannelInfo ci;
    std::string name;
    if (SQ_SUCCEEDED(be_->getChannelInfo(input != 0, static_cast<int32_t>(index), ci))) {
      ci.name[sizeof ci.name - 1] = '\0';
      name = ci.name;
    }
    if (name.empty()) name = std::string(input ? "In " : "Out ") + std::to_string(index + 1);
    return copyOut(name, buf, bufSize, needed);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }

  sq_result SQ_CALL configure(const sq_audio_device_config* request, sq_audio_device_config* actual) override {
    if (!request || !actual || request->struct_size < sizeof(sq_audio_device_config) ||
        actual->struct_size < sizeof(sq_audio_device_config))
      return SQ_E_INVALIDARG;
    if (running_.load()) return SQ_E_BUSY;
    try {
      return configureImpl(*request, *actual);
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL getStreamFormat(sq_stream_format* input, sq_stream_format* output) override {
    if ((input && input->struct_size < sizeof(sq_stream_format)) || (output && output->struct_size < sizeof(sq_stream_format)))
      return SQ_E_INVALIDARG;
    const double rate = rate_.load();
    if (input) {
      input->sample_rate = rate;
      input->channels = static_cast<uint32_t>(inCh_.size());
      input->max_block_frames = block_;
      input->format = SQ_SAMPLE_F32;
    }
    if (output) {
      output->sample_rate = rate;
      output->channels = static_cast<uint32_t>(outCh_.size());
      output->max_block_frames = block_;
      output->format = SQ_SAMPLE_F32;
    }
    return SQ_OK;
  }

  sq_result SQ_CALL getLatency(sq_latency_info* out) override {
    if (!out || out->struct_size < sizeof(sq_latency_info)) return SQ_E_INVALIDARG;
    // Always re-queried: after a latency-changed message the driver reports the new values.
    int32_t li = 0, lo = 0;
    if (SQ_SUCCEEDED(be_->getLatencies(li, lo))) {
      if (buffersCreated_) {
        latIn_ = li;
        latOut_ = lo;
      }
    } else if (buffersCreated_) {
      li = latIn_;
      lo = latOut_;
    } else {
      li = lo = 0;
    }
    out->input_frames = static_cast<uint32_t>(std::max(0, li));
    out->output_frames = static_cast<uint32_t>(std::max(0, lo));
    out->process_frames = 0;
    return SQ_OK;
  }

  sq_result SQ_CALL start(IAudioCallback* callback) override {
    if (!callback) return SQ_E_INVALIDARG;
    if (running_.load()) return SQ_E_BUSY;
    if (!buffersCreated_) {
      // not configured: defaults (first two outputs, no inputs)
      int32_t ins = 0, outs = 0;
      if (SQ_FAILED(be_->getChannels(ins, outs))) return SQ_E_FAIL;
      uint32_t ch[2] = {0, 1};
      sq_audio_device_config req{}, got{};
      req.struct_size = got.struct_size = sizeof req;
      req.output_channels = ch;
      req.output_channel_count = static_cast<uint32_t>(std::min(outs, 2));
      sq_result r = configure(&req, &got);
      if (SQ_FAILED(r)) return r;
    }
    sq_stream_format in{}, out{};
    in.struct_size = out.struct_size = sizeof in;
    getStreamFormat(&in, &out);
    sq_latency_info lat{};
    lat.struct_size = sizeof lat;
    getLatency(&lat);
    sq_result r = callback->onStart(&in, &out, &lat);
    if (SQ_FAILED(r)) return r;
    callback->addRef();
    cb_ = callback;
    devTime_ = 0;
    lastPosValid_ = false;
    xrunPending_.store(0);
    running_.store(true);
    r = be_->start();
    if (SQ_FAILED(r)) {
      sq::logf(SQ_LOG_ERROR, "start failed on '%s': %s", entry_.name.c_str(), be_->lastError().c_str());
      quiesce();
      cb_ = nullptr;
      callback->onStop();
      callback->release();
      return r;
    }
    return SQ_OK;
  }

  sq_result SQ_CALL stop() override {
    if (!running_.load()) return SQ_S_FALSE;
    const sq_result r = be_->stop();
    quiesce();
    IAudioCallback* cb = cb_;
    cb_ = nullptr;
    if (cb) {
      cb->onStop();
      cb->release();
    }
    return SQ_FAILED(r) ? r : SQ_OK;
  }

  sq_bool SQ_CALL isRunning() override { return running_.load() ? SQ_TRUE : SQ_FALSE; }
  uint64_t SQ_CALL getXrunCount() override { return xruns_.load(); }

  sq_result SQ_CALL showControlPanel(void*) override try { return be_->controlPanel(); } catch (...) {
    return SQ_E_FAIL;
  }

 private:
  // ---- configuration (control thread) -------------------------------------------------------

  sq_result configureImpl(const sq_audio_device_config& req, sq_audio_device_config& act) {
    if (buffersCreated_) {
      be_->disposeBuffers();
      buffersCreated_ = false;
    }
    inCh_.clear();
    outCh_.clear();
    inType_.clear();
    outType_.clear();
    inBuf_.clear();
    outBuf_.clear();
    block_ = 0;
    int32_t nIn = 0, nOut = 0;
    sq_result r = be_->getChannels(nIn, nOut);
    if (SQ_FAILED(r)) return r;
    auto validList = [](const uint32_t* list, uint32_t n, int32_t avail, std::vector<uint32_t>& out) {
      out.clear();
      if (n && !list) return false;
      for (uint32_t k = 0; k < n; ++k) {
        if (list[k] >= static_cast<uint32_t>(std::max(0, avail))) return false;
        if (std::find(out.begin(), out.end(), list[k]) != out.end()) return false;
        out.push_back(list[k]);
      }
      return true;
    };
    std::vector<uint32_t> ins, outs;
    if (!validList(req.input_channels, req.input_channel_count, nIn, ins) ||
        !validList(req.output_channels, req.output_channel_count, nOut, outs))
      return SQ_E_INVALIDARG;
    if (ins.empty() && outs.empty()) return SQ_E_INVALIDARG;

    bool differs = false;
    // sample rate
    double cur = 0;
    be_->getSampleRate(cur);
    if (req.sample_rate > 0 && std::fabs(req.sample_rate - cur) > 0.5) {
      if (be_->canSampleRate(req.sample_rate) == SQ_OK && SQ_SUCCEEDED(be_->setSampleRate(req.sample_rate))) {
        be_->getSampleRate(cur);
      }
      if (std::fabs(req.sample_rate - cur) > 0.5) differs = true;
    }
    if (cur > 0) rate_.store(cur);

    // block size
    BufferSizes sizes;
    r = be_->getBufferSize(sizes);
    if (SQ_FAILED(r)) return r;
    sizes = sanitizeSizes(sizes);
    uint32_t block = chooseBlockSize(sizes, req.block_frames);
    if (block == 0 || block > static_cast<uint32_t>(kMaxBlockFrames)) {
      sq::logf(SQ_LOG_ERROR, "'%s' reports unusable buffer sizes (min %d, max %d, preferred %d)", entry_.name.c_str(),
               static_cast<int>(sizes.minFrames), static_cast<int>(sizes.maxFrames),
               static_cast<int>(sizes.preferredFrames));
      return SQ_E_UNSUPPORTED;
    }
    if (req.block_frames && block != req.block_frames) differs = true;

    // channel formats
    std::vector<SampleType> inT(ins.size()), outT(outs.size());
    for (size_t k = 0; k < ins.size() + outs.size(); ++k) {
      const bool isIn = k < ins.size();
      const uint32_t ch = isIn ? ins[k] : outs[k - ins.size()];
      ChannelInfo ci;
      r = be_->getChannelInfo(isIn, static_cast<int32_t>(ch), ci);
      if (SQ_FAILED(r)) return r;
      if (!isSupported(ci.type)) {
        sq::logf(SQ_LOG_ERROR, "'%s' %s channel %u uses unsupported sample type %d", entry_.name.c_str(),
                 isIn ? "input" : "output", ch, static_cast<int>(ci.type));
        return SQ_E_UNSUPPORTED;
      }
      (isIn ? inT[k] : outT[k - ins.size()]) = ci.type;
    }

    // Everything that can allocate happens before the driver's buffers exist, so a failure leaves
    // the device cleanly unconfigured (never driver buffers without matching host state).
    std::vector<ChannelRequest> reqs;
    for (uint32_t c : ins) reqs.push_back({true, static_cast<int32_t>(c)});
    for (uint32_t c : outs) reqs.push_back({false, static_cast<int32_t>(c)});
    std::vector<ChannelBuffers> bufs(reqs.size());
    std::vector<ChannelBuffers> inB(ins.size()), outB(outs.size());
    std::vector<float> scratch(static_cast<size_t>(block) * (ins.size() + outs.size()), 0.0f);
    std::vector<float*> inP(ins.size()), outP(outs.size());
    r = be_->createBuffers(reqs.data(), static_cast<int32_t>(reqs.size()), static_cast<int32_t>(block), bufs.data(), this);
    if (SQ_FAILED(r)) {
      const uint32_t pref = chooseBlockSize(sizes, 0);
      if (pref != block) {
        r = be_->createBuffers(reqs.data(), static_cast<int32_t>(reqs.size()), static_cast<int32_t>(pref), bufs.data(), this);
        if (SQ_SUCCEEDED(r)) {
          block = pref;
          differs = true;
          try {
            scratch.assign(static_cast<size_t>(block) * (ins.size() + outs.size()), 0.0f);
          } catch (...) {
            be_->disposeBuffers();
            throw;
          }
        }
      }
      if (SQ_FAILED(r)) {
        sq::logf(SQ_LOG_ERROR, "creating buffers on '%s' failed: %s", entry_.name.c_str(), be_->lastError().c_str());
        return r;
      }
    }
    for (const ChannelBuffers& b : bufs) {
      if (!b.half[0] || !b.half[1]) {
        be_->disposeBuffers();
        return SQ_E_FAIL;
      }
    }
    buffersCreated_ = true;
    outputReady_ = be_->supportsOutputReady();
    if (SQ_FAILED(be_->getLatencies(latIn_, latOut_))) latIn_ = latOut_ = 0;

    // commit: no allocation from here on (nothing streams: running_ is false)
    std::copy(bufs.begin(), bufs.begin() + static_cast<std::ptrdiff_t>(ins.size()), inB.begin());
    std::copy(bufs.begin() + static_cast<std::ptrdiff_t>(ins.size()), bufs.end(), outB.begin());
    inCh_ = std::move(ins);
    outCh_ = std::move(outs);
    inType_ = std::move(inT);
    outType_ = std::move(outT);
    inBuf_ = std::move(inB);
    outBuf_ = std::move(outB);
    block_ = block;
    scratch_ = std::move(scratch);
    inPtrs_ = std::move(inP);
    outPtrs_ = std::move(outP);
    for (size_t c = 0; c < inCh_.size(); ++c) inPtrs_[c] = scratch_.data() + c * block;
    for (size_t c = 0; c < outCh_.size(); ++c) outPtrs_[c] = scratch_.data() + (inCh_.size() + c) * block;
    inBlock_ = sq_audio_block{inPtrs_.data(), static_cast<uint32_t>(inPtrs_.size()), block, 0};
    outBlock_ = sq_audio_block{outPtrs_.data(), static_cast<uint32_t>(outPtrs_.size()), block, 0};
    // ASIO drivers expect the first output half to be silent before streaming
    for (size_t c = 0; c < outCh_.size(); ++c) {
      writeSilence(outType_[c], outBuf_[c].half[0], block);
      writeSilence(outType_[c], outBuf_[c].half[1], block);
    }

    act.struct_size = sizeof act;
    act.sample_rate = rate_.load();
    act.block_frames = block;
    act.input_channels = inCh_.empty() ? nullptr : inCh_.data();
    act.input_channel_count = static_cast<uint32_t>(inCh_.size());
    act.output_channels = outCh_.empty() ? nullptr : outCh_.data();
    act.output_channel_count = static_cast<uint32_t>(outCh_.size());
    act.flags = req.flags;
    act.device_format = !outType_.empty() ? toSampleFormat(outType_[0]) : toSampleFormat(inType_[0]);

    sq_audio_device_info info{};
    info.struct_size = sizeof info;
    if (SQ_SUCCEEDED(getInfo(&info))) hub_->storeProbe(info);
    return differs ? SQ_S_FALSE : SQ_OK;
  }

  // Waits until the realtime callback is no longer inside process(). running_ must be false.
  void quiesce() {
    running_.store(false);
    while (inFlight_.load() != 0) std::this_thread::yield();
  }

  // ---- BackendSink (driver threads) ---------------------------------------------------------

  void bufferSwitch(int32_t index, bool, bool positionValid, int64_t samplePosition, bool, int64_t) noexcept override {
    const int h = index & 1;
    inFlight_.fetch_add(1);
    if (!running_.load() || !cb_) {
      for (size_t c = 0; c < outBuf_.size(); ++c) writeSilence(outType_[c], outBuf_[c].half[h], block_);
      inFlight_.fetch_sub(1);
      return;
    }
    const uint32_t n = block_;
    if (positionValid) {
      if (lastPosValid_ && samplePosition != lastPos_ + static_cast<int64_t>(n)) {
        xruns_.fetch_add(1, std::memory_order_relaxed);
        xrunPending_.store(1, std::memory_order_relaxed);
      }
      lastPos_ = samplePosition;
      lastPosValid_ = true;
    }
    for (size_t c = 0; c < inBuf_.size(); ++c) toFloat(inType_[c], inBuf_[c].half[h], inPtrs_[c], n);
    for (size_t c = 0; c < outPtrs_.size(); ++c) std::memset(outPtrs_[c], 0, sizeof(float) * n);
    inBlock_.position = devTime_;
    outBlock_.position = devTime_;
    sq_audio_io io{};
    io.struct_size = sizeof io;
    io.input = inBuf_.empty() ? nullptr : &inBlock_;
    io.output = &outBlock_;
    io.device_time = devTime_;
    io.host_time_ns = sq_time_monotonic_ns();
    io.flags = xrunPending_.exchange(0, std::memory_order_relaxed) ? SQ_AUDIO_IO_XRUN : 0u;
    cb_->process(&io);
    for (size_t c = 0; c < outBuf_.size(); ++c) fromFloat(outType_[c], outPtrs_[c], outBuf_[c].half[h], n);
    devTime_ += n;
    if (outputReady_) be_->outputReady();
    inFlight_.fetch_sub(1);
  }

  void sampleRateChanged(double rate) noexcept override {
    if (!(rate > 0) || std::fabs(rate - rate_.load()) <= 0.5) return;
    rate_.store(rate);  // getStreamFormat / getInfo report the driver's new rate from now on
    post(SQ_AUDIO_RESET_FORMAT_CHANGED);
  }

  int32_t message(int32_t selector, int32_t value) noexcept override {
    const MessageEffects e = routeMessage(selector, value);
    if (e.xrun) {
      xruns_.fetch_add(1, std::memory_order_relaxed);
      xrunPending_.store(1, std::memory_order_relaxed);
    }
    if (e.resetReason) post(e.resetReason);
    return e.result;
  }

  void post(int32_t reason) noexcept { nt_->pending.fetch_or(reasonBit(reason)); }  // lock-free; notifier polls

  // Notifier state shared by the device and its thread. The thread never touches the device
  // object (it only hands `self` to listeners while `stop` is false), so the device may be
  // destroyed from inside a listener callback.
  struct Notifier {
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    std::atomic<uint32_t> pending{0};
    bool stopped() {
      std::lock_guard<std::mutex> lk(m);
      return stop;
    }
  };

  static void notifierLoop(std::shared_ptr<Notifier> nt, std::shared_ptr<Hub> hub, IAudioDevice* self) {
    std::unique_lock<std::mutex> lk(nt->m);
    for (;;) {
      nt->cv.wait_for(lk, std::chrono::milliseconds(10), [&] { return nt->stop; });
      if (nt->stop) return;
      const uint32_t bits = nt->pending.exchange(0);
      if (!bits) continue;
      lk.unlock();
      try {
        // most severe first: format change implies a full re-configure
        for (int32_t reason : {SQ_AUDIO_RESET_DEVICE_LOST, SQ_AUDIO_RESET_FORMAT_CHANGED, SQ_AUDIO_RESET_REQUESTED}) {
          if (!(bits & reasonBit(reason))) continue;
          for (auto& l : hub->snapshot()) {
            if (nt->stopped()) break;  // device destroyed (possibly by the previous listener)
            l->onDeviceReset(self, reason);
          }
        }
      } catch (...) {
      }
      lk.lock();
    }
  }

  std::unique_ptr<Backend> be_;
  DriverEntry entry_;
  std::shared_ptr<Hub> hub_;

  // configuration (control thread; read by the callback only while streaming)
  bool buffersCreated_ = false;
  bool outputReady_ = false;
  uint32_t block_ = 0;
  std::atomic<double> rate_{0.0};
  int32_t latIn_ = 0, latOut_ = 0;
  std::vector<uint32_t> inCh_, outCh_;
  std::vector<SampleType> inType_, outType_;
  std::vector<ChannelBuffers> inBuf_, outBuf_;
  std::vector<float> scratch_;
  std::vector<float*> inPtrs_, outPtrs_;
  sq_audio_block inBlock_{}, outBlock_{};

  // realtime state
  std::atomic<bool> running_{false};
  std::atomic<int32_t> inFlight_{0};
  IAudioCallback* cb_ = nullptr;
  sq_sample_time devTime_ = 0;
  int64_t lastPos_ = 0;
  bool lastPosValid_ = false;
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint32_t> xrunPending_{0};

  // notifications
  std::shared_ptr<Notifier> nt_;
  std::thread notifier_;
};

// ---------------------------------------------------------------------------------------------
// driver

class AsioDriver final : public sq::Object<AsioDriver, IAudioDriver, IAsioDriverStatus> {
 public:
  explicit AsioDriver(DriverConfig cfg) : cfg_(std::move(cfg)), hub_(std::make_shared<Hub>()) {}

  sq_result SQ_CALL getDriverInfo(sq_audio_driver_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_driver_info)) return SQ_E_INVALIDARG;
    std::memset(out->name, 0, sizeof out->name);
    copyStr(out->name, sizeof out->name, "ASIO");
    out->flags = 0;
    return SQ_OK;
  }

  sq_result SQ_CALL refreshDevices() override {
    try {
      refresh();
      return SQ_OK;
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  uint32_t SQ_CALL getDeviceCount() override {
    ensure();
    return static_cast<uint32_t>(visible_.size());
  }

  sq_result SQ_CALL getDeviceInfo(uint32_t index, sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    ensure();
    if (index >= visible_.size()) return SQ_E_NOTFOUND;
    const DriverEntry& e = visible_[index];
    sq_audio_device_info i{};
    if (!hub_->findProbe(e.id, i)) {
      // Not opened yet: channel counts and sizes are only known once the driver is loaded.
      i = sq_audio_device_info{};
      i.flags = SQ_AUDIO_DEVICE_EXCLUSIVE | SQ_AUDIO_DEVICE_CONTROL_PANEL;
    }
    i.struct_size = sizeof i;
    copyStr(i.id, sizeof i.id, e.id);
    copyStr(i.name, sizeof i.name, e.name);
    std::memcpy(out, &i, sizeof i);
    return SQ_OK;
  }

  sq_result SQ_CALL openDevice(const char* deviceId, IAudioDevice** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    try {
      ensure();
      if (!cfg_.backends) return SQ_E_UNSUPPORTED;  // host not compiled in (see status text)
      const DriverEntry* e = nullptr;
      if (!deviceId || !*deviceId) {
        if (!visible_.empty()) e = &visible_.front();
      } else {
        for (const DriverEntry& d : visible_)
          if (d.id == deviceId) e = &d;
        if (!e) {
          refresh();  // installed since the last refresh?
          for (const DriverEntry& d : visible_)
            if (d.id == deviceId) e = &d;
        }
      }
      if (!e) return SQ_E_NOTFOUND;
      if (g_openDevices.load() != 0) return SQ_E_BUSY;
      std::string err;
      std::unique_ptr<Backend> be = cfg_.backends(*e, &err);
      if (!be) {
        sq::logf(SQ_LOG_ERROR, "cannot load '%s': %s", e->name.c_str(), err.c_str());
        return SQ_E_FAIL;
      }
      *out = new AsioDevice(std::move(be), *e, hub_);
      return SQ_OK;
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL addListener(IAudioDeviceListener* listener) override try {
    if (!listener) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lk(hub_->m);
    for (auto& l : hub_->listeners)
      if (l.get() == listener) return SQ_S_FALSE;
    hub_->listeners.emplace_back(listener);
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }

  sq_result SQ_CALL removeListener(IAudioDeviceListener* listener) override {
    std::lock_guard<std::mutex> lk(hub_->m);
    for (auto it = hub_->listeners.begin(); it != hub_->listeners.end(); ++it) {
      if (it->get() == listener) {
        hub_->listeners.erase(it);
        return SQ_OK;
      }
    }
    return SQ_E_NOTFOUND;
  }

  // ---- IAsioDriverStatus ---------------------------------------------------------------------

  sq_bool SQ_CALL isHostCompiled() override { return cfg_.backends ? SQ_TRUE : SQ_FALSE; }

  sq_result SQ_CALL getStatusText(char* buf, uint32_t bufSize, uint32_t* needed) override {
    try {
      ensure();
      return copyOut(status_, buf, bufSize, needed);
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  uint32_t SQ_CALL getInstalledDriverCount() override {
    ensure();
    return installed_;
  }

 private:
  void ensure() {
    if (!refreshed_) {
      try {
        refresh();
      } catch (...) {
      }
    }
  }

  void refresh() {
    refreshed_ = true;
    EnumerationResult res;
    if (cfg_.registry) {
      if (auto view = cfg_.registry()) res = enumerateDrivers(*view);
    }
    installed_ = static_cast<uint32_t>(res.drivers.size());
    std::string s;
    if (!cfg_.backends) {
      visible_.clear();
      s = cfg_.unavailableReason.empty() ? std::string("ASIO host not available in this build.") : cfg_.unavailableReason;
      s += " The system lists " + std::to_string(installed_) + " ASIO driver(s); 0 devices are available.";
    } else {
      visible_ = res.drivers;
      if (!res.keyPresent)
        s = "No ASIO drivers are installed (no ASIO registry key).";
      else
        s = std::to_string(visible_.size()) + " ASIO driver(s) available.";
    }
    if (!res.skipped.empty()) {
      s += " Ignored registry entries: ";
      for (size_t k = 0; k < res.skipped.size(); ++k) s += (k ? "; " : "") + res.skipped[k];
      s += ".";
    }
    if (s != status_) sq::logf(SQ_LOG_INFO, "%s", s.c_str());
    status_ = std::move(s);
  }

  DriverConfig cfg_;
  std::shared_ptr<Hub> hub_;
  bool refreshed_ = false;
  std::vector<DriverEntry> visible_;
  uint32_t installed_ = 0;
  std::string status_;
};

}  // namespace

uint32_t openDeviceCount() noexcept { return g_openDevices.load(); }

sq_result createDriverWith(DriverConfig cfg, const sq_guid* riid, void** out) noexcept {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  AsioDriver* d = nullptr;
  try {
    d = new AsioDriver(std::move(cfg));
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  } catch (...) {
    return SQ_E_FAIL;
  }
  const sq_result r = d->queryInterface(riid, out);
  d->release();
  return r;
}

}  // namespace sq::asio
