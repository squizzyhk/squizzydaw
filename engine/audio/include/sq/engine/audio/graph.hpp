// Squizzy Studio engine/audio - process graph: track -> fader/pan -> sub-mix -> master, with
// plugin delay compensation (PDC), optional 64-bit mix bus and a fast-lane monitor path.
//
// MODEL
//   GraphDesc (control thread, copyable, holds prototypes) --compile--> Graph (owns prepared
//   processor/node instances and every buffer). A Graph is used by exactly one thread at a time:
//   the audio thread (live) or a render worker (offline). The live engine swaps whole Graphs
//   without locks (see engine.hpp); nothing in a Graph is shared with another Graph except the
//   immutable MediaBuffers and the MeterBank (atomics).
//
// LATENCY BOOKKEEPING (plan 2.3)
//   arrival(track) = latency of its insert chain
//   maxIn(bus)     = max arrival over the bus inputs; every input edge gets a compensation delay
//                    of maxIn(bus) - arrival(input)
//   arrival(bus)   = maxIn(bus) + latency of the bus chain
//   latency()      = arrival(master) = total plugin delay of the main graph
//   Sources are read `latency()` frames ahead of the transport (ProcessContext::timelineFrame), so
//   the master output of a block represents timeline position ProcessContext::outputFrame: what
//   comes out of the graph is timeline-aligned whatever the plugin delay.
//   The fast lane (low-latency monitoring) bypasses all of this: device input -> zero-latency
//   inserts of the monitored track -> fader/pan -> device output, no compensation.
//   SENDS are edges too: a send from a track (or bus) to bus B arrives with the arrival of its
//   source (track: insert chain latency; bus: arrival(bus) incl. its chain), maxIn(B) is the max
//   over B's output inputs AND send inputs, and every send edge gets its own compensation delay
//   maxIn(B) - arrival(source). Send edges also order the buses (topological order over output +
//   send edges); a cycle fails compile (the doc builder already drops cyclic sends).
//
// INSTRUMENTS
//   A track with TrackDesc::instrument owns ONE synth instance. Its sequenced events
//   (TrackDesc::midi, timeline frames, read at ctx.timelineFrame while playing) and the live MIDI
//   routed to it (Graph::routeLiveMidi, tracks with liveMidi set) are merged per block and fed to
//   that synth, which renders whether or not the transport plays (live play while stopped). On a
//   transport stop / locate / loop wrap the track sends note-offs for the sequenced notes it
//   started and resets the controllers the sequence moved (sustain CC64=0 unless the live pedal
//   is down, pitch bend centre, mod wheel 0); a stop also sends all-notes-off (CC123) on the
//   channels the sequence used where no live note is held (no note chasing into a locate
//   position yet). Live notes and the live sustain pedal are tracked per track: when the track
//   stops receiving live MIDI (target / arm change) it releases them (note-offs, CC64=0).
//   The instrument's latency counts in the track's latency (PDC).
//
// AUTOMATION (automation.hpp format, resolved per graph into AutomationData)
//   While playing, Graph::process evaluates every lane at the timeline position of the audio the
//   target sees in this block: fader/pan/mute/send lanes at the read position minus the strip's
//   arrival (tracks: instrument + insert latency; buses: input arrival + bus chain latency),
//   insert parameter lanes minus the latency in front of that insert, and applies it before processing the
//   strip: volume/pan/mute as overrides of the static mixer values (the fader ramp smooths them),
//   send levels likewise, insert/instrument parameters through AudioProcessor::setParameter. When
//   the transport stops the overrides are dropped (static values again) and automated parameters
//   go back to AutoLane::staticValue. The live engine can swap a graph's automation without a
//   recompile (Graph::swapAutomation, see engine.hpp); offline renders use the compiled copy.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sq/guid.h"
#include "sq/engine/audio/automation.hpp"
#include "sq/engine/audio/nodes.hpp"
#include "sq/engine/audio/processor.hpp"

