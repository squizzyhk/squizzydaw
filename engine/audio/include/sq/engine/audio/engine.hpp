// Squizzy Studio engine/audio - the realtime engine core and the IAudioEngine service.
//
// EngineCore (IAudioCallback) is what runs on the audio thread:
//   - GRAPH SWAP without locks: the control thread compiles a new Graph and publishes it through
//     an atomic pointer; the audio thread picks it up at the start of the next callback and pushes
//     the old one into an RT->control SPSC "retire" queue; the control thread deletes retired
//     graphs in collectGarbage(). The control thread never frees a graph the RT thread may use.
//     When no device is running the control thread installs the graph directly.
//   - COMMANDS control->RT through an SPSC queue (transport, track params, monitoring...). Track
//     commands carry the graph generation and are ignored by any other graph.
//   - METERS RT->any thread through MeterBank atomics; TRANSPORT snapshot through a seqlock of
//     atomics (readers never block the writer; the writer never waits).
//   - Recording: RecordStream rings (recorder.hpp) drained by a non-RT disk writer thread.
//   - LIVE MIDI: pushLiveMidi() (any non-RT thread; producers serialise on a spinlock that the
//     audio thread never takes) -> bounded queue (overflow drops) -> at the start of each callback
//     the audio thread routes the events to the tracks whose liveMidi flag is set (instrument
//     tracks render while the transport is stopped). While recording with MIDI capture on, the
//     same events are copied with the transport frame into an RT->control capture queue.
//   - AUTOMATION SWAP: a SetAutomation command carries a heap AutomationData; the audio thread
//     installs it into the current graph (Graph::swapAutomation) and pushes the replaced one (or
//     the command's own data if the generation no longer matches) into an RT->control retire
//     queue; collectGarbage() deletes it. Graphs delete their installed data in ~Graph.
//
// AudioEngine implements IAudioEngine + ITransport + ISubject (main thread), owns the device and
// the document binding, builds GraphDescs from IDocument (doc_builder.hpp) and runs render tasks.
// A document edit whose structure key is unchanged is applied without a recompile: mixer values,
// insert/instrument parameter values (TrackParam/BusParam/MasterParam -> setParameter on the live
// processors), send levels, live-MIDI routing and automation (SetAutomation swap) are commands.
// Instrument tracks: one synth per track (InstrumentResolver), live MIDI to the armed instrument
// tracks or else the live MIDI target (selected track), MIDI recording into one clip per armed
// instrument track committed with the audio takes in one undo step.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/interfaces/audiodevice.h"
#include "sq/interfaces/audioengine.h"
#include "sq/interfaces/document.h"
#include "sq/engine/audio/doc_builder.hpp"
#include "sq/engine/audio/graph.hpp"
#include "sq/engine/audio/recorder.hpp"
#include "sq/engine/audio/render.hpp"

namespace sq::engine::audio {

enum class CmdType : uint32_t {
  Play, Stop, Locate, Loop, Metronome, Record, LowLatency, MasterVolume,
  TrackVolume, TrackPan, TrackMute, TrackSolo, TrackMonitor, BusVolume, BusMute,
  TrackParam, BusParam, MasterParam, TrackSendLevel, BusSendLevel, TrackLiveMidi, BusPan,
  SetAutomation, MidiCapture, MasterPan, MasterMute, BusSolo
};

struct Command {
  CmdType type = CmdType::Play;
  int32_t index = -1;           // track / bus index for Track* / Bus*
  uint64_t generation = 0;      // graph generation for Track* / Bus* / *Param / SetAutomation
  double value = 0.0;
  int64_t a = 0, b = 0;         // Locate: a; Loop: a..b, value != 0 = enabled;
                                // *Param: a = chain slot (-1 = instrument), b = parameter;
                                // *SendLevel: a = send index
  void* ptr = nullptr;          // SetAutomation: AutomationData* (ownership passes to the core)
  bool fastLaneOnly = false;    // TrackParam: the main-chain instance already holds the value
                                // (AudioEngine::notePluginValues); only the fast-lane copy gets it
};

// Live MIDI event (short channel message).
struct LiveMidiEvent {
  uint8_t bytes[3];
  uint8_t size;
};
// Live MIDI captured while recording: the timeline frame the player heard when the event arrived
// (transport frame of the block minus the device output latency). While looping, a note held
// across the loop wrap is closed at the loop end and re-opened at the loop start (pairs of
// synthetic note-off/note-on events), so every captured note-on pairs with a later note-off.
struct CapturedMidi {
  int64_t frame;
  uint8_t bytes[3];
  uint8_t size;
};

struct TransportSnapshot {
  uint32_t flags = 0;           // SQ_TRANSPORT_*
  int64_t frame = 0;
  double ppq = 0.0;
  double bpm = 120.0;
  uint32_t numerator = 4, denominator = 4;
  int64_t loopStart = 0, loopEnd = 0;
  double loopStartPpq = 0.0, loopEndPpq = 0.0;
  double sampleRate = 48000.0;
};

class EngineCore final : public sq::Object<EngineCore, IAudioCallback> {
 public:
  EngineCore();
  ~EngineCore() override;

