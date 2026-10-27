// time-stretch core: FIFOs, overlap-add, resampler and the streaming driver.
#include "stretch_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sq::stretch {

namespace {
constexpr double kPi = 3.14159265358979323846;

uint32_t roundToPow2(double x) {
  uint32_t n = 64;
  while (n < x * 0.75 && n < (1u << 16)) n <<= 1;
  return n;
}
}  // namespace

// ---- PlanarFifo ------------------------------------------------------------------------------

void PlanarFifo::allocate(uint32_t channels, uint32_t capacity) {
  ch.assign(channels, std::vector<float>(capacity, 0.0f));
  ptr.resize(channels);
  for (uint32_t c = 0; c < channels; ++c) ptr[c] = ch[c].data();
  cap = capacity;
  len = 0;
  base = 0;
}

void PlanarFifo::discardBefore(int64_t index) noexcept {
  if (index <= base) return;
  int64_t drop = std::min<int64_t>(index - base, len);
  if (drop <= 0) return;
  const uint32_t d = static_cast<uint32_t>(drop);
  for (auto& v : ch)
    if (len > d) std::memmove(v.data(), v.data() + d, sizeof(float) * (len - d));
  len -= d;
  base += d;
}

void PlanarFifo::appendZeros(uint32_t n) noexcept {
  n = std::min(n, space());
  for (auto& v : ch) std::memset(v.data() + len, 0, sizeof(float) * n);
  len += n;
}

// ---- Stretcher -------------------------------------------------------------------------------

Stretcher::Stretcher() = default;
Stretcher::~Stretcher() = default;

bool Stretcher::configure(const Settings& s) {
  if (s.channels == 0 || s.channels > 64 || s.maxBlock == 0 || s.maxBlock > (1u << 20) || !(s.sampleRate >= 1000.0) ||
      s.sampleRate > 1.0e6)
    return false;
  cfg_ = s;
  if (!(cfg_.maxTimeRatio >= 1.0)) cfg_.maxTimeRatio = 1.0;
  ops_ = sq_dsp_vector_ops();
  Algorithm a = s.algorithm;
  if (a == Algorithm::Auto) a = s.quality >= SQ_STRETCH_QUALITY_HIGH ? Algorithm::PhaseVocoder : Algorithm::Wsola;
  algo_ = a == Algorithm::PhaseVocoder ? makePhaseVocoder(cfg_, ops_) : makeWsola(cfg_, ops_);
  n_ = algo_->frameSize();
  hop_ = algo_->hop();
  radius_ = algo_->searchRadius();

  const uint32_t C = cfg_.channels;
  const double minEff = kMinTimeRatio * std::pow(2.0, -kMaxPitchSemitones / 12.0);
  const uint32_t maxAnalysisHop = static_cast<uint32_t>(std::ceil(hop_ / minEff)) + 1;
  maxAnalysisHop_ = maxAnalysisHop;
  // Frames whose synthesis window reaches y >= 0: k > -(n/2)/hop.
  firstFrame_ = -static_cast<int64_t>(n_ / 2 / hop_) + 1;
  const uint32_t inputPerBlock = static_cast<uint32_t>(std::ceil(cfg_.maxBlock / kMinTimeRatio)) + 1;
  in_.allocate(C, 2 * n_ + 2 * radius_ + static_cast<uint32_t>(2 - firstFrame_) * maxAnalysisHop +
                      2 * inputPerBlock + cfg_.maxBlock + 64);
  y_.allocate(C, 4 * n_ + 4 * kMaxSide + 256);
  out_.allocate(C, 2 * cfg_.maxBlock + 16 * hop_ + 64);
  ola_.assign(C, std::vector<float>(n_, 0.0f));
  frame_.assign(C, std::vector<float>(n_, 0.0f));
  framePtr_.resize(C);
  for (uint32_t c = 0; c < C; ++c) framePtr_[c] = frame_[c].data();

  // sinc table K(u), u in [0, kTaps], Blackman window
  sinc_.assign(static_cast<size_t>(kTaps) * kTableRes + 2, 0.0f);
  for (size_t i = 0; i < sinc_.size(); ++i) {
    double u = static_cast<double>(i) / kTableRes;
    if (u >= kTaps) continue;
    double sinc = u == 0.0 ? 1.0 : std::sin(kPi * u) / (kPi * u);
    double x = 0.5 + 0.5 * (u / kTaps);  // window over [-T, T] mapped to [0, 1]
    double w = 0.42 - 0.5 * std::cos(2 * kPi * x) + 0.08 * std::cos(4 * kPi * x);
    sinc_[i] = static_cast<float>(sinc * w);
  }
  timeRatio_ = 1.0;
  pitch_ = 1.0;
  reset();
  return true;
}

const char* Stretcher::algorithmName() const noexcept { return algo_ ? algo_->name() : "none"; }