namespace sq::engine::audio {

// Wait-free meter storage shared between the audio thread (writer) and any reader thread.
class MeterBank {
 public:
  static constexpr uint32_t kSlots = 512;   // slot 0 = master
  static constexpr uint32_t kChannels = 8;
  MeterBank();
  // RT: peak accumulates (max) until read; rms is the latest block value.
  void update(uint32_t slot, uint32_t ch, float peak, float rms) noexcept;
  // Any non-RT thread. Resets the peak.
  void read(uint32_t slot, uint32_t ch, float* peak, float* rms) noexcept;
  void clear() noexcept;

 private:
  struct Cell {
    std::atomic<float> peak{0.0f};
    std::atomic<float> rms{0.0f};
  };
  std::unique_ptr<Cell[]> cells_;
};

struct TempoSegment {
  int64_t frame = 0;  // start of the segment
  double ppq = 0.0;
  double bpm = 120.0; // constant within the segment (ramps are approximated by the builder)
};

// RT-safe tempo conversions over constant-tempo segments.
class TempoMapRt {
 public:
  void set(std::vector<TempoSegment> segs, double sampleRate);
  double frameToPpq(int64_t frame) const noexcept;
  double ppqToFrame(double ppq) const noexcept;
  double bpmAt(int64_t frame) const noexcept;
  double sampleRate() const noexcept { return sr_; }

 private:
  size_t segAtFrame(int64_t f) const noexcept;
  std::vector<TempoSegment> segs_{TempoSegment{}};
  double sr_ = 48000.0;
};

// Parameter values written in an insert / instrument spec: (index, plain value).
using ParamList = std::vector<std::pair<uint32_t, double>>;

struct SendDesc {
  int32_t bus = -1;                    // destination bus index
  float level = 1.0f;                  // linear
  bool pre = false;                    // pre-fader (post-insert) instead of post-fader
};

// One resolved automation lane (see AUTOMATION above).
enum class AutoTarget : uint8_t { Volume, Pan, Mute, Send, Insert, Instrument };
struct AutoLane {
  AutoTarget target = AutoTarget::Volume;
  int32_t slot = -1;                   // Send: send index; Insert: chain index (bypassed/failed lines skipped)
  uint32_t param = 0;                  // Insert / Instrument parameter index
  float staticValue = 0.0f;            // Insert / Instrument: value restored when the transport stops
                                       // (NaN = unknown, e.g. a plugin parameter: not restored)
  AutomationLane lane;                 // ppq points; volume/send in dB, pan -1..1, mute 0/1
};
struct AutomationData {
  std::vector<std::vector<AutoLane>> tracks;  // indexed like GraphDesc::tracks (may be shorter)
  std::vector<std::vector<AutoLane>> buses;   // indexed like GraphDesc::buses (may be shorter)
  bool empty() const noexcept {
    for (auto& t : tracks)
      if (!t.empty()) return false;
    for (auto& b : buses)
      if (!b.empty()) return false;
    return true;
  }
};

struct TrackDesc {
  sq_guid id{};
  std::string name;
  uint32_t channels = 2;               // internal channel count of the track
  std::vector<SourceProto> sources;    // summed
  std::vector<ProcessorProto> inserts; // insert chain (serial)
  std::vector<ParamList> insertParams; // per inserts[] entry: values written in its spec
  // "user.inserts" line -> inserts[] index, -1 for bypassed / unavailable lines (doc_builder).
  std::vector<int32_t> slotToChain;
  // Per inserts[] entry: state carried over from the previous graph (empty = none). Set by the
  // engine right before compiling; applied in prepare() before the plugin is activated.
  std::vector<PluginState> insertStates;
  ProcessorProto instrument;           // instrument track: its one synth (null = audio track)
  ParamList instrumentParams;
  PluginState instrumentState;
  std::vector<TimedMidi> midi;         // sequenced events for the instrument (timeline frames)
  bool liveMidi = false;               // receives live MIDI (Graph::routeLiveMidi)
  std::vector<SendDesc> sends;
  float volume = 1.0f;                 // linear
  float pan = 0.0f;                    // -1..1
  bool mute = false;
  bool solo = false;
  int32_t output = -1;                 // bus index, -1 = master
  std::vector<uint32_t> inputChannels; // device input channels (monitor/record), size = channels or 1
  bool monitor = false;                // input monitoring
  bool armed = false;                  // record arm
  int32_t meterSlot = -1;
};

struct BusDesc {
  sq_guid id{};
  std::string name;
  uint32_t channels = 2;
  std::vector<ProcessorProto> inserts;
  std::vector<ParamList> insertParams;
  std::vector<int32_t> slotToChain;    // as TrackDesc::slotToChain
  std::vector<PluginState> insertStates;
  std::vector<SendDesc> sends;         // to other buses (cycles are rejected)
  float volume = 1.0f;
  float pan = 0.0f;
  bool mute = false;
  bool solo = false;                   // see SOLO in Graph::updateSolo
  int32_t output = -1;                 // bus index, -1 = master
  int32_t meterSlot = -1;
};

struct GraphDesc {
  double sampleRate = 48000.0;
  std::vector<TrackDesc> tracks;
  std::vector<BusDesc> buses;
  std::vector<ProcessorProto> masterInserts;
  std::vector<ParamList> masterInsertParams;
  std::vector<int32_t> masterSlotToChain;
  std::vector<PluginState> masterInsertStates;
  float masterVolume = 1.0f;
  float masterPan = 0.0f;              // -1..1 (stereo balance law, like a bus)
  bool masterMute = false;             // silences the mix (not the metronome click)
  int32_t masterMeterSlot = 0;
  std::vector<TempoSegment> tempo;     // empty = 120 bpm from frame 0
  uint32_t numerator = 4;
  uint32_t denominator = 4;
  std::shared_ptr<const AutomationData> automation;  // null = none
};

struct GraphConfig {
  uint32_t maxBlock = 512;
  uint32_t outputChannels = 2;        // master channels
  bool mix64 = false;                 // sum buses in double precision
  bool stemTaps = false;              // keep timeline-aligned per-track taps (stem/bounce render)
  bool stemPreFader = false;          // taps before fader/pan (track channels) instead of after
  MeterBank* meters = nullptr;        // optional
};

class Graph;

// Track strip: sources (+ instrument) -> (input monitor) -> inserts -> fader/pan. A processor
// variant whose process() writes the strip output (destination channel count) into io.
class TrackProcessor final : public AudioProcessor {
 public:
  static constexpr uint32_t kLiveCapacity = 256;  // live MIDI events per block
  explicit TrackProcessor(const TrackDesc& d);
  Kind kind() const noexcept override { return Kind::Track; }
  ProcessorPtr clone() const override;
  bool prepare(double sr, uint32_t maxFrames, uint32_t outChannels) override;
  void reset() noexcept override;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return chain_.latency() + (instrument_ ? instrument_->latency() : 0u); }
  uint32_t tail() const noexcept override;
  // Latency in front of insert chain index `slot` (instrument + inserts before it); slot -1 = 0.
  uint32_t latencyBefore(int32_t slot) const noexcept;