  // ---- control thread ----
  // Publishes g (takes ownership). Returns false if the retire queue is congested (call
  // collectGarbage() and retry) - the graph is then kept and published by the next call.
  void setGraph(std::unique_ptr<Graph> g);
  void collectGarbage();
  // Queues a command (applied directly when the device is not running).
  bool post(const Command& c);
  void setDeviceLatency(uint32_t inFrames, uint32_t outFrames) noexcept;
  // Takes effect at the next record start: recorded timeline position = transport - in - out.
  uint32_t recordCompensation() const noexcept { return recordComp_.load(); }
  void startRecording(std::unique_ptr<RecordSession> session);
  // Detaches the session from the audio thread, finishes it (drains + finalizes the writers) and
  // returns its takes. false when no session was active. If the audio thread does not confirm
  // the detach in time (stalled device), the finished session is kept alive and freed by
  // collectGarbage() once a later callback completed: the RT side never sees freed memory.
  bool stopRecording(std::vector<RecordedTake>* takes);
  bool isRecordingSessionActive() const noexcept { return session_.load() != nullptr; }
  // Any non-RT thread: one or more short MIDI messages (running status allowed; sysex, system
  // common and realtime bytes are skipped). false when something was dropped (queue full).
  bool pushLiveMidi(const uint8_t* bytes, uint32_t size) noexcept;
  uint64_t liveMidiDropped() const noexcept { return liveDropped_.load(); }
  // Control thread: moves captured MIDI (recording) into out. Returns the number moved.
  size_t drainCapturedMidi(std::vector<CapturedMidi>& out);
  // Transport frame where the last record start was applied by the audio thread (-1 = none yet).
  int64_t recordStartFrame() const noexcept { return recStartFrame_.load(); }
  bool running() const noexcept { return running_.load(); }
  // Waits until the audio thread finished at least one more callback (no-op when not running).
  bool waitForCallback(uint32_t timeoutMs);

  // ---- any thread (wait-free / lock-free reads) ----
  TransportSnapshot transport() const noexcept;
  MeterBank& meters() noexcept { return meters_; }
  uint64_t xruns() const noexcept { return xruns_.load(); }
  uint64_t callbackCount() const noexcept { return epoch_.load(); }
  double cpuLoad() const noexcept { return cpu_.load(); }
  uint32_t currentLatency() const noexcept { return latency_.load(); }
  uint64_t currentGeneration() const noexcept { return generation_.load(); }
  uint64_t droppedRecordFrames() const noexcept;

  // ---- IAudioCallback ----
  sq_result SQ_CALL onStart(const sq_stream_format* input, const sq_stream_format* output, const sq_latency_info* latency) override;
  void SQ_CALL process(const sq_audio_io* io) override;  // REALTIME
  void SQ_CALL onStop() override;

  // Testing / offline: the graph currently owned by the RT side (only valid when not running).
  Graph* graphForControlThread() noexcept { return running_.load() ? nullptr : cur_; }
  // Control thread: the graph most recently handed to setGraph() (null before the first). It is
  // the newest graph that exists; it is freed only by a later setGraph()/collectGarbage() on this
  // same thread, so main-thread-only state (plugin instances) may be read from it between calls.
  Graph* newestGraph() const noexcept { return newest_; }

