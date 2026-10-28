// Squizzy Studio engine/audio - process graph, PDC, mixing.
#include "sq/engine/audio/graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sq::engine::audio {

namespace {
std::atomic<uint64_t> g_generation{0};

void allocPlanar(std::vector<float>& buf, std::vector<float*>& ptrs, uint32_t channels, uint32_t frames) {
  buf.assign(static_cast<size_t>(channels) * frames, 0.0f);
  ptrs.assign(channels, nullptr);
  for (uint32_t c = 0; c < channels; ++c) ptrs[c] = &buf[static_cast<size_t>(c) * frames];
}

void meterBlock(MeterBank* bank, int32_t slot, float* const* x, uint32_t channels, uint32_t frames) noexcept {
  if (!bank || slot < 0 || frames == 0) return;
  const uint32_t nch = std::min<uint32_t>(channels, MeterBank::kChannels);
  for (uint32_t c = 0; c < nch; ++c) {
    float peak = 0.0f;
    double sq = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
      const float v = x[c][i];
      const float a = v < 0.0f ? -v : v;
      if (a > peak) peak = a;
      sq += static_cast<double>(v) * v;
    }
    bank->update(static_cast<uint32_t>(slot), c, peak, static_cast<float>(std::sqrt(sq / frames)));
  }
}
}  // namespace

// ---- MeterBank ----
MeterBank::MeterBank() : cells_(new Cell[static_cast<size_t>(kSlots) * kChannels]) {}
void MeterBank::update(uint32_t slot, uint32_t ch, float peak, float rms) noexcept {
  if (slot >= kSlots || ch >= kChannels) return;
  Cell& cell = cells_[static_cast<size_t>(slot) * kChannels + ch];
  float cur = cell.peak.load(std::memory_order_relaxed);
  while (peak > cur && !cell.peak.compare_exchange_weak(cur, peak, std::memory_order_relaxed)) {
  }
  cell.rms.store(rms, std::memory_order_relaxed);
}
void MeterBank::read(uint32_t slot, uint32_t ch, float* peak, float* rms) noexcept {
  if (slot >= kSlots || ch >= kChannels) {
    *peak = *rms = 0.0f;
    return;
  }
  Cell& cell = cells_[static_cast<size_t>(slot) * kChannels + ch];
  *peak = cell.peak.exchange(0.0f, std::memory_order_relaxed);
  *rms = cell.rms.load(std::memory_order_relaxed);
}
void MeterBank::clear() noexcept {
  for (size_t i = 0; i < static_cast<size_t>(kSlots) * kChannels; ++i) {
    cells_[i].peak.store(0.0f);
    cells_[i].rms.store(0.0f);
  }
}

// ---- TempoMapRt ----
void TempoMapRt::set(std::vector<TempoSegment> segs, double sampleRate) {
  sr_ = sampleRate > 0 ? sampleRate : 48000.0;
  if (segs.empty()) segs.push_back(TempoSegment{});
  std::sort(segs.begin(), segs.end(), [](const TempoSegment& a, const TempoSegment& b) { return a.frame < b.frame; });
  for (auto& s : segs)
    if (!(s.bpm > 0.0)) s.bpm = 120.0;
  segs_ = std::move(segs);
}
size_t TempoMapRt::segAtFrame(int64_t f) const noexcept {
  size_t i = 0;
  while (i + 1 < segs_.size() && segs_[i + 1].frame <= f) ++i;
  return i;
}
double TempoMapRt::frameToPpq(int64_t frame) const noexcept {
  const TempoSegment& s = segs_[segAtFrame(frame)];
  return s.ppq + static_cast<double>(frame - s.frame) * s.bpm / (60.0 * sr_);
}
double TempoMapRt::ppqToFrame(double ppq) const noexcept {
  size_t i = 0;
  while (i + 1 < segs_.size() && segs_[i + 1].ppq <= ppq) ++i;
  const TempoSegment& s = segs_[i];
  return static_cast<double>(s.frame) + (ppq - s.ppq) * 60.0 * sr_ / s.bpm;
}
double TempoMapRt::bpmAt(int64_t frame) const noexcept { return segs_[segAtFrame(frame)].bpm; }

// ---- fader / pan ----
void applyFaderPan(const float* const* src, uint32_t srcCh, float* const* dst, uint32_t dstCh, uint32_t frames, float volume,
                   float pan, float* cur, bool* init) noexcept {
  if (dstCh == 0) return;
  float tgt[kMaxChannels];
  int32_t map[kMaxChannels];  // source channel per destination, -1 = silent, -2 = mono mix of 0+1
  const uint32_t nd = std::min<uint32_t>(dstCh, kMaxChannels);
  for (uint32_t c = 0; c < nd; ++c) {
    tgt[c] = 0.0f;
    map[c] = -1;
  }
  if (srcCh == 0) {
    // nothing
  } else if (nd == 1) {
    tgt[0] = volume;
    map[0] = srcCh >= 2 ? -2 : 0;
  } else if (srcCh == 1) {
    float l, r;
    panGainsMono(pan, &l, &r);
    tgt[0] = volume * l;
    tgt[1] = volume * r;
    map[0] = map[1] = 0;
  } else {
    float l, r;
    panGainsStereo(pan, &l, &r);
    tgt[0] = volume * l;
    tgt[1] = volume * r;
    map[0] = 0;
    map[1] = 1;
    for (uint32_t c = 2; c < nd; ++c)
      if (c < srcCh) {
        tgt[c] = volume;
        map[c] = static_cast<int32_t>(c);
      }
  }
  if (!*init) {
    for (uint32_t c = 0; c < nd; ++c) cur[c] = tgt[c];
    *init = true;
  }
  for (uint32_t c = 0; c < nd; ++c) {
    float* d = dst[c];
    if (map[c] == -1) {
      std::memset(d, 0, sizeof(float) * frames);
      cur[c] = tgt[c];
      continue;
    }
    const float g0 = cur[c], g1 = tgt[c];
    if (map[c] == -2) {
      const float* a = src[0];
      const float* b = src[1];
      if (g0 == g1) {
        const float g = 0.5f * g1;
        for (uint32_t i = 0; i < frames; ++i) d[i] = (a[i] + b[i]) * g;
      } else {
        for (uint32_t i = 0; i < frames; ++i) {
          const float g = g0 + (g1 - g0) * (static_cast<float>(i + 1) / static_cast<float>(frames));
          d[i] = (a[i] + b[i]) * (0.5f * g);
        }
      }
    } else {
      const float* s = src[map[c]];
      if (g0 == g1) {
        for (uint32_t i = 0; i < frames; ++i) d[i] = s[i] * g1;
      } else {
        for (uint32_t i = 0; i < frames; ++i) {
          const float g = g0 + (g1 - g0) * (static_cast<float>(i + 1) / static_cast<float>(frames));
          d[i] = s[i] * g;
        }
      }
    }
    cur[c] = g1;
  }
  for (uint32_t c = nd; c < dstCh; ++c) std::memset(dst[c], 0, sizeof(float) * frames);
}

