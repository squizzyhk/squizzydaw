// audiodriver-alsa: IAudioDriver / IAudioDevice over the ALSA PCM API (libasound loaded at
// runtime, see alsa_api.hpp).
//
// Devices
//   Enumerated from snd_device_name_hint("pcm"): default, sysdefault, pipewire, pulse, jack,
//   hw:/plughw:/hdmi: and null (other plugin aliases such as surround51/front/dmix are hidden
//   unless Services.audiodriver-alsa.ShowAllDevices is true). Ids are "alsa:<ALSA PCM name>";
//   openDevice() also accepts any other ALSA PCM name (with or without the "alsa:" prefix), so
//   user-defined ~/.asoundrc devices work. "default" carries the DEFAULT_INPUT/OUTPUT flags.
//
// Streaming
//   One PCM per direction, opened non-blocking, RW interleaved. The sample format is the first of
//   FLOAT_LE, S32_LE, S24_3LE, S24_LE, S16_LE the device accepts; the stream thread converts to
//   and from the engine's planar float32. Full duplex runs both directions with the same period
//   on one thread (capture linked to playback with snd_pcm_link when possible): read one period,
//   process, write one period. Playback is pre-filled with a full buffer of silence, so the
//   output latency is the ALSA buffer size and the input latency one period.
//   The stream thread asks for SCHED_FIFO (sq_thread_set_current_priority REALTIME) and falls
//   back to normal scheduling when that is not permitted. Xruns (EPIPE/ESTRPIPE) are counted,
//   both streams are restarted and the next buffer carries SQ_AUDIO_IO_XRUN. A fatal error
//   (unplugged: ENODEV ...) ends the stream and reports SQ_AUDIO_RESET_DEVICE_LOST.
//   ALSA's "null" PCM consumes/produces data instantly; streams on it are paced by the clock.
//
// Settings (read at configure time)
//   Services.audiodriver-alsa.Periods         periods per playback buffer (2..16, 0 = auto: 2 for
//                                             hw devices, 3 otherwise)
//   Services.audiodriver-alsa.ShowAllDevices  list every PCM hint (default false)
#include "alsa_driver.hpp"

#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "alsa_api.hpp"
#include "alsa_convert.hpp"
#include "sq/platform.h"
#include "sq/sdk.hpp"

namespace sq::alsa {
namespace {

constexpr double kStandardRates[] = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
constexpr uint32_t kDefaultBlock = 256;
constexpr uint32_t kMinBlock = 16;
constexpr uint32_t kMaxBlock = 8192;
constexpr unsigned kMaxReportedChannels = 64;  // plugins report "any count": shown as stereo

// Silences libasound's stderr diagnostics while the main thread probes devices (a missing card
// or server is an expected outcome there). The default handler is restored afterwards, so no
// pointer into this module stays installed after it unloads.
void silentHandler(const char*, int, const char*, int, const char*, ...) {}
class QuietAlsa {
 public:
  explicit QuietAlsa(const Api* a) : a_(a) {
    if (a_) a_->lib_error_set_handler(&silentHandler);
  }
  ~QuietAlsa() {
    if (a_) a_->lib_error_set_handler(nullptr);
  }
  QuietAlsa(const QuietAlsa&) = delete;
  QuietAlsa& operator=(const QuietAlsa&) = delete;