 private:
  void apply(const Command& c) noexcept;
  void publish() noexcept;
  void installPending() noexcept;
  // Applies queued commands in order. A command stamped with the generation of a graph that is
  // published (pending_) but not installed yet is held (held_) until that graph is installed
  // instead of being dropped; the commands behind it wait too (order is kept).
  void drainCommands() noexcept;
  bool deferForPending(const Command& c) noexcept;
  void captureLive(const LiveMidiEvent& e) noexcept;

  Graph* newest_ = nullptr;  // control thread bookkeeping (newestGraph())

  // RT-owned state
  Graph* cur_ = nullptr;
  int64_t pos_ = 0;
  bool playing_ = false, recording_ = false, looping_ = false, metronome_ = false, lowLatency_ = false;
  bool midiCapture_ = false;
  int64_t loopStart_ = 0, loopEnd_ = 0;
  Command held_{};
  bool hasHeld_ = false;
  // MIDI capture state (RT): velocity of the captured notes still held (0 = off), last captured
  // frame, and whether the loop wrapped since the record start.
  uint8_t capVel_[16][128] = {};
  int64_t capLast_ = INT64_MIN;
  bool capWrapped_ = false;
  uint64_t transportPass_ = 0;  // bumped on every loop wrap / locate (RT thread only)
  uint64_t loopWraps_ = 0;  // loop wraps only (a subset of transportPass_)
  uint64_t wrapPass_ = UINT64_MAX;  // transportPass_ right after the last loop wrap (a locate moves on)
  uint64_t capPass_ = 0, capWraps_ = 0;
  double sampleRate_ = 48000.0;
  double cpuAvg_ = 0.0;

  // control <-> RT
  std::atomic<Graph*> pending_{nullptr};
  SpscQueue<Graph*> retire_{64};
  SpscQueue<Command> cmds_{1024};
  std::atomic<RecordSession*> session_{nullptr};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> epoch_{0};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<double> cpu_{0.0};
  std::atomic<uint32_t> latency_{0};
  std::atomic<uint64_t> generation_{0};
  std::atomic<uint32_t> recordComp_{0};
  std::atomic<uint32_t> outLatency_{0};
  SpscQueue<LiveMidiEvent> live_{1024};
  std::atomic_flag liveLock_ = ATOMIC_FLAG_INIT;  // producers only
  std::atomic<uint64_t> liveDropped_{0};
  SpscQueue<CapturedMidi> capture_{16384};
  std::atomic<int64_t> recStartFrame_{-1};
  SpscQueue<AutomationData*> autoRetire_{2048};
  std::unique_ptr<RecordSession> sessionOwner_;
  std::vector<std::pair<uint64_t, std::unique_ptr<RecordSession>>> lingering_;  // (free at epoch, session)

  // transport seqlock
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint32_t> tFlags_{0};
  std::atomic<int64_t> tFrame_{0};
  std::atomic<double> tPpq_{0.0};
  std::atomic<double> tBpm_{120.0};
  std::atomic<uint32_t> tNum_{4}, tDen_{4};
  std::atomic<int64_t> tLoopStart_{0}, tLoopEnd_{0};
  std::atomic<double> tLoopStartPpq_{0.0}, tLoopEndPpq_{0.0};
  std::atomic<double> tRate_{48000.0};

  MeterBank meters_;
};

class AudioEngine final : public sq::Object<AudioEngine, IAudioEngine, ITransport, ISubject> {
 public:
  // reg: class registry for drivers and codecs (borrowed, must outlive the engine).
  // settings: nullable (Devices.*, Engine.*, Media.* defaults).
  AudioEngine(IClassRegistry* reg, ISettings* settings);
  ~AudioEngine() override;