// ---- TrackProcessor ----
TrackProcessor::TrackProcessor(const TrackDesc& d)
    : desc_(d), volume_(d.volume), pan_(d.pan), mute_(d.mute), solo_(d.solo), monitor_(d.monitor), liveMidi_(d.liveMidi) {
  if (desc_.channels == 0) desc_.channels = 1;
  if (desc_.channels > kMaxChannels) desc_.channels = kMaxChannels;
  std::stable_sort(desc_.midi.begin(), desc_.midi.end(), [](const TimedMidi& x, const TimedMidi& y) { return x.frame < y.frame; });
}
ProcessorPtr TrackProcessor::clone() const { return std::make_unique<TrackProcessor>(desc_); }
bool TrackProcessor::prepare(double sr, uint32_t maxFrames, uint32_t outChannels) {
  maxFrames_ = maxFrames;
  outChannels_ = outChannels;
  sources_.clear();
  for (auto& s : desc_.sources) {
    if (!s) continue;
    SourcePtr n = s->clone();
    if (!n->prepare(sr, maxFrames, desc_.channels)) return false;
    sources_.push_back(std::move(n));
  }
  chain_ = ProcessorChain{};
  for (auto& p : desc_.inserts)
    if (p) chain_.add(p->clone());
  if (!chain_.prepare(sr, maxFrames, desc_.channels)) return false;
  instrument_ = desc_.instrument ? desc_.instrument->clone() : nullptr;
  if (instrument_ && !instrument_->prepare(sr, maxFrames, desc_.channels)) return false;
  // sequenced events of one block (bounded) + live events + note-offs of a stop/jump + live
  // note releases + controller resets
  blockEvents_.assign(instrument_ ? std::min<size_t>(desc_.midi.size(), 4096) + kLiveCapacity + 2 * 16 * 128 + 16 * 8 : 0, sq_midi_event{});
  allocPlanar(buf_, bufPtrs_, desc_.channels, maxFrames);
  gainInit_ = false;
  cursor_ = 0;
  expectNext_ = INT64_MIN;
  seqPlaying_ = false;
  liveCount_ = 0;
  std::memset(seqNotes_, 0, sizeof seqNotes_);
  std::memset(seqCtl_, 0, sizeof seqCtl_);
  std::memset(liveNotes_, 0, sizeof liveNotes_);
  std::memset(liveSustain_, 0, sizeof liveSustain_);
  releaseLive_ = false;
  return true;
}
void TrackProcessor::reset() noexcept {
  for (auto& s : sources_) s->reset();
  chain_.reset();
  if (instrument_) instrument_->reset();
  gainInit_ = false;
  expectNext_ = INT64_MIN;
  seqPlaying_ = false;
  liveCount_ = 0;
  std::memset(seqNotes_, 0, sizeof seqNotes_);
  std::memset(seqCtl_, 0, sizeof seqCtl_);
  std::memset(liveNotes_, 0, sizeof liveNotes_);
  std::memset(liveSustain_, 0, sizeof liveSustain_);
  releaseLive_ = false;
}
uint32_t TrackProcessor::latencyBefore(int32_t slot) const noexcept {
  if (slot < 0) return 0;
  uint32_t l = instrument_ ? instrument_->latency() : 0u;
  for (size_t j = 0; j < static_cast<size_t>(slot) && j < chain_.size(); ++j)
    if (const AudioProcessor* p = chain_.at(j)) l += p->latency();
  return l;
}
uint32_t TrackProcessor::tail() const noexcept { return chain_.tail() + (instrument_ ? instrument_->tail() : 0); }
int64_t TrackProcessor::contentEnd() const noexcept {
  int64_t e = 0;
  for (auto& s : sources_) e = std::max(e, s->endFrame());
  if (instrument_ && !desc_.midi.empty()) e = std::max(e, desc_.midi.back().frame + 1);
  return e;
}
bool TrackProcessor::pushLiveMidi(const uint8_t* bytes, uint8_t size) noexcept {
  if (!instrument_ || liveCount_ >= kLiveCapacity || size == 0 || size > 3) return false;
  sq_midi_event& e = live_[liveCount_++];
  e.frame_offset = 0;
  e.bytes[0] = bytes[0];
  e.bytes[1] = size > 1 ? bytes[1] : 0;
  e.bytes[2] = size > 2 ? bytes[2] : 0;
  e.bytes[3] = 0;
  e.size = size;
  e.data = nullptr;
  return true;
}
void TrackProcessor::renderInstrument(const ProcessContext& ctx) noexcept {
  uint32_t n = 0;
  const uint32_t cap = static_cast<uint32_t>(blockEvents_.size());
  auto put = [&](uint32_t offset, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t size) {
    if (n >= cap) return;
    sq_midi_event& me = blockEvents_[n++];
    me.frame_offset = offset;
    me.bytes[0] = b0;
    me.bytes[1] = b1;
    me.bytes[2] = b2;
    me.bytes[3] = 0;
    me.size = size;
    me.data = nullptr;
  };
  constexpr uint8_t kSeqUsed = 1, kSeqSustain = 2, kSeqBend = 4, kSeqMod = 8;
  // Transport stop / locate / loop wrap: sequenced notes end and the controllers the sequence
  // moved go back to rest (a latched sustain pedal would otherwise hold every voice forever).
  auto releaseSequenced = [&](bool stop) {
    for (uint32_t ch = 0; ch < 16; ++ch) {
      const uint8_t chb = static_cast<uint8_t>(ch);
      for (uint32_t k = 0; k < 128; ++k)
        if (seqNotes_[ch][k]) {
          seqNotes_[ch][k] = 0;
          put(0, static_cast<uint8_t>(0x80u | chb), static_cast<uint8_t>(k), 0, 3);
        }
      const uint8_t f = seqCtl_[ch];
      seqCtl_[ch] = 0;
      if (!f) continue;
      bool liveHeld = liveSustain_[ch] != 0;
      for (uint32_t k = 0; k < 128 && !liveHeld; ++k) liveHeld = liveNotes_[ch][k] != 0;
      if ((f & kSeqSustain) && !liveSustain_[ch]) put(0, static_cast<uint8_t>(0xB0u | chb), 64, 0, 3);
      if (f & kSeqBend) put(0, static_cast<uint8_t>(0xE0u | chb), 0, 0x40, 3);
      if (f & kSeqMod) put(0, static_cast<uint8_t>(0xB0u | chb), 1, 0, 3);
      if (stop && !liveHeld) put(0, static_cast<uint8_t>(0xB0u | chb), 123, 0, 3);  // all notes off
    }
  };
  // The track stopped receiving live MIDI: release what the player still holds on it.
  if (releaseLive_) {
    releaseLive_ = false;
    for (uint32_t ch = 0; ch < 16; ++ch) {
      const uint8_t chb = static_cast<uint8_t>(ch);
      for (uint32_t k = 0; k < 128; ++k)
        if (liveNotes_[ch][k]) {
          liveNotes_[ch][k] = 0;
          put(0, static_cast<uint8_t>(0x80u | chb), static_cast<uint8_t>(k), 0, 3);
        }
      if (liveSustain_[ch]) {
        liveSustain_[ch] = 0;
        if (!(seqCtl_[ch] & kSeqSustain)) put(0, static_cast<uint8_t>(0xB0u | chb), 64, 0, 3);
      }
    }
  }
  const int64_t t0 = ctx.timelineFrame;
  const bool playing = ctx.playing;
  if (!playing) {
    if (seqPlaying_) releaseSequenced(true);  // transport stopped: sequenced notes end
    seqPlaying_ = false;
    expectNext_ = INT64_MIN;
  } else if (t0 != expectNext_) {
    if (seqPlaying_) releaseSequenced(false);  // locate / loop wrap
    size_t lo = 0, hi = desc_.midi.size();  // first event at or after t0
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (desc_.midi[mid].frame < t0) lo = mid + 1;
      else hi = mid;
    }
    cursor_ = lo;
  }
  for (uint32_t i = 0; i < liveCount_; ++i) {
    const sq_midi_event& le = live_[i];
    const uint8_t lst = le.bytes[0] & 0xF0u, lch = le.bytes[0] & 0x0Fu, d1 = le.bytes[1] & 0x7Fu;
    if (lst == 0x90 && le.bytes[2] > 0) liveNotes_[lch][d1] = 1;
    else if (lst == 0x80 || lst == 0x90) liveNotes_[lch][d1] = 0;
    else if (lst == 0xB0) {
      if (d1 == 64) liveSustain_[lch] = le.bytes[2] >= 64 ? 1 : 0;
      else if (d1 == 121) liveSustain_[lch] = 0;
      else if (d1 == 120 || d1 == 123) std::memset(liveNotes_[lch], 0, sizeof liveNotes_[lch]);
    }
    if (n < cap) blockEvents_[n++] = le;
  }
  liveCount_ = 0;
  if (playing) {
    seqPlaying_ = true;
    expectNext_ = t0 + ctx.frames;
    while (cursor_ < desc_.midi.size() && desc_.midi[cursor_].frame < t0 + static_cast<int64_t>(ctx.frames)) {
      const TimedMidi& e = desc_.midi[cursor_++];
      const uint8_t st = e.bytes[0] & 0xF0u, ch = e.bytes[0] & 0x0Fu, key = e.bytes[1] & 0x7Fu;
      uint8_t& ctl = seqCtl_[ch];
      ctl = static_cast<uint8_t>(ctl | kSeqUsed);
      if (st == 0x90 && e.bytes[2] > 0) seqNotes_[ch][key] = 1;
      else if (st == 0x80 || st == 0x90) seqNotes_[ch][key] = 0;
      else if (st == 0xE0) {
        const bool centre = e.bytes[1] == 0 && e.bytes[2] == 0x40;
        ctl = static_cast<uint8_t>(centre ? (ctl & ~kSeqBend) : (ctl | kSeqBend));
      } else if (st == 0xB0) {
        const bool on = key == 64 ? e.bytes[2] >= 64 : e.bytes[2] != 0;
        if (key == 64) ctl = static_cast<uint8_t>(on ? (ctl | kSeqSustain) : (ctl & ~kSeqSustain));
        else if (key == 1) ctl = static_cast<uint8_t>(on ? (ctl | kSeqMod) : (ctl & ~kSeqMod));
        else if (key == 121) ctl = static_cast<uint8_t>(ctl & ~(kSeqSustain | kSeqBend | kSeqMod));
      }
      put(static_cast<uint32_t>(std::max<int64_t>(0, e.frame - t0)), e.bytes[0], e.bytes[1], e.bytes[2], e.size);
    }
  }
  ProcessContext sc = ctx;
  sc.midi = blockEvents_.data();
  sc.midiCount = n;
  instrument_->process(sc, bufPtrs_.data(), desc_.channels);
}
void TrackProcessor::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  const uint32_t ch = desc_.channels;
  const uint32_t n = ctx.frames;
  for (uint32_t c = 0; c < ch; ++c) std::memset(bufPtrs_[c], 0, sizeof(float) * n);
  for (auto& s : sources_) {
    if (!ctx.playing && s->needsPlayback()) continue;
    s->render(ctx, bufPtrs_.data(), ch);
  }
  if (instrument_) renderInstrument(ctx);
  if (monitor_ && monitorInMain_ && ctx.input && !desc_.inputChannels.empty()) {
    for (uint32_t c = 0; c < ch; ++c) {
      const uint32_t src = desc_.inputChannels[c < desc_.inputChannels.size() ? c : desc_.inputChannels.size() - 1];
      if (src >= ctx.inputChannels || !ctx.input[src]) continue;
      for (uint32_t i = 0; i < n; ++i) bufPtrs_[c][i] += ctx.input[src][i];
    }
  }
  chain_.process(ctx, bufPtrs_.data(), ch);
  const float vol = audible() ? effectiveVolume() : 0.0f;
  applyFaderPan(bufPtrs_.data(), ch, io, channels, n, vol, effectivePan(), curGain_, &gainInit_);
}

