// Squizzy Studio engine/edit - arrange edit tools (plan 2.3 "Edit model", M3).
//
// Every tool is a native edoc::Action (executed through IEditor::execute, so it runs inside a
// transaction, rolls back on failure and is one undo step) plus a few declarative SideEffect
// propagators. Nothing here mutates the document any other way. On top of the actions sit
// keyboard-addressable commands (ICommandHandler classes, category "Command") that read the
// edit state (selection, cursor, grid, range) from document journals, so a full editing
// workflow needs no mouse path.
//
// MAIN THREAD ONLY (like engine/doc). Not RT-safe. Nothing here runs on the audio thread.
//
// ---- ACTIONS (class registry category "Action"; params are IAttributes keys) ----------------------
// Common params:
//   node:guid | nodes:blob(sq_guid[]) | nodes:string("{..} {..}")   target clips (deduplicated)
//   snap:string  "off" | "grid" | "relative" | "bar" | "events"     (default "off")
//   grid:double  grid step in ppq (default 1 = a quarter note)
// Positions/lengths are ppq (quarter notes) unless the key ends in _seconds or says "seconds".
// Locked clips (SQ_CLIP_LOCKED) named in node/nodes make the action fail with SQ_E_ACCESS.
// Results: "node" (first created/affected clip) and "nodes" (blob sq_guid[]) where noted.
//
//   kSplit         nodes at:double snap? grid?           splits every target clip that spans `at`
//                                                          -> nodes = the right parts
//   kTrimStart     nodes to:double snap? grid?           moves the clip start (content stays put);
//                                                          audio is clamped to the start of the source
//   kTrimEnd       nodes to:double snap? grid?           moves the clip end (clamped to the source end
//                                                          when the media length is known)
//   kSlip          nodes delta:double | delta_seconds:double  shifts the content inside fixed clip
//                                                          bounds (audio: source offset, clamped at 0;
//                                                          MIDI: event positions)
//   kMove          nodes (delta:double | start:double) track:guid? track_offset:int? snap? grid?
//                  tolerance:double?                      moves clips together; snapping applies to
//                                                          the earliest clip's start. Tagged as a move
//                                                          (automation-follow, remove-overlaps fire)
//   kFade          nodes in:double? out:double? (seconds) in_curve:string? out_curve:string?
//                  curves: "linear" | "exponential" | "logarithmic" | "scurve"  (stored in the clip
//                  user properties user.fadeInCurve / user.fadeOutCurve)
//   kCrossfade     nodes (exactly 2 audio clips on one track, overlapping or touching)
//                  length:double? (seconds, default 0.05) curve:string? (default "scurve")
//                  touching clips are extended into each other by length/2 each (the right clip
//                  only as far as its source allows); overlapping clips fade over the overlap.
//                  The placement edits are tagged as side-effect edits so remove-overlaps keeps the
//                  overlap.
//   kQuantize      nodes grid:double? strength:double(0..1, default 1) swing:double(0..1)
//                  lengths:bool?                         MIDI: note starts (and ends with lengths);
//                                                          audio: the clip start, or its anchor
//                                                          (user.anchor = seconds into the clip)
//   kStripSilence  nodes threshold_db:double(-48) min_silence:double(0.1 s) min_region:double(0.01 s)
//                  pad_before:double(0.005 s) pad_after:double(0.02 s)
//                                                          reads the source through a codec and keeps
//                                                          one clip per audible region -> nodes
//   kSilence       from:double to:double (tracks:blob(sq_guid[]) | nodes)?  removes the material in
//                  [from,to) from clips (split/trim/remove), no ripple. Unlocked clips only.
//   kDuplicate     nodes offset:double? count:int(1..256)?   copies placed `offset` later (default: the
//                                                          span of the targets, i.e. right after) -> nodes
//   kDelete        nodes ripple:string("none" | "track" | "all")   removes clips; "track" closes the
//                  gap on the clip's own track, "all" on every track (+ markers + automation)
//   kInsertTime    at:double length:double tracks:blob?  inserts empty time: content at/after `at`
//                  moves later (clips spanning `at` are split). Without `tracks` markers, automation,
//                  tempo and signature points move too.
//   kDeleteTime    from:double to:double tracks:blob?    removes [from,to) and closes the gap. Without
//                  `tracks` markers, automation and the tempo map are cut as well.
//   kFitTempo      from:double to:double seconds:double  sets one constant tempo on [from,to) so that
//                  range lasts `seconds`; the tempo after `to` is kept
//   kGlue          nodes (>= 2, one track, one kind)     MIDI: one clip with all visible events.
//                  Audio: only contiguous parts of one source with equal gain (else SQ_E_UNSUPPORTED;
//                  a rendered glue is a bounce-in-place job for the audio engine) -> node
//   kNormalize     nodes target_db:double(0) mode:string("independent" | "common") peak_db:double?
//                  sets clip gain so the played source range peaks at target_db (peak measured through
//                  a codec unless peak_db is given)
//   kGain          nodes (gain_db:double | delta_db:double)   clip gain, -144..+48 dB
//   kGainEnvelope  nodes points:blob(double[2n]: source seconds, gain dB)  audio only; stored as
//                  user.gainEnvelope (positions in source seconds, so trims/moves/tempo never shift
//                  it). An empty blob removes it.
//
// ---- SIDE EFFECTS (installed per document by installSideEffects) --------------------------------
//   audio-time-follow  after a tempo-map edit, audio clips on "musical" tracks keep their start in
//                      ppq but get a new ppq length so they still play the same number of seconds of
//                      source (unstretched audio). Clips flagged user.stretch=true and clips placed
//                      after the tempo edit in the same transaction are left alone. (Clips on "time"
//                      tracks are handled by engine/doc's time-map.)
//   fade-clamp         fades never exceed the clip: if fadeIn + fadeOut > the clip's duration in
//                      seconds (after a trim, split, tempo change...) both are scaled down to fit.
//
// ---- EDIT STATE (document journals: UI state, rides along with undo, never an undo step) ---------
//   edit.selection  space-separated "{GUID}" list of selected clips
//   edit.cursor     edit cursor (ppq)            edit.grid   grid step (ppq, default 1)
//   edit.snap       snap mode (default "grid")   edit.track  focused track "{GUID}"
//   edit.range      "from to" (ppq) time range
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sq/engine/doc/document.hpp"
#include "sq/interfaces/document.h"
#include "sq/interfaces/scripting.h"
#include "sq/module.h"

