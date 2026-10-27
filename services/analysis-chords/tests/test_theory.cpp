// sq::theory: names, parsing, chord identification, scales, keys, quantisation.
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <string>

#include "sq/theory/theory.hpp"
#include "sqtest.hpp"

using namespace sq::theory;

SQ_TEST("theory: pitch class names and parsing") {
  CHECK_EQ(std::string(pitchClassName(1)), std::string("C#"));
  CHECK_EQ(std::string(pitchClassName(1, true)), std::string("Db"));
  CHECK_EQ(std::string(pitchClassName(-1)), std::string("B"));
  CHECK_EQ(parsePitchClass("C"), 0);
  CHECK_EQ(parsePitchClass("f#"), 6);
  CHECK_EQ(parsePitchClass("Bb"), 10);
  CHECK_EQ(parsePitchClass("B#"), 0);
  CHECK_EQ(parsePitchClass("Cb"), 11);
  CHECK_EQ(parsePitchClass("E##"), 6);
  CHECK_EQ(parsePitchClass("H"), -1);
  CHECK_EQ(parsePitchClass("C#x"), -1);
  CHECK_EQ(parsePitchClass(""), -1);
}

SQ_TEST("theory: midi note names and frequencies") {
  CHECK_EQ(midiNoteName(60), std::string("C4"));
  CHECK_EQ(midiNoteName(69), std::string("A4"));
  CHECK_EQ(midiNoteName(70, true), std::string("Bb4"));
  CHECK_EQ(midiNoteName(0), std::string("C-1"));
  CHECK_EQ(parseMidiNote("A4"), 69);
  CHECK_EQ(parseMidiNote("C-1"), 0);
  CHECK_EQ(parseMidiNote("B#3"), 60);
  CHECK_EQ(parseMidiNote("G9"), 127);
  CHECK_EQ(parseMidiNote("A"), -1);
  CHECK_EQ(parseMidiNote("G#9"), -1);
  CHECK(std::abs(midiToFrequency(69) - 440.0) < 1e-9);
  CHECK(std::abs(midiToFrequency(57) - 220.0) < 1e-9);
  CHECK(std::abs(frequencyToMidi(261.6255653) - 60.0) < 1e-6);
  CHECK(std::abs(frequencyToMidi(432.0, 432.0) - 69.0) < 1e-9);
}

SQ_TEST("theory: chord names round-trip through the parser") {
  for (int q = 0; q < kChordQualityCount; ++q)
    for (int root = 0; root < 12; ++root)
      for (bool flats : {false, true}) {
        std::string name = chordName(root, static_cast<ChordQuality>(q), -1, flats);
        int r = -1, bass = -2;
        ChordQuality pq{};
        REQUIRE(parseChordName(name, r, pq, bass));
        CHECK_EQ(r, root);
        CHECK_EQ(static_cast<int>(pq), q);
        CHECK_EQ(bass, -1);
      }
  CHECK_EQ(chordName(9, ChordQuality::Minor), std::string("Am"));
  CHECK_EQ(chordName(10, ChordQuality::Dominant7, -1, true), std::string("Bb7"));
  CHECK_EQ(chordName(0, ChordQuality::Major, 4), std::string("C/E"));
  CHECK_EQ(chordName(11, ChordQuality::HalfDiminished7), std::string("Bm7b5"));
  int r = 0, bass = 0;
  ChordQuality q{};
  REQUIRE(parseChordName("F#min7/E", r, q, bass));
  CHECK_EQ(r, 6);
  CHECK(q == ChordQuality::Minor7);
  CHECK_EQ(bass, 4);
  REQUIRE(parseChordName("Ebmaj7", r, q, bass));
  CHECK_EQ(r, 3);
  CHECK(q == ChordQuality::Major7);
  REQUIRE(parseChordName("G+", r, q, bass));
  CHECK(q == ChordQuality::Augmented);
  CHECK(!parseChordName("Xm", r, q, bass));
  CHECK(!parseChordName("Cfoo", r, q, bass));
  CHECK(!parseChordName("C/Q", r, q, bass));
}