 private:
  const Api* a_;
};

void copyStr(char* dst, size_t cap, const std::string& s) {
  if (!cap) return;
  size_t n = std::min(s.size(), cap - 1);
  while (n > 0 && n < s.size() && (uint8_t(s[n]) & 0xC0u) == 0x80u) --n;  // keep UTF-8 whole
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

int64_t settingInt(const char* key, int64_t def) {
  IHostContext* h = host();
  return h ? h->getSettings()->getInt(key, def) : def;
}
bool settingBool(const char* key, bool def) {
  IHostContext* h = host();
  return h ? h->getSettings()->getBool(key, def ? SQ_TRUE : SQ_FALSE) != SQ_FALSE : def;
}

std::string stripPrefix(const std::string& id) { return id.rfind("alsa:", 0) == 0 ? id.substr(5) : id; }

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Hints worth showing by default.
bool interesting(const std::string& name) {
  static const char* exact[] = {"default", "pulse", "pipewire", "jack", "null"};
  for (const char* e : exact)
    if (name == e) return true;
  static const char* prefixes[] = {"default:", "sysdefault", "hw:", "plughw:", "hdmi:", "pipewire:", "pulse:"};
  for (const char* p : prefixes)
    if (startsWith(name, p)) return true;
  return false;
}

int sortRank(const std::string& name) {
  if (name == "default") return 0;
  if (name == "pipewire" || name == "pulse" || name == "jack") return 1;
  if (startsWith(name, "default:") || startsWith(name, "sysdefault")) return 2;
  if (startsWith(name, "hw:")) return 3;
  if (startsWith(name, "plughw:")) return 4;
  if (name == "null") return 9;
  return 5;
}

// ---- probing ---------------------------------------------------------------------------------

struct Caps {
  bool ok = false;
  int err = 0;
  unsigned chMin = 0, chMax = 0;
  unsigned rateMin = 0, rateMax = 0;
  unsigned long periodMin = 0, periodMax = 0;
  std::vector<double> rates;
};

Caps probe(const Api* a, const std::string& pcmName, int stream) {
  Caps c;
  snd_pcm_t* pcm = nullptr;
  int r = a->pcm_open(&pcm, pcmName.c_str(), stream, kModeNonblock);
  if (r < 0) {
    c.err = r;
    return c;
  }
  HwParams hw(a);
  if (hw.get() && a->hw_params_any(pcm, hw.get()) >= 0) {
    int dir = 0;
    a->hw_params_get_channels_min(hw.get(), &c.chMin);
    a->hw_params_get_channels_max(hw.get(), &c.chMax);
    a->hw_params_get_rate_min(hw.get(), &c.rateMin, &dir);
    a->hw_params_get_rate_max(hw.get(), &c.rateMax, &dir);
    a->hw_params_get_period_size_min(hw.get(), &c.periodMin, &dir);
    a->hw_params_get_period_size_max(hw.get(), &c.periodMax, &dir);
    for (double rate : kStandardRates)
      if (a->hw_params_test_rate(pcm, hw.get(), static_cast<unsigned>(rate), 0) == 0) c.rates.push_back(rate);
    c.ok = c.chMax > 0;
  } else {
    c.err = -EINVAL;
  }
  a->pcm_close(pcm);
  return c;
}

struct DeviceEntry {
  sq_audio_device_info info{};
  std::string pcm;  // ALSA PCM name
  std::vector<double> rates;
  std::vector<uint32_t> blocks;
  unsigned outChMin = 1, inChMin = 1;
  bool busy = false;
};

unsigned reportedChannels(const Caps& c) {
  if (!c.ok) return 0;
  return c.chMax > kMaxReportedChannels ? std::max(2u, std::min(c.chMin, kMaxReportedChannels)) : c.chMax;
}

// Builds a device entry by opening the PCM in the requested directions. false when neither works.
bool makeEntry(const Api* a, const std::string& pcmName, const std::string& desc, bool wantOut, bool wantIn, DeviceEntry& e) {
  Caps out = wantOut ? probe(a, pcmName, kStreamPlayback) : Caps{};
  Caps in = wantIn ? probe(a, pcmName, kStreamCapture) : Caps{};
  const bool busy = (wantOut && out.err == -EBUSY) || (wantIn && in.err == -EBUSY);
  if (!out.ok && !in.ok && !busy) return false;
  e = DeviceEntry{};
  e.pcm = pcmName;
  e.busy = busy && !out.ok && !in.ok;
  e.info.struct_size = sizeof e.info;
  copyStr(e.info.id, sizeof e.info.id, "alsa:" + pcmName);
  copyStr(e.info.name, sizeof e.info.name, (desc.empty() ? pcmName : desc) + (e.busy ? " (busy)" : ""));
  // A busy device could not be probed: assume stereo in the directions its hint declares.
  e.info.max_output_channels = out.ok ? reportedChannels(out) : (e.busy && wantOut ? 2u : 0u);
  e.info.max_input_channels = in.ok ? reportedChannels(in) : (e.busy && wantIn ? 2u : 0u);
  if (e.info.max_output_channels) e.info.flags |= SQ_AUDIO_DEVICE_OUTPUT;
  if (e.info.max_input_channels) e.info.flags |= SQ_AUDIO_DEVICE_INPUT;
  if (pcmName == "default") e.info.flags |= SQ_AUDIO_DEVICE_DEFAULT_OUTPUT | SQ_AUDIO_DEVICE_DEFAULT_INPUT;
  if (startsWith(pcmName, "hw:")) e.info.flags |= SQ_AUDIO_DEVICE_EXCLUSIVE;  // direct hardware access
  e.outChMin = out.ok ? std::max(1u, out.chMin) : 1u;
  e.inChMin = in.ok ? std::max(1u, in.chMin) : 1u;
  const Caps& p = out.ok ? out : in;
  e.rates = p.rates;
  if (out.ok && in.ok) {  // duplex needs a rate both sides accept
    std::vector<double> both;
    for (double r : out.rates)
      if (std::find(in.rates.begin(), in.rates.end(), r) != in.rates.end()) both.push_back(r);
    if (!both.empty()) e.rates = both;
  }
  if (e.rates.empty()) e.rates = {48000, 44100};
  e.info.default_sample_rate = std::find(e.rates.begin(), e.rates.end(), 48000.0) != e.rates.end()   ? 48000.0
                               : std::find(e.rates.begin(), e.rates.end(), 44100.0) != e.rates.end() ? 44100.0
                                                                                                     : e.rates.front();
  unsigned long pmin = p.ok && p.periodMin ? p.periodMin : kMinBlock;
  unsigned long pmax = p.ok && p.periodMax ? p.periodMax : kMaxBlock;
  pmin = std::clamp<unsigned long>(pmin, kMinBlock, kMaxBlock);
  pmax = std::clamp<unsigned long>(pmax, pmin, kMaxBlock);
  e.info.min_block_frames = static_cast<uint32_t>(pmin);
  e.info.max_block_frames = static_cast<uint32_t>(pmax);
  e.info.default_block_frames = std::clamp<uint32_t>(kDefaultBlock, e.info.min_block_frames, e.info.max_block_frames);
  e.info.block_granularity = 0;
  for (uint32_t b = 16; b <= kMaxBlock; b *= 2)
    if (b >= pmin && b <= pmax) e.blocks.push_back(b);
  if (e.blocks.empty()) e.blocks.push_back(e.info.default_block_frames);
  return true;
}

std::string hintText(const Api* a, void* hint, const char* key) {
  char* s = a->device_name_get_hint(hint, key);
  if (!s) return {};
  std::string v(s);
  std::free(s);
  return v;
}

std::vector<DeviceEntry> enumerate(const Api* a) {
  std::vector<DeviceEntry> out;
  QuietAlsa quiet(a);
  void** hints = nullptr;
  const bool all = settingBool("Services.audiodriver-alsa.ShowAllDevices", false);
  std::vector<std::string> seen;
  if (a->device_name_hint(-1, "pcm", &hints) == 0 && hints) {
    for (void** h = hints; *h; ++h) {
      const std::string name = hintText(a, *h, "NAME");
      if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      if (!all && !interesting(name)) continue;
      seen.push_back(name);
      std::string desc = hintText(a, *h, "DESC");
      for (char& ch : desc)
        if (ch == '\n') ch = ' ';
      const std::string ioid = hintText(a, *h, "IOID");  // "Input", "Output" or empty = both
      DeviceEntry e;
      if (makeEntry(a, name, desc, ioid != "Input", ioid != "Output", e)) out.push_back(std::move(e));
    }
    a->device_name_free_hint(hints);
  }
  std::stable_sort(out.begin(), out.end(), [](const DeviceEntry& x, const DeviceEntry& y) { return sortRank(x.pcm) < sortRank(y.pcm); });
  return out;
}

// ---- shared state between the driver and its devices -----------------------------------------

class Hub {
 public:
  sq_result addListener(IAudioDeviceListener* l) {
    if (!l) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lk(mu_);
    listeners_.push_back(Ptr<IAudioDeviceListener>(l));
    return SQ_OK;
  }
  sq_result removeListener(IAudioDeviceListener* l) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
      if (it->get() == l) {
        listeners_.erase(it);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }
  std::vector<Ptr<IAudioDeviceListener>> snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    return listeners_;
  }

 private:
  std::mutex mu_;
  std::vector<Ptr<IAudioDeviceListener>> listeners_;
};

// ---- one opened PCM direction -----------------------------------------------------------------

struct Pcm {
  snd_pcm_t* h = nullptr;
  Fmt fmt = Fmt::Unknown;
  unsigned channels = 0;  // device channels opened
  unsigned long period = 0;
  unsigned long buffer = 0;
  unsigned rate = 0;
  int type = 0;
  std::vector<uint8_t> bytes;  // one period, interleaved