void Stretcher::setRatios(double timeRatio, double pitchSemitones) noexcept {
  if (!(timeRatio > 0.0)) timeRatio = 1.0;
  timeRatio_ = std::clamp(timeRatio, kMinTimeRatio, cfg_.maxTimeRatio);
  if (!std::isfinite(pitchSemitones)) pitchSemitones = 0.0;
  pitchSemitones = std::clamp(pitchSemitones, -kMaxPitchSemitones, kMaxPitchSemitones);
  pitch_ = pitchSemitones == 0.0 ? 1.0 : std::pow(2.0, pitchSemitones / 12.0);
}

void Stretcher::reset() noexcept {
  if (!algo_) return;
  algo_->reset();
  // zeros before input frame 0, enough for the first (negative-index) frames at any ratio
  const uint32_t pad = n_ / 2 + radius_ + 2 + static_cast<uint32_t>(-firstFrame_) * maxAnalysisHop_;
  in_.clear(-static_cast<int64_t>(pad));
  in_.appendZeros(pad);
  y_.clear(-kMaxSide);
  y_.appendZeros(kMaxSide);
  out_.clear(0);
  for (auto& v : ola_) std::fill(v.begin(), v.end(), 0.0f);
  frameIndex_ = firstFrame_;
  olaY_ = frameIndex_ * static_cast<int64_t>(hop_) - static_cast<int64_t>(n_ / 2);
  a_ = 0.0;
  started_ = false;
  p_ = 0.0;
  outTotal_ = 0;
  inTotal_ = 0;
  limit_ = -1;
  eof_ = false;
}

uint32_t Stretcher::latencyFrames() const noexcept {
  if (!algo_) return 0;
  return n_ / 2 + radius_ + 1 + (pitch_ != 1.0 ? static_cast<uint32_t>(kMaxSide) : 0u);
}

double Stretcher::nextAnalysis() const noexcept {
  // Frame k's nominal analysis centre is k * hop / (time * pitch) until the first frame ran; from
  // then on a_ advances by the current analysis hop (ratios may change at any frame).
  return started_ ? a_ : static_cast<double>(frameIndex_) * hop_ / (timeRatio_ * pitch_);
}

int64_t Stretcher::frameCenter() const noexcept { return static_cast<int64_t>(std::llround(nextAnalysis())); }

int64_t Stretcher::frameInputEnd(int64_t center) const noexcept {
  return center + static_cast<int64_t>(n_ / 2 + radius_) + 1;
}

uint32_t Stretcher::requiredInput(uint32_t outFrames) const noexcept {
  if (!algo_ || eof_ || out_.len >= outFrames) return 0;
  const uint32_t need = outFrames - out_.len;
  const bool identity = pitch_ == 1.0 && p_ == std::floor(p_);
  const double pEnd = p_ + static_cast<double>(need - 1) * pitch_;
  const int64_t yNeedEnd = static_cast<int64_t>(std::floor(pEnd)) + 1 + (identity ? 0 : kMaxSide);
  if (yNeedEnd <= y_.end()) return 0;
  // after f more frames, y is final up to (frameIndex_ + f) * hop - n/2
  const int64_t h = hop_;
  int64_t f = (yNeedEnd + static_cast<int64_t>(n_ / 2) + h - 1) / h - frameIndex_;
  if (f < 1) f = 1;
  const double eff = timeRatio_ * pitch_;
  const double aLast = nextAnalysis() + static_cast<double>(f - 1) * (hop_ / eff);
  const int64_t inNeedEnd = frameInputEnd(static_cast<int64_t>(std::llround(aLast)));
  const int64_t have = in_.end();
  return inNeedEnd > have ? static_cast<uint32_t>(inNeedEnd - have) : 0u;
}

void Stretcher::compactInput() noexcept {
  in_.discardBefore(algo_->oldestNeeded(frameCenter()) - 1);
}

bool Stretcher::push(const float* const* in, uint32_t frames) noexcept {
  if (!algo_ || eof_) return false;
  if (frames == 0) return true;
  if (in_.space() < frames) compactInput();
  if (in_.space() < frames) {
    pump(out_.cap);  // advance the analysis position as far as output space allows
    compactInput();
  }
  if (in_.space() < frames) return false;
  for (uint32_t c = 0; c < cfg_.channels; ++c) std::memcpy(in_.ptr[c] + in_.len, in[c], sizeof(float) * frames);
  in_.len += frames;
  inTotal_ += frames;
  return true;
}

uint32_t Stretcher::pull(float* const* out, uint32_t frames) noexcept {
  if (!algo_) return 0;
  uint32_t done = 0;
  while (done < frames) {
    const uint32_t want = std::min(frames - done, out_.cap / 2);
    pump(want);
    const uint32_t n = std::min(want, out_.len);
    if (n == 0) break;
    for (uint32_t c = 0; c < cfg_.channels; ++c) std::memcpy(out[c] + done, out_.ptr[c], sizeof(float) * n);
    out_.discardBefore(out_.base + n);
    done += n;
  }
  return done;
}

void Stretcher::flush() noexcept {
  if (!algo_ || eof_) return;
  eof_ = true;
  const double eff = timeRatio_ * pitch_;
  const double sNext = static_cast<double>(frameIndex_) * hop_;
  const double yAtEnd = sNext + (static_cast<double>(inTotal_) - nextAnalysis()) * eff;
  const double outEnd = static_cast<double>(outTotal_) + (yAtEnd - p_) / pitch_;
  limit_ = std::max<int64_t>(outTotal_, static_cast<int64_t>(std::llround(outEnd)));
}

