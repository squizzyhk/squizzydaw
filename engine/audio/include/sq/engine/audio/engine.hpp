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
//
// AudioEngine implements IAudioEngine + ITransport + ISubject (main thread), owns the device and
// the document binding, builds GraphDescs from IDocument (doc_builder.hpp) and runs render tasks.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
  TrackVolume, TrackPan, TrackMute, TrackSolo, TrackMonitor, BusVolume, BusMute
};

struct Command {
  CmdType type = CmdType::Play;
  int32_t index = -1;           // track / bus index for Track* / Bus*
  uint64_t generation = 0;      // graph generation for Track* / Bus*
  double value = 0.0;
  int64_t a = 0, b = 0;         // Locate: a; Loop: a..b, value != 0 = enabled
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

 private:
  void apply(const Command& c) noexcept;
  void publish() noexcept;
  void installPending() noexcept;

  // RT-owned state
  Graph* cur_ = nullptr;
  int64_t pos_ = 0;
  bool playing_ = false, recording_ = false, looping_ = false, metronome_ = false, lowLatency_ = false;
  int64_t loopStart_ = 0, loopEnd_ = 0;
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
  // Turns track insert specs (track property "user.inserts") into processors; see plugin_fx.hpp.
  // Takes effect at the next rebuild.
  void setInsertResolver(InsertResolver resolver) { inserts_ = std::move(resolver); }
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

  // Called by the document observer (main thread).
  void onDocumentChanged();

 private:
  struct NodeLatency {
    uint32_t process = 0;
    uint32_t compensation = 0;
  };
  sq_result compileAndInstall();
  void notify(const char* topic);
  int32_t trackIndex(const sq_guid& id) const;
  int32_t meterSlotFor(const sq_guid& id);
  sq_result beginRecording();
  sq_result endRecording();
  sq_result commitTakes(const std::vector<RecordedTake>& takes);
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
  std::vector<std::string> buildWarnings_;
};

}  // namespace sq::engine::audio