  // ---- C++ extras (main thread) ----
  // Use a GraphDesc directly (no document). Replaces the document-built graph.
  sq_result setGraphDesc(GraphDesc desc);
  const GraphDesc& graphDesc() const noexcept { return desc_; }
  // Use an already opened device object.
  sq_result openDeviceObject(IAudioDevice* device, const sq_audio_device_config* request);
  // Main-thread pump (call from the app's idle/timer): frees retired graphs, completes
  // bounce-in-place tasks (their document edit), and recovers from device resets/losses:
  // a driver IAudioDeviceListener notification (registered on the driver openDevice() used) or a
  // device that stopped streaming on its own (isRunning() false while the core still thinks it
  // runs) makes idle() close the device (stop() -> onStop(), so the core leaves the "running"
  // path), notify "engine.device", and reopen the same device - or, if it is gone, the driver's
  // default device. When nothing can be reopened the engine stays closed (offline path).
  void idle();
  // Device recovery bookkeeping (main thread): number of recoveries handled by idle(), and the
  // result of the last reopen attempt (SQ_OK, or the failure that left the engine closed).
  uint32_t deviceRecoveries() const noexcept { return deviceRecoveries_; }
  sq_result lastRecoveryResult() const noexcept { return lastRecoveryResult_; }
  EngineCore& core() noexcept { return *core_; }
  IAudioDevice* device() noexcept { return device_.get(); }
  sq_result setMix64(bool enabled);
  bool mix64() const noexcept { return mix64_; }
  sq_result setTrackVolume(const sq_guid& trackId, float linear);
  sq_result setTrackPan(const sq_guid& trackId, float pan);
  sq_result setTrackMute(const sq_guid& trackId, bool mute);
  sq_result setTrackSolo(const sq_guid& trackId, bool solo);
  sq_result setTrackArmed(const sq_guid& trackId, bool armed);
  std::vector<RecordedTake> lastTakes() const { return lastTakes_; }
  std::string mediaFolder() const;
  int64_t ppqToFrame(sq_ppq_time ppq) const noexcept;
  sq_ppq_time frameToPpq(int64_t frame) const noexcept;
  sq_result rebuild();  // recompile the graph from the document / desc
  // Graph compilations so far (tests: proves parameter-only edits do not recompile).
  uint64_t compileCount() const noexcept { return compileCount_; }
  // ---- hosted plugins (main thread) ----
  // The plugin instance behind an insert of the newest compiled graph: node = track / bus id (null
  // = master), slot = "user.inserts" line index, or -1 for an instrument track's instrument. Null
  // for built-in devices, bypassed / unavailable lines and unknown nodes. The returned reference
  // keeps the instance alive after its graph is replaced; compare with a later call to notice that.
  sq::Ptr<IPluginInstance> pluginInstance(const sq_guid& node, int32_t slot);
  // Inverse lookup in the newest graph: which node / slot hosts `inst`. false when not found.
  bool locatePlugin(IPluginInstance* inst, sq_guid* node, int32_t* slot);
  // Every (node, slot) of the newest graph that hosts a plugin instance.
  struct PluginSlot {
    sq_guid node{};
    int32_t slot = 0;
    sq::Ptr<IPluginInstance> instance;
  };
  std::vector<PluginSlot> pluginSlots();
  // The plugin instance `inst` (hosted by the newest graph) already holds `values`: it reported
  // them itself (editor edits, values re-read from it) and the caller is about to write them into
  // its spec line. The document update that follows then does not send them back to `inst` (an
  // echo is applied in a later block, after and over a newer edit the plugin made meanwhile),
  // neither as parameter commands nor through the automation swap that restores an automated
  // parameter to its lane's new static value (AutoLane::mainHolds); the fast-lane copy of a track
  // insert, which did not see the edit, still gets them. The installed lanes keep the new static
  // value, so a later automation stop restores it to every copy as usual. Holds for the next
  // document update only (any rebuild clears it; so does forgetPluginValues). Instances the newest
  // graph does not host are ignored.
  void notePluginValues(IPluginInstance* inst, const ParamList& values);
  void forgetPluginValues() noexcept { heldValues_.clear(); }
  // Plugin state carry-over: a recompile hands each plugin's live state to the instance that
  // replaces it (lines matched by spec identity, see captureStatesInto; never across a switch to
  // another document), and offline renders start from the live states too. The live state of a
  // bypassed insert is kept with its line until it is enabled again; that of a removed / replaced
  // plugin is kept (bounded) for an undo / redo that brings the same line back. Kept count (tests).
  size_t parkedPluginStates() const noexcept { return parked_.size(); }
  // Every live state kept with a line that is still in the document but hosts no instance
  // (bypassed, or its plugin unavailable; only attached states, not those kept for an undo /
  // redo): what that line's plugin starts from when it is enabled again. node / slot as in
  // PluginSlot (track / bus id, null = master; slot = the line's index in the node's
  // "user.inserts"; an instrument is never bypassed, so never listed). Writing these states into
  // their lines' "s=" tokens (File.Save; the '!' marker kept) is a mix-only edit, after which each
  // stays kept with its line (re-keyed): enabling the line still restores it.
  struct KeptPluginState {
    sq_guid node{};
    int32_t slot = 0;
    PluginState state;
  };
  std::vector<KeptPluginState> keptPluginStates() const;
  // Fills d's insertStates / instrumentState / masterInsertStates from the live plugin states for
  // an OFFLINE graph compiled from `d` (startRender; tests), pairing lines like a recompile but
  // consuming / parking nothing. Does nothing while the live graph belongs to the previous document.
  void captureRenderStates(GraphDesc& d) {
    if (carryNext_) captureStatesInto(d, nullptr);
  }