bool Stretcher::drained() const noexcept { return eof_ && out_.len == 0 && outTotal_ >= limit_; }

void Stretcher::pump(uint32_t wantOut) noexcept {
  for (;;) {
    resample();
    if (out_.len >= wantOut || out_.space() == 0) break;
    if (limit_ >= 0 && outTotal_ >= limit_) break;
    if (!generateFrame()) break;
  }
}

double Stretcher::kernel(double u) const noexcept {
  u = std::fabs(u);
  if (u >= kTaps) return 0.0;
  const double x = u * kTableRes;
  const size_t i = static_cast<size_t>(x);
  const double f = x - static_cast<double>(i);
  return sinc_[i] + (sinc_[i + 1] - sinc_[i]) * f;
}

void Stretcher::resample() noexcept {
  const uint32_t C = cfg_.channels;
  for (;;) {
    if (out_.space() == 0) break;
    if (limit_ >= 0 && outTotal_ >= limit_) break;
    const int64_t yEnd = y_.end();
    if (pitch_ == 1.0 && p_ == std::floor(p_)) {
      // identity: block copy
      const int64_t j = static_cast<int64_t>(p_);
      int64_t n = std::min<int64_t>(yEnd - j, out_.space());
      if (limit_ >= 0) n = std::min<int64_t>(n, limit_ - outTotal_);
      if (n <= 0) break;
      for (uint32_t c = 0; c < C; ++c)
        std::memcpy(out_.ptr[c] + out_.len, y_.ptr[c] + (j - y_.base), sizeof(float) * static_cast<size_t>(n));
      out_.len += static_cast<uint32_t>(n);
      outTotal_ += n;
      p_ += static_cast<double>(n);
      continue;
    }
    const double fc = pitch_ > 1.0 ? 1.0 / pitch_ : 1.0;
    const int32_t side = static_cast<int32_t>(std::ceil(kTaps / fc));
    const int64_t i0 = static_cast<int64_t>(std::floor(p_));
    if (i0 + side >= yEnd) break;
    for (uint32_t c = 0; c < C; ++c) {
      const float* y = y_.ptr[c] - y_.base;  // absolute indexing
      double acc = 0.0;
      for (int64_t j = i0 - side + 1; j <= i0 + side; ++j) acc += y[j] * kernel((p_ - static_cast<double>(j)) * fc);
      out_.ptr[c][out_.len] = static_cast<float>(acc * fc);
    }
    ++out_.len;
    ++outTotal_;
    p_ += pitch_;
    // stay exact when returning to unity pitch
    if (pitch_ == 1.0 && std::fabs(p_ - std::round(p_)) < 1e-9) p_ = std::round(p_);
  }
  // keep enough history behind the read position for the widest kernel
  y_.discardBefore(static_cast<int64_t>(std::floor(p_)) - kMaxSide - 1);
}

bool Stretcher::generateFrame() noexcept {
  const uint32_t C = cfg_.channels;
  if (y_.space() < hop_) {
    y_.discardBefore(static_cast<int64_t>(std::floor(p_)) - kMaxSide - 1);
    if (y_.space() < hop_) return false;
  }
  const int64_t center = frameCenter();
  const int64_t needEnd = frameInputEnd(center);
  if (needEnd > in_.end()) {
    if (!eof_) return false;
    compactInput();
    const int64_t missing = needEnd - in_.end();
    if (missing > static_cast<int64_t>(in_.space())) return false;
    in_.appendZeros(static_cast<uint32_t>(missing));
  }
  InputView view{in_.ptr.data(), C, in_.base};
  algo_->frame(view, center, framePtr_.data());
  for (uint32_t c = 0; c < C; ++c) ops_->add(ola_[c].data(), frame_[c].data(), n_);
  // the first `hop` samples are final now
  for (uint32_t c = 0; c < C; ++c) {
    float* o = ola_[c].data();
    uint32_t skip = 0;
    if (olaY_ < 0) skip = static_cast<uint32_t>(std::min<int64_t>(-olaY_, hop_));
    if (skip < hop_) std::memcpy(y_.ptr[c] + y_.len, o + skip, sizeof(float) * (hop_ - skip));
    std::memmove(o, o + hop_, sizeof(float) * (n_ - hop_));
    std::memset(o + (n_ - hop_), 0, sizeof(float) * hop_);
  }
  {
    uint32_t skip = olaY_ < 0 ? static_cast<uint32_t>(std::min<int64_t>(-olaY_, hop_)) : 0u;
    y_.len += hop_ - skip;
  }
  a_ = nextAnalysis() + hop_ / (timeRatio_ * pitch_);  // before frameIndex_ moves
  started_ = true;
  olaY_ += hop_;
  ++frameIndex_;
  compactInput();
  return true;
}

// ---- helpers shared by the algorithms ---------------------------------------------------------

uint32_t scaledFrameSize(double sampleRate, double at44k) { return roundToPow2(at44k * sampleRate / 44100.0); }

}  // namespace sq::stretch
