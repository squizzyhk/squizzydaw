// time-stretch: streaming time/pitch modification core (own implementation).
//
// Pipeline:  input FIFO -> frame algorithm (WSOLA or phase vocoder) -> overlap-add -> "stretched"
//            FIFO -> windowed-sinc resampler (pitch) -> output FIFO.
// Pitch shift p (= 2^(semitones/12)) is done by stretching time by ratio*p and resampling by p.
// Output frame 0 is aligned with input frame 0 (no output delay); the algorithm needs some input
// lookahead instead, reported by latencyFrames().
//
// Backends are `FrameAlgorithm`s: one analysis/synthesis frame at a time. A third backend
// (e.g. a licensed commercial engine) would be a separate service implementing ITimeStretcher in
// the "TimeStretch" category; this core only swaps its own two algorithms.
//
// configure() allocates; everything else is allocation-free and lock-free (RT-safe).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "sq/engine/simd/fft.hpp"
#include "sq/interfaces/dsp.h"

namespace sq::stretch {

enum class Algorithm : int { Auto = 0, Wsola = 1, PhaseVocoder = 2 };

struct Settings {
  double sampleRate = 44100.0;
  uint32_t channels = 2;
  uint32_t maxBlock = 1024;
  int32_t quality = SQ_STRETCH_QUALITY_STANDARD;
  uint32_t flags = 0;
  double maxTimeRatio = 4.0;
  Algorithm algorithm = Algorithm::Auto;  // Auto: PV for HIGH quality, WSOLA otherwise
};

inline constexpr double kMinTimeRatio = 0.25;
inline constexpr double kMaxPitchSemitones = 24.0;

// Buffered input, addressed by absolute frame index (index 0 = first pushed frame).
struct InputView {
  const float* const* ch;
  uint32_t channels;
  int64_t base;  // absolute index of ch[c][0]
  const float* at(uint32_t c, int64_t index) const noexcept { return ch[c] + (index - base); }
};

class FrameAlgorithm {
 public:
  virtual ~FrameAlgorithm() = default;
  virtual const char* name() const noexcept = 0;
  virtual uint32_t frameSize() const noexcept = 0;     // N
  virtual uint32_t hop() const noexcept = 0;           // synthesis hop
  virtual uint32_t searchRadius() const noexcept = 0;  // extra input read on each side of a frame
  // One synthesis frame per channel (windowed + normalised for overlap-add at hop()), for the
  // nominal analysis centre `center`. Reads in [center - N/2 - radius, center + N/2 + radius).
  virtual void frame(const InputView& in, int64_t center, float* const* out) noexcept = 0;
  // Lowest input index any later frame (nominal centre >= nextCenter) may still read.
  virtual int64_t oldestNeeded(int64_t nextCenter) const noexcept = 0;
  virtual void reset() noexcept = 0;
};

// Power-of-two frame size near `at44k * sampleRate / 44100`.
uint32_t scaledFrameSize(double sampleRate, double at44k);

std::unique_ptr<FrameAlgorithm> makeWsola(const Settings& s, const sq_vector_ops* ops);
std::unique_ptr<FrameAlgorithm> makePhaseVocoder(const Settings& s, const sq_vector_ops* ops);

// Planar FIFO with absolute indexing; storage is allocated once.
struct PlanarFifo {
  std::vector<std::vector<float>> ch;
  std::vector<float*> ptr;
  uint32_t cap = 0;
  uint32_t len = 0;
  int64_t base = 0;  // absolute index of element 0
  void allocate(uint32_t channels, uint32_t capacity);
  int64_t end() const noexcept { return base + len; }
  uint32_t space() const noexcept { return cap - len; }
  void clear(int64_t newBase) noexcept { base = newBase, len = 0; }
  void discardBefore(int64_t index) noexcept;  // drop elements with absolute index < index
  void appendZeros(uint32_t n) noexcept;
};

class Stretcher {
 public:
  Stretcher();
  ~Stretcher();
  Stretcher(const Stretcher&) = delete;
  Stretcher& operator=(const Stretcher&) = delete;

  bool configure(const Settings& s);
  bool configured() const noexcept { return algo_ != nullptr; }
  const Settings& settings() const noexcept { return cfg_; }
  const char* algorithmName() const noexcept;

  void setRatios(double timeRatio, double pitchSemitones) noexcept;
  double timeRatio() const noexcept { return timeRatio_; }
  double pitchFactor() const noexcept { return pitch_; }

  uint32_t latencyFrames() const noexcept;
  uint32_t requiredInput(uint32_t outFrames) const noexcept;
  // false (nothing consumed) when the input FIFO is full: pull output first.
  bool push(const float* const* in, uint32_t frames) noexcept;
  uint32_t pull(float* const* out, uint32_t frames) noexcept;
  void flush() noexcept;
  bool flushed() const noexcept { return eof_; }
  bool drained() const noexcept;
  void reset() noexcept;

 private:
  void pump(uint32_t wantOut) noexcept;
  bool generateFrame() noexcept;
  void resample() noexcept;
  int64_t frameCenter() const noexcept;
  double nextAnalysis() const noexcept;  // a_ once started, else the first frame's position
  int64_t frameInputEnd(int64_t center) const noexcept;
  void compactInput() noexcept;
  double kernel(double u) const noexcept;

  Settings cfg_;
  const sq_vector_ops* ops_ = nullptr;
  std::unique_ptr<FrameAlgorithm> algo_;
  uint32_t n_ = 0, hop_ = 0, radius_ = 0;
  uint32_t maxAnalysisHop_ = 0;
  int64_t firstFrame_ = 0;  // first frame index: every y >= 0 gets the full overlap-add sum
  bool started_ = false;

  PlanarFifo in_, y_, out_;
  std::vector<std::vector<float>> ola_, frame_;
  std::vector<float*> framePtr_;
  int64_t olaY_ = 0;  // y index of ola_[c][0]

  double timeRatio_ = 1.0, pitch_ = 1.0;
  double a_ = 0.0;          // nominal analysis centre of the next frame
  int64_t frameIndex_ = 0;  // next frame k; its synthesis centre is k * hop
  double p_ = 0.0;          // y position of the next output sample
  int64_t outTotal_ = 0;    // output samples generated so far
  int64_t inTotal_ = 0;     // real input frames pushed
  int64_t limit_ = -1;      // output length after flush
  bool eof_ = false;

  // windowed-sinc table: K(u) = sinc(u) * window(u / kTaps), sampled at kTableRes per unit
  static constexpr int32_t kTaps = 16;
  static constexpr int32_t kTableRes = 512;
  static constexpr int32_t kMaxSide = kTaps * 4 + 2;
  std::vector<float> sinc_;
};

}  // namespace sq::stretch
