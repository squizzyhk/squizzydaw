// Squizzy Studio - ASIO driver service: IAudioDriver / IAudioDevice on top of the Backend seam.
// SDK-independent: compiled in every build and unit-tested with a fake Backend.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "asio_backend.hpp"
#include "asio_registry.hpp"
#include "sq/interfaces/audiodevice.h"

namespace sq::asio {

// What a host message does (pure routing; the device applies the effects).
struct MessageEffects {
  int32_t result = 0;       // value returned to the driver
  int32_t resetReason = 0;  // SQ_AUDIO_RESET_* to post to listeners, 0 = none
  bool xrun = false;        // count a dropout and flag the next buffer
};
MessageEffects routeMessage(int32_t selector, int32_t value) noexcept;

// Block-size negotiation helpers.
uint32_t chooseBlockSize(const BufferSizes& sizes, uint32_t requested) noexcept;
std::vector<uint32_t> supportedBlockSizes(const BufferSizes& sizes);
// Sample rates probed through Backend::canSampleRate.
const std::vector<double>& candidateSampleRates();

struct DriverConfig {
  // Opens the registry view for each refresh. Required.
  std::function<std::unique_ptr<RegistryView>()> registry;
  // Loads one driver. Empty = the ASIO host is not compiled into this build.
  BackendFactory backends;
  // Status text used when `backends` is empty (why there are no devices).
  std::string unavailableReason;
};

// Creates the driver object (IAudioDriver + IAsioDriverStatus).
sq_result createDriverWith(DriverConfig cfg, const sq_guid* riid, void** out) noexcept;

// Number of ASIO devices currently open in this process (ASIO callbacks carry no context, so at
// most one device can be open at a time; openDevice returns SQ_E_BUSY otherwise).
uint32_t openDeviceCount() noexcept;

}  // namespace sq::asio
