// Squizzy Studio engine/audio - IDocument -> GraphDesc translation (main thread only).
//
// Mapping (document.h):
//   SQ_TRACK_AUDIO       -> TrackDesc; each unmuted audio clip -> ClipNode(FileNode(source))
//   SQ_TRACK_INSTRUMENT  -> TrackDesc; each MIDI clip -> EventNode(built-in SineSynth) (plugin
//                           instruments are not part of the document model yet); audio clips
//                           (e.g. from bounce-in-place) play like on an audio track
//   SQ_TRACK_BUS / FX    -> BusDesc
//   SQ_TRACK_MASTER      -> master volume
//   SQ_TRACK_FOLDER      -> ignored
//   sq_track_info.output -> bus by GUID (null / unknown = master)
//   tempo map            -> constant-tempo segments at every tempo point (positions converted via
//                           ITempoMap::ppqToSeconds, so segment boundaries are exact; inside a
//                           ramp the frame<->ppq mapping is linear)
//   input channels       -> track property "input_channel" or "user.input_channel" (int, first device input),
//                           else 0; stereo tracks take two consecutive inputs.
//   inserts              -> track property "user.inserts" (one spec per line) through the optional
//                           InsertResolver (plugin_fx.hpp); unresolvable specs become warnings.
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
  // pan, mute, solo, monitor, arm, master volume) and cosmetic properties (names, colours, other
  // user.* keys). Two builds with the same key compile to the same graph structure, so the engine
  // can apply a document edit as RT mixer commands instead of recompiling (no audible
  // discontinuity, plugin instances and their state are kept).
  std::string structureKey;
};

sq_result buildGraphDesc(IDocument* doc, IClassRegistry* reg, MediaCache& cache, double engineSampleRate, GraphDesc* out,
                         DocBuildResult* info = nullptr, const InsertResolver* inserts = nullptr);

// Helpers shared with the engine.
std::string nodeName(INode* node);
std::string clipSourcePath(IClip* clip);

}  // namespace sq::engine::audio