// ---- SubMixProcessor ----
SubMixProcessor::SubMixProcessor(uint32_t channels, std::vector<ProcessorProto> inserts, float volume, float pan, bool mute)
    : channels_(channels == 0 ? 1 : std::min<uint32_t>(channels, kMaxChannels)),
      protos_(std::move(inserts)),
      volume_(volume),
      pan_(pan),
      mute_(mute) {}
ProcessorPtr SubMixProcessor::clone() const {
  auto p = std::make_unique<SubMixProcessor>(channels_, protos_, volume_, pan_, mute_);
  p->setMix64(mix64_);
  return p;
}
bool SubMixProcessor::prepare(double sr, uint32_t maxFrames, uint32_t outChannels) {
  maxFrames_ = maxFrames;
  outChannels_ = outChannels;
  chain_ = ProcessorChain{};
  for (auto& p : protos_)
    if (p) chain_.add(p->clone());
  if (!chain_.prepare(sr, maxFrames, channels_)) return false;
  sumF_.assign(static_cast<size_t>(channels_) * maxFrames, 0.0f);
  sumD_.assign(static_cast<size_t>(channels_) * maxFrames, 0.0);
  allocPlanar(work_, workPtrs_, channels_, maxFrames);
  gainInit_ = false;
  return true;
}
uint32_t SubMixProcessor::latencyBefore(int32_t slot) const noexcept {
  uint32_t l = 0;
  for (size_t j = 0; slot > 0 && j < static_cast<size_t>(slot) && j < chain_.size(); ++j)
    if (const AudioProcessor* p = chain_.at(j)) l += p->latency();
  return l;
}
void SubMixProcessor::reset() noexcept {
  chain_.reset();
  gainInit_ = false;
}
void SubMixProcessor::clearInput(uint32_t frames) noexcept {
  for (uint32_t c = 0; c < channels_; ++c) {
    if (mix64_) std::memset(&sumD_[static_cast<size_t>(c) * maxFrames_], 0, sizeof(double) * frames);
    else std::memset(&sumF_[static_cast<size_t>(c) * maxFrames_], 0, sizeof(float) * frames);
  }
}
void SubMixProcessor::addInput(const float* const* src, uint32_t srcChannels, uint32_t frames) noexcept {
  const uint32_t nch = std::min(channels_, srcChannels);
  for (uint32_t c = 0; c < nch; ++c) {
    const float* s = src[c];
    if (mix64_) {
      double* d = &sumD_[static_cast<size_t>(c) * maxFrames_];
      for (uint32_t i = 0; i < frames; ++i) d[i] += static_cast<double>(s[i]);
    } else {
      float* d = &sumF_[static_cast<size_t>(c) * maxFrames_];
      for (uint32_t i = 0; i < frames; ++i) d[i] += s[i];
    }
  }
}
void SubMixProcessor::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  const uint32_t n = ctx.frames;
  for (uint32_t c = 0; c < channels_; ++c) {
    float* w = workPtrs_[c];
    if (mix64_) {
      const double* d = &sumD_[static_cast<size_t>(c) * maxFrames_];
      for (uint32_t i = 0; i < n; ++i) w[i] = static_cast<float>(d[i]);
    } else {
      std::memcpy(w, &sumF_[static_cast<size_t>(c) * maxFrames_], sizeof(float) * n);
    }
  }
  chain_.process(ctx, workPtrs_.data(), channels_);
  const float vol = audible() ? effectiveVolume() : 0.0f;
  applyFaderPan(workPtrs_.data(), channels_, io, channels, n, vol, effectivePan(), curGain_, &gainInit_);
}

