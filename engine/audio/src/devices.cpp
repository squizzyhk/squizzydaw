// Squizzy Studio engine/audio - NullDevice / OfflineDevice / NullDriver.
#include "sq/engine/audio/devices.hpp"

#include <algorithm>
#include <cstring>

#include "sq/platform.h"
#include "sq/engine/audio/rt.hpp"

namespace sq::engine::audio {

namespace {
void copyStr(char* dst, size_t cap, const std::string& s) {
  const size_t n = std::min(cap - 1, s.size());
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}
void allocPlanar(std::vector<float>& buf, std::vector<float*>& ptrs, size_t channels, uint32_t frames) {
  buf.assign(channels * frames, 0.0f);
  ptrs.assign(channels, nullptr);
  for (size_t c = 0; c < channels; ++c) ptrs[c] = &buf[c * frames];
}
}  // namespace

NullDevice::NullDevice(NullDeviceOptions opt) : opt_(std::move(opt)) {
  if (opt_.blockFrames == 0) opt_.blockFrames = 256;
  for (uint32_t c = 0; c < opt_.inputChannels; ++c) inChans_.push_back(c);
  for (uint32_t c = 0; c < opt_.outputChannels; ++c) outChans_.push_back(c);
  allocPlanar(inBuf_, inPtrs_, inChans_.size(), opt_.blockFrames);
  allocPlanar(outBuf_, outPtrs_, outChans_.size(), opt_.blockFrames);
}

NullDevice::~NullDevice() { stop(); }

sq_result NullDevice::getInfo(sq_audio_device_info* out) {
  if (!out) return SQ_E_INVALIDARG;
  const uint32_t ss = out->struct_size;
  std::memset(out, 0, sizeof *out);
  out->struct_size = ss;
  copyStr(out->id, sizeof out->id, opt_.id);
  copyStr(out->name, sizeof out->name, opt_.name);
  out->flags = SQ_AUDIO_DEVICE_INPUT | SQ_AUDIO_DEVICE_OUTPUT | SQ_AUDIO_DEVICE_EXCLUSIVE;
  out->max_input_channels = opt_.inputChannels;
  out->max_output_channels = opt_.outputChannels;
  out->default_sample_rate = opt_.sampleRate;
  out->default_block_frames = opt_.blockFrames;
  out->min_block_frames = 16;
  out->max_block_frames = 8192;
  out->block_granularity = 0;
  return SQ_OK;
}

sq_result NullDevice::getSupportedSampleRates(double* rates, uint32_t capacity, uint32_t* count) {
  static const double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
  const uint32_t n = static_cast<uint32_t>(sizeof kRates / sizeof kRates[0]);
  if (count) *count = n;
  for (uint32_t i = 0; rates && i < capacity && i < n; ++i) rates[i] = kRates[i];
  return SQ_OK;
}

sq_result NullDevice::getSupportedBlockSizes(uint32_t* sizes, uint32_t capacity, uint32_t* count) {
  static const uint32_t kSizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
  const uint32_t n = static_cast<uint32_t>(sizeof kSizes / sizeof kSizes[0]);
  if (count) *count = n;
  for (uint32_t i = 0; sizes && i < capacity && i < n; ++i) sizes[i] = kSizes[i];
  return SQ_OK;
}

sq_result NullDevice::getChannelName(sq_bool input, uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) {
  if (index >= (input ? opt_.inputChannels : opt_.outputChannels)) return SQ_E_NOTFOUND;
  const std::string s = std::string(input ? "In " : "Out ") + std::to_string(index + 1);
  const uint32_t need = static_cast<uint32_t>(s.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, s.c_str(), need);
  return SQ_OK;
}

sq_result NullDevice::configure(const sq_audio_device_config* request, sq_audio_device_config* actual) {
  if (!request || !actual) return SQ_E_INVALIDARG;
  if (running_.load()) return SQ_E_BUSY;
  bool differs = false;
  if (request->sample_rate > 0) opt_.sampleRate = request->sample_rate;
  if (request->block_frames > 0) {
    const uint32_t b = std::clamp<uint32_t>(request->block_frames, 16, 8192);
    differs = differs || b != request->block_frames;
    opt_.blockFrames = b;
  }
  inChans_.clear();
  for (uint32_t i = 0; request->input_channels && i < request->input_channel_count; ++i) {
    if (request->input_channels[i] < opt_.inputChannels) inChans_.push_back(request->input_channels[i]);
    else differs = true;
  }
  outChans_.clear();
  for (uint32_t i = 0; request->output_channels && i < request->output_channel_count; ++i) {
    if (request->output_channels[i] < opt_.outputChannels) outChans_.push_back(request->output_channels[i]);
    else differs = true;
  }
  allocPlanar(inBuf_, inPtrs_, inChans_.size(), opt_.blockFrames);
  allocPlanar(outBuf_, outPtrs_, outChans_.size(), opt_.blockFrames);
  const uint32_t ss = actual->struct_size;
  *actual = *request;
  actual->struct_size = ss;
  actual->sample_rate = opt_.sampleRate;
  actual->block_frames = opt_.blockFrames;
  actual->input_channel_count = static_cast<uint32_t>(inChans_.size());
  actual->input_channels = inChans_.empty() ? nullptr : inChans_.data();
  actual->output_channel_count = static_cast<uint32_t>(outChans_.size());
  actual->output_channels = outChans_.empty() ? nullptr : outChans_.data();
  actual->device_format = SQ_SAMPLE_F32;
  return differs ? SQ_S_FALSE : SQ_OK;
}

sq_result NullDevice::getStreamFormat(sq_stream_format* input, sq_stream_format* output) {
  if (input) {
    input->struct_size = sizeof *input;
    input->sample_rate = opt_.sampleRate;
    input->channels = static_cast<uint32_t>(inChans_.size());
    input->max_block_frames = opt_.blockFrames;
    input->format = SQ_SAMPLE_F32;
  }
  if (output) {
    output->struct_size = sizeof *output;
    output->sample_rate = opt_.sampleRate;
    output->channels = static_cast<uint32_t>(outChans_.size());
    output->max_block_frames = opt_.blockFrames;
    output->format = SQ_SAMPLE_F32;
  }
  return SQ_OK;
}

sq_result NullDevice::getLatency(sq_latency_info* out) {
  if (!out) return SQ_E_INVALIDARG;
  out->struct_size = sizeof *out;
  out->input_frames = opt_.blockFrames + opt_.inputLatency;
  out->output_frames = opt_.blockFrames + opt_.outputLatency;
  out->process_frames = 0;
  return SQ_OK;
}

sq_result NullDevice::start(IAudioCallback* callback) {
  if (!callback) return SQ_E_INVALIDARG;
  if (running_.load()) return SQ_E_BUSY;
  sq_stream_format in{}, out{};
  getStreamFormat(&in, &out);
  sq_latency_info lat{};
  getLatency(&lat);
  callback->addRef();
  const sq_result r = callback->onStart(&in, &out, &lat);
  if (SQ_FAILED(r)) {
    callback->release();
    return r;
  }
  cb_ = callback;
  pos_.store(0);
  running_.store(true);
  if (!opt_.offline) {
    AudioRenderThread::Options o;
    o.name = "NullDevice";
    thread_.start([this](const std::atomic<bool>& stop) { threadBody(stop); }, o);
  }
  return SQ_OK;
}

sq_result NullDevice::stop() {
  if (!running_.load()) return SQ_S_FALSE;
  thread_.stop();
  running_.store(false);
  lost_.store(false);
  IAudioCallback* cb = cb_;
  cb_ = nullptr;
  cb->onStop();
  cb->release();
  return SQ_OK;
}

sq_bool NullDevice::isRunning() { return (running_.load() && !lost_.load()) ? SQ_TRUE : SQ_FALSE; }

void NullDevice::simulateLoss() {
  if (!running_.load() || lost_.exchange(true)) return;
  thread_.stop();  // the stream thread dies; onStop() is NOT called (like a driver stream error)
  if (listeners_) {
    const NullListenerList copy = *listeners_;
    for (auto& l : copy) l->onDeviceReset(this, SQ_AUDIO_RESET_DEVICE_LOST);
  }
}
uint64_t NullDevice::getXrunCount() { return xruns_.load(); }
sq_result NullDevice::showControlPanel(void*) { return SQ_E_NOTIMPL; }

void NullDevice::runOne(uint32_t flags) noexcept {
  const uint32_t n = opt_.blockFrames;
  const int64_t pos = pos_.load(std::memory_order_relaxed);
  const uint32_t nIn = static_cast<uint32_t>(inPtrs_.size());
  const uint32_t nOut = static_cast<uint32_t>(outPtrs_.size());
  if (nIn) {
    if (opt_.inputGenerator) opt_.inputGenerator(pos, inPtrs_.data(), nIn, n);
    else std::fill(inBuf_.begin(), inBuf_.end(), 0.0f);
  }
  std::fill(outBuf_.begin(), outBuf_.end(), 0.0f);
  sq_audio_block in{inPtrs_.data(), nIn, n, pos};
  sq_audio_block out{outPtrs_.data(), nOut, n, pos};
  sq_audio_io io{};
  io.struct_size = sizeof io;
  io.input = nIn ? &in : nullptr;
  io.output = &out;
  io.device_time = pos;
  io.host_time_ns = sq_time_monotonic_ns();
  io.flags = flags;
  cb_->process(&io);
  if (opt_.outputSink) opt_.outputSink(pos, outPtrs_.data(), nOut, n);
  pos_.store(pos + n, std::memory_order_relaxed);
  callbacks_.fetch_add(1, std::memory_order_relaxed);
}

void NullDevice::threadBody(const std::atomic<bool>& stop) noexcept {
  const double speed = opt_.speed;
  const uint64_t period = speed > 0 ? static_cast<uint64_t>(1e9 * opt_.blockFrames / (opt_.sampleRate * speed)) : 0;
  uint64_t anchor = sq_time_monotonic_ns();
  uint64_t k = 0;
  uint32_t flags = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    if (period) {
      const uint64_t due = anchor + k * period;
      for (;;) {
        const uint64_t now = sq_time_monotonic_ns();
        if (now >= due || stop.load(std::memory_order_relaxed)) break;
        if (due - now > 3000000ull) sq_thread_sleep_ms(1);
        else sq_thread_yield();
      }
    }
    runOne(flags);
    flags = 0;
    if (period) {
      const uint64_t end = sq_time_monotonic_ns();
      // double-buffered device model: late by more than one extra period = audible dropout
      if (end > anchor + (k + 2) * period) {
        xruns_.fetch_add(1, std::memory_order_relaxed);
        flags = SQ_AUDIO_IO_XRUN;
        anchor = end - (k + 1) * period;
      }
    }
    ++k;
  }
}

