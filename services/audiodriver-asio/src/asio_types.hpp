// Squizzy Studio - ASIO driver service: SDK-independent wire constants.
//
// These are the numeric values of the ASIO driver ABI (sample type codes, host message selectors,
// time-info flags). They are interoperability facts, written here so the conversion, registry and
// device logic compile and are unit-tested without the (separately licensed) SDK. The SDK adapter
// (asio_sdk_backend.cpp) static_asserts that every value matches the SDK headers.
#pragma once

#include <cstdint>

namespace sq::asio {

// Native sample formats a driver can report per channel. LSB = little endian, MSB = big endian.
// IntNN: packed NN-bit integers. Int32xxNN: an NN-bit sample right-aligned (in the low bits) of a
// 32-bit container, sign-extended.
enum class SampleType : int32_t {
  Int16MSB = 0,
  Int24MSB = 1,
  Int32MSB = 2,
  Float32MSB = 3,
  Float64MSB = 4,
  Int32MSB16 = 8,
  Int32MSB18 = 9,
  Int32MSB20 = 10,
  Int32MSB24 = 11,
  Int16LSB = 16,
  Int24LSB = 17,
  Int32LSB = 18,
  Float32LSB = 19,
  Float64LSB = 20,
  Int32LSB16 = 24,
  Int32LSB18 = 25,
  Int32LSB20 = 26,
  Int32LSB24 = 27,
};

// Host message selectors (driver -> host "message" callback).
namespace msg {
inline constexpr int32_t kSelectorSupported = 1;
inline constexpr int32_t kEngineVersion = 2;
inline constexpr int32_t kResetRequest = 3;
inline constexpr int32_t kBufferSizeChange = 4;
inline constexpr int32_t kResyncRequest = 5;
inline constexpr int32_t kLatenciesChanged = 6;
inline constexpr int32_t kSupportsTimeInfo = 7;
inline constexpr int32_t kSupportsTimeCode = 8;
inline constexpr int32_t kOverload = 15;
}  // namespace msg

// Host engine version we report for kEngineVersion.
inline constexpr int32_t kHostEngineVersion = 2;

}  // namespace sq::asio