  size_t frameBytes() const { return size_t(bytesPerSample(fmt)) * channels; }
};

int alsaFormat(Fmt f) {
  switch (f) {
    case Fmt::F32: return kFormatFloatLe;
    case Fmt::S32: return kFormatS32Le;
    case Fmt::S24_3: return kFormatS24_3Le;
    case Fmt::S24_32: return kFormatS24Le;
    case Fmt::S16: return kFormatS16Le;
    default: return -1;
  }
}

sq_sample_format sqFormat(Fmt f) {
  switch (f) {
    case Fmt::F32: return SQ_SAMPLE_F32;
    case Fmt::S16: return SQ_SAMPLE_I16;
    case Fmt::S24_3: return SQ_SAMPLE_I24;
    case Fmt::S24_32: return SQ_SAMPLE_I24_IN32;
    case Fmt::S32: return SQ_SAMPLE_I32;
    default: return SQ_SAMPLE_UNKNOWN;
  }
}

sq_result mapErr(int err) {
  switch (-err) {
    case EBUSY: return SQ_E_BUSY;
    case ENOENT:
    case ENODEV:
    case ENXIO: return SQ_E_NOTFOUND;
    case EACCES:
    case EPERM: return SQ_E_ACCESS;
    case EINVAL: return SQ_E_UNSUPPORTED;
    case ENOMEM: return SQ_E_OUTOFMEMORY;
    default: return SQ_E_FAIL;
  }
}

// Opens and configures one direction. rate/period are requests; the granted values are in `p`.
sq_result openPcm(const Api* a, const std::string& name, int stream, unsigned wantChannels, unsigned rate, unsigned long period,
                  unsigned periods, Pcm& p, std::string& err) {
  int r = a->pcm_open(&p.h, name.c_str(), stream, kModeNonblock);
  if (r < 0) {
    p.h = nullptr;
    err = "open: " + errorText(r);
    return mapErr(r);
  }
  auto fail = [&](const char* what, int code) {
    err = std::string(what) + ": " + errorText(code);
    a->pcm_close(p.h);
    p.h = nullptr;
    return mapErr(code);
  };
  HwParams hw(a);
  if (!hw.get()) return fail("hw params", -ENOMEM);
  if ((r = a->hw_params_any(p.h, hw.get())) < 0) return fail("hw params", r);
  if ((r = a->hw_params_set_access(p.h, hw.get(), kAccessRwInterleaved)) < 0) return fail("interleaved access", r);
  p.fmt = Fmt::Unknown;
  for (Fmt f : {Fmt::F32, Fmt::S32, Fmt::S24_3, Fmt::S24_32, Fmt::S16})
    if (a->hw_params_test_format(p.h, hw.get(), alsaFormat(f)) == 0) {
      p.fmt = f;
      break;
    }
  if (p.fmt == Fmt::Unknown) return fail("sample format (none of FLOAT/S32/S24_3/S24/S16 LE)", -EINVAL);
  if ((r = a->hw_params_set_format(p.h, hw.get(), alsaFormat(p.fmt))) < 0) return fail("sample format", r);
  unsigned chMin = 1, chMax = 1;
  a->hw_params_get_channels_min(hw.get(), &chMin);
  a->hw_params_get_channels_max(hw.get(), &chMax);
  p.channels = std::clamp(wantChannels, std::max(1u, chMin), std::max(1u, chMax));
  if ((r = a->hw_params_set_channels(p.h, hw.get(), p.channels)) < 0) return fail("channels", r);
  a->hw_params_set_rate_resample(p.h, hw.get(), 1);  // plug devices may resample
  unsigned rr = rate;
  int dir = 0;
  if ((r = a->hw_params_set_rate_near(p.h, hw.get(), &rr, &dir)) < 0) return fail("sample rate", r);
  p.rate = rr;
  snd_pcm_uframes_t per = period;
  dir = 0;
  if ((r = a->hw_params_set_period_size_near(p.h, hw.get(), &per, &dir)) < 0) return fail("period size", r);
  snd_pcm_uframes_t buf = per * periods;
  if ((r = a->hw_params_set_buffer_size_near(p.h, hw.get(), &buf)) < 0) return fail("buffer size", r);
  if ((r = a->hw_params(p.h, hw.get())) < 0) return fail("apply hw params", r);
  dir = 0;
  a->hw_params_get_period_size(hw.get(), &per, &dir);
  a->hw_params_get_buffer_size(hw.get(), &buf);
  p.period = per;
  p.buffer = buf;
  if (p.period == 0 || p.buffer < p.period) return fail("period/buffer", -EINVAL);

  SwParams sw(a);
  if (!sw.get()) return fail("sw params", -ENOMEM);
  if ((r = a->sw_params_current(p.h, sw.get())) < 0) return fail("sw params", r);
  // Playback starts once the pre-filled buffer is full; capture is started explicitly.
  a->sw_params_set_start_threshold(p.h, sw.get(), stream == kStreamPlayback ? p.buffer : p.buffer * 2);
  a->sw_params_set_avail_min(p.h, sw.get(), p.period);
  if ((r = a->sw_params(p.h, sw.get())) < 0) return fail("apply sw params", r);
  p.type = a->pcm_type(p.h);
  return SQ_OK;
}

void closePcm(const Api* a, Pcm& p) {
  if (p.h) {
    a->pcm_drop(p.h);
    a->pcm_close(p.h);
  }
  p = Pcm{};
}

// ---- the device -----------------------------------------------------------------------------

class AlsaDevice final : public sq::Object<AlsaDevice, IAudioDevice> {
 public:
  AlsaDevice(const Api* a, std::shared_ptr<Hub> hub, DeviceEntry e) : a_(a), hub_(std::move(hub)), entry_(std::move(e)) {}
  ~AlsaDevice() override {
    stopImpl();
    releasePcms();
  }