  // Turns track insert specs (track property "user.inserts") into processors; see plugin_fx.hpp.
  // Takes effect at the next rebuild.
  void setInsertResolver(InsertResolver resolver) { inserts_ = std::move(resolver); }
  // Turns an instrument track's "user.instrument" spec into its synth prototype (doc_builder.hpp:
  // empty / unresolvable -> kDefaultInstrument through this resolver -> SineSynth). Takes effect
  // at the next rebuild.
  void setInstrumentResolver(InstrumentResolver resolver) { instruments_ = std::move(resolver); }

  // ---- live MIDI ----
  // Any non-RT thread (MIDI driver threads, the main thread): short MIDI messages for the live
  // MIDI target tracks; lock-free for the audio thread, bounded (drops on overflow).
  bool pushLiveMidi(const uint8_t* bytes, uint32_t size) noexcept { return core_->pushLiveMidi(bytes, size); }
  // Main thread. Live MIDI goes to every ARMED instrument track; when none is armed, to this
  // track (the GUI sets the selected track; null = none). Non-instrument tracks are ignored.
  void setLiveMidiTarget(const sq_guid& trackId);
  const sq_guid& liveMidiTarget() const noexcept { return liveTarget_; }
  // Track ids currently receiving live MIDI (main thread view).
  std::vector<sq_guid> liveMidiTracks() const;
  // While recording with armed instrument tracks the live MIDI is captured; at the record stop
  // one MIDI clip per armed instrument track is added in the same undo step as the audio takes.
  // Events captured by the last recording (frames = transport frames).
  const std::vector<CapturedMidi>& lastMidiTake() const noexcept { return lastMidi_; }
  // Warnings of the last document build (missing media, unavailable inserts).
  const std::vector<std::string>& buildWarnings() const noexcept { return buildWarnings_; }
  // Registers / unregisters this engine as the IAudioEngine service.
  sq_result registerService(IHostContext* host);
  void unregisterService(IHostContext* host);

  // ---- IAudioEngine ----
  sq_result SQ_CALL openDevice(const sq_guid* driverClassId, const char* deviceId, const sq_audio_device_config* request) override;
  sq_result SQ_CALL closeDevice() override;
  sq_result SQ_CALL getStatus(sq_engine_status* out) override;
  sq_result SQ_CALL setDocument(IDocument* doc) override;
  sq_result SQ_CALL setLowLatencyMonitoring(sq_bool enabled) override;
  sq_result SQ_CALL setTrackMonitoring(const sq_guid* trackId, sq_bool enabled) override;
  sq_result SQ_CALL getLatency(sq_engine_latency* out) override;
  sq_result SQ_CALL getNodeLatency(const sq_guid* nodeId, sq_latency_info* out) override;
  sq_result SQ_CALL getMeter(const sq_guid* trackId, sq_meter_value* out, uint32_t capacity, uint32_t* count) override;
  sq_result SQ_CALL startRender(const sq_render_request* request, IRenderObserver* observer, IRenderTask** out) override;

  // ---- ITransport ----
  sq_result SQ_CALL play() override;
  sq_result SQ_CALL stop() override;
  sq_result SQ_CALL record(sq_bool enable) override;
  sq_result SQ_CALL locate(sq_ppq_time position) override;
  sq_result SQ_CALL setLoop(sq_ppq_time start, sq_ppq_time end, sq_bool enabled) override;
  sq_result SQ_CALL setMetronome(sq_bool enabled) override;
  sq_result SQ_CALL getTransportState(sq_transport_state* out) override;

