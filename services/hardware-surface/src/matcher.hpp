// hardware-surface: incoming MIDI -> control events, control values -> outgoing MIDI.
//
// Matcher::match() is allocation- and lock-free (an O(1) table lookup plus a little per-control
// state), but it is NOT thread-safe: one thread (the service worker) owns a Matcher. Toggle state
// lives in atomics so setControlValue() on the main thread can update an LED-backed toggle.
//
// 14-bit control change: the MSB alone produces an event (value = MSB << 7) until the device has
// been seen sending an LSB for that control; from then on the MSB is held and the event comes
// with the LSB, so a fine move produces one event with the full value instead of a coarse jump.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sq/interfaces/midi.h"
#include "surface_map.hpp"

namespace sq::hwsurface {

struct ControlEvent {
  uint32_t control = 0;  // index into SurfaceMap::controls
  double value = 0.0;    // absolute: 0..1; relative: delta in -1..1
  bool relative = false;
};

class Matcher {
 public:
  Matcher() = default;
  explicit Matcher(const SurfaceMap& map) { build(map); }
  void build(const SurfaceMap& map);

  // bytes/size: one short MIDI message. True when it maps to a control (out filled).
  bool match(const uint8_t* bytes, uint32_t size, ControlEvent& out) noexcept;

  bool toggleState(uint32_t control) const noexcept;
  void setToggleState(uint32_t control, bool on) noexcept;
  size_t controlCount() const noexcept { return controls_.size(); }

 private:
  static constexpr uint32_t kTableSize = uint32_t(MsgKind::Count) * 16u * 128u;
  static constexpr uint16_t kLsbRole = 0x8000u;
  static uint32_t slot(MsgKind k, uint32_t channel, uint32_t address) noexcept {
    return (uint32_t(k) * 16u + channel) * 128u + address;
  }
  struct State {
    uint8_t msb = 0;
    bool lsbSeen = false;
  };
  std::vector<uint16_t> table_;  // 0 = none, else control index + 1 (| kLsbRole)
  std::vector<Control> controls_;
  std::vector<State> state_;
  std::unique_ptr<std::atomic<uint8_t>[]> toggles_;
};

// Normalized value -> raw value in [min, max] of the control's message (rounded).
int32_t toRaw(const Control& c, double value) noexcept;

// Feedback MIDI for value (0..1) on a non-display control. Writes up to 2 events (14-bit CC:
// MSB then LSB) with time_ns = 0 and returns the count; 0 if the control cannot show feedback.
uint32_t encodeFeedback(const Control& c, double value, sq_midi_timed_event out[2]) noexcept;

// Display text -> complete sysex (header, text as 7-bit ASCII padded/truncated to the control's
// length, F7). Non-ASCII bytes become '?'.
std::vector<uint8_t> encodeDisplay(const Control& c, const char* text);

}  // namespace sq::hwsurface
