// Squizzy Studio engine/doc - plain C++ document model (records + store + change records).
//
// The model is a set of value records keyed by persistent GUIDs. Every mutation goes through a
// Change (before/after snapshot of exactly one record), which makes undo/redo, cancel and the
// side-effect propagators trivial and exact. The COM views (INode/ITrack/...) in nodes.cpp are
// thin read-only wrappers that look records up by id at call time.
//
// MAIN THREAD ONLY. Not RT-safe (allocates).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "sq/guid.h"
#include "sq/interfaces/document.h"

namespace sq::edoc {

// Typed value for user ("user.*") properties. type = SQ_VALUE_* (no OBJECT; blobs live in s).
struct Value {
  int32_t type = SQ_VALUE_NONE;
  int64_t i = 0;
  double d = 0.0;
  std::string s;
  sq_guid g{};
  bool operator==(const Value&) const = default;
};
using PropertyBag = std::map<std::string, Value>;  // sorted: deterministic serialization

struct AutoPoint {
  double pos = 0.0;  // ppq
  double value = 0.0;
  bool operator==(const AutoPoint&) const = default;
};
using AutomationLanes = std::map<std::string, std::vector<AutoPoint>>;  // param id -> points (sorted)

struct EventRec {  // mirror of sq_clip_event without struct_size
  double pos = 0.0;
  double len = 0.0;
  uint8_t bytes[4] = {0, 0, 0, 0};
  uint32_t size = 0;
  bool operator==(const EventRec&) const = default;
};

struct TrackRec {
  sq_guid id{};
  sq_guid parent{};  // folder track, null = top level
  int32_t kind = SQ_TRACK_AUDIO;
  std::string name;
  uint32_t channels = 2;
  uint32_t flags = 0;  // SQ_TRACK_MUTE ...
  uint32_t color = 0;
  double volumeDb = 0.0;
  double pan = 0.0;
  sq_guid output{};                  // null = master
  std::string timebase = "musical";  // "musical" | "time" (time: clips keep seconds on tempo edits)
  bool chordTrack = false;           // its MIDI clips define the chord progression
  bool followChords = false;         // its MIDI clips are transposed when chords change
  AutomationLanes automation;
  PropertyBag user;
  bool operator==(const TrackRec&) const = default;
};

struct ClipRec {
  sq_guid id{};
  sq_guid track{};
  int32_t kind = SQ_CLIP_AUDIO;
  std::string name;
  double start = 0.0;   // ppq
  double length = 0.0;  // ppq
  double sourceOffset = 0.0;  // seconds
  double gainDb = 0.0;
  double fadeIn = 0.0;   // seconds
  double fadeOut = 0.0;  // seconds
  uint32_t flags = 0;    // SQ_CLIP_MUTED / SQ_CLIP_LOCKED
  uint32_t color = 0;
  std::string source;    // audio file path
  std::vector<EventRec> events;  // MIDI, positions relative to clip start
  PropertyBag user;
  double end() const { return start + length; }
  bool operator==(const ClipRec&) const = default;
};

struct MarkerRec {
  sq_guid id{};
  double position = 0.0;
  double length = 0.0;
  uint32_t flags = 0;
  uint32_t color = 0;
  std::string name;
  PropertyBag user;
  bool operator==(const MarkerRec&) const = default;
};

struct TempoPoint {
  double pos = 0.0;
  double bpm = 120.0;
  uint32_t flags = 0;  // SQ_TEMPO_RAMP
  bool operator==(const TempoPoint&) const = default;
};
struct SigPoint {
  double pos = 0.0;
  uint32_t num = 4;
  uint32_t den = 4;
  bool operator==(const SigPoint&) const = default;
};

// Invariant: tempos and sigs are sorted by pos, non-empty, and the first point is at 0.
struct TempoMapRec {
  sq_guid id{};
  std::vector<TempoPoint> tempos{TempoPoint{}};
  std::vector<SigPoint> sigs{SigPoint{}};
  double ppqToSeconds(double ppq) const;
  double secondsToPpq(double seconds) const;
  bool operator==(const TempoMapRec&) const = default;
};

struct DocProps {
  int32_t kind = SQ_DOC_SONG;
  sq_guid id{};
  std::string name = "Untitled";
  double sampleRate = 48000.0;
  bool removeOverlaps = false;          // side effect: edited clips replace what they cover
  bool automationFollowsEdits = false;  // side effect: automation moves with moved clips
  PropertyBag user;
  bool operator==(const DocProps&) const = default;
};

// ---- change records ---------------------------------------------------------------------------
enum ChangeKind : uint32_t {
  kChangeDoc = 1u << 0,
  kChangeTempo = 1u << 1,
  kChangeTrack = 1u << 2,
  kChangeClip = 1u << 3,
  kChangeMarker = 1u << 4,
  kChangeJournal = 1u << 5,
};

// Why a change was made (side effects filter on it).
enum ChangeTag : uint32_t {
  kTagEdit = 0,        // generic
  kTagMove = 1,        // clip moved (MoveClip)
  kTagResize = 2,      // clip trimmed
  kTagSideEffect = 3,  // produced by a propagator
};

template <class R>
struct Delta {
  std::optional<R> before, after;
};
struct TrackDelta : Delta<TrackRec> {
  int32_t indexBefore = -1, indexAfter = -1;  // position in the flat track order
};
struct JournalDelta {
  std::string key;
  std::optional<std::string> before, after;
};

struct Change {
  ChangeKind kind = kChangeDoc;
  ChangeTag tag = kTagEdit;
  sq_guid id{};  // node affected (doc id for kChangeDoc / tempo map id for kChangeTempo)
  std::variant<Delta<DocProps>, Delta<TempoMapRec>, TrackDelta, Delta<ClipRec>, Delta<MarkerRec>, JournalDelta> data;
};

// ---- the store ----------------------------------------------------------------------------------
class Store {
 public:
  DocProps props;
  TempoMapRec tempo;
  std::vector<sq_guid> trackOrder;  // flat display order
  std::unordered_map<sq_guid, TrackRec> tracks;
  std::unordered_map<sq_guid, ClipRec> clips;
  std::unordered_map<sq_guid, MarkerRec> markers;
  std::map<std::string, std::string> journal;       // UI state (journals); not part of the "modified" state
  std::map<std::string, std::string> foreignParts;  // archive parts of other editors, kept verbatim

  const TrackRec* track(const sq_guid& id) const;
  const ClipRec* clip(const sq_guid& id) const;
  const MarkerRec* marker(const sq_guid& id) const;
  int32_t trackIndex(const sq_guid& id) const;  // -1 if missing

  // Derived, cached indices (invalidated by apply()).
  const std::vector<sq_guid>& clipsOf(const sq_guid& trackId) const;  // sorted by (start, id)
  const std::vector<sq_guid>& markerOrder() const;                    // sorted by (position, id)
  double contentLength() const;

  // Applies one change forward (after) or backward (before).
  void apply(const Change& c, bool forward);
  void invalidate() const {
    clipIndexDirty_ = true;
    markerIndexDirty_ = true;
  }

 private:
  void applyTrack(const sq_guid& id, const std::optional<TrackRec>& rec, int32_t index);
  mutable bool clipIndexDirty_ = true;
  mutable bool markerIndexDirty_ = true;
  mutable std::unordered_map<sq_guid, std::vector<sq_guid>> clipsByTrack_;
  mutable std::vector<sq_guid> markerOrder_;
};

// Helpers
sq_guid newGuid();
std::string guidStr(const sq_guid& g);

}  // namespace sq::edoc