  sq_result SQ_CALL getInfo(sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    *out = entry_.info;
    out->struct_size = sizeof(sq_audio_device_info);
    return SQ_OK;
  }
  sq_result SQ_CALL getSupportedSampleRates(double* rates, uint32_t capacity, uint32_t* count) override {
    return fillList(entry_.rates, rates, capacity, count);
  }
  sq_result SQ_CALL getSupportedBlockSizes(uint32_t* sizes, uint32_t capacity, uint32_t* count) override {
    return fillList(entry_.blocks, sizes, capacity, count);
  }
  sq_result SQ_CALL getChannelName(sq_bool input, uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override {
    const uint32_t n = input ? entry_.info.max_input_channels : entry_.info.max_output_channels;
    if (index >= n) return SQ_E_NOTFOUND;
    const std::string name = (input ? "In " : "Out ") + std::to_string(index + 1);
    const uint32_t need = static_cast<uint32_t>(name.size() + 1);
    if (needed) *needed = need;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, name.c_str(), need);
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
      const sq_audio_device_info& i = entry_.info;
      if (input) *input = sq_stream_format{sizeof(sq_stream_format), i.default_sample_rate, i.max_input_channels, i.default_block_frames, SQ_SAMPLE_F32};
      if (output)
        *output = sq_stream_format{sizeof(sq_stream_format), i.default_sample_rate, i.max_output_channels, i.default_block_frames, SQ_SAMPLE_F32};
      return SQ_S_FALSE;
    }
    if (input) *input = sq_stream_format{sizeof(sq_stream_format), double(rate_), static_cast<uint32_t>(inMap_.size()), block_, SQ_SAMPLE_F32};
    if (output) *output = sq_stream_format{sizeof(sq_stream_format), double(rate_), static_cast<uint32_t>(outMap_.size()), block_, SQ_SAMPLE_F32};
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
    if (!configured_ || lost_.load()) {
      // Default configuration (also re-opens a device that was lost and is back).
      std::vector<uint32_t> ins, outs;
      for (uint32_t i = 0; i < std::min(entry_.info.max_output_channels, 2u); ++i) outs.push_back(i);
      if (outs.empty())
        for (uint32_t i = 0; i < std::min(entry_.info.max_input_channels, 2u); ++i) ins.push_back(i);
      sq_audio_device_config rq{};
      rq.struct_size = sizeof rq;
      rq.input_channels = ins.data();
      rq.input_channel_count = static_cast<uint32_t>(ins.size());
      rq.output_channels = outs.data();
      rq.output_channel_count = static_cast<uint32_t>(outs.size());
      if (configured_) {  // keep the previous request's shape
        rq.sample_rate = rate_;
        rq.block_frames = block_;
        rq.input_channels = lastIn_.data();
        rq.input_channel_count = static_cast<uint32_t>(lastIn_.size());
        rq.output_channels = lastOut_.data();
        rq.output_channel_count = static_cast<uint32_t>(lastOut_.size());
      }
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
    quit_.store(false);
    lost_.store(false);
    lostNotified_.store(false);
    rtGranted_.store(-1);
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
    for (int i = 0; i < 200 && rtGranted_.load() < 0; ++i) sq_thread_sleep_ms(1);
    sq::logf(SQ_LOG_INFO, "%s streaming (%s scheduling)", entry_.info.id, rtGranted_.load() == 1 ? "SCHED_FIFO" : "normal");
    return SQ_OK;
  }