  // RT parameter setters (audio thread / owner thread of the graph).
  void setVolume(float v) noexcept { volume_ = v; }
  void setPan(float p) noexcept { pan_ = p; }
  void setMute(bool m) noexcept { mute_ = m; }
  void setSolo(bool s) noexcept { solo_ = s; }
  void setSoloSilenced(bool s) noexcept { soloSilenced_ = s; }
  void setMonitor(bool m) noexcept { monitor_ = m; }
  void setMonitorInMain(bool m) noexcept { monitorInMain_ = m; }
  // Turning live MIDI off releases the live notes / sustain pedal this track received (next block).
  void setLiveMidi(bool on) noexcept {
    if (liveMidi_ && !on) releaseLive_ = true;
    liveMidi_ = on;
  }
  // Automation overrides (RT): replace the static values while set.
  void setAutoVolume(float v) noexcept { autoVolume_ = v, hasAutoVolume_ = true; }
  void setAutoPan(float p) noexcept { autoPan_ = p, hasAutoPan_ = true; }
  void setAutoMute(bool m) noexcept { autoMute_ = m, hasAutoMute_ = true; }
  void clearAutomation() noexcept { hasAutoVolume_ = hasAutoPan_ = hasAutoMute_ = false; }
  float volume() const noexcept { return volume_; }
  float pan() const noexcept { return pan_; }
  float effectiveVolume() const noexcept { return hasAutoVolume_ ? autoVolume_ : volume_; }
  float effectivePan() const noexcept { return hasAutoPan_ ? autoPan_ : pan_; }
  bool solo() const noexcept { return solo_; }
  bool monitor() const noexcept { return monitor_; }
  bool liveMidi() const noexcept { return liveMidi_; }
  // Main output (and fast lane): false when muted (static or automated) or silenced by a solo
  // elsewhere (Graph::updateSolo).
  bool audible() const noexcept { return unmuted() && !soloSilenced_; }
  // Not muted (static or automated): gates the sends (each send has its own solo gate).
  bool unmuted() const noexcept { return !(hasAutoMute_ ? autoMute_ : mute_); }
  const TrackDesc& desc() const noexcept { return desc_; }
  uint32_t channels() const noexcept { return desc_.channels; }
  // Post-insert, pre-fader buffer of the last process() call.
  float* const* preFader() const noexcept { return bufPtrs_.data(); }
  int64_t contentEnd() const noexcept;
  // Insert chain entry (chain index) / the instrument; null when absent.
  AudioProcessor* insert(size_t i) const noexcept { return i < chain_.size() ? chain_.at(i) : nullptr; }
  size_t insertCount() const noexcept { return chain_.size(); }
  AudioProcessor* instrument() const noexcept { return instrument_.get(); }
  // RT: queue one live event for the next process() (dropped when the block buffer is full or
  // the track has no instrument).
  bool pushLiveMidi(const uint8_t* bytes, uint8_t size) noexcept;

