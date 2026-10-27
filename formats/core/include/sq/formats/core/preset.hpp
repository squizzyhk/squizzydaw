// Squizzy Studio formats - presets: .preset / .multipreset / .instrument / .trackpreset.
// ZIP container: metainfo.xml + preset.xml + one opaque state blob per slot
// (slots/<index>.state) + optional attachments. Schema: SCHEMAS.md "Presets".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sq/formats/core/metainfo.hpp"

namespace sq::formats {

inline constexpr int kPresetFormat = 1;
inline constexpr const char* kPresetXml = "preset.xml";

enum class PresetKind { Preset, MultiPreset, Instrument, TrackPreset };

const char* presetKindName(PresetKind k);       // "preset", "multipreset", ...
const char* presetKindExtension(PresetKind k);  // same as the name (file extension)
bool presetKindFromName(std::string_view s, PresetKind& out);  // accepts names and extensions

// One processor (plugin or built-in device) with its saved state.
struct PresetSlot {
  std::string classId;   // braced GUID or format-specific plugin id
  std::string name;      // display name of the processor
  std::string format;    // "clap", "vst3", "builtin", ...
  std::string vendor;
  bool bypassed = false;
  std::vector<std::pair<std::string, double>> params;  // optional readable parameter snapshot
  std::string state;     // opaque saveState() bytes (stored as its own ZIP member)
};

struct Preset {
  PresetKind kind = PresetKind::Preset;
  MetaInfo meta;
  KeyValues settings;               // container-level settings (trackpreset: volume, pan, color ...)
  std::vector<PresetSlot> slots;    // preset/instrument: exactly 1; multipreset/trackpreset: >= 1
  std::vector<FileEntry> attachments;  // extra members under "attachments/" (names relative to it)
};

Status presetWrite(const Preset& p, std::string& zipOut);
Status presetRead(std::string_view zipBytes, Preset& out);

}  // namespace sq::formats
