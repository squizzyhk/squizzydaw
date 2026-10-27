// Squizzy Studio formats - plain-XML song-adjacent data: .chords, .musicalscale, .colorscheme.
// Schemas: SCHEMAS.md "Plain XML data".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

inline constexpr int kChordSetFormat = 1;
inline constexpr int kScaleFormat = 1;
inline constexpr int kColorSchemeFormat = 1;

// ---- .chords ----------------------------------------------------------------------------
struct ChordDef {
  std::string name;            // display name, e.g. "maj7"
  std::string symbol;          // short symbol used in chord tracks, e.g. "M7"
  std::vector<int> intervals;  // semitones above the root, ascending, first is 0
};
struct ChordSet {
  std::string name;
  std::vector<ChordDef> chords;
};
std::string chordSetToXml(const ChordSet& s);
Status chordSetFromXml(std::string_view xml, ChordSet& out);

// ---- .musicalscale ------------------------------------------------------------------------
struct ScaleDegree {
  double cents = 0.0;  // offset from the root, 0 <= cents < period
  std::string name;
};
struct MusicalScale {
  std::string name;
  double periodCents = 1200.0;  // octave by default; non-octave scales allowed
  std::vector<ScaleDegree> degrees;  // ascending, first is 0
};
std::string musicalScaleToXml(const MusicalScale& s);
Status musicalScaleFromXml(std::string_view xml, MusicalScale& out);

// ---- .colorscheme ---------------------------------------------------------------------------
struct NamedColor {
  std::string id;      // e.g. "track.1", "ui.accent"
  uint32_t rgba = 0;   // 0xRRGGBBAA
};
struct ColorScheme {
  std::string name;
  std::string appearance;  // "dark", "light" or "" (any)
  std::vector<NamedColor> colors;
};
std::string colorSchemeToXml(const ColorScheme& s);
Status colorSchemeFromXml(std::string_view xml, ColorScheme& out);

}  // namespace sq::formats
