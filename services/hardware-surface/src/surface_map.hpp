// hardware-surface: control-surface map model, parser and validator.
//
// SCHEMA (format 1). Two small XML files; numbers are decimal or 0x-hex.
//
// <name>.device - identifies a piece of hardware and how to find its MIDI ports:
//   <Device format="1" id="example.k8" name="K8 Controller" vendor="Example" model="K8">
//     <Ports input="K8" output="K8"/>        case-insensitive substrings of the MIDI port names,
//                                             used when loadSurfaceMap() gets no connection id
//     <Surface file="k8.surface.xml"/>        path relative to the .device file
//   </Device>
//
// <name>.surface.xml - the controls and how they speak MIDI:
//   <ControlSurface format="1" name="K8">
//     <Controls>
//       <Control id="fader1" type="fader" label="Fader 1" feedback="true">
//         <MidiMessage status="0xE0"/>                          pitch bend, channel 1 (14-bit)
//       </Control>
//       <Control id="knob1" type="knob">
//         <MidiMessage status="0xB0" address="16" resolution="14"/>   CC 16 MSB + CC 48 LSB
//       </Control>
//       <Control id="enc1" type="encoder" mode="twos-complement" step="0.01">
//         <MidiMessage status="0xB0" address="20"/>
//       </Control>
//       <Control id="mute1" type="button" toggle="true" feedback="true" on="127" off="0">
//         <MidiMessage status="0x90" address="16"/>             note 16 (note-off matches too)
//       </Control>
//       <Control id="next" type="button" action="page.next">
//         <MidiMessage status="0xB0" address="102"/>
//       </Control>
//       <Control id="lcd" type="display" length="8">
//         <Sysex header="F0 7D 01 00"/>                         header + text + F7
//       </Control>
//     </Controls>
//     <Bindings>                                                 optional default bindings
//       <Bind control="fader1" target="object://doc/tracks/0" property="volumeDb" min="-60" max="6"/>
//     </Bindings>
//   </ControlSurface>
//
// Control attributes
//   id        required, unique, at most 63 bytes
//   type      knob | encoder | fader | button | pad | display | led       (ui.h SQ_REMOTE_*)
//   label     free text (at most 63 bytes)
//   feedback  true: the service sends values back (motor fader, LED, ring). Implicit for display/led.
//   toggle    buttons/pads: a press flips the state; releases are ignored.
//   mode      encoders: twos-complement (default) | sign-magnitude | offset-64 | absolute
//   step      encoders: normalized change per tick (default 1/128)
//   on / off  raw feedback values for buttons, pads and LEDs (default 127 / 0)
//   action    buttons/pads: page.next | page.prev | page.<n> (0-based) - handled by the service
//   length    display: number of characters (1..128)
// MidiMessage attributes
//   status      0x80..0xEF; the low nibble is the channel. 0x80 and 0x90 both mean "note"
//               (note-on with velocity 0 and note-off both count as release).
//               0xA0 poly pressure, 0xB0 control change, 0xC0 program change (a press),
//               0xD0 channel pressure, 0xE0 pitch bend (always 14-bit).
//   address     note / controller / program number (0..127); unused for 0xD0 and 0xE0.
//   resolution  7 (default) or 14. 14-bit control change: MSB at `address`, LSB at `lsb`
//               (default address + 32).
//   min / max   raw value range that maps to 0..1 (default the full 7- or 14-bit range).
// Bind attributes: control, target (object:// URL), property, optional min/max: the target
//   value range a normalized 0..1 control value maps to (default 0..1, i.e. normalized).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sq/interfaces/ui.h"

namespace sq::hwsurface {

enum class MsgKind : uint8_t { Note = 0, PolyPressure, ControlChange, Program, ChannelPressure, PitchBend, Count };
enum class EncoderMode : uint8_t { Absolute, TwosComplement, SignMagnitude, Offset64 };
enum class Action : uint8_t { None, PageNext, PagePrev, PageSelect };

struct MidiSpec {
  MsgKind kind = MsgKind::ControlChange;
  uint8_t channel = 0;     // 0..15
  uint8_t address = 0;     // note / controller / program (0 for channel pressure, pitch bend)
  bool fourteenBit = false;
  uint8_t lsbAddress = 0;  // 14-bit control change only
  int32_t min = 0;
  int32_t max = 127;
};

struct Control {
  std::string id;
  std::string label;
  int32_t type = SQ_REMOTE_KNOB;  // SQ_REMOTE_*
  uint32_t flags = 0;             // SQ_REMOTE_TOGGLE | SQ_REMOTE_FEEDBACK
  bool hasMidi = false;
  MidiSpec midi;
  EncoderMode mode = EncoderMode::Absolute;
  double step = 1.0 / 128.0;
  int32_t onValue = 127;
  int32_t offValue = 0;
  Action action = Action::None;
  uint32_t actionPage = 0;
  std::vector<uint8_t> sysexHeader;  // display controls
  uint32_t textLength = 0;
};

struct StaticBinding {
  std::string control;
  std::string target;
  std::string property;
  double min = 0.0;
  double max = 1.0;
};

struct SurfaceMap {
  int32_t format = 1;
  std::string name;
  std::vector<Control> controls;
  std::vector<StaticBinding> bindings;
  int32_t find(std::string_view id) const;  // -1 if missing
};

struct DeviceInfo {
  int32_t format = 1;
  std::string id;
  std::string name;
  std::string vendor;
  std::string model;
  std::string inputMatch;   // <Ports input>
  std::string outputMatch;  // <Ports output>
  std::string surfaceFile;  // as written in <Surface file>
};

struct Diagnostics {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  bool ok() const { return errors.empty(); }
  std::string text() const;  // errors then warnings, one per line
};

inline constexpr int32_t kFormatVersion = 1;
inline constexpr size_t kMaxControls = 1024;

// Parse only (structure + attribute syntax). Problems go to diag; SQ_E_PARSE when any error.
sq_result parseSurfaceMap(std::string_view xml, SurfaceMap& out, Diagnostics& diag);
sq_result parseDevice(std::string_view xml, DeviceInfo& out, Diagnostics& diag);
// Semantic checks: unique ids, unique MIDI messages (incl. 14-bit LSBs), ranges, mode/type
// combinations, bindings that name existing controls and object:// targets.
void validateSurfaceMap(const SurfaceMap& map, Diagnostics& diag);
// Loads a .device (and the .surface.xml it names) or a .surface.xml directly (dev then gets the
// map's name as id/name), parses and validates. SQ_E_IO / SQ_E_PARSE / SQ_E_INVALIDARG.
sq_result loadSurfaceFiles(const std::string& path, DeviceInfo& dev, SurfaceMap& map, Diagnostics& diag);

// Numeric attribute syntax shared with the tests: decimal or 0x-hex, optional leading '-'.
bool parseInt(std::string_view s, int64_t& out);
// "F0 7D 01" / "F0,7D,01" / "0xF0 0x7D" -> bytes.
bool parseHexBytes(std::string_view s, std::vector<uint8_t>& out);
int32_t remoteTypeFromName(std::string_view s);  // 0 if unknown
const char* remoteTypeName(int32_t type);

}  // namespace sq::hwsurface
