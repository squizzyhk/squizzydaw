// Music-theory helper (own implementation). See theory.hpp.
#include "sq/theory/theory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

namespace sq::theory {

namespace {

constexpr const char* kSharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr const char* kFlatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

constexpr uint16_t iv(std::initializer_list<int> list) {
  uint16_t m = 0;
  for (int i : list) m = static_cast<uint16_t>(m | (1u << i));
  return m;
}

constexpr ChordQualityInfo kQualities[kChordQualityCount] = {
    {ChordQuality::Major, "", iv({0, 4, 7})},
    {ChordQuality::Minor, "m", iv({0, 3, 7})},
    {ChordQuality::Diminished, "dim", iv({0, 3, 6})},
    {ChordQuality::Augmented, "aug", iv({0, 4, 8})},
    {ChordQuality::Sus2, "sus2", iv({0, 2, 7})},
    {ChordQuality::Sus4, "sus4", iv({0, 5, 7})},
    {ChordQuality::Dominant7, "7", iv({0, 4, 7, 10})},
    {ChordQuality::Major7, "maj7", iv({0, 4, 7, 11})},
    {ChordQuality::Minor7, "m7", iv({0, 3, 7, 10})},
    {ChordQuality::HalfDiminished7, "m7b5", iv({0, 3, 6, 10})},
    {ChordQuality::Diminished7, "dim7", iv({0, 3, 6, 9})},
    {ChordQuality::MinorMajor7, "mMaj7", iv({0, 3, 7, 11})},
    {ChordQuality::Power5, "5", iv({0, 7})},
};

struct SuffixAlias {
  const char* text;
  ChordQuality q;
};
constexpr SuffixAlias kAliases[] = {
    {"", ChordQuality::Major},          {"M", ChordQuality::Major},          {"maj", ChordQuality::Major},
    {"m", ChordQuality::Minor},         {"min", ChordQuality::Minor},        {"-", ChordQuality::Minor},
    {"dim", ChordQuality::Diminished},  {"o", ChordQuality::Diminished},     {"aug", ChordQuality::Augmented},
    {"+", ChordQuality::Augmented},     {"sus2", ChordQuality::Sus2},        {"sus4", ChordQuality::Sus4},
    {"sus", ChordQuality::Sus4},        {"7", ChordQuality::Dominant7},      {"dom7", ChordQuality::Dominant7},
    {"maj7", ChordQuality::Major7},     {"M7", ChordQuality::Major7},        {"m7", ChordQuality::Minor7},
    {"min7", ChordQuality::Minor7},     {"-7", ChordQuality::Minor7},        {"m7b5", ChordQuality::HalfDiminished7},
    {"min7b5", ChordQuality::HalfDiminished7}, {"\xC3\xB8", ChordQuality::HalfDiminished7},
    {"\xC3\xB8" "7", ChordQuality::HalfDiminished7}, {"dim7", ChordQuality::Diminished7},
    {"o7", ChordQuality::Diminished7},  {"mMaj7", ChordQuality::MinorMajor7}, {"mM7", ChordQuality::MinorMajor7},
    {"minmaj7", ChordQuality::MinorMajor7}, {"5", ChordQuality::Power5},
};

constexpr uint16_t kScales[static_cast<int>(Scale::Count)] = {
    iv({0, 2, 4, 5, 7, 9, 11}),  // major
    iv({0, 2, 3, 5, 7, 8, 10}),  // natural minor
    iv({0, 2, 3, 5, 7, 8, 11}),  // harmonic minor
    iv({0, 2, 3, 5, 7, 9, 11}),  // melodic minor (ascending)
    iv({0, 2, 3, 5, 7, 9, 10}),  // dorian
    iv({0, 1, 3, 5, 7, 8, 10}),  // phrygian
    iv({0, 2, 4, 6, 7, 9, 11}),  // lydian
    iv({0, 2, 4, 5, 7, 9, 10}),  // mixolydian
    iv({0, 1, 3, 5, 6, 8, 10}),  // locrian
    iv({0, 2, 4, 7, 9}),         // major pentatonic
    iv({0, 3, 5, 7, 10}),        // minor pentatonic
    iv({0, 3, 5, 6, 7, 10}),     // blues
    0x0FFF,                      // chromatic
};
constexpr const char* kScaleNames[static_cast<int>(Scale::Count)] = {
    "major", "natural minor", "harmonic minor", "melodic minor", "dorian", "phrygian", "lydian",
    "mixolydian", "locrian", "major pentatonic", "minor pentatonic", "blues", "chromatic"};

// letter -> semitone; -1 if not a note letter
int letterSemitone(char c) {
  switch (c) {
    case 'C': case 'c': return 0;
    case 'D': case 'd': return 2;
    case 'E': case 'e': return 4;
    case 'F': case 'f': return 5;
    case 'G': case 'g': return 7;
    case 'A': case 'a': return 9;
    case 'B': case 'b': return 11;
    default: return -1;
  }
}

// Parses letter + accidentals at the start of s. Returns chars consumed (0 = failure); semis may be
// outside 0..11 (B#, Cb).
size_t parseNoteHead(std::string_view s, int& semis) {
  if (s.empty()) return 0;
  int base = letterSemitone(s[0]);
  if (base < 0) return 0;
  size_t i = 1;
  int acc = 0;
  while (i < s.size()) {
    if (s[i] == '#') {
      ++acc, ++i;
    } else if (s[i] == 'b') {
      --acc, ++i;
    } else if (s.substr(i, 3) == "\xE2\x99\xAF") {  // U+266F sharp sign
      ++acc, i += 3;
    } else if (s.substr(i, 3) == "\xE2\x99\xAD") {  // U+266D flat sign
      --acc, i += 3;
    } else {
      break;
    }
  }
  semis = base + acc;
  return i;
}

int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

}  // namespace

const char* pitchClassName(int pc, bool flats) { return (flats ? kFlatNames : kSharpNames)[wrapPc(pc)]; }

int parsePitchClass(std::string_view name) {
  int semis = 0;
  size_t n = parseNoteHead(name, semis);
  if (n == 0 || n != name.size()) return -1;
  return wrapPc(semis);
}

std::string midiNoteName(int midi, bool flats) {
  return std::string(pitchClassName(midi, flats)) + std::to_string(floorDiv(midi, 12) - 1);
}

int parseMidiNote(std::string_view name) {
  int semis = 0;
  size_t n = parseNoteHead(name, semis);
  if (n == 0 || n >= name.size()) return -1;
  std::string_view rest = name.substr(n);
  bool neg = false;
  if (rest[0] == '-') neg = true, rest.remove_prefix(1);
  if (rest.empty() || rest.size() > 2) return -1;
  int oct = 0;
  for (char c : rest) {
    if (c < '0' || c > '9') return -1;
    oct = oct * 10 + (c - '0');
  }
  if (neg) oct = -oct;
  int midi = (oct + 1) * 12 + semis;
  return (midi < 0 || midi > 127) ? -1 : midi;
}

double midiToFrequency(double midi, double a4Hz) { return a4Hz * std::pow(2.0, (midi - 69.0) / 12.0); }
double frequencyToMidi(double hz, double a4Hz) { return hz > 0.0 ? 69.0 + 12.0 * std::log2(hz / a4Hz) : 0.0; }

const ChordQualityInfo& chordQualityInfo(ChordQuality q) {
  int i = static_cast<int>(q);
  if (i < 0 || i >= kChordQualityCount) i = 0;
  return kQualities[i];
}

uint16_t pcMaskOf(int root, uint16_t intervals) {
  root = wrapPc(root);
  uint32_t m = static_cast<uint32_t>(intervals & 0x0FFF);
  m = (m << root) | (m >> (12 - root));
  return static_cast<uint16_t>(m & 0x0FFF);
}

uint16_t chordMask(int root, ChordQuality q) { return pcMaskOf(root, chordQualityInfo(q).intervals); }

int popcount12(uint16_t mask) {
  int n = 0;
  for (int i = 0; i < 12; ++i) n += (mask >> i) & 1;
  return n;
}

std::string chordName(int root, ChordQuality q, int bass, bool flats) {
  std::string s = pitchClassName(root, flats);
  s += chordQualityInfo(q).suffix;
  if (bass >= 0 && wrapPc(bass) != wrapPc(root)) {
    s += '/';
    s += pitchClassName(bass, flats);
  }
  return s;
}

bool parseChordName(std::string_view name, int& root, ChordQuality& q, int& bass) {
  int semis = 0;
  size_t n = parseNoteHead(name, semis);
  if (n == 0) return false;
  // "Bb" vs "B" + "b..." is ambiguous only for suffixes starting with 'b', none of ours do.
  std::string_view rest = name.substr(n);
  int bassPc = -1;
  size_t slash = rest.find('/');
  if (slash != std::string_view::npos) {
    bassPc = parsePitchClass(rest.substr(slash + 1));
    if (bassPc < 0) return false;
    rest = rest.substr(0, slash);
  }
  for (const SuffixAlias& a : kAliases)
    if (rest == a.text) {
      root = wrapPc(semis);
      q = a.q;
      bass = bassPc;
      return true;
    }
  return false;
}

bool identifyChord(uint16_t pcMask, ChordMatch& out, int bassPc) {
  pcMask &= 0x0FFF;
  const int count = popcount12(pcMask);
  if (count < 2) return false;
  bool found = false;
  ChordMatch best;
  best.score = -1e9;
  for (int qi = 0; qi < kChordQualityCount; ++qi) {
    const ChordQuality q = static_cast<ChordQuality>(qi);
    if (q == ChordQuality::Power5 && count != 2) continue;
    for (int root = 0; root < 12; ++root) {
      const uint16_t t = chordMask(root, q);
      const int matched = popcount12(static_cast<uint16_t>(t & pcMask));
      const int missing = popcount12(static_cast<uint16_t>(t & ~pcMask));
      const int extra = popcount12(static_cast<uint16_t>(pcMask & ~t));
      if (matched < 2) continue;
      double score = (matched - 0.6 * missing - 0.8 * extra) / static_cast<double>(std::max(count, popcount12(t)));
      if (!(pcMask & (1u << root))) score -= 0.25;  // rootless voicing: possible but unlikely
      if (bassPc >= 0 && wrapPc(bassPc) == root) score += 0.02;
      if (score > best.score + 1e-12) {
        best.root = root;
        best.quality = q;
        best.score = score;
        found = true;
      }
    }
  }
  if (!found) return false;
  best.bass = (bassPc >= 0 && wrapPc(bassPc) != best.root) ? wrapPc(bassPc) : -1;
  if (best.score > 1.0) best.score = 1.0;
  out = best;
  return true;
}

const char* scaleName(Scale s) {
  int i = static_cast<int>(s);
  return (i >= 0 && i < static_cast<int>(Scale::Count)) ? kScaleNames[i] : "";
}
uint16_t scaleIntervals(Scale s) {
  int i = static_cast<int>(s);
  return (i >= 0 && i < static_cast<int>(Scale::Count)) ? kScales[i] : 0;
}
uint16_t scaleMask(int tonic, Scale s) { return pcMaskOf(tonic, scaleIntervals(s)); }
bool inScale(int midi, int tonic, Scale s) { return (scaleMask(tonic, s) >> wrapPc(midi)) & 1; }

int quantizeToScale(int midi, int tonic, Scale s, int direction) {
  const uint16_t m = scaleMask(tonic, s);
  if (!m) return midi;
  auto ok = [&](int n) { return (m >> wrapPc(n)) & 1; };
  for (int d = 0; d < 12; ++d) {
    if (direction >= 0 && ok(midi + d)) return midi + d;
    if (direction <= 0 && ok(midi - d)) return midi - d;
  }
  return midi;
}

double quantizePitch(double midi, int tonic, Scale s) {
  const uint16_t m = scaleMask(tonic, s);
  if (!m) return midi;
  const int c = static_cast<int>(std::floor(midi));
  double best = midi, bestDist = 1e9;
  for (int n = c - 6; n <= c + 7; ++n) {
    if (!((m >> wrapPc(n)) & 1)) continue;
    double d = std::fabs(midi - n);
    if (d < bestDist - 1e-12) bestDist = d, best = n;
  }
  return best;
}

bool keyPrefersFlats(int tonic, bool minor) {
  tonic = wrapPc(tonic);
  if (!minor) return tonic == 5 || tonic == 10 || tonic == 3 || tonic == 8 || tonic == 1;
  return tonic == 2 || tonic == 7 || tonic == 0 || tonic == 5 || tonic == 10 || tonic == 3;
}

std::string keyName(int tonic, bool minor) {
  return std::string(pitchClassName(tonic, keyPrefersFlats(tonic, minor))) + (minor ? " minor" : " major");
}

ChordQuality diatonicTriad(bool minorKey, int degree) {
  static constexpr ChordQuality kMaj[7] = {ChordQuality::Major, ChordQuality::Minor, ChordQuality::Minor,
                                           ChordQuality::Major, ChordQuality::Major, ChordQuality::Minor,
                                           ChordQuality::Diminished};
  static constexpr ChordQuality kMin[7] = {ChordQuality::Minor, ChordQuality::Diminished, ChordQuality::Major,
                                           ChordQuality::Minor, ChordQuality::Minor, ChordQuality::Major,
                                           ChordQuality::Major};
  degree = ((degree % 7) + 7) % 7;
  return minorKey ? kMin[degree] : kMaj[degree];
}

std::string romanNumeral(int keyTonic, bool minorKey, int chordRoot, ChordQuality q) {
  static constexpr const char* kUpper[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
  static constexpr const char* kLower[7] = {"i", "ii", "iii", "iv", "v", "vi", "vii"};
  const uint16_t scale = scaleIntervals(minorKey ? Scale::NaturalMinor : Scale::Major);
  const int interval = wrapPc(chordRoot - keyTonic);
  int degree = 0;
  std::string prefix;
  // degree index = number of scale notes below the interval
  auto degreeOf = [&](int semis) {
    int d = 0;
    for (int i = 0; i < semis; ++i) d += (scale >> i) & 1;
    return d;
  };
  if ((scale >> interval) & 1) {
    degree = degreeOf(interval);
  } else if (minorKey && interval == 11) {
    degree = 6;  // leading tone of the harmonic minor: written vii (viio) without an accidental
  } else if (minorKey && interval == 9) {
    prefix = "#";  // raised sixth (melodic minor)
    degree = 5;
  } else {
    prefix = "b";  // other chromatic roots: the lowered next degree (interval + 1 is never 12 here)
    degree = degreeOf(interval + 1);
  }
  bool lower = false;
  std::string suffix;
  switch (q) {
    case ChordQuality::Minor: lower = true; break;
    case ChordQuality::Diminished: lower = true, suffix = "o"; break;
    case ChordQuality::Augmented: suffix = "+"; break;
    case ChordQuality::Sus2: suffix = "sus2"; break;
    case ChordQuality::Sus4: suffix = "sus4"; break;
    case ChordQuality::Dominant7: suffix = "7"; break;
    case ChordQuality::Major7: suffix = "maj7"; break;
    case ChordQuality::Minor7: lower = true, suffix = "7"; break;
    case ChordQuality::HalfDiminished7: lower = true, suffix = "\xC3\xB8" "7"; break;
    case ChordQuality::Diminished7: lower = true, suffix = "o7"; break;
    case ChordQuality::MinorMajor7: lower = true, suffix = "maj7"; break;
    case ChordQuality::Power5: suffix = "5"; break;
    default: break;
  }
  return prefix + (lower ? kLower[degree % 7] : kUpper[degree % 7]) + suffix;
}

double quantizeTime(double t, double grid, double strength, double swing) {
  if (!(grid > 0.0)) return t;
  if (strength < 0.0) strength = 0.0;
  if (strength > 1.0) strength = 1.0;
  const long long n0 = static_cast<long long>(std::floor(t / grid));
  double best = t, bestDist = 1e300;
  for (long long n = n0 - 1; n <= n0 + 2; ++n) {
    double p = static_cast<double>(n) * grid + ((n & 1) ? swing * grid * 0.5 : 0.0);
    double d = std::fabs(p - t);
    if (d < bestDist) bestDist = d, best = p;
  }
  return t + (best - t) * strength;
}

}  // namespace sq::theory