namespace sq::edit {

// ---- action class ids ----------------------------------------------------------------------------
inline constexpr sq_guid kSplit = sq::guid("{DD5328D8-4DE9-455B-9663-340011E94477}");
inline constexpr sq_guid kTrimStart = sq::guid("{5DFFBBE5-8B37-48B4-96D5-1816F5B8DD4B}");
inline constexpr sq_guid kTrimEnd = sq::guid("{3E761907-86BF-4B8F-B3CF-453897EC957C}");
inline constexpr sq_guid kSlip = sq::guid("{F988BB13-A792-4E16-A453-E59D8D2EA717}");
inline constexpr sq_guid kMove = sq::guid("{D23BC49B-1994-44DD-ACFE-45582912D1DD}");
inline constexpr sq_guid kFade = sq::guid("{59F4351B-3B34-4A23-BB2D-DAA65632602E}");
inline constexpr sq_guid kCrossfade = sq::guid("{669ADAAB-5CE1-4E99-A904-380A0AC92831}");
inline constexpr sq_guid kQuantize = sq::guid("{CAA292C5-0D31-47A3-8D7A-3146280D45C3}");
inline constexpr sq_guid kStripSilence = sq::guid("{32552BB0-D9DC-44BE-AA4C-C664843E9C08}");
inline constexpr sq_guid kSilence = sq::guid("{614060F3-BF89-4A4B-9DDF-C2F6156D37A8}");
inline constexpr sq_guid kDuplicate = sq::guid("{70F3883A-888D-4CE4-9649-4A532FFC72B3}");
inline constexpr sq_guid kDelete = sq::guid("{520491D3-D5DE-48B3-95F8-CA6BDA06F915}");
inline constexpr sq_guid kInsertTime = sq::guid("{1227D1E1-832A-4E88-8678-CDC742543A69}");
inline constexpr sq_guid kDeleteTime = sq::guid("{5330589D-38B1-477D-B571-F86600CE5556}");
inline constexpr sq_guid kFitTempo = sq::guid("{749C473D-C195-4667-8453-B4C056D0E96B}");
inline constexpr sq_guid kGlue = sq::guid("{402B17B7-BD16-4297-8A23-724E82546E0C}");
inline constexpr sq_guid kNormalize = sq::guid("{8B298575-D00B-40BD-BB1D-18121FCBC9E1}");
inline constexpr sq_guid kGain = sq::guid("{4D5C8AB3-9C42-4328-81FD-2993ED01E8F2}");
inline constexpr sq_guid kGainEnvelope = sq::guid("{304A5D46-684C-4FE9-BDF6-C1347B8CF020}");

// ---- command class ids (category "Command", ICommandHandler) -------------------------------------
// Each command reads its targets from args first, then from the edit state:
//   args: document:object(IDocument)? doc:guid? nodes/node? at/position? grid? snap? + per-command
// Commands that edit run one transaction named after the command (one undo step); the selection
// update rides along with it. Navigation commands only touch journals (no undo step).
namespace cmd {
// editing
inline constexpr sq_guid kSplit = sq::guid("{7278BF5B-018C-4982-9C70-F7EB26FC72A3}");          // Edit.Split
inline constexpr sq_guid kTrimStart = sq::guid("{D94357C6-8CC0-41EB-9DBF-A384D25E72F8}");      // Edit.TrimStartToCursor
inline constexpr sq_guid kTrimEnd = sq::guid("{5B1AF2BA-E08A-422B-93A6-430991775241}");        // Edit.TrimEndToCursor
inline constexpr sq_guid kSlipLeft = sq::guid("{28265DD2-6126-4FDC-BF20-4D359DDD1368}");       // Edit.SlipLeft
inline constexpr sq_guid kSlipRight = sq::guid("{FC89DFAA-23E1-48C7-A9D5-36EAA6F8D84A}");      // Edit.SlipRight
inline constexpr sq_guid kNudgeLeft = sq::guid("{73CDC6F7-BC78-47EE-A53C-4110FEF7D679}");      // Edit.NudgeLeft
inline constexpr sq_guid kNudgeRight = sq::guid("{C281E043-95B4-4918-A3EE-DFBE2C08C9DB}");     // Edit.NudgeRight
inline constexpr sq_guid kMoveToCursor = sq::guid("{0B800C17-46B2-4F5D-8AB1-B564257B00EF}");   // Edit.MoveToCursor
inline constexpr sq_guid kMoveTrackUp = sq::guid("{F54D22AC-8BE2-437D-BDEF-A63EC3C4CFF1}");    // Edit.MoveToTrackAbove
inline constexpr sq_guid kMoveTrackDown = sq::guid("{01FD85BA-26F5-430A-B64C-51B668D07D46}");  // Edit.MoveToTrackBelow
inline constexpr sq_guid kFadeInToCursor = sq::guid("{B81018A9-17D6-4EDA-9485-6513F737CF9C}"); // Edit.FadeInToCursor
inline constexpr sq_guid kFadeOutToCursor = sq::guid("{1D6BA7DC-BEB4-4160-AFA6-1A4CE50500BE}");// Edit.FadeOutFromCursor
inline constexpr sq_guid kCrossfade = sq::guid("{C0631551-6641-43F6-8748-D9DBD65171A5}");      // Edit.Crossfade
inline constexpr sq_guid kQuantize = sq::guid("{CE6D452D-7B0B-421B-B37F-5C620CEFF5CB}");       // Edit.Quantize
inline constexpr sq_guid kStripSilence = sq::guid("{D437C90E-BF0C-4B51-8364-B6E70B50943D}");   // Edit.StripSilence
inline constexpr sq_guid kSilence = sq::guid("{A47CFF9A-F8E0-461B-857A-441326F232D4}");        // Edit.SilenceRange
inline constexpr sq_guid kInsertTime = sq::guid("{BA291A0D-7798-4F55-92FC-B5D573D7E787}");     // Edit.InsertTime
inline constexpr sq_guid kDeleteTime = sq::guid("{2E935342-4C8F-4744-927B-3649FE4B3DB7}");     // Edit.DeleteTime
inline constexpr sq_guid kDuplicate = sq::guid("{AFCEA500-EECA-4DF9-B74A-706B734B14C9}");      // Edit.Duplicate
inline constexpr sq_guid kDelete = sq::guid("{2D7E677E-2F5D-405F-9DEE-68B74CFC1D3E}");         // Edit.Delete
inline constexpr sq_guid kDeleteRipple = sq::guid("{66CB95FF-5FB1-49FC-8114-073987244C69}");   // Edit.DeleteRipple
inline constexpr sq_guid kDeleteRippleAll = sq::guid("{2030B229-56B9-4B2E-91F3-34004D2424CB}");// Edit.DeleteRippleAll
inline constexpr sq_guid kGlue = sq::guid("{DE999512-0FCB-4BA6-A8D3-0BDB35121EC6}");           // Edit.Glue
inline constexpr sq_guid kNormalize = sq::guid("{C954F61A-A283-4493-86E2-F54312F14A9C}");      // Edit.Normalize
inline constexpr sq_guid kGainUp = sq::guid("{61D85405-108A-40A6-8285-021EA06AE9A6}");         // Edit.GainUp (+1 dB)
inline constexpr sq_guid kGainDown = sq::guid("{81285796-4545-4EB5-9370-C0C666DF73BA}");       // Edit.GainDown (-1 dB)
inline constexpr sq_guid kFitTempo = sq::guid("{976C36DB-8C2C-46C7-B670-7CD4D5AD542C}");       // Edit.FitTempoToRange
// navigation / edit state (journal only)
inline constexpr sq_guid kSelectNext = sq::guid("{780DD401-50EC-4A12-9E75-F557C6160CAC}");     // Navigate.SelectNextClip
inline constexpr sq_guid kSelectPrev = sq::guid("{382E0C06-04EF-45CF-9877-7CC46E003219}");     // Navigate.SelectPreviousClip
inline constexpr sq_guid kSelectAll = sq::guid("{191278E9-23EB-4146-8BF3-6D3500B1B929}");      // Navigate.SelectAll
inline constexpr sq_guid kSelectNone = sq::guid("{96AC497E-4B23-472D-A2D0-3C34CE0D62CE}");     // Navigate.SelectNone
inline constexpr sq_guid kCursorNextGrid = sq::guid("{1A9AFC9E-00B3-463D-ADD8-0A3D7961391F}"); // Navigate.CursorNextGrid
inline constexpr sq_guid kCursorPrevGrid = sq::guid("{04A7FB9C-82AF-444B-B08F-5D633F07AB30}"); // Navigate.CursorPreviousGrid
inline constexpr sq_guid kCursorToSelection = sq::guid("{5240261D-8A0F-4E4D-B5BF-4AF8D8C476B9}"); // Navigate.CursorToSelection
inline constexpr sq_guid kTrackNext = sq::guid("{EA410523-A9BD-435D-88E6-FC955A21AC15}");      // Navigate.NextTrack
inline constexpr sq_guid kTrackPrev = sq::guid("{08C2DE91-AED7-4137-8D33-6350AFA42927}");      // Navigate.PreviousTrack
inline constexpr sq_guid kRangeFromSelection = sq::guid("{177A7A96-6B1D-47D4-9FAA-8E7FE0E22C44}"); // Navigate.RangeFromSelection
inline constexpr sq_guid kSetEditState = sq::guid("{AC869C48-5F7D-4D5E-9ADF-99CFC82E7AB5}");   // Navigate.SetEditState
// Routes by sq_command category/name ("Edit"/"Split", "Navigate"/"NextTrack", ...).
inline constexpr sq_guid kDispatcher = sq::guid("{8B6EDD79-DEF0-4011-A747-B15ABF22C7D0}");
}  // namespace cmd

struct CommandInfo {
  sq_guid id;
  const char* category;  // "Edit" | "Navigate"
  const char* name;      // "Split", ...
  const char* title;     // undo description / menu text
  bool editsDocument;    // false: journal-only (no undo step)
};
const std::vector<CommandInfo>& commandTable();
const CommandInfo* findCommand(const sq_guid& id);
const CommandInfo* findCommand(const char* category, const char* name);

// Runs a command on `doc`. checkOnly: SQ_OK if enabled, SQ_COMMAND_DISABLED if not.
sq_result runCommand(IDocument* doc, const sq_guid& command, IAttributes* args, bool checkOnly);

// ---- edit state (journals) -----------------------------------------------------------------------
struct EditState {
  std::vector<sq_guid> selection;  // existing clips only
  double cursor = 0.0;
  double grid = 1.0;
  std::string snap = "grid";
  sq_guid track{};
  bool hasRange = false;
  double rangeFrom = 0.0, rangeTo = 0.0;
};
EditState readEditState(const edoc::DocumentImpl& doc);
// Journal writers. Inside a transaction the change rides along with it.
void setSelection(edoc::DocumentImpl& doc, const std::vector<sq_guid>& clips);
void setCursor(edoc::DocumentImpl& doc, double ppq);
void setGrid(edoc::DocumentImpl& doc, double ppq);
void setSnap(edoc::DocumentImpl& doc, const std::string& mode);
void setFocusTrack(edoc::DocumentImpl& doc, const sq_guid& track);
void setRange(edoc::DocumentImpl& doc, double from, double to);  // from >= to clears it

// ---- snapping ------------------------------------------------------------------------------------
enum class SnapMode { Off, Grid, Relative, Bar, Events };
bool parseSnapMode(const std::string& s, SnapMode& out);
// Absolute snap of a position (Grid: multiples of grid; Bar: bar lines of the signature map;
// Events: nearest clip edge / marker within `tolerance` ppq, else grid; Relative = Off here).
double snapPosition(const edoc::Store& s, double pos, SnapMode mode, double grid, double tolerance = -1.0,
                    const std::vector<sq_guid>& exclude = {});
// Bar line positions in [from, to] from the signature map.
std::vector<double> barLines(const edoc::TempoMapRec& tempo, double from, double to);

// ---- side effects --------------------------------------------------------------------------------
std::shared_ptr<edoc::SideEffect> makeAudioTimeFollow();
std::shared_ptr<edoc::SideEffect> makeFadeClamp();
// Adds the engine/edit side effects to a document once (idempotent). The edit actions and
// commands call it lazily; hosts call it for every opened document (registerEditServices does).
void installSideEffects(edoc::DocumentImpl& doc);

// ---- registration --------------------------------------------------------------------------------
// Registers the native actions in engine/doc's action table (idempotent).
void registerEditActions();
// Class registry entries: every edit action (category "Action", served by engine/doc's native
// action factory) and every command (category "Command"). Commands without an explicit document
// argument act on `manager`'s active document (manager may be null: then a document must be passed).
sq_result registerEditClasses(IClassRegistry* reg, IDocumentManager* manager);
void unregisterEditClasses(IClassRegistry* reg);
// Media access for normalize / strip silence / trim-end clamping: codecs are looked up in `reg`
// (category "Codec"). Borrowed; null disables media reads (those actions then need peak_db or fail
// with SQ_E_UNSUPPORTED).
void setMediaRegistry(IClassRegistry* reg);

// Host wiring (after engine/doc's registerDocumentServices): actions + classes, media registry,
// side-effect installation for every open and future document (observes the IDocumentManager).
sq_result registerEditServices(IHostContext* host);
void unregisterEditServices(IHostContext* host);

}  // namespace sq::edit
