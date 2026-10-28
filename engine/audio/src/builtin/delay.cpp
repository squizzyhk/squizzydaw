// Squizzy Studio engine/audio - "Delay": stereo / ping-pong echo, free time in ms or synced to the
// song tempo (ProcessContext::tempo, filled by the graph; the device's own `bpm` parameter is only
// the fallback when the host gives no tempo), feedback with low/high cut and soft saturation in
// the loop. Delay time changes (incl. tempo changes) glide (tape style, no clicks).
#include <atomic>

#include "device.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t { kMode, kSync, kTimeL, kTimeR, kBpm, kFeedback, kLowCut, kHighCut, kWidth, kMix, kCount };
// Longest delay a line holds: the slowest tempo (20 bpm) times the longest division (1/1 = 4
// beats) is 12 s; free time tops out at 3 s.
constexpr double kMaxSeconds = 12.5;
// quarter-note multiples of the sync divisions (index 0 = off)
constexpr double kBeats[] = {0.0,  0.125, 0.25 * 2.0 / 3.0, 0.25,      0.375, 0.5 * 2.0 / 3.0, 0.5, 0.75,
                             2.0 / 3.0, 1.0,   1.5,             4.0 / 3.0, 2.0,   3.0,             4.0};

struct Line {
  dsp::FracDelay buf;
  dsp::Biquad hp, lp;
  float time = 1.0f;  // smoothed delay in samples
  void reset() noexcept {
    buf.reset();
    hp.reset();
    lp.reset();
  }
};

class Delay final : public FxDevice {
 public:
  Delay() : FxDevice(infoOf("delay")) {}
  uint32_t tail() const noexcept override {
    if (!prepared()) return 0;
    const double secs = std::max(clampedSamples(timeMs(true)), clampedSamples(timeMs(false))) / sr();
    const double fb = t(kFeedback) * 0.01;
    double repeats = fb > 0.001 ? std::log(0.001) / std::log(std::min(fb, 0.995)) : 1.0;
    double total = secs * (repeats + 1.0);
    if (t(kMode) >= 0.5f) total *= 2.0;
    return static_cast<uint32_t>(std::min(total, 60.0) * sr());
  }

 protected:
  std::unique_ptr<FxDevice> fresh() const override { return std::make_unique<Delay>(); }
  bool onPrepare() override {
    lines_.clear();
    lines_.resize(channels());
    for (auto& l : lines_) l.buf.resize(static_cast<size_t>(kMaxSeconds * sr()) + 8);
    timeCoef_ = dsp::onePoleCoef(sr(), 60.0);
    return true;
  }
  void onReset() noexcept override {
    for (auto& l : lines_) {
      l.reset();
      l.time = 0.0f;
    }
    first_ = true;
  }
  void onContext(const ProcessContext& ctx) noexcept override {
    const float bpm = ctx.tempo > 0.0 ? static_cast<float>(std::clamp(ctx.tempo, 1.0, 1000.0)) : 0.0f;
    if (bpm == hostBpm_.load(std::memory_order_relaxed)) return;
    hostBpm_.store(bpm, std::memory_order_relaxed);
    targetL_ = static_cast<float>(clampedSamples(timeMs(true)));
    targetR_ = static_cast<float>(clampedSamples(timeMs(false)));
  }
  void onParams(bool snap) noexcept override {
    const double s = sr();
    hpC_ = dsp::designBiquad(dsp::BqType::Highpass, s, p(kLowCut), 0.7071, 0);
    lpC_ = dsp::designBiquad(dsp::BqType::Lowpass, s, p(kHighCut), 0.7071, 0);
    targetL_ = static_cast<float>(clampedSamples(timeMs(true)));
    targetR_ = static_cast<float>(clampedSamples(timeMs(false)));
    const float mix = p(kMix) * 0.01f;
    const float fb = p(kFeedback) * 0.01f;
    const float dry = std::min(1.0f, 2.0f * (1.0f - mix)), wet = std::min(1.0f, 2.0f * mix);
    if (snap) dry_.snap(dry), wet_.snap(wet), fb_.snap(fb), width_.snap(p(kWidth));
    else dry_.target = dry, wet_.target = wet, fb_.target = fb, width_.target = p(kWidth);
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    const bool ping = ci(kMode) == 1;
    if (first_) {
      for (uint32_t c = 0; c < lines_.size(); ++c) lines_[c].time = (c & 1u) && !ping ? targetR_ : targetL_;
      first_ = false;
    }
    for (uint32_t c = 0; c < nch; c += 2) {
      float* L = io[c];
      float* R = c + 1 < nch ? io[c + 1] : nullptr;
      Line& a = lines_[c];
      Line* b = R ? &lines_[c + 1] : nullptr;
      dsp::Ramp dry = dry_, wet = wet_, fb = fb_, width = width_;
      dry.begin(dry_.target, n);
      wet.begin(wet_.target, n);
      fb.begin(fb_.target, n);
      width.begin(width_.target, n);
      const float tR = ping ? targetL_ : targetR_;
      for (uint32_t i = 0; i < n; ++i) {
        a.time = targetL_ + (a.time - targetL_) * timeCoef_;
        const float g = fb.next();
        const float ya = a.buf.read(a.time);
        const float fa = dsp::softClip(a.lp.process(lpC_, a.hp.process(hpC_, ya)) * g);
        const float dg = dry.next(), wg = wet.next(), wd = width.next();
        if (b) {
          b->time = tR + (b->time - tR) * timeCoef_;
          const float yb = b->buf.read(b->time);
          const float fbb = dsp::softClip(b->lp.process(lpC_, b->hp.process(hpC_, yb)) * g);
          if (ping) {
            a.buf.push(dsp::flush(0.5f * (L[i] + R[i]) + fbb));
            b->buf.push(dsp::flush(fa));
          } else {
            a.buf.push(dsp::flush(L[i] + fa));
            b->buf.push(dsp::flush(R[i] + fbb));
          }
          const float mid = 0.5f * (ya + yb), side = 0.5f * (ya - yb) * wd;
          L[i] = L[i] * dg + (mid + side) * wg;
          R[i] = R[i] * dg + (mid - side) * wg;
        } else {
          a.buf.push(dsp::flush(L[i] + fa));
          L[i] = L[i] * dg + ya * wg;
        }
      }
      a.hp.flushDenormals();
      a.lp.flushDenormals();
      if (b) b->hp.flushDenormals(), b->lp.flushDenormals();
    }
    dry_.end();
    wet_.end();
    fb_.end();
    width_.end();
  }