  // ---- ISubject ----
  sq_result SQ_CALL addObserver(IObserver* observer) override;
  sq_result SQ_CALL removeObserver(IObserver* observer) override;

  // Called by the document observer (main thread) with the kind of the change ("document.changed"
  // reason "commit" / "undo" / "redo"; any other reason, e.g. "relink", is Other). Only an undo /
  // redo may give a line coming back the live state its plugin had when the line went away: an
  // undo the state a commit / redo kept (it reverses that step), a redo the state an undo kept. A
  // commit drops the states kept for a redo (the document's redo history is gone).
  enum class DocChange : uint8_t { Commit, Undo, Redo, Other };
  void onDocumentChanged(DocChange change = DocChange::Other);

 private:
  struct NodeLatency {
    uint32_t process = 0;
    uint32_t compensation = 0;
  };
  sq_result compileAndInstall();
  // ---- plugin state carry-over (main thread) ----
  // A node of the graph: track / bus by id, the master (null id). The kind keeps id-less nodes of
  // descs built without a document apart.
  enum class NodeKind : uint8_t { Track, Bus, Master };
  using NodeKey = std::pair<NodeKind, sq_guid>;
  // One "user.inserts" line of a node as the carry sees it: its identity (TrackDesc::lineKeys),
  // the chain position of its processor (-1: bypassed / unavailable) and, for a line without one,
  // the kept state that belongs to it (ParkedState::id, 0 = none). A desc without lineKeys yields
  // one keyless line per chain entry (they pair in order).
  struct LineIdent {
    std::string key;
    int32_t entry = -1;
    uint64_t parked = 0;
  };
  struct NodeIdent {
    std::vector<LineIdent> lines;
    bool hasInstrument = false;
    std::string instrument;  // TrackDesc::instrumentKey
  };
  // The lines and instrument of every node `d` compiles into (no kept states attached).
  static std::map<NodeKey, NodeIdent> identify(const GraphDesc& d);
  // Pairs the lines of one node across an edit: result[new line] = old line index or -1. Leading
  // and trailing runs of identical lines pair by position (a bypass toggle or an edit of one line
  // leaves every other line, duplicates included, with its own partner); in between, a new line
  // takes the first unpaired old line with the identical identity, then the first one with the
  // same specStructure (same plugin, other values / state).
  static std::vector<int32_t> alignLines(const std::vector<LineIdent>& olds, const std::vector<LineIdent>& news);
  // Live state of a plugin instance that left the graph. Attached: it belongs to a bypassed /
  // unavailable line of the installed graph (LineIdent::parked) and returns when that line is
  // enabled again, whatever edits the line saw meanwhile. Detached: its line was removed (or the
  // instrument replaced, the node deleted); only an undo / redo that puts the identical line back
  // at the same index takes it, and only one that reverses the step that detached it: an undo
  // takes what a commit / redo detached, a redo what an undo detached (byUndo). The undo history
  // is a stack, so the newest such entry is the one of the step being reversed; an entry detached
  // by the opposite move (the same line leaving again on the way back) never stands for it.
  struct ParkedState {
    uint64_t id = 0;        // increasing: a higher id is newer
    NodeKey node{};
    bool instrument = false;
    bool attached = false;
    bool byUndo = false;    // detached: by an undo (waits for a redo), else by a commit / redo
    std::string line;       // identity of the line (instrument spec) it stands for
    int32_t at = -1;        // detached insert: the line index it left
    PluginState state;      // carries the plugin id
  };
  // Changes to the kept states by one recompile, committed only when its graph is installed.
  struct ParkMove {         // an existing entry re-attached / detached / re-keyed
    uint64_t id = 0;
    bool attached = false;
    bool byUndo = false;
    std::string line;
    int32_t at = -1;
  };
  struct ParkPlan {
    std::vector<uint64_t> consumed;      // handed to the new graph
    std::vector<ParkMove> moves;
    std::vector<ParkedState> parks;      // live states of old instances that have no partner
    std::map<NodeKey, NodeIdent> ident;  // identity of the new graph, kept states attached
  };
  // Fills d's insertStates / instrumentState / masterInsertStates so that the graph compiled from
  // `d` starts where the newest graph left off. Per node the new lines are paired with the
  // installed ones (alignLines); a new chain entry takes the live state of its partner's instance,
  // or the state kept for its partner's bypassed line. An instrument takes the old instrument's
  // state when their specStructures match. Every state handed out gets the new entry's spec values
  // as PluginState::params, applied over the carried bytes (the document's values win).
  //   plan != null (recompile): a partner instance whose line is bypassed now is kept attached to
  //   that line; old instances / kept states without a partner are kept detached; during an undo /
  //   redo (applying_) an unpaired new line takes the newest detached state of the same node,
  //   identity and line index that the opposite move detached (ParkedState::byUndo). `plan`
  //   collects all of it; commitParkPlan applies it once the new graph is installed.
  //   plan == null (offline render): reads only (saveState of the live instances, kept states);
  //   nothing is consumed or kept, so a render does not change what the next recompile carries.
  void captureStatesInto(GraphDesc& d, ParkPlan* plan);
  void commitParkPlan(ParkPlan&& plan);
  ParkedState* findParked(uint64_t id);
  // A document edit applied without a recompile (same structure: the same lines in the same
  // places): the installed identity becomes `d`'s; kept states stay with their bypassed lines and
  // follow those lines' value edits.
  void refreshIdentity(const GraphDesc& d);
  // true when a node's lines are the installed ones in another order (Track.MoveInsert of lines
  // that differ only in values): such an edit is recompiled so that every state moves with its
  // line instead of being applied as parameter commands to the instances in place.
  bool insertsReordered(const GraphDesc& next) const;
  void notify(const char* topic);
  int32_t trackIndex(const sq_guid& id) const;
  int32_t meterSlotFor(const sq_guid& id);
  sq_result beginRecording();
  sq_result endRecording();
  sq_result commitTakes(const std::vector<RecordedTake>& takes);
  void appendMidiClips(IEditor* ed, bool* any);
  void updateLiveTargets(bool post);
  bool wantsLiveMidi(const TrackDesc& t) const;
  sq_result applyBounce(RenderTask& task);
  void detachDocument();
  void checkDevice();
  sq_result rebuildFromDocument(bool allowMixOnly);
  bool applyMixChanges(const GraphDesc& next);
  void dropDriverListener();