 private:
  void renderInstrument(const ProcessContext& ctx) noexcept;
  TrackDesc desc_;
  std::vector<SourcePtr> sources_;
  ProcessorChain chain_;
  ProcessorPtr instrument_;
  std::vector<float> buf_;
  std::vector<float*> bufPtrs_;
  uint32_t maxFrames_ = 0;
  uint32_t outChannels_ = 2;
  float volume_ = 1.0f, pan_ = 0.0f;
  bool mute_ = false, solo_ = false, soloSilenced_ = false, monitor_ = false, monitorInMain_ = true;
  bool liveMidi_ = false;
  float autoVolume_ = 1.0f, autoPan_ = 0.0f;
  bool autoMute_ = false, hasAutoVolume_ = false, hasAutoPan_ = false, hasAutoMute_ = false;
  float curGain_[kMaxChannels] = {};
  bool gainInit_ = false;
  // instrument event state (RT)
  std::vector<sq_midi_event> blockEvents_;
  sq_midi_event live_[kLiveCapacity] = {};
  uint32_t liveCount_ = 0;
  size_t cursor_ = 0;
  int64_t expectNext_ = INT64_MIN;
  bool seqPlaying_ = false;
  uint8_t seqNotes_[16][128] = {};  // sequenced notes currently on (note-offs on stop / jump)
  // Per channel: controllers the sequence moved (kSeq* bits; reset on stop / jump).
  uint8_t seqCtl_[16] = {};
  uint8_t liveNotes_[16][128] = {};  // live notes currently on
  uint8_t liveSustain_[16] = {};     // live sustain pedal down
  bool releaseLive_ = false;
};