 private:
  // The delay (in samples) a line really plays for `ms`: limited to what the buffer holds.
  double clampedSamples(double ms) const noexcept { return std::clamp(ms * 0.001 * sr(), 1.0, kMaxSeconds * sr() - 4.0); }
  double timeMs(bool left) const noexcept {
    const int sync = static_cast<int>(t(kSync) + 0.5f);
    if (sync > 0) {
      const float host = hostBpm_.load(std::memory_order_relaxed);
      const double bpm = std::max(1.0f, host > 0.0f ? host : t(kBpm));
      return 60000.0 / bpm * kBeats[sync];
    }
    return left ? t(kTimeL) : t(kTimeR);
  }

  std::vector<Line> lines_;
  dsp::BiquadCoefs hpC_, lpC_;
  dsp::Ramp dry_, wet_, fb_, width_;
  float targetL_ = 1.0f, targetR_ = 1.0f, timeCoef_ = 0.0f;
  bool first_ = true;
  // Song tempo of the last block (0 = none: the Tempo parameter). Atomic: tail() may read it from
  // the control thread.
  std::atomic<float> hostBpm_{0.0f};
};

}  // namespace

DeviceInfo DelayInfo() {
  return makeInfo("delay", "Delay", "Delay", false,
                  {
                      pChoice("mode", "Mode", {"Stereo", "Ping-Pong"}, 1),
                      pChoice("sync", "Sync",
                              {"Off", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2T",
                               "1/2", "1/2D", "1/1"},
                              0),
                      pLog("time_l", "Time L", 1, 3000, 375, "ms"),
                      pLog("time_r", "Time R", 1, 3000, 500, "ms"),
                      pCont("bpm", "Tempo", 20, 300, 120, "BPM"),
                      pCont("feedback", "Feedback", 0, 100, 40, "%"),
                      pLog("lowcut", "Low Cut", 20, 2000, 120, "Hz"),
                      pLog("highcut", "High Cut", 500, 20000, 6500, "Hz"),
                      pCont("width", "Width", 0, 1, 1),
                      pCont("mix", "Mix", 0, 100, 30, "%"),
                  });
}
ProcessorPtr makeDelay() { return std::make_unique<Delay>(); }

}  // namespace sq::engine::audio::builtin
