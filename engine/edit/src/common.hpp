// Squizzy Studio engine/edit - private helpers shared by the action, side-effect and command files.
#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/engine/doc/document.hpp"
#include "sq/engine/edit/edit.hpp"

namespace sq::edit::detail {

using edoc::ClipRec;
using edoc::Editing;
using edoc::Store;
using edoc::TempoMapRec;
using edoc::TrackRec;

inline constexpr double kEps = 1e-9;  // ppq / seconds comparison tolerance

// ---- parameter readers (SQ_E_TYPE for a wrong type, SQ_E_INVALIDARG for non-finite) -------------
bool has(IAttributes* p, const char* key);
sq_result num(IAttributes* p, const char* key, double& out);
// Optional number: absent -> keeps `out` and returns SQ_OK.
sq_result optNum(IAttributes* p, const char* key, double& out);
sq_result optInt(IAttributes* p, const char* key, int64_t& out);
sq_result optBool(IAttributes* p, const char* key, bool& out);
sq_result optStr(IAttributes* p, const char* key, std::string& out);
sq_result guidParam(IAttributes* p, const char* key, sq_guid& out);
// "nodes" (blob sq_guid[] or string list) or "node" (guid or string). Empty -> SQ_E_INVALIDARG.
sq_result readNodes(IAttributes* p, std::vector<sq_guid>& out, const char* listKey = "nodes", const char* oneKey = "node");
// Optional guid list ("tracks"); absent -> empty + SQ_OK.
sq_result readGuidList(IAttributes* p, const char* key, std::vector<sq_guid>& out);
std::vector<sq_guid> parseGuidList(const std::string& s);
std::string formatGuidList(const std::vector<sq_guid>& ids);

// Target clips: SQ_E_NOTFOUND if one is missing, SQ_E_ACCESS if one is locked.
sq_result targetClips(const Store& s, const std::vector<sq_guid>& ids, std::vector<ClipRec>& out);
void putResultNodes(IAttributes* result, const std::vector<sq_guid>& ids);

// Snapping params: snap (default "off"), grid (default 1), tolerance (default grid/2).
struct SnapParams {
  SnapMode mode = SnapMode::Off;
  double grid = 1.0;
  double tolerance = -1.0;
};
sq_result readSnap(IAttributes* p, SnapParams& out);

// ---- time helpers ---------------------------------------------------------------------------------
inline double secs(const Store& s, double ppq) { return s.tempo.ppqToSeconds(ppq); }
inline double ppqAt(const Store& s, double seconds) { return s.tempo.secondsToPpq(seconds); }
double clipSeconds(const Store& s, const ClipRec& c);  // played duration in seconds
bool fitsTrack(int32_t clipKind, int32_t trackKind);
std::string fmt(double v);

// Content primitives (same semantics as engine/doc's trim/split; reimplemented because those
// helpers are private to engine/doc).
void trimClip(ClipRec& c, double newStart, double newEnd, const TempoMapRec& tempo);
ClipRec splitClip(ClipRec& left, double at, const TempoMapRec& tempo);
// Removes the material in [from,to) from clip `c` through `ed` (trim, split or remove). Returns the
// ids of what remains (c's id and/or the new right part).
std::vector<sq_guid> cutRange(Editing& ed, const ClipRec& c, double from, double to);

// ---- media (codec registry) -----------------------------------------------------------------------
struct MediaInfo {
  double sampleRate = 0.0;
  uint32_t channels = 0;
  int64_t frames = -1;  // -1 unknown
  double seconds() const { return frames >= 0 && sampleRate > 0 ? static_cast<double>(frames) / sampleRate : -1.0; }
};
IClassRegistry* mediaRegistry();
sq_result mediaInfo(const std::string& path, MediaInfo& out);
// Calls `fn(frameIndexFromStartSec, peakOfFrameAcrossChannels)` for each frame in
// [startSec, startSec + durSec) of the source (durSec < 0: to the end). SQ_E_UNSUPPORTED without a
// media registry or a codec for the file.
sq_result scanFrames(const std::string& path, double startSec, double durSec, double& sampleRate,
                     const std::function<void(int64_t, float)>& fn);

// ---- action plumbing --------------------------------------------------------------------------------
using PerformFn = sq_result (*)(Editing&, IAttributes*, IAttributes*);
struct ActionDef {
  sq_guid id;
  const char* name;
  PerformFn fn;
};
const std::vector<ActionDef>& clipActionDefs();   // actions_clip.cpp
const std::vector<ActionDef>& audioActionDefs();  // actions_audio.cpp
const std::vector<ActionDef>& timeActionDefs();   // actions_time.cpp

// Shifts tempo and signature points at/after `at` (never the one at 0) by `delta`. Record it
// BEFORE the clip edits of the same transaction: engine/doc's time-map (and audio-time-follow)
// leave clips placed after a tempo edit where the edit put them.
void shiftTempoMap(Editing& ed, double at, double delta);
// Shifts every clip (on `tracks`, or all when empty) starting at or after `at` by `delta`, the
// automation points of those tracks, and (when `markers`) the markers. Clips spanning `at` must
// have been split by the caller. Locked clips move along (they are never cut).
void shiftAfter(Editing& ed, double at, double delta, const std::vector<sq_guid>& tracks, bool markers);

}  // namespace sq::edit::detail

namespace sq::edit {
// Every edit action (clip + audio + time lists).
std::vector<detail::ActionDef> allActionDefs();
}  // namespace sq::edit