// Sub-mix / bus: (summed inputs) -> inserts -> fader/pan. The graph sums inputs into input().
class SubMixProcessor final : public AudioProcessor {
 public:
  SubMixProcessor(uint32_t channels, std::vector<ProcessorProto> inserts, float volume, float pan, bool mute);
  Kind kind() const noexcept override { return Kind::SubMix; }
  ProcessorPtr clone() const override;
  bool prepare(double sr, uint32_t maxFrames, uint32_t outChannels) override;
  void reset() noexcept override;
  // Sum buffers: callers add into them (float or double depending on mix64), then process().
  void clearInput(uint32_t frames) noexcept;
  void addInput(const float* const* src, uint32_t srcChannels, uint32_t frames) noexcept;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return chain_.latency(); }
  uint32_t tail() const noexcept override { return chain_.tail(); }
  // Latency of the inserts in front of chain index `slot`.
  uint32_t latencyBefore(int32_t slot) const noexcept;
  // Control thread, before prepare(): carried plugin states per protos_[] entry (see TrackDesc).
  void setInitialStates(std::vector<PluginState> states) { states_ = std::move(states); }
  void setVolume(float v) noexcept { volume_ = v; }
  void setPan(float p) noexcept { pan_ = p; }
  void setMute(bool m) noexcept { mute_ = m; }
  void setSolo(bool s) noexcept { solo_ = s; }
  // Main output silenced by a solo elsewhere (Graph::updateSolo); sends are gated per edge.
  void setSoloSilenced(bool s) noexcept { soloSilenced_ = s; }
  void setAutoVolume(float v) noexcept { autoVolume_ = v, hasAutoVolume_ = true; }
  void setAutoPan(float p) noexcept { autoPan_ = p, hasAutoPan_ = true; }
  void setAutoMute(bool m) noexcept { autoMute_ = m, hasAutoMute_ = true; }
  void clearAutomation() noexcept { hasAutoVolume_ = hasAutoPan_ = hasAutoMute_ = false; }
  float volume() const noexcept { return volume_; }
  float pan() const noexcept { return pan_; }
  bool mute() const noexcept { return mute_; }
  bool solo() const noexcept { return solo_; }
  float effectiveVolume() const noexcept { return hasAutoVolume_ ? autoVolume_ : volume_; }
  float effectivePan() const noexcept { return hasAutoPan_ ? autoPan_ : pan_; }
  // Not muted (static or automated): gates the sends (with their own solo gate).
  bool unmuted() const noexcept { return !(hasAutoMute_ ? autoMute_ : mute_); }
  // Main output audible: not muted and not silenced by a solo.
  bool audible() const noexcept { return unmuted() && !soloSilenced_; }
  uint32_t channels() const noexcept { return channels_; }
  void setMix64(bool on) noexcept { mix64_ = on; }
  // Post-insert, pre-fader buffer of the last process() call (bus sends).
  float* const* preFader() const noexcept { return workPtrs_.data(); }
  AudioProcessor* insert(size_t i) const noexcept { return i < chain_.size() ? chain_.at(i) : nullptr; }

 private:
  uint32_t channels_;
  std::vector<ProcessorProto> protos_;
  std::vector<PluginState> states_;
  ProcessorChain chain_;
  float volume_, pan_;
  bool mute_;
  bool solo_ = false, soloSilenced_ = false;
  bool mix64_ = false;
  float autoVolume_ = 1.0f, autoPan_ = 0.0f;
  bool autoMute_ = false, hasAutoVolume_ = false, hasAutoPan_ = false, hasAutoMute_ = false;
  std::vector<float> sumF_;
  std::vector<double> sumD_;
  std::vector<float> work_;
  std::vector<float*> workPtrs_;
  uint32_t maxFrames_ = 0;
  uint32_t outChannels_ = 2;
  float curGain_[kMaxChannels] = {};
  bool gainInit_ = false;
};

// Metronome click generator (stateless: block-size invariant, bit-exact across renders).
class Metronome {
 public:
  void configure(const TempoMapRt* tempo, uint32_t numerator, uint32_t denominator, double sr);
  // RT: adds clicks for output frames [frame, frame+frames) into out channels.
  void render(int64_t frame, uint32_t frames, float* const* out, uint32_t channels) noexcept;
  void setGain(float g) noexcept { gain_ = g; }

 private:
  const TempoMapRt* tempo_ = nullptr;
  uint32_t num_ = 4, den_ = 4;
  double sr_ = 48000.0;
  int64_t clickLen_ = 1440;
  float gain_ = 0.5f;
};

class Graph {
 public:
  ~Graph();
  // Control thread: instantiate + prepare everything. Null (and *error) on invalid routing.
  static std::unique_ptr<Graph> compile(const GraphDesc& desc, const GraphConfig& cfg, std::string* error = nullptr);

  // RT. ctx.frames <= maxBlock; ctx.outputFrame set by the caller (timelineFrame is derived from
  // it, unless ctx.timelineExplicit). The fast lane (lowLatencyMonitoring) follows each track's
  // fader, pan, mute and solo state and the master volume, but no bus and no latent insert.
  // Writes exactly ctx.frames frames to out[0..outChannels) (silence where nothing plays).
  void process(const ProcessContext& ctx, float* const* out, uint32_t outChannels, bool lowLatencyMonitoring,
               bool metronome) noexcept;
  void reset() noexcept;

