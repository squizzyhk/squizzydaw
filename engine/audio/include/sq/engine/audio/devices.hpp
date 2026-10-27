// Squizzy Studio engine/audio - in-process test devices implementing sq::IAudioDevice.
//
// NullDevice   : a device clock on an AudioRenderThread. Paced at `speed` x realtime (1.0 =
//                realtime, 16.0 = sixteen times faster, 0 = free running). Input comes from an
//                optional generator, output goes to an optional sink. A callback that finishes
//                more than one block period after its deadline counts as an xrun (like a
//                double-buffered device) and flags the next sq_audio_io with SQ_AUDIO_IO_XRUN.
// OfflineDevice: no thread; the caller drives callbacks with pump(). Deterministic.
// NullDriver   : IAudioDriver exposing "null" (NullDevice, realtime) and "offline" devices so
//                the engine's openDevice() path works without hardware; register it with
//                registerNullDriver() (tests / headless). Category "AudioDriver".
//
// Generators/sinks are called on the device thread: they must be RT safe.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/interfaces/audiodevice.h"
#include "sq/engine/audio/render_thread.hpp"

namespace sq::engine::audio {

inline constexpr sq_guid kNullDriverClassId = sq::guid("{9AC93497-373A-40DF-B785-0A6ED04DC352}");

struct NullDeviceOptions {
  std::string id = "null";
  std::string name = "Null Device";
  double sampleRate = 48000.0;
  uint32_t blockFrames = 256;
  uint32_t inputChannels = 2;
  uint32_t outputChannels = 2;
  uint32_t inputLatency = 0;    // reported by getLatency (frames, on top of one block)
  uint32_t outputLatency = 0;
  double speed = 1.0;           // NullDevice pacing; 0 = free running
  bool offline = false;         // true: OfflineDevice behaviour (pump())
  // (devicePosition, channels, channelCount, frames): fill device input.
  std::function<void(int64_t, float* const*, uint32_t, uint32_t)> inputGenerator;
  // (devicePosition, channels, channelCount, frames): observe device output.
  std::function<void(int64_t, const float* const*, uint32_t, uint32_t)> outputSink;
};

// Listeners of a NullDriver, shared with the devices it opened (control thread only).
using NullListenerList = std::vector<sq::Ptr<IAudioDeviceListener>>;

class NullDevice : public sq::Object<NullDevice, IAudioDevice> {
 public:
  explicit NullDevice(NullDeviceOptions opt);
  ~NullDevice() override;

  sq_result SQ_CALL getInfo(sq_audio_device_info* out) override;
  sq_result SQ_CALL getSupportedSampleRates(double* rates, uint32_t capacity, uint32_t* count) override;
  sq_result SQ_CALL getSupportedBlockSizes(uint32_t* sizes, uint32_t capacity, uint32_t* count) override;
  sq_result SQ_CALL getChannelName(sq_bool input, uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL configure(const sq_audio_device_config* request, sq_audio_device_config* actual) override;
  sq_result SQ_CALL getStreamFormat(sq_stream_format* input, sq_stream_format* output) override;
  sq_result SQ_CALL getLatency(sq_latency_info* out) override;
  sq_result SQ_CALL start(IAudioCallback* callback) override;
  sq_result SQ_CALL stop() override;
  sq_bool SQ_CALL isRunning() override;
  uint64_t SQ_CALL getXrunCount() override;
  sq_result SQ_CALL showControlPanel(void* parentWindow) override;

  // Offline mode: runs `blocks` callbacks on the calling thread. Returns blocks run.
  uint32_t pump(uint32_t blocks);
  int64_t position() const noexcept { return pos_.load(); }
  uint64_t callbacks() const noexcept { return callbacks_.load(); }
  const AudioRenderThread& renderThread() const noexcept { return thread_; }
  NullDeviceOptions& options() noexcept { return opt_; }
  // Test hook: behaves like a hardware endpoint that disappeared while streaming (WASAPI stream
  // error / unplug). The device thread stops WITHOUT calling IAudioCallback::onStop, isRunning()
  // turns false, and the driver's listeners get onDeviceReset(SQ_AUDIO_RESET_DEVICE_LOST).
  // A later stop() still calls onStop() (as the real drivers do). Control thread.
  void simulateLoss();
  bool lost() const noexcept { return lost_.load(); }
  void setListeners(std::shared_ptr<NullListenerList> l) { listeners_ = std::move(l); }

 private:
  void runOne(uint32_t flags) noexcept;
  void threadBody(const std::atomic<bool>& stop) noexcept;

  NullDeviceOptions opt_;
  std::vector<uint32_t> inChans_, outChans_;
  std::vector<float> inBuf_, outBuf_;
  std::vector<float*> inPtrs_, outPtrs_;
  IAudioCallback* cb_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> lost_{false};
  std::shared_ptr<NullListenerList> listeners_;
  std::atomic<int64_t> pos_{0};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint64_t> callbacks_{0};
  AudioRenderThread thread_;
};

// Driver over a fixed list of NullDevice option sets.
class NullDriver final : public sq::Object<NullDriver, IAudioDriver> {
 public:
  NullDriver();
  explicit NullDriver(std::vector<NullDeviceOptions> devices) : devices_(std::move(devices)) {}
  ~NullDriver() override;
  sq_result SQ_CALL getDriverInfo(sq_audio_driver_info* out) override;
  sq_result SQ_CALL refreshDevices() override { return SQ_OK; }
  uint32_t SQ_CALL getDeviceCount() override { return static_cast<uint32_t>(devices_.size()); }
  sq_result SQ_CALL getDeviceInfo(uint32_t index, sq_audio_device_info* out) override;
  sq_result SQ_CALL openDevice(const char* deviceId, IAudioDevice** out) override;
  sq_result SQ_CALL addListener(IAudioDeviceListener* listener) override;
  sq_result SQ_CALL removeListener(IAudioDeviceListener* listener) override;
  // Process-wide default device list used by the class-registry factory (tests tweak it).
  static std::vector<NullDeviceOptions>& defaults();

 private:
  std::vector<NullDeviceOptions> devices_;
  std::shared_ptr<NullListenerList> listeners_ = std::make_shared<NullListenerList>();
};

// Registers NullDriver (kNullDriverClassId, category "AudioDriver") in `reg`.
sq_result registerNullDriver(IClassRegistry* reg);
sq_result unregisterNullDriver(IClassRegistry* reg);

}  // namespace sq::engine::audio
