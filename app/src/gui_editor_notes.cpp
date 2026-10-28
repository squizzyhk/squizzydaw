// Squizzy Studio GUI - piano roll note model.
#include "gui_editor_notes.hpp"

#include <algorithm>
#include <cmath>

namespace sq::app::gui::pr {

bool isNoteEvent(const sq_clip_event& e) { return e.size >= 3 && (e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0 && e.length > 0; }

std::vector<Note> extractNotes(const std::vector<sq_clip_event>& events) {
  std::vector<Note> out;
  out.reserve(events.size());
  for (size_t i = 0; i < events.size(); ++i) {
    const sq_clip_event& e = events[i];
    if (!isNoteEvent(e)) continue;
    Note n;
    n.pos = e.position;
    n.len = e.length;
    n.pitch = e.bytes[1] & 0x7F;
    n.vel = e.bytes[2] & 0x7F;
    n.chan = e.bytes[0] & 0x0F;
    n.src = static_cast<int>(i);
    out.push_back(n);
  }
  return out;
}

namespace {
sq_clip_event toEvent(const Note& n, const sq_clip_event* orig) {
  sq_clip_event e{};
  if (orig) e = *orig;
  e.struct_size = sizeof e;
  e.position = n.pos;
  e.length = n.len;
  e.bytes[0] = static_cast<uint8_t>(0x90 | (n.chan & 0x0F));
  e.bytes[1] = static_cast<uint8_t>(std::clamp(n.pitch, 0, 127));
  e.bytes[2] = static_cast<uint8_t>(std::clamp(n.vel, 1, 127));
  if (e.size < 3) e.size = 3;
  return e;
}
}  // namespace

std::vector<sq_clip_event> buildEvents(const std::vector<sq_clip_event>& original, const std::vector<Note>& notes) {
  // Edited notes by their source slot (a slot can be claimed once; copies carry src = -1).
  std::vector<int> bySrc(original.size(), -1);
  for (size_t k = 0; k < notes.size(); ++k) {
    const int s = notes[k].src;
    if (s >= 0 && static_cast<size_t>(s) < original.size() && bySrc[static_cast<size_t>(s)] < 0) bySrc[static_cast<size_t>(s)] = static_cast<int>(k);
  }
  std::vector<sq_clip_event> out;
  out.reserve(original.size() + notes.size());
  std::vector<bool> used(notes.size(), false);
  for (size_t i = 0; i < original.size(); ++i) {
    if (!isNoteEvent(original[i])) {
      out.push_back(original[i]);
      continue;
    }
    const int k = bySrc[i];
    if (k < 0) continue;  // deleted
    used[static_cast<size_t>(k)] = true;
    out.push_back(toEvent(notes[static_cast<size_t>(k)], &original[i]));
  }
  for (size_t k = 0; k < notes.size(); ++k)
    if (!used[k]) out.push_back(toEvent(notes[k], nullptr));
  std::stable_sort(out.begin(), out.end(), [](const sq_clip_event& a, const sq_clip_event& b) { return a.position < b.position; });
  return out;
}

void quantize(std::vector<Note>& notes, double grid, double origin, double strength, bool onlySelected) {
  if (!(grid > 0)) return;
  strength = std::clamp(strength, 0.0, 1.0);
  for (Note& n : notes) {
    if (onlySelected && !n.sel) continue;
    const double abs = origin + n.pos;
    const double q = std::round(abs / grid) * grid - origin;
    n.pos = std::max(0.0, n.pos + (q - n.pos) * strength);
  }
}

void legato(std::vector<Note>& notes, bool onlySelected) {
  std::vector<size_t> idx;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!onlySelected || notes[i].sel) idx.push_back(i);
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return notes[a].pos < notes[b].pos; });
  for (size_t k = 0; k < idx.size(); ++k) {
    Note& n = notes[idx[k]];
    for (size_t j = k + 1; j < idx.size(); ++j) {
      const double next = notes[idx[j]].pos;
      if (next > n.pos + 1e-9) {
        n.len = next - n.pos;
        break;
      }
    }
  }
}

std::string noteName(int pitch) {
  static const char* kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  pitch = std::clamp(pitch, 0, 127);
  return std::string(kNames[pitch % 12]) + std::to_string(pitch / 12 - 1);
}

const char* drumName(int pitch) {
  // Mirrors the piece mapping of the built-in drum kit (engine/audio/src/builtin/drums.cpp, pieceOf).
  switch (pitch) {
    case 35: return "Kick 2";
    case 36: return "Kick";
    case 37: return "Rim";
    case 38: return "Snare";
    case 39: return "Clap";
    case 40: return "Snare 2";
    case 41: return "Low Tom 2";
    case 42: return "Closed HH";
    case 43: return "Low Tom";
    case 44: return "Pedal HH";
    case 45: return "Mid Tom 2";
    case 46: return "Open HH";
    case 47: return "Mid Tom";
    case 48: return "High Tom";
    case 49: return "Crash";
    case 50: return "High Tom 2";
    case 51: return "Ride";
    case 52: return "China";
    case 53: return "Ride Bell";
    case 55: return "Splash";
    case 57: return "Crash 2";
    case 59: return "Ride 2";
    default: return nullptr;
  }
}

}  // namespace sq::app::gui::pr