  sq_result SQ_CALL stop() override {
    if (thread_.joinable() && std::this_thread::get_id() == thread_.get_id()) return SQ_E_BUSY;
    if (!callback_) return SQ_S_FALSE;
    stopImpl();
    return SQ_OK;
  }

  sq_bool SQ_CALL isRunning() override { return (running_.load() && !lost_.load()) ? SQ_TRUE : SQ_FALSE; }
  uint64_t SQ_CALL getXrunCount() override { return xruns_.load(); }
  sq_result SQ_CALL showControlPanel(void*) override { return SQ_E_NOTIMPL; }

 private:
  template <class T>
  static sq_result fillList(const std::vector<T>& v, T* out, uint32_t capacity, uint32_t* count) {
    if (count) *count = static_cast<uint32_t>(v.size());
    if (!out) return capacity ? SQ_E_INVALIDARG : SQ_OK;
    const size_t n = std::min<size_t>(capacity, v.size());
    std::copy(v.begin(), v.begin() + static_cast<ptrdiff_t>(n), out);
    return n < v.size() ? SQ_E_BUFFER_TOO_SMALL : SQ_OK;
  }

  void releasePcms() {
    if (linked_ && cap_.h) a_->pcm_unlink(cap_.h);
    linked_ = false;
    closePcm(a_, out_);
    closePcm(a_, cap_);
    configured_ = false;
  }

