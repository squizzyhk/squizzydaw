// OSC 1.0 encoder/decoder (messages, bundles, address pattern matching). Own implementation of
// the public OSC 1.0 specification. Pure functions, no sockets, no globals: thread-safe.
// Allocates, so never call it from the audio thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sq/interfaces/remote.h"

namespace sq::osc {

// OSC time tag for "immediately".
inline constexpr uint64_t kImmediately = 1;
// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
inline constexpr uint64_t kNtpUnixOffsetSeconds = 2208988800ull;

// Time tag <-> Unix time in nanoseconds. The fraction is rounded to the nearest unit, so a
// round trip is exact to within 1 ns. NTP era 0 only: valid up to 2036-02-07 (the 32-bit seconds
// field wraps there). kImmediately maps to 0.
uint64_t timeTagFromUnixNs(uint64_t unixNs);
uint64_t timeTagToUnixNs(uint64_t timeTag);

// Encodes one message. Supported argument types: i h f d s b T F N (sq_osc_arg). For 'i' and 'h'
// the value comes from arg.i, for 'f' and 'd' from arg.d. A null 's' is sent as "".
// Returns false (and fills err) on an invalid address or unknown type.
bool encodeMessage(const char* address, const sq_osc_arg* args, uint32_t count, std::vector<uint8_t>& out,
                   std::string* err = nullptr);

// Wraps already-encoded elements (messages or bundles) into a bundle.
void encodeBundle(uint64_t timeTag, const std::vector<std::vector<uint8_t>>& elements, std::vector<uint8_t>& out);

// Called once per message found in a packet (bundles are flattened, depth first).
// Argument views are valid during the call only. For numeric args both .i and .d are filled.
// Decoded type mapping onto sq_osc_arg: 'S' -> 's', 'c' 'r' 'm' -> 'i', 't' -> 'h',
// 'I' (infinitum) -> 'd' = +inf. Array brackets '[' ']' are flattened away.
using MessageFn = std::function<void(const char* address, const sq_osc_arg* args, uint32_t count, uint64_t timeTag)>;

// Decodes a UDP datagram. Returns false (with err) on malformed input; messages decoded before
// the error were already delivered.
bool decodePacket(const uint8_t* data, size_t size, const MessageFn& fn, std::string* err = nullptr);

// OSC 1.0 address pattern matching: '?', '*', '[abc]', '[a-z]', '[!a-z]', '{foo,bar}'.
// Patterns and addresses are matched part by part ('/' is never matched by a wildcard).
// Work is bounded (patterns come from the network): a part nesting more than 32 '*'/'{' levels,
// or a match needing more than 200000 steps, does not match.
bool patternMatch(const char* pattern, const char* address);

// Owning copy of a decoded message (handy for tests and queues).
struct OwnedArg {
  char type = 'N';
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<uint8_t> blob;
};
struct OwnedMessage {
  std::string address;
  std::vector<OwnedArg> args;
  uint64_t timeTag = kImmediately;
};
std::vector<OwnedMessage> decodeAll(const uint8_t* data, size_t size, std::string* err = nullptr);

}  // namespace sq::osc