  // ---- latency bookkeeping ----
  uint32_t latency() const noexcept { return latency_; }
  uint32_t trackArrival(size_t i) const noexcept { return trackArrival_[i]; }
  uint32_t trackCompensation(size_t i) const noexcept { return trackComp_[i].delay(); }
  uint32_t busArrival(size_t i) const noexcept { return busArrival_[i]; }
  uint32_t busCompensation(size_t i) const noexcept { return busComp_[i].delay(); }

  // ---- structure ----
  size_t trackCount() const noexcept { return tracks_.size(); }
  TrackProcessor& track(size_t i) noexcept { return *tracks_[i]; }
  size_t busCount() const noexcept { return buses_.size(); }
  SubMixProcessor& bus(size_t i) noexcept { return *buses_[i]; }
  const sq_guid& busId(size_t i) const noexcept { return busIds_[i]; }
  SubMixProcessor& master() noexcept { return *master_; }
  int32_t findTrack(const sq_guid& id) const noexcept;
  int32_t findBus(const sq_guid& id) const noexcept;
  void setMasterVolume(float v) noexcept { masterVolume_ = v; }
  void setMasterPan(float p) noexcept { master_->setPan(p); }
  void setMasterMute(bool m) noexcept { master_->setMute(m); }
  // SOLO (RT, allocation-free; call after any track/bus solo change). With nothing soloed every
  // strip plays. Otherwise an edge (a strip's main output or one of its sends) X -> Y carries
  // signal only when X is soloed or fed (directly or through buses/sends) by a soloed strip, or
  // Y is soloed or feeds a soloed bus. So a soloed track is heard with the buses it reaches
  // (solo-in-place through sends: its FX returns stay audible), a soloed bus is heard with every
  // track/bus feeding it (only the paths into it: their dry outputs elsewhere are silenced), and
  // everything else is silent.
  void updateSolo() noexcept;
  int64_t contentEnd() const noexcept;
  uint32_t maxTail() const noexcept;
  const TempoMapRt& tempo() const noexcept { return tempo_; }
  uint32_t numerator() const noexcept { return numerator_; }
  uint32_t denominator() const noexcept { return denominator_; }
  double sampleRate() const noexcept { return sr_; }
  uint32_t maxBlock() const noexcept { return maxBlock_; }
  uint32_t outputChannels() const noexcept { return outChannels_; }
  Metronome& metronome() noexcept { return metronome_; }

  // ---- live parameters (RT / owner thread) ----
  // slot = insert chain index; slot -1 on a track = its instrument. Also reaches the fast lane's
  // copy of a zero-latency insert.
  void setTrackParam(size_t track, int32_t slot, uint32_t param, float value) noexcept;
  void setBusParam(size_t bus, int32_t slot, uint32_t param, float value) noexcept;
  void setMasterParam(int32_t slot, uint32_t param, float value) noexcept;
  void setTrackSendLevel(size_t track, size_t send, float level) noexcept;
  void setBusSendLevel(size_t bus, size_t send, float level) noexcept;
  size_t trackSendCount(size_t track) const noexcept { return track < trackSends_.size() ? trackSends_[track].size() : 0; }
  // Compensation delay of a track's send edge (PDC bookkeeping, tests).
  uint32_t sendCompensation(size_t track, size_t send) const noexcept;
  // Live MIDI to every track whose liveMidi flag is set (RT).
  void routeLiveMidi(const uint8_t* bytes, uint8_t size) noexcept;

  // ---- automation ----
  // RT: installs `data` as this graph's automation (owned; deleted by ~Graph on the control
  // thread) and returns the previously installed external data (caller retires it), or null.
  AutomationData* swapAutomation(AutomationData* data) noexcept;
  const AutomationData* automation() const noexcept { return automation_; }

  // ---- stem taps (GraphConfig::stemTaps) ----
  uint32_t stemChannels(size_t track) const noexcept { return stemChannels_[track]; }
  // Timeline-aligned tap of the last process() block.
  float* const* stem(size_t track) const noexcept { return stemPtrs_[track].data(); }

  uint64_t generation() const noexcept { return generation_; }