// ---- Metronome ----
void Metronome::configure(const TempoMapRt* tempo, uint32_t numerator, uint32_t denominator, double sr) {
  tempo_ = tempo;
  num_ = numerator ? numerator : 4;
  den_ = denominator ? denominator : 4;
  sr_ = sr;
  clickLen_ = static_cast<int64_t>(sr * 0.03);
  if (clickLen_ < 1) clickLen_ = 1;
}
void Metronome::render(int64_t frame, uint32_t frames, float* const* out, uint32_t channels) noexcept {
  if (!tempo_ || frames == 0) return;
  const double beatLen = 4.0 / static_cast<double>(den_);
  const double p0 = tempo_->frameToPpq(frame - clickLen_);
  int64_t k = static_cast<int64_t>(std::ceil(p0 / beatLen - 1e-9));
  if (k < 0) k = 0;
  const uint32_t nch = std::min<uint32_t>(channels, 2);
  const double twoPi = 2.0 * 3.14159265358979323846;
  for (int guard = 0; guard < 100000; ++guard, ++k) {
    const int64_t bf = static_cast<int64_t>(std::floor(tempo_->ppqToFrame(static_cast<double>(k) * beatLen) + 0.5));
    if (bf >= frame + frames) break;
    const bool accent = (k % num_) == 0;
    const double freq = accent ? 1760.0 : 880.0;
    const float amp = gain_ * (accent ? 1.0f : 0.6f);
    const int64_t s = std::max(bf, frame);
    const int64_t e = std::min(bf + clickLen_, frame + static_cast<int64_t>(frames));
    for (int64_t f = s; f < e; ++f) {
      const int64_t rel = f - bf;
      const double env = 1.0 - static_cast<double>(rel) / static_cast<double>(clickLen_);
      const float v = amp * static_cast<float>(std::sin(twoPi * freq * static_cast<double>(rel) / sr_) * env);
      for (uint32_t c = 0; c < nch; ++c) out[c][f - frame] += v;
    }
  }
}

// ---- Graph ----
Graph::~Graph() { delete extAutomation_; }