  sq_result configureImpl(const sq_audio_device_config& req, sq_audio_device_config* actual) {
    releasePcms();
    if ((req.output_channel_count && !req.output_channels) || (req.input_channel_count && !req.input_channels)) return SQ_E_INVALIDARG;
    bool differs = false;
    std::vector<uint32_t> outs, ins;
    for (uint32_t i = 0; i < req.output_channel_count; ++i) {
      const uint32_t c = req.output_channels[i];
      if (c < entry_.info.max_output_channels && std::find(outs.begin(), outs.end(), c) == outs.end()) outs.push_back(c);
      else differs = true;
    }
    for (uint32_t i = 0; i < req.input_channel_count; ++i) {
      const uint32_t c = req.input_channels[i];
      if (c < entry_.info.max_input_channels && std::find(ins.begin(), ins.end(), c) == ins.end()) ins.push_back(c);
      else differs = true;
    }
    if (outs.empty() && ins.empty()) return SQ_E_INVALIDARG;
    lastOut_ = outs;
    lastIn_ = ins;

    const unsigned rate = static_cast<unsigned>((req.sample_rate > 0 ? req.sample_rate : entry_.info.default_sample_rate) + 0.5);
    const uint32_t block = std::clamp<uint32_t>(req.block_frames ? req.block_frames : entry_.info.default_block_frames, kMinBlock, kMaxBlock);
    int64_t periods = settingInt("Services.audiodriver-alsa.Periods", 0);
    if (periods <= 0) periods = startsWith(entry_.pcm, "hw:") ? 2 : 3;
    periods = std::clamp<int64_t>(periods, 2, 16);

    QuietAlsa quiet(a_);
    std::string err;
    if (!outs.empty()) {
      const unsigned need = std::max(*std::max_element(outs.begin(), outs.end()) + 1, entry_.outChMin);
      sq_result r = openPcm(a_, entry_.pcm, kStreamPlayback, need, rate, block, static_cast<unsigned>(periods), out_, err);
      if (SQ_FAILED(r)) {
        sq::logf(SQ_LOG_WARN, "%s playback: %s", entry_.pcm.c_str(), err.c_str());
        releasePcms();
        return r;
      }
    }
    if (!ins.empty()) {
      const unsigned need = std::max(*std::max_element(ins.begin(), ins.end()) + 1, entry_.inChMin);
      const unsigned crate = out_.h ? out_.rate : rate;
      const unsigned long cper = out_.h ? out_.period : block;
      // Capture gets more periods: it only has to hold data until the next read.
      sq_result r = openPcm(a_, entry_.pcm, kStreamCapture, need, crate, cper, static_cast<unsigned>(std::max<int64_t>(periods, 4)), cap_, err);
      if (SQ_SUCCEEDED(r) && out_.h && cap_.rate != out_.rate) {
        err = "capture rate " + std::to_string(cap_.rate) + " != playback rate " + std::to_string(out_.rate);
        closePcm(a_, cap_);
        r = SQ_E_UNSUPPORTED;
      }
      if (SQ_FAILED(r)) {
        if (!out_.h) {
          sq::logf(SQ_LOG_WARN, "%s capture: %s", entry_.pcm.c_str(), err.c_str());
          releasePcms();
          return r;
        }
        // Output works: carry on without inputs and tell the caller (SQ_S_FALSE).
        sq::logf(SQ_LOG_WARN, "%s capture unavailable (%s); output only", entry_.pcm.c_str(), err.c_str());
        ins.clear();
        differs = true;
      }
    }
    if (out_.h && cap_.h) linked_ = a_->pcm_link(cap_.h, out_.h) == 0;

    rate_ = out_.h ? out_.rate : cap_.rate;
    block_ = static_cast<uint32_t>(out_.h ? out_.period : cap_.period);
    outMap_ = outs;
    inMap_ = ins;
    pace_ = (!out_.h || out_.type == kPcmTypeNull) && (!cap_.h || cap_.type == kPcmTypeNull);
    // Output: the whole pre-filled buffer is queued ahead of the DAC. Input: process() sees a
    // period once it has been fully captured.
    latencyOut_ = out_.h ? static_cast<uint32_t>(out_.buffer) : 0;
    latencyIn_ = cap_.h ? block_ : 0;
    try {
      if (out_.h) out_.bytes.assign(out_.frameBytes() * block_, 0);
      if (cap_.h) cap_.bytes.assign(cap_.frameBytes() * block_, 0);
      outBufs_.assign(outMap_.size(), std::vector<float>(block_, 0.0f));
      inBufs_.assign(inMap_.size(), std::vector<float>(block_, 0.0f));
      outPtrs_.clear();
      inPtrs_.clear();
      for (auto& b : outBufs_) outPtrs_.push_back(b.data());
      for (auto& b : inBufs_) inPtrs_.push_back(b.data());
    } catch (...) {
      releasePcms();
      return SQ_E_OUTOFMEMORY;
    }
    configured_ = true;
    *actual = req;
    actual->struct_size = sizeof(sq_audio_device_config);
    actual->sample_rate = rate_;
    actual->block_frames = block_;
    actual->input_channels = inMap_.empty() ? nullptr : inMap_.data();
    actual->input_channel_count = static_cast<uint32_t>(inMap_.size());
    actual->output_channels = outMap_.empty() ? nullptr : outMap_.data();
    actual->output_channel_count = static_cast<uint32_t>(outMap_.size());
    actual->device_format = sqFormat(out_.h ? out_.fmt : cap_.fmt);
    if (req.sample_rate > 0 && static_cast<unsigned>(req.sample_rate + 0.5) != rate_) differs = true;
    if (req.block_frames && req.block_frames != block_) differs = true;
    sq::logf(SQ_LOG_INFO, "configured %s: %u Hz, %u frames, %s, out %u/%u ch (buffer %lu), in %u/%u ch, latency in/out %u/%u%s",
             entry_.pcm.c_str(), rate_, block_, fmtName(out_.h ? out_.fmt : cap_.fmt), static_cast<unsigned>(outMap_.size()), out_.channels,
             out_.buffer, static_cast<unsigned>(inMap_.size()), cap_.channels, latencyIn_, latencyOut_, pace_ ? " (clock-paced null PCM)" : "");
    return differs ? SQ_S_FALSE : SQ_OK;
  }

  void stopImpl() {
    if (thread_.joinable()) {
      quit_.store(true);
      thread_.join();
    }
    running_.store(false);
    if (out_.h) a_->pcm_drop(out_.h);
    if (cap_.h) a_->pcm_drop(cap_.h);
    if (callback_) {
      callback_->onStop();
      callback_->release();
      callback_ = nullptr;
    }
  }

  // ---- stream thread (RT rules apply inside the loop) -----------------------------------------

