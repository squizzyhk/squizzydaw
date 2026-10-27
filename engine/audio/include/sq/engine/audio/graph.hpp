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
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sq/guid.h"
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

struct TrackDesc {
  sq_guid id{};
  std::string name;
  uint32_t channels = 2;               // internal channel count of the track
  std::vector<SourceProto> sources;    // summed
  std::vector<ProcessorProto> inserts; // insert chain (serial)
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
  float volume = 1.0f;
  float pan = 0.0f;
  bool mute = false;
  int32_t output = -1;                 // bus index, -1 = master
  int32_t meterSlot = -1;
};

struct GraphDesc {
  double sampleRate = 48000.0;
  std::vector<TrackDesc> tracks;
  std::vector<BusDesc> buses;
  std::vector<ProcessorProto> masterInserts;
  float masterVolume = 1.0f;
  int32_t masterMeterSlot = 0;
  std::vector<TempoSegment> tempo;     // empty = 120 bpm from frame 0
  uint32_t numerator = 4;
  uint32_t denominator = 4;
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

// Track strip: sources -> (input monitor) -> inserts -> fader/pan. A processor variant whose
// process() writes the strip output (destination channel count) into io.
class TrackProcessor final : public AudioProcessor {
 public:
  explicit TrackProcessor(const TrackDesc& d);
  Kind kind() const noexcept override { return Kind::Track; }
  ProcessorPtr clone() const override;
  bool prepare(double sr, uint32_t maxFrames, uint32_t outChannels) override;
  void reset() noexcept override;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return chain_.latency(); }
  uint32_t tail() const noexcept override { return chain_.tail(); }

  // RT parameter setters (audio thread / owner thread of the graph).
  void setVolume(float v) noexcept { volume_ = v; }
  void setPan(float p) noexcept { pan_ = p; }
  void setMute(bool m) noexcept { mute_ = m; }
  void setSolo(bool s) noexcept { solo_ = s; }
  void setSoloSilenced(bool s) noexcept { soloSilenced_ = s; }
  void setMonitor(bool m) noexcept { monitor_ = m; }
  void setMonitorInMain(bool m) noexcept { monitorInMain_ = m; }
  float volume() const noexcept { return volume_; }
  float pan() const noexcept { return pan_; }
  bool solo() const noexcept { return solo_; }
  bool monitor() const noexcept { return monitor_; }
  // False when muted or silenced by another track's solo (applies to the fast lane too).
  bool audible() const noexcept { return !mute_ && !soloSilenced_; }
  const TrackDesc& desc() const noexcept { return desc_; }
  uint32_t channels() const noexcept { return desc_.channels; }
  // Post-insert, pre-fader buffer of the last process() call.
  float* const* preFader() const noexcept { return bufPtrs_.data(); }
  int64_t contentEnd() const noexcept;

 private:
  TrackDesc desc_;
  std::vector<SourcePtr> sources_;
  ProcessorChain chain_;
  std::vector<float> buf_;
  std::vector<float*> bufPtrs_;
  uint32_t maxFrames_ = 0;
  uint32_t outChannels_ = 2;
  float volume_ = 1.0f, pan_ = 0.0f;
  bool mute_ = false, solo_ = false, soloSilenced_ = false, monitor_ = false, monitorInMain_ = true;
  float curGain_[kMaxChannels] = {};
  bool gainInit_ = false;
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
  void setVolume(float v) noexcept { volume_ = v; }
  void setPan(float p) noexcept { pan_ = p; }
  void setMute(bool m) noexcept { mute_ = m; }
  float volume() const noexcept { return volume_; }
  uint32_t channels() const noexcept { return channels_; }
  void setMix64(bool on) noexcept { mix64_ = on; }

 private:
  uint32_t channels_;
  std::vector<ProcessorProto> protos_;
  ProcessorChain chain_;
  float volume_, pan_;
  bool mute_;
  bool mix64_ = false;
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
  int32_t findTrack(const sq_guid& id) const noexcept;
  int32_t findBus(const sq_guid& id) const noexcept;
  void setMasterVolume(float v) noexcept { masterVolume_ = v; }
  void updateSolo() noexcept;  // RT: re-evaluates solo-silencing after solo changes
  int64_t contentEnd() const noexcept;
  uint32_t maxTail() const noexcept;
  const TempoMapRt& tempo() const noexcept { return tempo_; }
  uint32_t numerator() const noexcept { return numerator_; }
  uint32_t denominator() const noexcept { return denominator_; }
  double sampleRate() const noexcept { return sr_; }
  uint32_t maxBlock() const noexcept { return maxBlock_; }
  uint32_t outputChannels() const noexcept { return outChannels_; }
  Metronome& metronome() noexcept { return metronome_; }

  // ---- stem taps (GraphConfig::stemTaps) ----
  uint32_t stemChannels(size_t track) const noexcept { return stemChannels_[track]; }
  // Timeline-aligned tap of the last process() block.
  float* const* stem(size_t track) const noexcept { return stemPtrs_[track].data(); }

  uint64_t generation() const noexcept { return generation_; }

 private:
  Graph() = default;
  void sumInto(int32_t dest, float* const* src, uint32_t ch, uint32_t frames) noexcept;
  void processFastLane(const ProcessContext& ctx, float* const* out, uint32_t outChannels) noexcept;

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

  TempoMapRt tempo_;
  uint32_t numerator_ = 4, denominator_ = 4;
  Metronome metronome_;
};

// Mixing helper shared by tracks/buses: fader+pan of src (srcCh) into dst (dstCh), ramping the
// per-channel gain from cur[] to the new target over the block (cur[] updated).
void applyFaderPan(const float* const* src, uint32_t srcCh, float* const* dst, uint32_t dstCh, uint32_t frames,
                   float volume, float pan, float* cur, bool* init) noexcept;

}  // namespace sq::engine::audio
