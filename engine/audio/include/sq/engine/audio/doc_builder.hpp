// Squizzy Studio engine/audio - IDocument -> GraphDesc translation (main thread only).
//
// Mapping (document.h):
//   SQ_TRACK_AUDIO       -> TrackDesc; each unmuted audio clip -> ClipNode(FileNode(source))
//   SQ_TRACK_INSTRUMENT  -> TrackDesc with ONE instrument (TrackDesc::instrument) built from the
//                           track property "user.instrument" (a spec, builtin.hpp) through the
//                           InstrumentResolver; an empty or unresolvable spec falls back to
//                           kDefaultInstrument through the resolver, then to the built-in SineSynth
//                           (warnings). Without a resolver the SineSynth is used. The events of all
//                           unmuted MIDI clips are merged into TrackDesc::midi (cut at the clip
//                           bounds, note-offs at the note or clip end). The instrument exists with no
//                           clips too (live MIDI). Audio clips (e.g. from bounce-in-place) play like
//                           on an audio track.
//   SQ_TRACK_BUS / FX    -> BusDesc (+ its "user.inserts" chain)
//   SQ_TRACK_MASTER      -> master volume + master inserts ("user.inserts")
//   SQ_TRACK_FOLDER      -> ignored
//   sq_track_info.output -> bus by GUID (null / unknown = master)
//   tempo map            -> constant-tempo segments at every tempo point (positions converted via
//                           ITempoMap::ppqToSeconds, so segment boundaries are exact; inside a
//                           ramp the frame<->ppq mapping is linear)
//   input channels       -> track property "input_channel" or "user.input_channel" (int, first device input),
//                           else 0; stereo tracks take two consecutive inputs.
//   inserts              -> track property "user.inserts" (one spec per line) through the optional
//                           InsertResolver (plugin_fx.hpp); unresolvable specs become warnings. A
//                           line starting with '!' is BYPASSED (left out of the chain, keeps its
//                           slot number). Slots (commands, automation "insert.<slot>.<p>") count
//                           every non-empty line.
//   sends                -> track property "user.sends": one send per line
//                           "<bus guid> <level dB> [pre]" (post-fader unless "pre") to a bus/FX
//                           track, from audio/instrument tracks and buses. Missing destinations and
//                           sends that would close a loop (a bus to itself or a cycle through
//                           outputs/sends) are ignored with a warning. "send.<n>" = line n.
//   automation           -> track property "automation" (read-only text, automation.hpp; written by
//                           edoc::kSetAutomation), resolved into GraphDesc::automation.
// Structure key: parameter values in specs, send levels and automation are NOT part of it, so
// edits of those are applied live by the engine (no recompile).
// Audio sources are decoded through the codec registry once and cached by path (MediaCache).
#pragma once

#include <map>
#include <string>

#include "sq/sdk.hpp"
#include "sq/interfaces/document.h"
#include "sq/engine/audio/graph.hpp"
#include "sq/engine/audio/plugin_fx.hpp"

namespace sq::engine::audio {

class MediaCache {
 public:
  // Returns cached media or decodes it. reg = codec registry.
  sq_result get(IClassRegistry* reg, const std::string& path, MediaPtr* out);
  void clear() { map_.clear(); }
  size_t size() const { return map_.size(); }

 private:
  std::map<std::string, MediaPtr> map_;
};

struct DocBuildResult {
  std::vector<std::string> warnings;  // missing media etc. (the graph is still built)
  // Everything that shapes the compiled graph EXCEPT the live mixer parameters (track/bus volume,
  // pan, mute, solo, monitor, arm, master volume), parameter values inside insert/instrument specs,
  // send levels, automation, and cosmetic properties (names, colours, other user.* keys). Two builds with the same key compile to the same graph structure, so the engine
  // can apply a document edit as RT mixer commands instead of recompiling (no audible
  // discontinuity, plugin instances and their state are kept).
  std::string structureKey;
};

sq_result buildGraphDesc(IDocument* doc, IClassRegistry* reg, MediaCache& cache, double engineSampleRate, GraphDesc* out,
                         DocBuildResult* info = nullptr, const InsertResolver* inserts = nullptr,
                         const InstrumentResolver* instruments = nullptr);

// Helpers shared with the engine.
std::string nodeName(INode* node);
std::string clipSourcePath(IClip* clip);

}  // namespace sq::engine::audio