uint32_t NullDevice::pump(uint32_t blocks) {
  if (!running_.load() || lost_.load() || !opt_.offline || !cb_) return 0;
  rt::Scope scope;
  for (uint32_t i = 0; i < blocks; ++i) runOne(0);
  return blocks;
}

// ---- NullDriver ----
std::vector<NullDeviceOptions>& NullDriver::defaults() {
  static std::vector<NullDeviceOptions> d = [] {
    std::vector<NullDeviceOptions> v;
    NullDeviceOptions rtDev;
    rtDev.id = "null";
    rtDev.name = "Null Device";
    v.push_back(rtDev);
    NullDeviceOptions off;
    off.id = "offline";
    off.name = "Offline Device";
    off.offline = true;
    v.push_back(off);
    return v;
  }();
  return d;
}

NullDriver::NullDriver() : devices_(defaults()) {}
NullDriver::~NullDriver() = default;

sq_result NullDriver::getDriverInfo(sq_audio_driver_info* out) {
  if (!out) return SQ_E_INVALIDARG;
  out->struct_size = sizeof *out;
  copyStr(out->name, sizeof out->name, "Null");
  out->flags = 0;
  return SQ_OK;
}

sq_result NullDriver::getDeviceInfo(uint32_t index, sq_audio_device_info* out) {
  if (index >= devices_.size()) return SQ_E_NOTFOUND;
  sq::Ptr<NullDevice> d = sq::Ptr<NullDevice>::adopt(new NullDevice(devices_[index]));
  sq_result r = d->getInfo(out);
  if (SQ_SUCCEEDED(r) && index == 0) out->flags |= SQ_AUDIO_DEVICE_DEFAULT_INPUT | SQ_AUDIO_DEVICE_DEFAULT_OUTPUT;
  return r;
}