std::unique_ptr<Graph> Graph::compile(const GraphDesc& desc, const GraphConfig& cfg, std::string* error) {
  auto fail = [&](const char* msg) -> std::unique_ptr<Graph> {
    if (error) *error = msg;
    return nullptr;
  };
  if (cfg.maxBlock == 0 || cfg.outputChannels == 0 || cfg.outputChannels > kMaxChannels) return fail("invalid graph config");
  const int32_t nb = static_cast<int32_t>(desc.buses.size());
  for (auto& t : desc.tracks) {
    if (t.output < -1 || t.output >= nb) return fail("track routed to a missing bus");
    for (auto& sd : t.sends)
      if (sd.bus < 0 || sd.bus >= nb) return fail("track send to a missing bus");
  }
  for (size_t i = 0; i < desc.buses.size(); ++i) {
    const BusDesc& b = desc.buses[i];
    if (b.output < -1 || b.output >= nb) return fail("bus routed to a missing bus");
    for (auto& sd : b.sends)
      if (sd.bus < 0 || sd.bus >= nb || sd.bus == static_cast<int32_t>(i)) return fail("bus send to a missing bus / itself");
  }

  std::unique_ptr<Graph> g(new Graph());
  g->sr_ = desc.sampleRate > 0 ? desc.sampleRate : 48000.0;
  g->maxBlock_ = cfg.maxBlock;
  g->outChannels_ = cfg.outputChannels;
  g->mix64_ = cfg.mix64;
  g->meters_ = cfg.meters;
  g->masterMeter_ = desc.masterMeterSlot;
  g->generation_ = ++g_generation;
  g->tempo_.set(desc.tempo, g->sr_);
  g->numerator_ = desc.numerator ? desc.numerator : 4;
  g->denominator_ = desc.denominator ? desc.denominator : 4;
  g->metronome_.configure(&g->tempo_, g->numerator_, g->denominator_, g->sr_);
  g->automationKeep_ = desc.automation;
  g->automation_ = g->automationKeep_.get();

  // topological order of buses over output + send edges (inputs before consumers)
  std::vector<std::vector<uint32_t>> busEdges(desc.buses.size());
  std::vector<int32_t> pendingIn(desc.buses.size(), 0);
  for (uint32_t i = 0; i < desc.buses.size(); ++i) {
    const BusDesc& b = desc.buses[i];
    if (b.output >= 0) busEdges[i].push_back(static_cast<uint32_t>(b.output));
    for (auto& sd : b.sends) busEdges[i].push_back(static_cast<uint32_t>(sd.bus));
    for (uint32_t d : busEdges[i]) ++pendingIn[d];
  }
  std::vector<uint32_t> order;
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < desc.buses.size(); ++i)
    if (pendingIn[i] == 0) ready.push_back(i);
  while (!ready.empty()) {
    const uint32_t b = ready.back();
    ready.pop_back();
    order.push_back(b);
    for (uint32_t d : busEdges[b])
      if (--pendingIn[d] == 0) ready.push_back(d);
  }
  if (order.size() != desc.buses.size()) return fail("bus routing has a cycle");
  g->busOrder_ = order;

  auto destChannels = [&](int32_t dest) -> uint32_t {
    if (dest < 0) return cfg.outputChannels;
    const uint32_t c = desc.buses[static_cast<size_t>(dest)].channels;
    return c == 0 ? 1 : std::min<uint32_t>(c, kMaxChannels);
  };

  // instantiate + prepare
  for (auto& td : desc.tracks) {
    auto tp = std::make_unique<TrackProcessor>(td);
    if (!tp->prepare(g->sr_, cfg.maxBlock, destChannels(td.output))) return fail("track prepare failed");
    g->tracks_.push_back(std::move(tp));
    g->trackDest_.push_back(td.output);
    g->trackMeter_.push_back(td.meterSlot);
  }
  for (auto& bd : desc.buses) {
    auto bp = std::make_unique<SubMixProcessor>(bd.channels, bd.inserts, bd.volume, bd.pan, bd.mute);
    bp->setMix64(cfg.mix64);
    if (!bp->prepare(g->sr_, cfg.maxBlock, destChannels(bd.output))) return fail("bus prepare failed");
    g->buses_.push_back(std::move(bp));
    g->busDest_.push_back(bd.output);
    g->busMeter_.push_back(bd.meterSlot);
    g->busIds_.push_back(bd.id);
  }
  g->master_ = std::make_unique<SubMixProcessor>(cfg.outputChannels, desc.masterInserts, desc.masterVolume, 0.0f, false);
  g->master_->setMix64(cfg.mix64);
  if (!g->master_->prepare(g->sr_, cfg.maxBlock, cfg.outputChannels)) return fail("master prepare failed");
  g->masterVolume_ = desc.masterVolume;
  allocPlanar(g->masterBuf_, g->masterPtrs_, cfg.outputChannels, cfg.maxBlock);

  // sends (edges)
  const size_t nt = g->tracks_.size();
  g->trackSends_.assign(nt, {});
  g->busSends_.assign(desc.buses.size(), {});
  auto addSend = [&](bool fromBus, uint32_t src, const SendDesc& sd) {
    auto s = std::make_unique<Send>();
    s->fromBus = fromBus;
    s->source = src;
    s->dest = sd.bus;
    s->pre = sd.pre;
    s->level = sd.level;
    allocPlanar(s->buf, s->ptrs, destChannels(sd.bus), cfg.maxBlock);
    (fromBus ? g->busSends_ : g->trackSends_)[src].push_back(static_cast<uint32_t>(g->sends_.size()));
    g->sends_.push_back(std::move(s));
  };
  for (size_t i = 0; i < nt; ++i)
    for (auto& sd : desc.tracks[i].sends) addSend(false, static_cast<uint32_t>(i), sd);
  for (size_t b = 0; b < desc.buses.size(); ++b)
    for (auto& sd : desc.buses[b].sends) addSend(true, static_cast<uint32_t>(b), sd);

  // ---- latency bookkeeping ----
  g->trackArrival_.resize(nt);
  for (size_t i = 0; i < nt; ++i) g->trackArrival_[i] = g->tracks_[i]->latency();
  std::vector<uint32_t> busMaxIn(desc.buses.size(), 0);
  g->busArrival_.assign(desc.buses.size(), 0);
  uint32_t masterMaxIn = 0;
  auto feed = [&](int32_t dest, uint32_t arrival) {
    if (dest < 0) masterMaxIn = std::max(masterMaxIn, arrival);
    else busMaxIn[static_cast<size_t>(dest)] = std::max(busMaxIn[static_cast<size_t>(dest)], arrival);
  };
  for (size_t i = 0; i < nt; ++i) {
    feed(g->trackDest_[i], g->trackArrival_[i]);
    for (uint32_t si : g->trackSends_[i]) feed(g->sends_[si]->dest, g->trackArrival_[i]);
  }
  for (uint32_t b : order) {
    g->busArrival_[b] = busMaxIn[b] + g->buses_[b]->latency();
    feed(g->busDest_[b], g->busArrival_[b]);
    for (uint32_t si : g->busSends_[b]) feed(g->sends_[si]->dest, g->busArrival_[b]);
  }
  g->latency_ = masterMaxIn + g->master_->latency();
  g->busMaxIn_ = busMaxIn;
  auto maxIn = [&](int32_t dest) { return dest < 0 ? masterMaxIn : busMaxIn[static_cast<size_t>(dest)]; };

  g->trackComp_.resize(nt);
  g->trackOut_.resize(nt);
  g->trackOutPtrs_.resize(nt);
  for (size_t i = 0; i < nt; ++i) {
    const uint32_t ch = destChannels(g->trackDest_[i]);
    g->trackComp_[i].prepare(ch, maxIn(g->trackDest_[i]) - g->trackArrival_[i], cfg.maxBlock);
    allocPlanar(g->trackOut_[i], g->trackOutPtrs_[i], ch, cfg.maxBlock);
  }
  const size_t nbus = desc.buses.size();
  g->busComp_.resize(nbus);
  g->busOut_.resize(nbus);
  g->busOutPtrs_.resize(nbus);
  for (size_t b = 0; b < nbus; ++b) {
    const uint32_t ch = destChannels(g->busDest_[b]);
    g->busComp_[b].prepare(ch, maxIn(g->busDest_[b]) - g->busArrival_[b], cfg.maxBlock);
    allocPlanar(g->busOut_[b], g->busOutPtrs_[b], ch, cfg.maxBlock);
  }
  for (auto& s : g->sends_) {
    const uint32_t arrival = s->fromBus ? g->busArrival_[s->source] : g->trackArrival_[s->source];
    s->comp.prepare(destChannels(s->dest), maxIn(s->dest) - arrival, cfg.maxBlock);
  }

  // ---- fast lane: zero-latency clones of each track's inserts ----
  g->fastChains_.resize(nt);
  g->fastSlot_.assign(nt, {});
  uint32_t maxTrackCh = 1;
  for (size_t i = 0; i < nt; ++i) {
    const TrackDesc& td = desc.tracks[i];
    const uint32_t ch = g->tracks_[i]->channels();
    maxTrackCh = std::max(maxTrackCh, ch);
    for (auto& proto : td.inserts) {
      if (!proto) continue;  // TrackProcessor skips null prototypes as well
      ProcessorPtr p = proto->clone();
      if (!p->prepare(g->sr_, cfg.maxBlock, ch)) return fail("fast lane prepare failed");
      if (p->latency() == 0) {
        g->fastSlot_[i].push_back(static_cast<int32_t>(g->fastChains_[i].size()));
        g->fastChains_[i].add(std::move(p));
      } else {
        g->fastSlot_[i].push_back(-1);
      }
    }
  }
  allocPlanar(g->fastBuf_, g->fastPtrs_, maxTrackCh + cfg.outputChannels, cfg.maxBlock);
  g->fastGain_.assign(nt * kMaxChannels, 0.0f);
  g->fastGainInit_.reset(new bool[nt ? nt : 1]());

  // ---- stems ----
  g->stemTaps_ = cfg.stemTaps;
  g->stemPre_ = cfg.stemPreFader;
  if (cfg.stemTaps) {
    g->stemChannels_.resize(nt);
    g->stemBuf_.resize(nt);
    g->stemPtrs_.resize(nt);
    g->stemDelay_.resize(nt);
    for (size_t i = 0; i < nt; ++i) {
      const uint32_t ch = cfg.stemPreFader ? g->tracks_[i]->channels() : destChannels(g->trackDest_[i]);
      g->stemChannels_[i] = ch;
      allocPlanar(g->stemBuf_[i], g->stemPtrs_[i], ch, cfg.maxBlock);
      g->stemDelay_[i].prepare(ch, g->latency_ - g->trackArrival_[i], cfg.maxBlock);
    }
  }
  g->updateSolo();
  return g;
}

