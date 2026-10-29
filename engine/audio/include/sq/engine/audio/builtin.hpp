// Squizzy Studio engine/audio - built-in devices (instruments + effects) shipped with the engine.
//
// Every built-in device is an AudioProcessor (SynthProcessor for instruments, FxProcessor for
// effects) whose parameters are exposed through AudioProcessor::parameterCount / setParameter /
// parameter using PLAIN values in [ParamInfo::min, ParamInfo::max].
//
// DOCUMENT MODEL (same spec grammar as plugin inserts, plugin_fx.hpp):
//   spec          "<format>|<id>|<path>[|<param index>=<plain value>]...[|s=<base64>]"
//   builtin spec  "builtin|<device id>|[|<param index>=<plain value>]..."
//   e.g.          "builtin|eq||0=-3|4=1.5"   (unlisted params keep their defaults)
//   plugin spec   "<clap|vst3>|<plugin id>|<plugin path>[|<param id>=<plain value>]...[|s=<base64>]"
//                 "s=" carries the plugin's opaque state (IPluginInstance::saveState, standard
//                 base64 with '=' padding, no '|' possible); it is always the LAST token and is
//                 written by the host when the song is saved (Shell::capturePluginStates). The
//                 host loads it into a fresh instance BEFORE applying the "i=v" values.
//   tokens after the three fixed fields may come in any order; unknown keys are ignored.
//   track property "user.inserts"    one insert spec per line (built-in or plugin effects)
//   track property "user.instrument" one spec (built-in or plugin instrument) for an instrument
//                                    track; empty = the default instrument (kDefaultInstrument)
// The GUI builds device panels and "insert" menus from builtinDevices(); the app's insert /
// instrument resolver calls makeBuiltin() for specs whose format field is "builtin".
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sq/engine/audio/nodes.hpp"
#include "sq/engine/audio/processor.hpp"

namespace sq::engine::audio {

enum class ParamKind : uint8_t { Continuous, Integer, Toggle, Choice };

struct ParamInfo {
  std::string id;      // stable key ("cutoff")
  std::string name;    // display name ("Cutoff")
  float min = 0, max = 1, def = 0;
  std::string unit;    // "dB", "Hz", "ms", "%", "" ...
  ParamKind kind = ParamKind::Continuous;
  bool logScale = false;              // GUI knob mapping (frequencies, times)
  std::vector<std::string> choices;   // ParamKind::Choice: index -> label (min = 0, max = n-1)
};

struct DeviceInfo {
  std::string id;        // "synth", "eq", "reverb" ...
  std::string name;      // "Squizzy Synth"
  std::string category;  // "Instrument", "EQ", "Dynamics", "Reverb", "Delay", "Modulation", "Distortion", "Filter", "Utility"
  bool instrument = false;
  std::vector<ParamInfo> params;
};

inline constexpr const char* kDefaultInstrument = "builtin|synth|";

// Catalog of every built-in device (stable order; static lifetime).
const std::vector<DeviceInfo>& builtinDevices();
const DeviceInfo* findBuiltin(const std::string& id);

// Creates a prototype with the given plain parameter values (index, value); nullptr + error for
// an unknown id. Values are clamped to the parameter ranges.
ProcessorProto makeBuiltin(const std::string& id, const std::vector<std::pair<uint32_t, double>>& params, std::string* error);

// Spec helpers ("builtin|<id>|" + "|i=v" for every value that differs from its default).
std::string builtinSpec(const std::string& id, const std::vector<float>& values);
// Splits any insert/instrument spec "<format>|<id>|<path>[|i=v]...[|s=<base64>]" (built-in or
// plugin). parseSpec is tolerant: tokens after the three fixed fields may come in any order,
// malformed "i=v" tokens and unknown non-numeric keys are skipped. formatSpec writes the params in
// list order and "|s=<state>" last (only when `state` is non-empty), so parse(format(x)) == x.
struct SpecParts {
  std::string format, id, path;
  std::vector<std::pair<uint32_t, double>> params;
  std::string state;  // "s=" token: base64 text of the plugin's saved state; empty = none
};
bool parseSpec(const std::string& spec, SpecParts* out);
std::string formatSpec(const SpecParts& parts);

// Insert-line helpers (spec.cpp). A line of "user.inserts" that starts with '!' is BYPASSED: the
// doc builder keeps its slot number (automation / command slots count every line) but leaves the
// processor out of the chain. Toggling the marker is a structural edit (the graph recompiles).
inline bool specBypassed(const std::string& line) { return !line.empty() && line[0] == '!'; }
inline std::string specWithoutBypass(const std::string& line) { return specBypassed(line) ? line.substr(1) : line; }
// The spec with its "|i=v" parameter values AND its "|s=" state removed (structure key: parameter
// and state edits are applied live instead of recompiling). Unparsable specs are returned unchanged.
std::string specStructure(const std::string& spec);
// Sets (or adds) one parameter value of a spec; the other fields, values and the state stay as they are.
std::string specSetParam(const std::string& spec, uint32_t index, double value);
// Replaces the "s=" state token of a spec with `base64` (empty removes the token); everything else
// stays as it is. Unparsable specs are returned unchanged.
std::string specSetState(const std::string& spec, const std::string& base64);

// Devices that play an audio file ("sampler"): the HOST resolves the spec's path field and
// decodes it (loadMediaFile) before calling this. A null media makes a silent sampler (that is
// also what makeBuiltin("sampler", ...) returns). For ids that take no media this is makeBuiltin.
ProcessorProto makeBuiltinWithMedia(const std::string& id, MediaPtr media, const std::vector<std::pair<uint32_t, double>>& params,
                                    std::string* error);

// Level meter of a built-in dynamics device for the GUI: current gain reduction in dB (>= 0) for
// "compressor", "limiter" and "gate"; 0 for anything else (incl. non built-in processors). Safe to
// call from any thread while the processor is alive (relaxed atomic read).
float builtinMeterValue(const AudioProcessor* processor) noexcept;

}  // namespace sq::engine::audio
