// Squizzy Studio engine/audio - "Sampler": plays one decoded audio file across the keyboard
// (root note, ADSR, optional loop). The host decodes the file (makeBuiltinWithMedia).
#include "device.hpp"
#include "voices.hpp"

namespace sq::engine::audio::builtin {
namespace {

enum P : uint32_t {
  kRoot, kTranspose, kFine, kAttack, kDecay, kSustain, kRelease,
  kLoop, kLoopStart, kLoopEnd, kStart, kVelSens, kKeyTrack, kGain, kCount
};
constexpr int kVoices = 16;

struct Voice {
  VoiceSlot slot;
  double pos = 0.0;
  float vel = 0.0f;
  dsp::Adsr env;
  // Declick: when a sounding voice is retriggered or stolen, its old playback keeps running here
  // for a few ms and fades out while the new note's attack starts.
  double ghostPos = 0.0;
  float ghostAmp = 0.0f, ghostFade = 0.0f;
  uint8_t ghostNote = 0;
};

class Sampler final : public SynthDevice {
 public:
  explicit Sampler(MediaPtr media) : SynthDevice(infoOf("sampler")), media_(std::move(media)) {
    if (media_ && (media_->channels == 0 || media_->frames < 2 || media_->data.size() < media_->channels)) media_.reset();
  }

 protected:
  std::unique_ptr<SynthDevice> fresh() const override { return std::make_unique<Sampler>(media_); }
  bool onPrepare() override {
    ghostStep_ = static_cast<float>(1.0 / std::max(1.0, 0.003 * sr()));
    return true;
  }
  void onReset() noexcept override {
    for (auto& v : v_) v = Voice{};
    bend_ = 0.0f;
    sustain_ = false;
  }
  void onParams(bool snap) noexcept override {
    envP_.set(sr(), p(kAttack), p(kDecay), p(kSustain), p(kRelease));
    const float g = dsp::dbToLin(p(kGain));
    if (snap) gain_.snap(g); else gain_.target = g;
  }
  void onMidi(const sq_midi_event& e) noexcept override {
    const uint8_t st = midiStatus(e);
    if (isNoteOn(e)) {
      if (!media_) return;
      const uint8_t note = e.bytes[1] & 0x7F;
      const int i = allocateVoice(v_, kVoices, note, [](const Voice& v) { return v.env.level; });
      Voice& v = v_[i];
      if (v.slot.active && v.env.level > 0.0f) {  // retriggered or stolen while sounding
        v.ghostPos = v.pos;
        v.ghostNote = v.slot.note;
        v.ghostAmp = v.env.level * v.vel;
        v.ghostFade = 1.0f;
      }
      v.slot.active = v.slot.held = true;
      v.slot.sustained = false;
      v.slot.note = note;
      v.slot.age = ++clock_;
      const float vs = p(kVelSens);
      v.vel = (1.0f - vs) + vs * static_cast<float>(e.bytes[2]) / 127.0f;
      v.pos = static_cast<double>(media_->frames - 1) * p(kStart) * 0.01;
      v.env.level = 0.0f;  // restart; the old playback (if any) fades out as the ghost
      v.env.gateOn();
    } else if (isNoteOff(e)) {
      noteOffVoices(v_, kVoices, e.bytes[1] & 0x7F, sustain_, [](Voice& v) { v.env.gateOff(); });
    } else if (st == 0xE0) {
      bend_ = pitchBendValue(e) * 2.0f;
    } else if (st == 0xB0) {
      const uint8_t cc = e.bytes[1];
      if (cc == 64) {
        sustain_ = e.bytes[2] >= 64;
        if (!sustain_)
          for (auto& v : v_)
            if (v.slot.sustained) v.slot.sustained = false, v.env.gateOff();
      } else if (cc == 123) {
        for (auto& v : v_) v.slot.held = v.slot.sustained = false, v.env.gateOff();
      } else if (cc == 120) {
        for (auto& v : v_) v = Voice{};
      }
    }
  }