void Graph::reset() noexcept {
  for (auto& t : tracks_) t->reset();
  for (auto& b : buses_) b->reset();
  master_->reset();
  for (auto& d : trackComp_) d.reset();
  for (auto& d : busComp_) d.reset();
  for (auto& d : stemDelay_) d.reset();
  for (auto& s : sends_) {
    s->comp.reset();
    s->init = false;
  }
  for (auto& c : fastChains_) c.reset();
  for (size_t i = 0; i < tracks_.size(); ++i) fastGainInit_[i] = false;
}

int32_t Graph::findTrack(const sq_guid& id) const noexcept {
  for (size_t i = 0; i < tracks_.size(); ++i)
    if (tracks_[i]->desc().id == id) return static_cast<int32_t>(i);
  return -1;
}
int32_t Graph::findBus(const sq_guid& id) const noexcept {
  for (size_t i = 0; i < busIds_.size(); ++i)
    if (busIds_[i] == id) return static_cast<int32_t>(i);
  return -1;
}

void Graph::updateSolo() noexcept {
  bool any = false;
  for (auto& t : tracks_) any = any || t->solo();
  for (auto& t : tracks_) t->setSoloSilenced(any && !t->solo());
}

int64_t Graph::contentEnd() const noexcept {
  int64_t e = 0;
  for (auto& t : tracks_) e = std::max(e, t->contentEnd());
  return e;
}
uint32_t Graph::maxTail() const noexcept {
  uint32_t tt = 0, bt = 0;
  for (auto& t : tracks_) tt = std::max(tt, t->tail());
  for (auto& b : buses_) bt = std::max(bt, b->tail());
  return tt + bt + master_->tail();
}

