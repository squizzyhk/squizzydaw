// audiodriver-pulse: IAudioDriver over the PulseAudio "simple" API (libpulse-simple.so.0 loaded
// at runtime with dlopen; no development headers needed). Works with PulseAudio and with
// PipeWire's pulse server, which is what most Linux desktops route audio through.
//
// Devices: "pulse:default" (the server's default sink/source). openDevice() also accepts
// "pulse:<sink or source name>" for a specific server device. No enumeration: the simple API has
// none, and a server is only contacted when a device is configured.
// Streaming: float32 interleaved, 2 channels per direction. One blocking thread: read one block
// (capture), process, write one block (playback). The server buffers tlength = Periods * block
// frames ahead of the sink (Services.audiodriver-pulse.Periods, default 3, 2..16).
// Limitations: no xrun signal from the simple API (getXrunCount() stays 0); stop() waits for the
// blocking call in flight (at most about one block); latency is the requested server buffer.
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pulse_driver.hpp"
#include "sq/platform.h"
#include "sq/sdk.hpp"

namespace sq::pulse {
namespace {

// ---- the libpulse-simple ABI subset ----------------------------------------------------------
struct pa_simple;
struct pa_sample_spec {
  int format;  // pa_sample_format_t
  uint32_t rate;
  uint8_t channels;
};
struct pa_buffer_attr {
  uint32_t maxlength, tlength, prebuf, minreq, fragsize;
};
constexpr int kSampleFloat32Le = 5;
constexpr int kStreamPlayback = 1;
constexpr int kStreamRecord = 2;

struct Api {
  pa_simple* (*simple_new)(const char* server, const char* name, int dir, const char* dev, const char* streamName, const pa_sample_spec* ss,
                           const void* map, const pa_buffer_attr* attr, int* error);
  void (*simple_free)(pa_simple*);
  int (*simple_write)(pa_simple*, const void* data, size_t bytes, int* error);
  int (*simple_read)(pa_simple*, void* data, size_t bytes, int* error);
  int (*simple_flush)(pa_simple*, int* error);
  const char* (*strerror)(int error);
};

const Api* load(std::string* why) {
  static Api api{};
  static bool ok = false;
  static std::string reason;
  static std::once_flag once;
  std::call_once(once, [] {
    void* lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);  // never unloaded
    if (!lib) {
      const char* e = dlerror();
      reason = std::string("libpulse-simple.so.0 not found (") + (e ? e : "dlopen failed") + ")";
      return;
    }
    auto sym = [&](const char* n) { return dlsym(lib, n); };  // also searches libpulse.so.0
    api.simple_new = reinterpret_cast<decltype(api.simple_new)>(sym("pa_simple_new"));
    api.simple_free = reinterpret_cast<decltype(api.simple_free)>(sym("pa_simple_free"));
    api.simple_write = reinterpret_cast<decltype(api.simple_write)>(sym("pa_simple_write"));
    api.simple_read = reinterpret_cast<decltype(api.simple_read)>(sym("pa_simple_read"));
    api.simple_flush = reinterpret_cast<decltype(api.simple_flush)>(sym("pa_simple_flush"));
    api.strerror = reinterpret_cast<decltype(api.strerror)>(sym("pa_strerror"));
    if (!api.simple_new || !api.simple_free || !api.simple_write || !api.simple_read || !api.simple_flush || !api.strerror) {
      reason = "libpulse-simple.so.0 lacks a required symbol";
      return;
    }
    ok = true;
  });
  if (why) *why = reason;
  return ok ? &api : nullptr;
}

constexpr uint32_t kChannels = 2;
constexpr uint32_t kDefaultBlock = 512;
constexpr double kRates[] = {44100, 48000, 88200, 96000};
const uint32_t kBlocks[] = {64, 128, 256, 512, 1024, 2048, 4096};

void copyStr(char* dst, size_t cap, const std::string& s) {
  if (!cap) return;
  const size_t n = std::min(s.size(), cap - 1);
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

sq_audio_device_info makeInfo(const std::string& id, const std::string& name, bool isDefault) {
  sq_audio_device_info i{};
  i.struct_size = sizeof i;
  copyStr(i.id, sizeof i.id, id);
  copyStr(i.name, sizeof i.name, name);
  i.flags = SQ_AUDIO_DEVICE_INPUT | SQ_AUDIO_DEVICE_OUTPUT | (isDefault ? SQ_AUDIO_DEVICE_DEFAULT_INPUT | SQ_AUDIO_DEVICE_DEFAULT_OUTPUT : 0u);
  i.max_input_channels = kChannels;
  i.max_output_channels = kChannels;
  i.default_sample_rate = 48000;
  i.default_block_frames = kDefaultBlock;
  i.min_block_frames = 64;
  i.max_block_frames = 4096;
  return i;
}

class PulseDevice final : public sq::Object<PulseDevice, IAudioDevice> {
 public:
  PulseDevice(const Api* a, sq_audio_device_info info, std::string server) : a_(a), info_(info), dev_(std::move(server)) {}
  ~PulseDevice() override {
    stopImpl();
    closeStreams();
  }

  sq_result SQ_CALL getInfo(sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    *out = info_;
    return SQ_OK;
  }
  sq_result SQ_CALL getSupportedSampleRates(double* rates, uint32_t capacity, uint32_t* count) override {
    return fill(kRates, 4, rates, capacity, count);
  }
  sq_result SQ_CALL getSupportedBlockSizes(uint32_t* sizes, uint32_t capacity, uint32_t* count) override {
    return fill(kBlocks, 7, sizes, capacity, count);
  }
  sq_result SQ_CALL getChannelName(sq_bool input, uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override {
    if (index >= kChannels) return SQ_E_NOTFOUND;
    const std::string n = std::string(input ? "In " : "Out ") + (index == 0 ? "1 (L)" : "2 (R)");
    const uint32_t need = static_cast<uint32_t>(n.size() + 1);
    if (needed) *needed = need;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, n.c_str(), need);
    return SQ_OK;
  }
  sq_result SQ_CALL configure(const sq_audio_device_config* req, sq_audio_device_config* actual) override {
    if (!req || !actual || req->struct_size < sizeof(sq_audio_device_config) || actual->struct_size < sizeof(sq_audio_device_config))
      return SQ_E_INVALIDARG;
    if (running_.load()) return SQ_E_BUSY;
    return configureImpl(*req, actual);
  }
  sq_result SQ_CALL getStreamFormat(sq_stream_format* input, sq_stream_format* output) override {
    if ((input && input->struct_size < sizeof(sq_stream_format)) || (output && output->struct_size < sizeof(sq_stream_format)))
      return SQ_E_INVALIDARG;
    if (!configured_) {
      if (input) *input = sq_stream_format{sizeof(sq_stream_format), 48000.0, kChannels, kDefaultBlock, SQ_SAMPLE_F32};
      if (output) *output = sq_stream_format{sizeof(sq_stream_format), 48000.0, kChannels, kDefaultBlock, SQ_SAMPLE_F32};
      return SQ_S_FALSE;
    }
    if (input) *input = sq_stream_format{sizeof(sq_stream_format), double(rate_), uint32_t(inMap_.size()), block_, SQ_SAMPLE_F32};
    if (output) *output = sq_stream_format{sizeof(sq_stream_format), double(rate_), uint32_t(outMap_.size()), block_, SQ_SAMPLE_F32};
    return SQ_OK;
  }
  sq_result SQ_CALL getLatency(sq_latency_info* out) override {
    if (!out || out->struct_size < sizeof(sq_latency_info)) return SQ_E_INVALIDARG;
    out->input_frames = configured_ && rec_ ? block_ : 0;
    out->output_frames = configured_ && play_ ? block_ * periods_ : 0;
    out->process_frames = 0;
    return configured_ ? SQ_OK : SQ_S_FALSE;
  }
  sq_result SQ_CALL start(IAudioCallback* cb) override {
    if (!cb) return SQ_E_INVALIDARG;
    if (running_.load()) return SQ_E_BUSY;
    if (!configured_) {
      const uint32_t outs[2] = {0, 1};
      sq_audio_device_config rq{};
      rq.struct_size = sizeof rq;
      rq.output_channels = outs;
      rq.output_channel_count = 2;
      sq_audio_device_config got{};
      got.struct_size = sizeof got;
      sq_result r = configureImpl(rq, &got);
      if (SQ_FAILED(r)) return r;
    }
    sq_stream_format inF{}, outF{};
    inF.struct_size = outF.struct_size = sizeof inF;
    getStreamFormat(&inF, &outF);
    sq_latency_info lat{};
    lat.struct_size = sizeof lat;
    getLatency(&lat);
    sq_result r = cb->onStart(&inF, &outF, &lat);
    if (SQ_FAILED(r)) return r;
    cb->addRef();
    callback_ = cb;
    quit_.store(false);
    lost_.store(false);
    running_.store(true);
    try {
      thread_ = std::thread([this] { streamMain(); });
    } catch (...) {
      running_.store(false);
      callback_->onStop();
      callback_->release();
      callback_ = nullptr;
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL stop() override {
    if (thread_.joinable() && std::this_thread::get_id() == thread_.get_id()) return SQ_E_BUSY;
    if (!callback_) return SQ_S_FALSE;
    stopImpl();
    return SQ_OK;
  }
  sq_bool SQ_CALL isRunning() override { return running_.load() && !lost_.load() ? SQ_TRUE : SQ_FALSE; }
  uint64_t SQ_CALL getXrunCount() override { return 0; }
  sq_result SQ_CALL showControlPanel(void*) override { return SQ_E_NOTIMPL; }

  void setListeners(std::function<void(IAudioDevice*)> onLost) { onLost_ = std::move(onLost); }

 private:
  template <class T>
  static sq_result fill(const T* v, uint32_t n, T* out, uint32_t capacity, uint32_t* count) {
    if (count) *count = n;
    if (!out) return capacity ? SQ_E_INVALIDARG : SQ_OK;
    const uint32_t k = std::min(capacity, n);
    std::copy(v, v + k, out);
    return k < n ? SQ_E_BUFFER_TOO_SMALL : SQ_OK;
  }
  void closeStreams() {
    if (play_) a_->simple_free(play_);
    if (rec_) a_->simple_free(rec_);
    play_ = rec_ = nullptr;
    configured_ = false;
  }

  sq_result configureImpl(const sq_audio_device_config& req, sq_audio_device_config* actual) {
    closeStreams();
    if ((req.output_channel_count && !req.output_channels) || (req.input_channel_count && !req.input_channels)) return SQ_E_INVALIDARG;
    bool differs = false;
    std::vector<uint32_t> outs, ins;
    for (uint32_t i = 0; i < req.output_channel_count; ++i) {
      const uint32_t c = req.output_channels[i];
      if (c < kChannels && std::find(outs.begin(), outs.end(), c) == outs.end()) outs.push_back(c);
      else differs = true;
    }
    for (uint32_t i = 0; i < req.input_channel_count; ++i) {
      const uint32_t c = req.input_channels[i];
      if (c < kChannels && std::find(ins.begin(), ins.end(), c) == ins.end()) ins.push_back(c);
      else differs = true;
    }
    if (outs.empty() && ins.empty()) return SQ_E_INVALIDARG;
    rate_ = static_cast<uint32_t>((req.sample_rate > 0 ? req.sample_rate : 48000.0) + 0.5);
    if (rate_ < 8000 || rate_ > 384000) return SQ_E_UNSUPPORTED;
    block_ = std::clamp<uint32_t>(req.block_frames ? req.block_frames : kDefaultBlock, 64, 4096);
    IHostContext* h = host();
    periods_ = static_cast<uint32_t>(std::clamp<int64_t>(h ? h->getSettings()->getInt("Services.audiodriver-pulse.Periods", 3) : 3, 2, 16));
    const pa_sample_spec ss{kSampleFloat32Le, rate_, uint8_t(kChannels)};
    const uint32_t frameBytes = kChannels * 4;
    const char* dev = dev_.empty() ? nullptr : dev_.c_str();
    int err = 0;
    if (!outs.empty()) {
      pa_buffer_attr at{UINT32_MAX, block_ * periods_ * frameBytes, UINT32_MAX, block_ * frameBytes, UINT32_MAX};
      play_ = a_->simple_new(nullptr, "Squizzy Studio", kStreamPlayback, dev, "Playback", &ss, nullptr, &at, &err);
      if (!play_) {
        sq::logf(SQ_LOG_WARN, "pulse playback: %s", a_->strerror(err));
        return SQ_E_NOTFOUND;
      }
    }
    if (!ins.empty()) {
      pa_buffer_attr at{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, block_ * frameBytes};
      rec_ = a_->simple_new(nullptr, "Squizzy Studio", kStreamRecord, dev, "Capture", &ss, nullptr, &at, &err);
      if (!rec_) {
        sq::logf(SQ_LOG_WARN, "pulse capture: %s", a_->strerror(err));
        if (!play_) return SQ_E_NOTFOUND;
        ins.clear();
        differs = true;
      }
    }
    outMap_ = outs;
    inMap_ = ins;
    try {
      bytes_.assign(size_t(block_) * kChannels, 0.0f);
      outBufs_.assign(outMap_.size(), std::vector<float>(block_, 0.0f));
      inBufs_.assign(inMap_.size(), std::vector<float>(block_, 0.0f));
      outPtrs_.clear();
      inPtrs_.clear();
      for (auto& b : outBufs_) outPtrs_.push_back(b.data());
      for (auto& b : inBufs_) inPtrs_.push_back(b.data());
    } catch (...) {
      closeStreams();
      return SQ_E_OUTOFMEMORY;
    }
    configured_ = true;
    *actual = req;
    actual->struct_size = sizeof(sq_audio_device_config);
    actual->sample_rate = rate_;
    actual->block_frames = block_;
    actual->input_channels = inMap_.empty() ? nullptr : inMap_.data();
    actual->input_channel_count = uint32_t(inMap_.size());
    actual->output_channels = outMap_.empty() ? nullptr : outMap_.data();
    actual->output_channel_count = uint32_t(outMap_.size());
    actual->device_format = SQ_SAMPLE_F32;
    if (req.block_frames && req.block_frames != block_) differs = true;
    sq::logf(SQ_LOG_INFO, "configured %s: %u Hz, %u frames x %u periods, out %u ch, in %u ch", info_.id, rate_, block_, periods_,
             unsigned(outMap_.size()), unsigned(inMap_.size()));
    return differs ? SQ_S_FALSE : SQ_OK;
  }

  void stopImpl() {
    if (thread_.joinable()) {
      quit_.store(true);
      thread_.join();
    }
    running_.store(false);
    if (play_) a_->simple_flush(play_, nullptr);
    if (callback_) {
      callback_->onStop();
      callback_->release();
      callback_ = nullptr;
    }
  }

  // Stream thread. The blocking server calls pace it.
  void streamMain() {
    sq_thread_set_current_name("sq-pulse-audio");
    sq_thread_set_current_priority(SQ_PRIORITY_REALTIME);
    const size_t bytes = size_t(block_) * kChannels * sizeof(float);
    sq_audio_block inBlock{inPtrs_.data(), uint32_t(inPtrs_.size()), block_, 0};
    sq_audio_block outBlock{outPtrs_.data(), uint32_t(outPtrs_.size()), block_, 0};
    uint64_t t = 0;
    int err = 0;
    bool failed = false;
    while (!quit_.load(std::memory_order_relaxed)) {
      if (rec_) {
        if (a_->simple_read(rec_, bytes_.data(), bytes, &err) < 0) {
          failed = true;
          break;
        }
        for (size_t k = 0; k < inMap_.size(); ++k)
          for (uint32_t i = 0; i < block_; ++i) inPtrs_[k][i] = bytes_[size_t(i) * kChannels + inMap_[k]];
      }
      sq_audio_io io{};
      io.struct_size = sizeof io;
      inBlock.position = outBlock.position = static_cast<sq_sample_time>(t);
      io.input = inPtrs_.empty() ? nullptr : &inBlock;
      io.output = &outBlock;
      io.device_time = static_cast<sq_sample_time>(t);
      io.host_time_ns = sq_time_monotonic_ns();
      callback_->process(&io);
      t += block_;
      if (play_) {
        std::fill(bytes_.begin(), bytes_.end(), 0.0f);
        for (size_t k = 0; k < outMap_.size(); ++k)
          for (uint32_t i = 0; i < block_; ++i) bytes_[size_t(i) * kChannels + outMap_[k]] = outPtrs_[k][i];
        if (a_->simple_write(play_, bytes_.data(), bytes, &err) < 0) {
          failed = true;
          break;
        }
      }
    }
    if (failed) {  // off the realtime path from here on
      lost_.store(true);
      sq::logf(SQ_LOG_ERROR, "%s stopped: %s", info_.id, a_->strerror(err));
      if (onLost_) onLost_(this);
    }
  }

  const Api* a_;
  sq_audio_device_info info_;
  std::string dev_;
  pa_simple* play_ = nullptr;
  pa_simple* rec_ = nullptr;
  bool configured_ = false;
  uint32_t rate_ = 48000, block_ = kDefaultBlock, periods_ = 3;
  std::vector<uint32_t> outMap_, inMap_;
  std::vector<float> bytes_;
  std::vector<std::vector<float>> outBufs_, inBufs_;
  std::vector<float*> outPtrs_, inPtrs_;
  IAudioCallback* callback_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false}, quit_{false}, lost_{false};
  std::function<void(IAudioDevice*)> onLost_;
};

struct Listeners {
  std::mutex mu;
  std::vector<Ptr<IAudioDeviceListener>> list;
  void lost(IAudioDevice* d) {
    std::vector<Ptr<IAudioDeviceListener>> copy;
    {
      std::lock_guard<std::mutex> lk(mu);
      copy = list;
    }
    for (auto& l : copy) l->onDeviceReset(d, SQ_AUDIO_RESET_DEVICE_LOST);
  }
};

class PulseDriver final : public sq::Object<PulseDriver, IAudioDriver> {
 public:
  sq_result SQ_CALL getDriverInfo(sq_audio_driver_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_driver_info)) return SQ_E_INVALIDARG;
    *out = sq_audio_driver_info{};
    out->struct_size = sizeof(sq_audio_driver_info);
    copyStr(out->name, sizeof out->name, "Pulse");
    return SQ_OK;
  }
  sq_result SQ_CALL refreshDevices() override {
    std::string why;
    available_ = load(&why) != nullptr;
    if (!available_) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) sq::logf(SQ_LOG_WARN, "PulseAudio unavailable, 0 devices: %s", why.c_str());
    }
    return SQ_OK;
  }
  uint32_t SQ_CALL getDeviceCount() override {
    refreshDevices();
    return available_ ? 1u : 0u;
  }
  sq_result SQ_CALL getDeviceInfo(uint32_t index, sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    if (index >= getDeviceCount()) return SQ_E_NOTFOUND;
    *out = makeInfo("pulse:default", "PulseAudio / PipeWire default device", true);
    return SQ_OK;
  }
  sq_result SQ_CALL openDevice(const char* deviceId, IAudioDevice** out) override {
    if (!deviceId || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    const Api* a = load(nullptr);
    if (!a) return SQ_E_UNSUPPORTED;
    std::string id = deviceId;
    if (id.rfind("pulse:", 0) != 0) return SQ_E_NOTFOUND;
    std::string dev = id.substr(6);
    if (dev.empty()) return SQ_E_NOTFOUND;
    const bool isDefault = dev == "default";
    auto d = Ptr<PulseDevice>::adopt(new (std::nothrow) PulseDevice(a, makeInfo(id, isDefault ? "PulseAudio / PipeWire default device" : dev, isDefault),
                                                                 isDefault ? std::string() : dev));
    if (!d) return SQ_E_OUTOFMEMORY;
    auto ls = listeners_;
    d->setListeners([ls](IAudioDevice* dv) { ls->lost(dv); });
    *out = d.detach();
    return SQ_OK;
  }
  sq_result SQ_CALL addListener(IAudioDeviceListener* l) override {
    if (!l) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lk(listeners_->mu);
    listeners_->list.push_back(Ptr<IAudioDeviceListener>(l));
    return SQ_OK;
  }
  sq_result SQ_CALL removeListener(IAudioDeviceListener* l) override {
    std::lock_guard<std::mutex> lk(listeners_->mu);
    auto& v = listeners_->list;
    for (auto it = v.begin(); it != v.end(); ++it)
      if (it->get() == l) {
        v.erase(it);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }

 private:
  bool available_ = false;
  std::shared_ptr<Listeners> listeners_ = std::make_shared<Listeners>();
};

}  // namespace

sq_result createDriver(const sq_guid* riid, void** out) { return sq::createObject<PulseDriver>(riid, out); }

}  // namespace sq::pulse
