// Squizzy Studio GUI - piano roll note model (pure functions, no UI). The clip editor edits a
// working copy of the clip's notes and writes them back with ONE kSetClipEvents per gesture;
// everything that is not a note (CC, pitch bend, program changes...) is carried through
// untouched, in its original order and with all of its fields.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sq/interfaces/document.h"

namespace sq::app::gui::pr {

struct Note {
  double pos = 0;  // ppq, relative to the clip start
  double len = 1;  // ppq (> 0)
  int pitch = 60;  // 0..127
  int vel = 100;   // 1..127
  int chan = 0;    // 0..15
  int src = -1;    // index of the original event in the clip (-1 = new note)
  bool sel = false;
  double end() const { return pos + len; }
};

// A note event: status 0x9n, velocity > 0 and a length (the note-off is implied).
bool isNoteEvent(const sq_clip_event& e);
std::vector<Note> extractNotes(const std::vector<sq_clip_event>& events);
// The full event list for kSetClipEvents: non-note events and surviving edited notes keep their
// original slot (and every other field), new notes are appended; then stable-sorted by position
// (the order the document stores them in).
std::vector<sq_clip_event> buildEvents(const std::vector<sq_clip_event>& original, const std::vector<Note>& notes);

// Moves note starts toward the grid (absolute grid: `origin` = clip start in song ppq).
// strength 0..1. Only selected notes when onlySelected.
void quantize(std::vector<Note>& notes, double grid, double origin, double strength, bool onlySelected);
// Extends/shortens every (selected) note up to the next later note start; the last one is kept.
void legato(std::vector<Note>& notes, bool onlySelected);

// "C4" (MIDI 60 = C4), "F#2"...
std::string noteName(int pitch);
// Piece name of the built-in drum kit ("Kick", "Snare", "Closed HH"...) or nullptr for a pitch
// the kit does not play (it plays 35..59).
const char* drumName(int pitch);
constexpr int kDrumLo = 35, kDrumHi = 59;
inline bool isBlackKey(int pitch) {
  const int k = ((pitch % 12) + 12) % 12;
  return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

}  // namespace sq::app::gui::pr