sq_result NullDriver::openDevice(const char* deviceId, IAudioDevice** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  for (auto& o : devices_) {
    if (deviceId && o.id == deviceId) {
      NullDevice* d = new NullDevice(o);
      d->setListeners(listeners_);
      *out = d;
      return SQ_OK;
    }
  }
  return SQ_E_NOTFOUND;
}

sq_result NullDriver::addListener(IAudioDeviceListener* listener) {
  if (!listener) return SQ_E_INVALIDARG;
  for (auto& l : *listeners_)
    if (l.get() == listener) return SQ_S_FALSE;
  listeners_->emplace_back(listener);
  return SQ_OK;
}

sq_result NullDriver::removeListener(IAudioDeviceListener* listener) {
  for (auto it = listeners_->begin(); it != listeners_->end(); ++it)
    if (it->get() == listener) {
      listeners_->erase(it);
      return SQ_OK;
    }
  return SQ_E_NOTFOUND;
}

namespace {
class NullDriverFactory final : public sq::Object<NullDriverFactory, IClassFactory> {
 public:
  sq_result SQ_CALL getModuleInfo(sq_module_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getClassCount() override { return 1; }
  sq_result SQ_CALL getClassInfo(uint32_t index, sq_class_info* out) override {
    if (!out || index != 0) return SQ_E_INVALIDARG;
    out->class_id = kNullDriverClassId;
    out->category = audiodevice::kCategory;
    out->name = "Null Audio Driver";
    out->flags = 0;
    out->module_fourcc = 0;
    return SQ_OK;
  }
  sq_result SQ_CALL createInstance(const sq_guid* classId, const sq_guid* riid, void** out) override {
    if (!classId || *classId != kNullDriverClassId) return SQ_E_NOTFOUND;
    return sq::createObject<NullDriver>(riid, out);
  }
  sq_bool SQ_CALL canUnload() override { return SQ_TRUE; }
};
}  // namespace

sq_result registerNullDriver(IClassRegistry* reg) {
  if (!reg) return SQ_E_INVALIDARG;
  sq_class_info info{};
  info.struct_size = sizeof info;
  info.class_id = kNullDriverClassId;
  info.category = audiodevice::kCategory;
  info.name = "Null Audio Driver";
  sq::Ptr<NullDriverFactory> f = sq::Ptr<NullDriverFactory>::adopt(new NullDriverFactory());
  return reg->registerClass(&info, f.get());
}

sq_result unregisterNullDriver(IClassRegistry* reg) {
  if (!reg) return SQ_E_INVALIDARG;
  return reg->unregisterClass(&kNullDriverClassId);
}

}  // namespace sq::engine::audio