  IClassRegistry* reg_;
  ISettings* settings_;
  sq::Ptr<EngineCore> core_;
  sq::Ptr<IAudioDevice> device_;
  // Driver the current device came from (openDevice path) + our listener on it. The listener
  // runs on the driver notification thread and only stores the reason into deviceReset_.
  sq::Ptr<IAudioDriver> driver_;
  sq::Ptr<IAudioDeviceListener> deviceListener_;
  std::shared_ptr<std::atomic<int32_t>> deviceReset_ = std::make_shared<std::atomic<int32_t>>(0);
  std::shared_ptr<std::atomic<bool>> deviceListChanged_ = std::make_shared<std::atomic<bool>>(false);
  // Request used to (re)open the device: owned copies of the channel lists.
  bool haveRequest_ = false;
  sq_audio_device_config request_{};
  std::vector<uint32_t> requestIns_, requestOuts_;
  uint32_t deviceRecoveries_ = 0;
  sq_result lastRecoveryResult_ = SQ_OK;
  bool recovering_ = false;
  sq_guid driverClass_{};
  std::string deviceId_;
  double sampleRate_ = 48000.0;
  uint32_t blockFrames_ = 256;
  uint32_t inChannels_ = 0, outChannels_ = 2;
  uint32_t devInLatency_ = 0, devOutLatency_ = 0;
  bool mix64_ = false;
  bool lowLatency_ = false;