SQ_TEST("theory: chord identification from pitch-class sets") {
  auto mask = [](std::initializer_list<int> pcs) {
    uint16_t m = 0;
    for (int p : pcs) m = static_cast<uint16_t>(m | (1u << wrapPc(p)));
    return m;
  };
  ChordMatch m;
  REQUIRE(identifyChord(mask({0, 4, 7}), m));
  CHECK_EQ(chordName(m.root, m.quality, m.bass), std::string("C"));
  CHECK(m.score > 0.99);
  REQUIRE(identifyChord(mask({9, 0, 4}), m));
  CHECK_EQ(chordName(m.root, m.quality, m.bass), std::string("Am"));
  REQUIRE(identifyChord(mask({7, 11, 2, 5}), m));
  CHECK_EQ(chordName(m.root, m.quality, m.bass), std::string("G7"));
  REQUIRE(identifyChord(mask({0, 4, 7, 11}), m));
  CHECK_EQ(chordName(m.root, m.quality, m.bass), std::string("Cmaj7"));
  REQUIRE(identifyChord(mask({11, 2, 5, 9}), m));
  CHECK_EQ(chordName(m.root, m.quality, m.bass), std::string("Bm7b5"));
  REQUIRE(identifyChord(mask({0, 4, 7}), m, 4));
  CHECK_EQ(chordName(m.root, m.quality, m.bass), std::string("C/E"));
  REQUIRE(identifyChord(mask({2, 9}), m));
  CHECK_EQ(chordName(m.root, m.quality, m.bass), std::string("D5"));
  // C E G with an added D: still a C-rooted chord, below exact
  REQUIRE(identifyChord(mask({0, 2, 4, 7}), m));
  CHECK_EQ(m.root, 0);
  CHECK(m.score < 1.0);
  CHECK(!identifyChord(mask({5}), m));
}

SQ_TEST("theory: scales, keys and quantisation") {
  CHECK_EQ(scaleMask(0, Scale::Major), uint16_t(0xAB5));
  CHECK(inScale(64, 0, Scale::Major));
  CHECK(!inScale(61, 0, Scale::Major));
  CHECK(inScale(70, 0, Scale::Blues) == true);
  CHECK_EQ(quantizeToScale(61, 0, Scale::Major), 62);      // tie -> up
  CHECK_EQ(quantizeToScale(61, 0, Scale::Major, -1), 60);
  CHECK_EQ(quantizeToScale(66, 0, Scale::Major, 1), 67);
  CHECK_EQ(quantizeToScale(63, 9, Scale::NaturalMinor), 64);  // D# -> E in A minor (tie up)
  CHECK_EQ(quantizeToScale(68, 9, Scale::HarmonicMinor), 68);  // G# is in A harmonic minor
  CHECK(std::abs(quantizePitch(61.3, 0, Scale::Major) - 62.0) < 1e-12);
  CHECK(std::abs(quantizePitch(60.4, 0, Scale::Major) - 60.0) < 1e-12);
  CHECK_EQ(std::string(scaleName(Scale::Dorian)), std::string("dorian"));
  CHECK_EQ(keyName(0, false), std::string("C major"));
  CHECK_EQ(keyName(10, false), std::string("Bb major"));
  CHECK_EQ(keyName(6, true), std::string("F# minor"));
  CHECK_EQ(keyName(2, true), std::string("D minor"));
  CHECK_EQ(keyName(3, true), std::string("Eb minor"));
  CHECK(diatonicTriad(false, 1) == ChordQuality::Minor);
  CHECK(diatonicTriad(false, 6) == ChordQuality::Diminished);
  CHECK(diatonicTriad(true, 2) == ChordQuality::Major);
  CHECK_EQ(romanNumeral(0, false, 7, ChordQuality::Dominant7), std::string("V7"));
  CHECK_EQ(romanNumeral(0, false, 9, ChordQuality::Minor), std::string("vi"));
  CHECK_EQ(romanNumeral(0, false, 10, ChordQuality::Major), std::string("bVII"));
  CHECK_EQ(romanNumeral(9, true, 2, ChordQuality::Minor), std::string("iv"));
  CHECK_EQ(romanNumeral(0, false, 11, ChordQuality::Diminished), std::string("viio"));
}

SQ_TEST("theory: time quantisation with strength and swing") {
  CHECK(std::abs(quantizeTime(1.07, 0.25) - 1.0) < 1e-12);
  CHECK(std::abs(quantizeTime(1.2, 0.25) - 1.25) < 1e-12);
  CHECK(std::abs(quantizeTime(1.1, 0.25, 0.5) - 1.05) < 1e-12);
  CHECK(std::abs(quantizeTime(-0.1, 0.25) - 0.0) < 1e-12);
  // swing 0.5 moves odd 8ths from 0.25 to 0.3125
  CHECK(std::abs(quantizeTime(0.30, 0.25, 1.0, 0.5) - 0.3125) < 1e-12);
  CHECK(std::abs(quantizeTime(0.49, 0.25, 1.0, 0.5) - 0.5) < 1e-12);
  CHECK_EQ(quantizeTime(3.3, 0.0), 3.3);
}