  enum : int { kOk = 0, kXrun = 1, kQuit = 2, kFatal = 3 };

  // Moves exactly `frames` frames; waits (in 100 ms slices, watching quit_) while the device is
  // not ready.
  int transfer(Pcm& p, uint8_t* data, uint32_t frames, bool write, int* fatalErr) noexcept {
    const size_t fb = p.frameBytes();
    uint32_t done = 0;
    while (done < frames) {
      if (quit_.load(std::memory_order_relaxed)) return kQuit;
      snd_pcm_sframes_t r = write ? a_->pcm_writei(p.h, data + done * fb, frames - done) : a_->pcm_readi(p.h, data + done * fb, frames - done);
      if (r > 0) {
        done += static_cast<uint32_t>(r);
        continue;
      }
      if (r == 0 || r == -EAGAIN) {
        const int w = a_->pcm_wait(p.h, 100);
        if (w >= 0) continue;
        r = w;
      }
      if (r == -EINTR) continue;
      if (r == -EPIPE || r == -ESTRPIPE) return kXrun;
      *fatalErr = static_cast<int>(r);
      return kFatal;
    }
    return kOk;
  }

  // (Re)starts both streams from a prepared state: playback pre-filled with silence.
  int restart(int* fatalErr) noexcept {
    if (out_.h) {
      a_->pcm_drop(out_.h);
      int r = a_->pcm_prepare(out_.h);
      if (r < 0) {
        r = a_->pcm_recover(out_.h, r, 1);
        if (r < 0) {
          *fatalErr = r;
          return kFatal;
        }
      }
    }
    if (cap_.h) {
      if (!linked_ || !out_.h) a_->pcm_drop(cap_.h);
      int r = a_->pcm_prepare(cap_.h);
      if (r < 0 && (r = a_->pcm_recover(cap_.h, r, 1)) < 0) {
        *fatalErr = r;
        return kFatal;
      }
    }
    if (out_.h) {
      std::memset(out_.bytes.data(), 0, out_.bytes.size());
      unsigned long left = out_.buffer;
      while (left > 0) {
        const uint32_t n = static_cast<uint32_t>(std::min<unsigned long>(left, block_));
        const int t = transfer(out_, out_.bytes.data(), n, true, fatalErr);
        if (t != kOk) return t;
        left -= n;
      }
    }
    if (cap_.h && (!linked_ || !out_.h)) {
      const int r = a_->pcm_start(cap_.h);
      if (r < 0 && r != -EBADFD) {  // EBADFD: already running through the link
        *fatalErr = r;
        return kFatal;
      }
    }
    return kOk;
  }

  void streamMain() {
    sq_thread_set_current_name("sq-alsa-audio");
    rtGranted_.store(sq_thread_set_current_priority(SQ_PRIORITY_REALTIME) == SQ_OK ? 1 : 0);
    int fatalErr = 0;
    int state = restart(&fatalErr);
    uint64_t deviceTime = 0;
    bool pendingXrun = false;
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    const uint64_t periodNs = rate_ ? uint64_t(block_) * 1000000000ull / rate_ : 0;

    sq_audio_block inBlock{inPtrs_.data(), static_cast<uint32_t>(inPtrs_.size()), block_, 0};
    sq_audio_block outBlock{outPtrs_.data(), static_cast<uint32_t>(outPtrs_.size()), block_, 0};
    while (state == kOk || state == kXrun) {
      if (state == kXrun) {
        xruns_.fetch_add(1, std::memory_order_relaxed);
        pendingXrun = true;
        state = restart(&fatalErr);
        clock_gettime(CLOCK_MONOTONIC, &next);
        continue;
      }
      if (quit_.load(std::memory_order_relaxed)) break;
      if (cap_.h) {
        state = transfer(cap_, cap_.bytes.data(), block_, false, &fatalErr);
        if (state != kOk) continue;
        deinterleave(cap_.fmt, cap_.bytes.data(), cap_.channels, inPtrs_.data(), inMap_.data(), static_cast<uint32_t>(inMap_.size()), block_);
      }
      sq_audio_io io{};
      io.struct_size = sizeof io;
      inBlock.position = outBlock.position = static_cast<sq_sample_time>(deviceTime);
      io.input = inPtrs_.empty() ? nullptr : &inBlock;
      io.output = &outBlock;
      io.device_time = static_cast<sq_sample_time>(deviceTime);
      io.host_time_ns = sq_time_monotonic_ns();
      io.flags = pendingXrun ? SQ_AUDIO_IO_XRUN : 0u;
      pendingXrun = false;
      callback_->process(&io);
      deviceTime += block_;
      if (out_.h) {
        interleave(out_.fmt, outPtrs_.data(), outMap_.data(), static_cast<uint32_t>(outMap_.size()), out_.bytes.data(), out_.channels, block_);
        state = transfer(out_, out_.bytes.data(), block_, true, &fatalErr);
        if (state != kOk) continue;
      }
      if (pace_ && periodNs) {
        uint64_t ns = uint64_t(next.tv_nsec) + periodNs;
        next.tv_sec += static_cast<time_t>(ns / 1000000000ull);
        next.tv_nsec = static_cast<long>(ns % 1000000000ull);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now;  // fell far behind: do not burst to catch up
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr) == EINTR) {
        }
      }
    }
    // Off the realtime path from here on.
    if (state == kFatal) {
      lost_.store(true);
      sq::logf(SQ_LOG_ERROR, "%s stopped: %s", entry_.pcm.c_str(), errorText(fatalErr).c_str());
      if (!lostNotified_.exchange(true))
        for (auto& l : hub_->snapshot()) l->onDeviceReset(this, SQ_AUDIO_RESET_DEVICE_LOST);
    }
  }