 private:
  Graph() = default;
  void sumInto(int32_t dest, float* const* src, uint32_t ch, uint32_t frames) noexcept;
  void processFastLane(const ProcessContext& ctx, float* const* out, uint32_t outChannels) noexcept;
  void applyAutomation(const ProcessContext& ctx) noexcept;
  void endAutomation() noexcept;

  // One send edge (track or bus -> bus).
  struct Send {
    bool fromBus = false;
    uint32_t source = 0;
    int32_t dest = 0;
    bool pre = false;
    float level = 1.0f;
    float autoLevel = 1.0f;
    bool hasAuto = false;
    DelayLine comp;
    std::vector<float> buf;
    std::vector<float*> ptrs;
    float cur[kMaxChannels] = {};
    bool init = false;
    bool soloSilenced = false;  // updateSolo
  };
  void runSend(Send& s, const float* const* src, uint32_t srcCh, float faderVolume, float pan, bool audible, uint32_t frames) noexcept;

  double sr_ = 48000.0;
  uint32_t maxBlock_ = 512;
  uint32_t outChannels_ = 2;
  bool mix64_ = false;
  uint64_t generation_ = 0;
  MeterBank* meters_ = nullptr;
  int32_t masterMeter_ = 0;

  std::vector<std::unique_ptr<TrackProcessor>> tracks_;
  std::vector<int32_t> trackDest_;
  std::vector<uint32_t> trackArrival_;
  std::vector<DelayLine> trackComp_;
  std::vector<std::vector<float>> trackOut_;
  std::vector<std::vector<float*>> trackOutPtrs_;
  std::vector<int32_t> trackMeter_;

  std::vector<std::unique_ptr<SubMixProcessor>> buses_;
  std::vector<int32_t> busDest_;
  std::vector<uint32_t> busOrder_;
  std::vector<uint32_t> busArrival_;
  std::vector<DelayLine> busComp_;
  std::vector<std::vector<float>> busOut_;
  std::vector<std::vector<float*>> busOutPtrs_;
  std::vector<int32_t> busMeter_;
  std::vector<sq_guid> busIds_;
  std::vector<uint8_t> busDown_, busUp_;  // updateSolo scratch (fed by / feeds a soloed strip)

  std::unique_ptr<SubMixProcessor> master_;
  std::vector<float> masterBuf_;
  std::vector<float*> masterPtrs_;
  float masterVolume_ = 1.0f;
  uint32_t latency_ = 0;

  // fast lane: one zero-latency chain per track, with its own fader ramp state
  std::vector<ProcessorChain> fastChains_;
  std::vector<float> fastGain_;          // trackCount * kMaxChannels
  std::unique_ptr<bool[]> fastGainInit_;
  std::vector<float> fastBuf_;
  std::vector<float*> fastPtrs_;

  // stems
  bool stemTaps_ = false, stemPre_ = false;
  std::vector<uint32_t> stemChannels_;
  std::vector<std::vector<float>> stemBuf_;
  std::vector<std::vector<float*>> stemPtrs_;
  std::vector<DelayLine> stemDelay_;

  // fast lane slot map: per track, chain index -> fast chain index (-1 = latent, not in the lane)
  std::vector<std::vector<int32_t>> fastSlot_;

  // sends
  std::vector<std::unique_ptr<Send>> sends_;
  std::vector<std::vector<uint32_t>> trackSends_, busSends_;  // indices into sends_
  std::vector<uint32_t> busMaxIn_;

  // automation
  std::shared_ptr<const AutomationData> automationKeep_;  // compiled from the desc
  AutomationData* extAutomation_ = nullptr;               // swapped in live (owned)
  const AutomationData* automation_ = nullptr;
  bool automating_ = false;

  TempoMapRt tempo_;
  uint32_t numerator_ = 4, denominator_ = 4;
  Metronome metronome_;
};

// Mixing helper shared by tracks/buses: fader+pan of src (srcCh) into dst (dstCh), ramping the
// per-channel gain from cur[] to the new target over the block (cur[] updated).
void applyFaderPan(const float* const* src, uint32_t srcCh, float* const* dst, uint32_t dstCh, uint32_t frames,
                   float volume, float pan, float* cur, bool* init) noexcept;

}  // namespace sq::engine::audio