  sq::Ptr<IDocument> doc_;
  sq::Ptr<IObserver> docObserver_;
  GraphDesc desc_;
  bool haveDesc_ = false;
  MediaCache media_;
  std::map<sq_guid, bool> monitorOverride_;
  std::map<sq_guid, bool> armOverride_;
  std::map<sq_guid, NodeLatency> nodeLatency_;
  uint32_t graphLatency_ = 0;
  std::string structureKey_;   // DocBuildResult::structureKey of the installed document graph
  uint64_t compileCount_ = 0;
  // Plugin state carry-over (captureStatesInto). carryNext_ is false from a setDocument() that
  // switches to another document (or to none) until a graph of it is installed: the newest graph
  // then belongs to another song, so its states are neither carried, kept nor rendered.
  // Re-binding the document already bound keeps carrying.
  bool carryNext_ = true;
  // The document change being applied (onDocumentChanged); Other outside one (rebuild(), a
  // setDocument() rebuild): detached states are looked up only during an Undo / Redo.
  DocChange applying_ = DocChange::Other;
  // Identity of the installed graph (identify(), kept states attached to its bypassed lines): set
  // when a graph is installed and refreshed by a document edit applied without a recompile, so it
  // always names the lines the live instances stand for.
  std::map<NodeKey, NodeIdent> liveIdent_;
  // Kept states, oldest first, at most kMaxParked entries and kMaxParkedBytes of state bytes. Over
  // either bound the detached ones go first, oldest first (each waits only for an undo / redo);
  // attached ones (a line still in the document) only when no detached one is left. Cleared by a
  // setDocument() that switches documents.
  static constexpr size_t kMaxParked = 64;
  static constexpr size_t kMaxParkedBytes = size_t(256) << 20;
  std::vector<ParkedState> parked_;
  uint64_t nextParkId_ = 1;
  // notePluginValues: (node, inserts[] index or -1 = instrument) -> values its main-chain instance
  // already holds. Read by the next applyMixChanges, cleared by every rebuildFromDocument.
  std::map<std::pair<NodeKey, int32_t>, ParamList> heldValues_;

  // Meter slot directory. Append-only table published with a release store of the count, so
  // getMeter (any non-RT thread, wait-free per audioengine.h) scans it without a lock. Entries
  // are written only by the main thread; channels is updated atomically on rebuild.
  struct MeterEntry {
    sq_guid id{};
    int32_t slot = -1;
    std::atomic<uint32_t> channels{0};
  };
  void setMeterChannels(const sq_guid& id, uint32_t channels);
  std::unique_ptr<MeterEntry[]> meterTable_{new MeterEntry[MeterBank::kSlots]};
  std::atomic<uint32_t> meterCount_{0};
  std::map<sq_guid, uint32_t> meterIndex_;  // main thread: id -> table index

  struct BouncePlan {
    std::vector<sq_guid> tracks;
    std::vector<std::string> paths;
    sq_ppq_time start = 0, end = 0, endWithTail = 0;
  };
  // Offline render graphs: compiled on the main thread by startRender, run by the render worker,
  // destroyed on the main thread by idle() once the worker set `done` (pluginhost.h threading).
  struct OfflineGraphSet {
    OfflineGraphs graphs;
    std::atomic<bool> done{false};
  };
  struct OfflineEntry {
    std::shared_ptr<OfflineGraphSet> graphs;
    sq::Ptr<RenderTask> task;
  };
  void reapOfflineGraphs(bool all, uint32_t waitMs);
  std::vector<OfflineEntry> offlineGraphs_;

 public:
  // Offline render graphs not yet released on the main thread (tests).
  size_t pendingOfflineGraphs() const noexcept { return offlineGraphs_.size(); }

 private:
  TempoMapRt tempo_;
  std::vector<sq::Ptr<IObserver>> observers_;
  std::vector<sq::Ptr<RenderTask>> mainThreadTasks_;
  std::map<RenderTask*, BouncePlan> bouncePlans_;
  std::shared_ptr<std::atomic<int32_t>> activeRenders_ = std::make_shared<std::atomic<int32_t>>(0);
  char deviceIdBuf_[256] = {};
  std::vector<RecordedTake> lastTakes_;
  bool recordingActive_ = false;
  InsertResolver inserts_;
  InstrumentResolver instruments_;
  std::vector<std::string> buildWarnings_;
  sq_guid liveTarget_{};
  std::vector<sq_guid> midiRecTracks_;     // armed instrument tracks of the running recording
  std::vector<CapturedMidi> capturedMidi_;  // drained while recording
  std::vector<CapturedMidi> lastMidi_;
  int64_t midiRecStart_ = -1, midiRecEnd_ = -1;
};

}  // namespace sq::engine::audio
