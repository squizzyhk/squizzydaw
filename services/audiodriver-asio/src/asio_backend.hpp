// Squizzy Studio - ASIO driver service: backend seam.
//
// The device logic (asio_driver.cpp) talks to one loaded ASIO driver through `Backend`. The real
// implementation (asio_sdk_backend.cpp) wraps the driver's COM object and is compiled only when
// the build is configured with -DSQ_ASIO_SDK_DIR. Tests use a fake Backend, so configuration,
// streaming, conversion, message routing and listener notification are tested without the SDK.
//
// Threading: every Backend method is called on the control thread that created it (ASIO drivers
// are apartment-bound), except outputReady(), which the BackendSink's bufferSwitch may call from
// the driver's realtime thread. The sink is invoked on driver threads (bufferSwitch on the
// realtime thread; message / sampleRateChanged on any thread).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "asio_registry.hpp"
#include "asio_types.hpp"
#include "sq/types.h"

namespace sq::asio {

struct BufferSizes {
  int32_t minFrames = 0;
  int32_t maxFrames = 0;
  int32_t preferredFrames = 0;
  int32_t granularity = 0;  // 0: min==max==preferred; -1: powers of two; > 0: step
};

struct ChannelInfo {
  SampleType type = SampleType::Float32LSB;
  int32_t group = 0;
  bool active = false;
  char name[32] = {};
};

struct ChannelRequest {
  bool input = false;
  int32_t channel = 0;  // device channel index
};

// Double buffer of one opened channel as handed out by the driver.
struct ChannelBuffers {
  void* half[2] = {nullptr, nullptr};
};

// Implemented by the device; called by the driver.
class BackendSink {
 public:
  // REALTIME thread. index = which half (0/1) to process. samplePosition / systemTimeNs are
  // valid only when the corresponding flag is true.
  virtual void bufferSwitch(int32_t index, bool directProcess, bool positionValid, int64_t samplePosition,
                            bool timeValid, int64_t systemTimeNs) noexcept = 0;
  virtual void sampleRateChanged(double rate) noexcept = 0;
  // Host message callback (selectors in asio_types.hpp).
  virtual int32_t message(int32_t selector, int32_t value) noexcept = 0;

 protected:
  ~BackendSink() = default;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual std::string driverName() = 0;
  virtual sq_result getChannels(int32_t& inputs, int32_t& outputs) = 0;
  virtual sq_result getLatencies(int32_t& input, int32_t& output) = 0;
  virtual sq_result getBufferSize(BufferSizes& out) = 0;
  virtual sq_result canSampleRate(double rate) = 0;
  virtual sq_result getSampleRate(double& rate) = 0;
  virtual sq_result setSampleRate(double rate) = 0;
  virtual sq_result getChannelInfo(bool input, int32_t channel, ChannelInfo& out) = 0;
  // Creates the double buffers for `count` channels; `out[i]` receives the buffers of reqs[i].
  // The sink receives callbacks from the next start() until disposeBuffers().
  virtual sq_result createBuffers(const ChannelRequest* reqs, int32_t count, int32_t blockFrames, ChannelBuffers* out,
                                  BackendSink* sink) = 0;
  virtual sq_result disposeBuffers() = 0;
  virtual sq_result start() = 0;
  virtual sq_result stop() = 0;
  // Whether the driver supports the outputReady() optimisation (probed once after createBuffers).
  virtual bool supportsOutputReady() = 0;
  virtual void outputReady() noexcept = 0;  // REALTIME thread
  virtual sq_result controlPanel() = 0;
  virtual std::string lastError() = 0;
};

// Loads the driver of `entry` on the calling thread. On failure returns null and sets *error.
using BackendFactory = std::function<std::unique_ptr<Backend>(const DriverEntry& entry, std::string* error)>;

#if defined(SQ_HAVE_ASIO_SDK) && SQ_HAVE_ASIO_SDK
// The real host (asio_sdk_backend.cpp): COM-instantiates the driver's CLSID and talks to it.
BackendFactory makeSdkBackendFactory();
#endif

}  // namespace sq::asio
