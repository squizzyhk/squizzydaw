// Squizzy Studio - small music-theory helper (own implementation): pitch classes and note names,
// chord qualities / naming / parsing / identification, scales and key names, pitch and time
// quantisation. Pure functions, no allocation except where std::string is returned.
// Conventions: pitch class 0 = C; MIDI 60 = C4 (middle C); A4 = 69.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sq::theory {

// ---- pitch ------------------------------------------------------------------------------------
inline int wrapPc(int pc) { return ((pc % 12) + 12) % 12; }
const char* pitchClassName(int pc, bool flats = false);
int parsePitchClass(std::string_view name);  // "C", "c#", "Db", "B#", "Cb", "E##"... ; -1 if invalid
std::string midiNoteName(int midi, bool flats = false);  // 69 -> "A4", 61 -> "C#4"
int parseMidiNote(std::string_view name);                // "A4" -> 69, "Bb-1" -> 10; -1 if invalid
double midiToFrequency(double midi, double a4Hz = 440.0);
double frequencyToMidi(double hz, double a4Hz = 440.0);  // fractional

// ---- chords -----------------------------------------------------------------------------------
enum class ChordQuality : int {
  Major, Minor, Diminished, Augmented, Sus2, Sus4, Dominant7, Major7, Minor7, HalfDiminished7, Diminished7,
  MinorMajor7, Power5, Count
};
inline constexpr int kChordQualityCount = static_cast<int>(ChordQuality::Count);

struct ChordQualityInfo {
  ChordQuality quality;
  const char* suffix;     // appended to the root name: "", "m", "dim", "7", "maj7", "m7b5" ...
  uint16_t intervals;     // bit i = interval of i semitones above the root (bit 0 always set)
};
const ChordQualityInfo& chordQualityInfo(ChordQuality q);

uint16_t pcMaskOf(int root, uint16_t intervals);  // rotate an interval mask to absolute pitch classes
uint16_t chordMask(int root, ChordQuality q);
int popcount12(uint16_t mask);

// "C", "Am", "F#dim", "Bb7", "Cmaj7", "Bm7b5", "C/E" (bass < 0 or == root: no slash).
std::string chordName(int root, ChordQuality q, int bass = -1, bool flats = false);
// Accepts what chordName produces plus common aliases ("min", "maj", "M7", "-", "+", "o", "ø").
bool parseChordName(std::string_view name, int& root, ChordQuality& q, int& bass);

struct ChordMatch {
  int root = -1;
  ChordQuality quality = ChordQuality::Major;
  int bass = -1;
  double score = 0.0;  // 1 = exact match
};
// Best chord for a set of sounding pitch classes (12-bit mask). bassPc (optional) breaks ties in
// favour of the chord rooted on the bass and is reported as a slash bass otherwise.
bool identifyChord(uint16_t pcMask, ChordMatch& out, int bassPc = -1);

// ---- scales and keys ---------------------------------------------------------------------------
enum class Scale : int {
  Major, NaturalMinor, HarmonicMinor, MelodicMinor, Dorian, Phrygian, Lydian, Mixolydian, Locrian,
  MajorPentatonic, MinorPentatonic, Blues, Chromatic, Count
};
const char* scaleName(Scale s);
uint16_t scaleIntervals(Scale s);
uint16_t scaleMask(int tonic, Scale s);
bool inScale(int midi, int tonic, Scale s);
// Nearest note of the scale. direction: 0 nearest (ties go up), +1 at or above, -1 at or below.
int quantizeToScale(int midi, int tonic, Scale s, int direction = 0);
// Fractional pitch quantisation to the nearest scale note (for pitch-corrected display).
double quantizePitch(double midi, int tonic, Scale s);

bool keyPrefersFlats(int tonic, bool minor);
std::string keyName(int tonic, bool minor);  // "C major", "F# minor", "Bb major"
// Diatonic triad quality on a 0-based degree of a major or natural-minor key.
ChordQuality diatonicTriad(bool minorKey, int degree);
// Roman numeral of a chord in a key: "I", "ii", "V7", "viio", "bVII", "iv".
std::string romanNumeral(int keyTonic, bool minorKey, int chordRoot, ChordQuality q);

// ---- rhythm -------------------------------------------------------------------------------------
// Moves t toward the nearest grid line by `strength` (0..1). swing (0..1) delays every second grid
// line by swing * grid / 2.
double quantizeTime(double t, double grid, double strength = 1.0, double swing = 0.0);

}  // namespace sq::theory