// ---- live parameters ----
void Graph::setTrackParam(size_t track, int32_t slot, uint32_t param, float value) noexcept {
  if (track >= tracks_.size()) return;
  TrackProcessor& t = *tracks_[track];
  if (slot < 0) {
    if (AudioProcessor* p = t.instrument()) p->setParameter(param, value);
    return;
  }
  if (AudioProcessor* p = t.insert(static_cast<size_t>(slot))) p->setParameter(param, value);
  const auto& fs = fastSlot_[track];
  if (static_cast<size_t>(slot) < fs.size() && fs[static_cast<size_t>(slot)] >= 0)
    if (AudioProcessor* p = fastChains_[track].at(static_cast<size_t>(fs[static_cast<size_t>(slot)]))) p->setParameter(param, value);
}
void Graph::setBusParam(size_t bus, int32_t slot, uint32_t param, float value) noexcept {
  if (bus >= buses_.size() || slot < 0) return;
  if (AudioProcessor* p = buses_[bus]->insert(static_cast<size_t>(slot))) p->setParameter(param, value);
}
void Graph::setMasterParam(int32_t slot, uint32_t param, float value) noexcept {
  if (slot < 0) return;
  if (AudioProcessor* p = master_->insert(static_cast<size_t>(slot))) p->setParameter(param, value);
}
void Graph::setTrackSendLevel(size_t track, size_t send, float level) noexcept {
  if (track < trackSends_.size() && send < trackSends_[track].size()) sends_[trackSends_[track][send]]->level = level;
}
void Graph::setBusSendLevel(size_t bus, size_t send, float level) noexcept {
  if (bus < busSends_.size() && send < busSends_[bus].size()) sends_[busSends_[bus][send]]->level = level;
}
uint32_t Graph::sendCompensation(size_t track, size_t send) const noexcept {
  if (track < trackSends_.size() && send < trackSends_[track].size()) return sends_[trackSends_[track][send]]->comp.delay();
  return 0;
}
void Graph::routeLiveMidi(const uint8_t* bytes, uint8_t size) noexcept {
  for (auto& t : tracks_)
    if (t->liveMidi()) t->pushLiveMidi(bytes, size);
}

AutomationData* Graph::swapAutomation(AutomationData* data) noexcept {
  // Restore static values for whatever the old lanes drove (a removed or retargeted lane must
  // not leave its parameter at the last automated value), then for the new lanes; the new lanes
  // apply from the next block (only while playing).
  endAutomation();
  AutomationData* old = extAutomation_;
  extAutomation_ = data;
  automation_ = data ? data : automationKeep_.get();
  endAutomation();
  return old;
}

// ---- automation (RT) ----
void Graph::applyAutomation(const ProcessContext& ctx) noexcept {
  const AutomationData* a = automation_;
  if (!a) return;
  // Each lane is evaluated at the timeline position of the audio its target processes in this
  // block: sources are read at timelineFrame and reach insert k / the fader after the latency in
  // front of it.
  const int64_t tf = ctx.timelineFrame;
  const size_t nt = std::min(tracks_.size(), a->tracks.size());
  for (size_t i = 0; i < nt; ++i) {
    TrackProcessor& t = *tracks_[i];
    const double ppqTrack = tempo_.frameToPpq(tf - static_cast<int64_t>(trackArrival_[i]));
    for (const AutoLane& l : a->tracks[i]) {
      switch (l.target) {
        case AutoTarget::Volume: t.setAutoVolume(dbToGain(l.lane.valueAt(ppqTrack, 0.0))); break;
        case AutoTarget::Pan: t.setAutoPan(static_cast<float>(std::clamp(l.lane.valueAt(ppqTrack, 0.0), -1.0, 1.0))); break;
        case AutoTarget::Mute: t.setAutoMute(l.lane.valueAt(ppqTrack, 0.0) >= 0.5); break;
        case AutoTarget::Send:
          if (l.slot >= 0 && static_cast<size_t>(l.slot) < trackSends_[i].size()) {
            Send& s = *sends_[trackSends_[i][static_cast<size_t>(l.slot)]];
            s.autoLevel = dbToGain(l.lane.valueAt(ppqTrack, 0.0));
            s.hasAuto = true;
          }
          break;
        case AutoTarget::Insert:
        case AutoTarget::Instrument: {
          const int32_t slot = l.target == AutoTarget::Instrument ? -1 : l.slot;
          const double ppqIns = tempo_.frameToPpq(tf - static_cast<int64_t>(t.latencyBefore(slot)));
          setTrackParam(i, slot, l.param, static_cast<float>(l.lane.valueAt(ppqIns, l.staticValue)));
          break;
        }
      }
    }
  }
  const size_t nb = std::min(buses_.size(), a->buses.size());
  for (size_t b = 0; b < nb; ++b) {
    SubMixProcessor& bus = *buses_[b];
    const double ppq = tempo_.frameToPpq(tf - static_cast<int64_t>(busArrival_[b]));
    for (const AutoLane& l : a->buses[b]) {
      switch (l.target) {
        case AutoTarget::Volume: bus.setAutoVolume(dbToGain(l.lane.valueAt(ppq, 0.0))); break;
        case AutoTarget::Pan: bus.setAutoPan(static_cast<float>(std::clamp(l.lane.valueAt(ppq, 0.0), -1.0, 1.0))); break;
        case AutoTarget::Mute: bus.setAutoMute(l.lane.valueAt(ppq, 0.0) >= 0.5); break;
        case AutoTarget::Send:
          if (l.slot >= 0 && static_cast<size_t>(l.slot) < busSends_[b].size()) {
            Send& s = *sends_[busSends_[b][static_cast<size_t>(l.slot)]];
            s.autoLevel = dbToGain(l.lane.valueAt(ppq, 0.0));
            s.hasAuto = true;
          }
          break;
        case AutoTarget::Insert: {
          const double ppqIns = tempo_.frameToPpq(tf - static_cast<int64_t>(busMaxIn_[b] + bus.latencyBefore(l.slot)));
          setBusParam(b, l.slot, l.param, static_cast<float>(l.lane.valueAt(ppqIns, l.staticValue)));
          break;
        }
        case AutoTarget::Instrument: break;
      }
    }
  }
  automating_ = true;
}

void Graph::endAutomation() noexcept {
  for (auto& t : tracks_) t->clearAutomation();
  for (auto& b : buses_) b->clearAutomation();
  for (auto& s : sends_) s->hasAuto = false;
  // automated parameters go back to their static (document) values
  if (const AutomationData* a = automation_) {
    for (size_t i = 0; i < std::min(tracks_.size(), a->tracks.size()); ++i)
      for (const AutoLane& l : a->tracks[i])
        if ((l.target == AutoTarget::Insert || l.target == AutoTarget::Instrument) && !std::isnan(l.staticValue))
          setTrackParam(i, l.target == AutoTarget::Instrument ? -1 : l.slot, l.param, l.staticValue);
    for (size_t b = 0; b < std::min(buses_.size(), a->buses.size()); ++b)
      for (const AutoLane& l : a->buses[b])
        if (l.target == AutoTarget::Insert && !std::isnan(l.staticValue)) setBusParam(b, l.slot, l.param, l.staticValue);
  }
  automating_ = false;
}

