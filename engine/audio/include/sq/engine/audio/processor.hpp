// Squizzy Studio engine/audio - AudioProcessor variants (plan 2.3: FX, synth, parallel, track,
// sub-mix). In-process C++ API; no DLL boundary.
//
// LIFETIME / THREADING
// - Processors are created and prepare()d on the control thread, owned by a compiled Graph and
//   only touched by the thread that runs that graph (the audio thread, or a render worker).
// - process() is RT: no allocation, no locks, no blocking calls (CONTRACTS section 8).
// - clone() makes a fresh, unprepared processor with the same configuration (no runtime state).
//   GraphDesc keeps prototypes; every compiled Graph (live, render) clones its own instances.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "sq/media.h"
#include "sq/engine/audio/rt.hpp"

namespace sq::engine::audio {

// Per-block information handed to processors and source nodes.
struct ProcessContext {
  double sampleRate = 48000.0;
  uint32_t frames = 0;           // <= the maxFrames given to prepare()
  int64_t outputFrame = 0;       // transport frame represented by this block's (compensated) output
  int64_t timelineFrame = 0;     // source read position (outputFrame + graph read-ahead latency)
  // Graph::process derives timelineFrame from outputFrame unless the caller sets this (the live
  // engine does while looping: the read-ahead must wrap at the loop end before the output does).
  bool timelineExplicit = false;
  bool playing = false;          // sources render only while playing
  // Song tempo (bpm) at the block start, at timelineFrame. Graph::process fills it from its tempo
  // map (live and offline); 0 = unknown (a processor run outside a graph: use its own default).
  double tempo = 0.0;
  // Device input of this block (non-interleaved), may be null.
  const float* const* input = nullptr;
  uint32_t inputChannels = 0;
  // MIDI for synths (frame_offset within this block), filled by EventNode.
  const sq_midi_event* midi = nullptr;
  uint32_t midiCount = 0;
};

class AudioProcessor {
 public:
  enum class Kind : uint8_t { Fx, Synth, Parallel, Track, SubMix };
  virtual ~AudioProcessor() = default;
  virtual Kind kind() const noexcept = 0;
  virtual std::unique_ptr<AudioProcessor> clone() const = 0;
  // Control thread. channels = channel count of the buffers process() receives.
  virtual bool prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) = 0;
  // RT safe: clears runtime state (delay lines, voices).
  virtual void reset() noexcept {}
  // RT: in-place on io[0..channels), ctx.frames frames.
  virtual void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept = 0;
  // Algorithmic latency in frames (plugin delay compensation). Must be stable after prepare().
  virtual uint32_t latency() const noexcept { return 0; }
  virtual uint32_t tail() const noexcept { return 0; }
  // Live parameters (built-in devices, plugins). Plain values (ParamInfo ranges, builtin.hpp).
  // setParameter is RT safe and may be called from the thread that owns the graph between
  // process() calls; out-of-range indices are ignored.
  virtual uint32_t parameterCount() const noexcept { return 0; }
  virtual void setParameter(uint32_t index, float plainValue) noexcept { (void)index, (void)plainValue; }
  virtual float parameter(uint32_t index) const noexcept { (void)index; return 0.0f; }
};
using ProcessorPtr = std::unique_ptr<AudioProcessor>;
using ProcessorProto = std::shared_ptr<const AudioProcessor>;

// Insert effect base (in-place transform).
class FxProcessor : public AudioProcessor {
 public:
  Kind kind() const noexcept override { return Kind::Fx; }
};

// Instrument base: renders ctx.midi and ADDS the result into io.
class SynthProcessor : public AudioProcessor {
 public:
  Kind kind() const noexcept override { return Kind::Synth; }
};

// Serial chain of processors; latency = sum.
class ProcessorChain {
 public:
  ProcessorChain() = default;
  ProcessorChain(ProcessorChain&&) = default;
  ProcessorChain& operator=(ProcessorChain&&) = default;
  void add(ProcessorPtr p) { procs_.push_back(std::move(p)); }
  bool prepare(double sr, uint32_t maxFrames, uint32_t channels);
  void reset() noexcept;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept;
  uint32_t latency() const noexcept;
  uint32_t tail() const noexcept;
  size_t size() const noexcept { return procs_.size(); }
  AudioProcessor* at(size_t i) const noexcept { return procs_[i].get(); }
  ProcessorChain cloneChain() const;

 private:
  std::vector<ProcessorPtr> procs_;
};

// ---- built-in processors ----

// Constant linear gain.
class GainFx final : public FxProcessor {
 public:
  explicit GainFx(float gain) : gain_(gain) {}
  ProcessorPtr clone() const override { return std::make_unique<GainFx>(gain_); }
  bool prepare(double, uint32_t, uint32_t) override { return true; }
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;

 private:
  float gain_;
};

// Pure delay that reports its delay as latency (a "lookahead" processor; PDC reference case).
class DelayFx final : public FxProcessor {
 public:
  explicit DelayFx(uint32_t frames) : frames_(frames) {}
  ProcessorPtr clone() const override { return std::make_unique<DelayFx>(frames_); }
  bool prepare(double sr, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override { line_.reset(); }
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return frames_; }

 private:
  uint32_t frames_;
  DelayLine line_;
};

// Parallel split: every branch gets a copy of the input; branch outputs are delay-compensated to
// the slowest branch and summed. latency = max branch latency.
class ParallelProcessor final : public AudioProcessor {
 public:
  ParallelProcessor() = default;
  Kind kind() const noexcept override { return Kind::Parallel; }
  // Each branch is a list of prototypes (an empty branch = dry path).
  void addBranch(std::vector<ProcessorProto> branch) { protoBranches_.push_back(std::move(branch)); }
  ProcessorPtr clone() const override;
  bool prepare(double sr, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return latency_; }
  uint32_t branchCompensation(size_t i) const noexcept { return comp_[i].delay(); }

 private:
  std::vector<std::vector<ProcessorProto>> protoBranches_;
  std::vector<ProcessorChain> branches_;
  std::vector<DelayLine> comp_;
  std::vector<float> scratch_;           // channels * maxFrames
  std::vector<float*> scratchPtrs_;
  uint32_t channels_ = 0;
  uint32_t maxFrames_ = 0;
  uint32_t latency_ = 0;
};

// Deterministic polyphonic sine instrument (built-in placeholder until plugin instruments).
class SineSynth final : public SynthProcessor {
 public:
  ProcessorPtr clone() const override { return std::make_unique<SineSynth>(); }
  bool prepare(double sr, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;

 private:
  static constexpr int kVoices = 16;
  struct Voice {
    bool active = false;
    uint8_t note = 0;
    double phase = 0.0;
    double inc = 0.0;
    float amp = 0.0f;
  };
  void handle(const sq_midi_event& e) noexcept;
  Voice voices_[kVoices];
  double sr_ = 48000.0;
};

// Gain / pan laws shared by tracks, buses and tests (reference renders use the same functions).
float dbToGain(double db) noexcept;
// Constant-power pan of a mono source into stereo. pan -1..1.
void panGainsMono(float pan, float* left, float* right) noexcept;
// Balance of a stereo source. pan -1..1, centre = unity on both sides.
void panGainsStereo(float pan, float* left, float* right) noexcept;

}  // namespace sq::engine::audio