  const Api* a_;
  std::shared_ptr<Hub> hub_;
  DeviceEntry entry_;
  Pcm out_, cap_;
  bool linked_ = false;
  bool configured_ = false;
  bool pace_ = false;
  unsigned rate_ = 0;
  uint32_t block_ = 0;
  uint32_t latencyIn_ = 0, latencyOut_ = 0;
  std::vector<uint32_t> outMap_, inMap_, lastOut_, lastIn_;
  std::vector<std::vector<float>> outBufs_, inBufs_;
  std::vector<float*> outPtrs_, inPtrs_;
  IAudioCallback* callback_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false}, quit_{false}, lost_{false}, lostNotified_{false};
  std::atomic<int> rtGranted_{-1};
  std::atomic<uint64_t> xruns_{0};
};

// ---- the driver -----------------------------------------------------------------------------

class AlsaDriver final : public sq::Object<AlsaDriver, IAudioDriver> {
 public:
  AlsaDriver() : hub_(std::make_shared<Hub>()) {}

  sq_result SQ_CALL getDriverInfo(sq_audio_driver_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_driver_info)) return SQ_E_INVALIDARG;
    *out = sq_audio_driver_info{};
    out->struct_size = sizeof(sq_audio_driver_info);
    copyStr(out->name, sizeof out->name, "ALSA");
    return SQ_OK;
  }

  sq_result SQ_CALL refreshDevices() override {
    std::string why;
    const Api* a = load(&why);
    std::vector<DeviceEntry> fresh;
    if (!a) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) sq::logf(SQ_LOG_WARN, "ALSA unavailable, 0 devices: %s", why.c_str());
    } else {
      fresh = enumerate(a);
    }
    bool changed;
    {
      std::lock_guard<std::mutex> lk(mu_);
      changed = fresh.size() != devices_.size() ||
                !std::equal(fresh.begin(), fresh.end(), devices_.begin(),
                            [](const DeviceEntry& x, const DeviceEntry& y) { return std::strcmp(x.info.id, y.info.id) == 0; });
      devices_ = std::move(fresh);
      enumerated_ = true;
    }
    if (changed && hadList_)
      for (auto& l : hub_->snapshot()) l->onDeviceListChanged();
    hadList_ = true;
    return SQ_OK;
  }
  uint32_t SQ_CALL getDeviceCount() override {
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<uint32_t>(devices_.size());
  }
  sq_result SQ_CALL getDeviceInfo(uint32_t index, sq_audio_device_info* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_device_info)) return SQ_E_INVALIDARG;
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    if (index >= devices_.size()) return SQ_E_NOTFOUND;
    *out = devices_[index].info;
    out->struct_size = sizeof(sq_audio_device_info);
    return SQ_OK;
  }
  sq_result SQ_CALL openDevice(const char* deviceId, IAudioDevice** out) override {
    if (!deviceId || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    const Api* a = load();
    if (!a) return SQ_E_UNSUPPORTED;
    const std::string pcm = stripPrefix(deviceId);
    if (pcm.empty()) return SQ_E_NOTFOUND;
    ensure();
    DeviceEntry e;
    bool found = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (const auto& d : devices_)
        if (d.pcm == pcm) {
          e = d;
          found = !d.busy;
        }
    }
    // Not listed (custom name) or busy at enumeration time: probe it now.
    if (!found) {
      QuietAlsa quiet(a);
      if (!makeEntry(a, pcm, std::string(), true, true, e) || e.busy) return e.busy ? SQ_E_BUSY : SQ_E_NOTFOUND;
    }
    auto dev = Ptr<AlsaDevice>::adopt(new (std::nothrow) AlsaDevice(a, hub_, std::move(e)));
    if (!dev) return SQ_E_OUTOFMEMORY;
    *out = dev.detach();
    return SQ_OK;
  }
  sq_result SQ_CALL addListener(IAudioDeviceListener* l) override { return hub_->addListener(l); }
  sq_result SQ_CALL removeListener(IAudioDeviceListener* l) override { return hub_->removeListener(l); }

 private:
  void ensure() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (enumerated_) return;
    }
    refreshDevices();
  }

  std::shared_ptr<Hub> hub_;
  std::mutex mu_;
  std::vector<DeviceEntry> devices_;
  bool enumerated_ = false;
  bool hadList_ = false;
};

}  // namespace

sq_result createDriver(const sq_guid* riid, void** out) { return sq::createObject<AlsaDriver>(riid, out); }

}  // namespace sq::alsa