void Graph::sumInto(int32_t dest, float* const* src, uint32_t ch, uint32_t frames) noexcept {
  if (dest < 0) master_->addInput(src, ch, frames);
  else buses_[static_cast<size_t>(dest)]->addInput(src, ch, frames);
}

void Graph::runSend(Send& s, const float* const* src, uint32_t srcCh, float faderVolume, float pan, bool audible, uint32_t frames) noexcept {
  const float level = s.hasAuto ? s.autoLevel : s.level;
  const float vol = audible ? level * (s.pre ? 1.0f : faderVolume) : 0.0f;
  const uint32_t ch = static_cast<uint32_t>(s.ptrs.size());
  applyFaderPan(src, srcCh, s.ptrs.data(), ch, frames, vol, s.pre ? 0.0f : pan, s.cur, &s.init);
  s.comp.process(s.ptrs.data(), ch, frames);
  sumInto(s.dest, s.ptrs.data(), ch, frames);
}

void Graph::process(const ProcessContext& ctx, float* const* out, uint32_t outChannels, bool lowLatencyMonitoring,
                    bool metronome) noexcept {
  const uint32_t n = ctx.frames;
  if (n == 0) return;
  ProcessContext c2 = ctx;
  if (!ctx.timelineExplicit) c2.timelineFrame = ctx.outputFrame + static_cast<int64_t>(latency_);
  c2.midi = nullptr;
  c2.midiCount = 0;

  if (ctx.playing) applyAutomation(c2);
  else if (automating_) endAutomation();

  for (auto& b : buses_) b->clearInput(n);
  master_->clearInput(n);

  // tracks
  for (size_t i = 0; i < tracks_.size(); ++i) {
    TrackProcessor& t = *tracks_[i];
    t.setMonitorInMain(!lowLatencyMonitoring);
    float* const* to = trackOutPtrs_[i].data();
    const uint32_t ch = static_cast<uint32_t>(trackOutPtrs_[i].size());
    t.process(c2, to, ch);
    meterBlock(meters_, trackMeter_[i], to, ch, n);
    if (stemTaps_) {
      float* const* src = stemPre_ ? t.preFader() : to;
      for (uint32_t c = 0; c < stemChannels_[i]; ++c) std::memcpy(stemPtrs_[i][c], src[c], sizeof(float) * n);
      stemDelay_[i].process(stemPtrs_[i].data(), stemChannels_[i], n);
    }
    for (uint32_t si : trackSends_[i]) runSend(*sends_[si], t.preFader(), t.channels(), t.effectiveVolume(), t.effectivePan(), t.audible(), n);
    trackComp_[i].process(to, ch, n);
    sumInto(trackDest_[i], to, ch, n);
  }
  // buses
  for (uint32_t b : busOrder_) {
    SubMixProcessor& bus = *buses_[b];
    float* const* bo = busOutPtrs_[b].data();
    const uint32_t ch = static_cast<uint32_t>(busOutPtrs_[b].size());
    bus.process(c2, bo, ch);
    meterBlock(meters_, busMeter_[b], bo, ch, n);
    for (uint32_t si : busSends_[b]) runSend(*sends_[si], bus.preFader(), bus.channels(), bus.effectiveVolume(), bus.effectivePan(), bus.audible(), n);
    busComp_[b].process(bo, ch, n);
    sumInto(busDest_[b], bo, ch, n);
  }
  // master -> device
  master_->setVolume(masterVolume_);
  const uint32_t nOut = std::min<uint32_t>(outChannels, kMaxChannels);
  const uint32_t nMaster = std::min(nOut, outChannels_);
  master_->process(c2, masterPtrs_.data(), outChannels_);
  for (uint32_t c = 0; c < nMaster; ++c) std::memcpy(out[c], masterPtrs_[c], sizeof(float) * n);
  for (uint32_t c = nMaster; c < nOut; ++c) std::memset(out[c], 0, sizeof(float) * n);
  meterBlock(meters_, masterMeter_, out, nMaster, n);

  if (metronome) metronome_.render(ctx.outputFrame, n, out, nOut);
  if (lowLatencyMonitoring) processFastLane(ctx, out, nOut);
}

void Graph::processFastLane(const ProcessContext& ctx, float* const* out, uint32_t outChannels) noexcept {
  if (!ctx.input) return;
  const uint32_t n = ctx.frames;
  for (size_t i = 0; i < tracks_.size(); ++i) {
    TrackProcessor& t = *tracks_[i];
    if (!t.monitor() || t.desc().inputChannels.empty()) {
      fastGainInit_[i] = false;  // restart without a ramp when monitoring comes back
      continue;
    }
    const uint32_t ch = t.channels();
    float* const* fb = fastPtrs_.data();
    float* const* fo = fastPtrs_.data() + ch;
    const auto& ins = t.desc().inputChannels;
    for (uint32_t c = 0; c < ch; ++c) {
      const uint32_t src = ins[c < ins.size() ? c : ins.size() - 1];
      if (src < ctx.inputChannels && ctx.input[src]) std::memcpy(fb[c], ctx.input[src], sizeof(float) * n);
      else std::memset(fb[c], 0, sizeof(float) * n);
    }
    fastChains_[i].process(ctx, fb, ch);
    const uint32_t oc = std::min<uint32_t>(outChannels, static_cast<uint32_t>(fastPtrs_.size()) - ch);
    // Same fader/pan law, mute/solo and master volume as the main path (no level jump when the
    // fast lane is toggled); the gain ramps per track across blocks like the main fader.
    const float vol = t.audible() ? t.effectiveVolume() : 0.0f;
    applyFaderPan(fb, ch, fo, oc, n, vol, t.effectivePan(), &fastGain_[i * kMaxChannels], &fastGainInit_[i]);
    const float mv = masterVolume_;
    for (uint32_t c = 0; c < oc; ++c)
      for (uint32_t k = 0; k < n; ++k) out[c][k] += fo[c][k] * mv;
  }
}

}  // namespace sq::engine::audio