  void render(float* const* io, uint32_t nch, uint32_t n) noexcept override {
    gain_.begin(gain_.target, n);
    if (media_) {
      const MediaBuffer& m = *media_;
      const double last = static_cast<double>(m.frames - 1);
      const bool loop = on(kLoop);
      double ls = last * p(kLoopStart) * 0.01, le = last * p(kLoopEnd) * 0.01;
      if (le < ls) std::swap(ls, le);
      const bool looping = loop && le - ls >= 16.0;
      const double baseRate = m.sampleRate / sr();
      const bool keyTrack = on(kKeyTrack);
      const double tune = static_cast<double>(ci(kTranspose)) + p(kFine) * 0.01 + bend_;
      // Advances `pos` past the end / around the loop; false when a one-shot ran out.
      auto wrap = [&](double& pos) noexcept {
        if (pos >= last) {
          if (!looping) return false;
          pos = ls + std::fmod(pos - ls, le - ls);
        }
        if (looping && pos >= le) pos -= (le - ls);
        return true;
      };
      auto mixAt = [&](double pos, float amp, uint32_t i) noexcept {
        const double fi = std::floor(pos);
        const size_t i0 = static_cast<size_t>(fi);
        const float fr = static_cast<float>(pos - fi);
        const size_t i1 = i0 + 1 < static_cast<size_t>(m.frames) ? i0 + 1 : i0;
        if (nch == 1 || m.channels == 1) {
          float x = 0.0f;
          for (uint32_t c = 0; c < m.channels; ++c) {
            const float* d = m.data[c].data();
            x += d[i0] + fr * (d[i1] - d[i0]);
          }
          x *= amp / static_cast<float>(m.channels);
          for (uint32_t c = 0; c < nch; ++c) io[c][i] += x;
        } else {
          for (uint32_t c = 0; c < nch; ++c) {
            const float* d = m.data[c % m.channels].data();
            io[c][i] += (d[i0] + fr * (d[i1] - d[i0])) * amp;
          }
        }
      };
      auto rateOf = [&](uint8_t note) {
        const double semis = (keyTrack ? static_cast<double>(note) - ci(kRoot) : 0.0) + tune;
        return baseRate * std::exp2(semis / 12.0);
      };
      for (auto& v : v_) {
        if (v.ghostFade > 0.0f) {
          const double rate = rateOf(v.ghostNote);
          dsp::Ramp g = gain_;
          for (uint32_t i = 0; i < n && v.ghostFade > 0.0f; ++i) {
            if (!wrap(v.ghostPos)) {
              v.ghostFade = 0.0f;
              break;
            }
            mixAt(v.ghostPos, v.ghostAmp * v.ghostFade * g.next(), i);
            v.ghostPos += rate;
            v.ghostFade -= ghostStep_;
          }
          if (v.ghostFade < 0.0f) v.ghostFade = 0.0f;
        }
        if (!v.slot.active) continue;
        const double rate = rateOf(v.slot.note);
        dsp::Ramp g = gain_;
        for (uint32_t i = 0; i < n; ++i) {
          if (!wrap(v.pos)) {
            v.env.kill();
            break;
          }
          mixAt(v.pos, v.env.tick(envP_) * v.vel * g.next(), i);
          v.pos += rate;
          if (!v.env.active()) break;
        }
        if (!v.env.active()) v.slot.active = false;
      }
    }
    gain_.end();
  }

 private:
  MediaPtr media_;
  Voice v_[kVoices];
  dsp::AdsrParams envP_;
  dsp::Ramp gain_;
  float bend_ = 0.0f;
  bool sustain_ = false;
  uint64_t clock_ = 0;
  float ghostStep_ = 0.01f;
};

}  // namespace

DeviceInfo SamplerInfo() {
  return makeInfo("sampler", "Sampler", "Instrument", true,
                  {
                      pInt("root", "Root Note", 0, 127, 60),
                      pInt("transpose", "Transpose", -48, 48, 0, "st"),
                      pCont("fine", "Fine", -100, 100, 0, "ct"),
                      pLog("attack", "Attack", 0.1f, 10000, 1, "ms"),
                      pLog("decay", "Decay", 1, 20000, 1000, "ms"),
                      pCont("sustain", "Sustain", 0, 1, 1),
                      pLog("release", "Release", 1, 20000, 150, "ms"),
                      pToggle("loop", "Loop", false),
                      pCont("loop_start", "Loop Start", 0, 100, 0, "%"),
                      pCont("loop_end", "Loop End", 0, 100, 100, "%"),
                      pCont("start", "Sample Start", 0, 100, 0, "%"),
                      pCont("velocity", "Velocity Sensitivity", 0, 1, 0.7f),
                      pToggle("key_track", "Key Tracking", true),
                      pCont("gain", "Gain", -36, 12, 0, "dB"),
                  });
}

ProcessorPtr makeSampler() { return std::make_unique<Sampler>(nullptr); }
ProcessorPtr makeSamplerWithMedia(MediaPtr media) { return std::make_unique<Sampler>(std::move(media)); }

}  // namespace sq::engine::audio::builtin
