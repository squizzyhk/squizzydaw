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
    : desc_(d), volume_(d.volume), pan_(d.pan), mute_(d.mute), solo_(d.solo), monitor_(d.monitor) {
  if (desc_.channels == 0) desc_.channels = 1;
  if (desc_.channels > kMaxChannels) desc_.channels = kMaxChannels;
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
  allocPlanar(buf_, bufPtrs_, desc_.channels, maxFrames);
  gainInit_ = false;
  return true;
}
void TrackProcessor::reset() noexcept {
  for (auto& s : sources_) s->reset();
  chain_.reset();
  gainInit_ = false;
}
int64_t TrackProcessor::contentEnd() const noexcept {
  int64_t e = 0;
  for (auto& s : sources_) e = std::max(e, s->endFrame());
  return e;
}
void TrackProcessor::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  const uint32_t ch = desc_.channels;
  const uint32_t n = ctx.frames;
  for (uint32_t c = 0; c < ch; ++c) std::memset(bufPtrs_[c], 0, sizeof(float) * n);
  for (auto& s : sources_) {
    if (!ctx.playing && s->needsPlayback()) continue;
    s->render(ctx, bufPtrs_.data(), ch);
  }
  if (monitor_ && monitorInMain_ && ctx.input && !desc_.inputChannels.empty()) {
    for (uint32_t c = 0; c < ch; ++c) {
      const uint32_t src = desc_.inputChannels[c < desc_.inputChannels.size() ? c : desc_.inputChannels.size() - 1];
      if (src >= ctx.inputChannels || !ctx.input[src]) continue;
      for (uint32_t i = 0; i < n; ++i) bufPtrs_[c][i] += ctx.input[src][i];
    }
  }
  chain_.process(ctx, bufPtrs_.data(), ch);
  const float vol = (mute_ || soloSilenced_) ? 0.0f : volume_;
  applyFaderPan(bufPtrs_.data(), ch, io, channels, n, vol, pan_, curGain_, &gainInit_);
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
  const float vol = mute_ ? 0.0f : volume_;
  applyFaderPan(workPtrs_.data(), channels_, io, channels, n, vol, pan_, curGain_, &gainInit_);
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
Graph::~Graph() = default;

std::unique_ptr<Graph> Graph::compile(const GraphDesc& desc, const GraphConfig& cfg, std::string* error) {
  auto fail = [&](const char* msg) -> std::unique_ptr<Graph> {
    if (error) *error = msg;
    return nullptr;
  };
  if (cfg.maxBlock == 0 || cfg.outputChannels == 0 || cfg.outputChannels > kMaxChannels) return fail("invalid graph config");
  const int32_t nb = static_cast<int32_t>(desc.buses.size());
  for (auto& t : desc.tracks)
    if (t.output < -1 || t.output >= nb) return fail("track routed to a missing bus");
  for (auto& b : desc.buses)
    if (b.output < -1 || b.output >= nb) return fail("bus routed to a missing bus");

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

  // topological order of buses (inputs before consumers)
  std::vector<int32_t> pendingIn(desc.buses.size(), 0);
  for (auto& b : desc.buses)
    if (b.output >= 0) ++pendingIn[static_cast<size_t>(b.output)];
  std::vector<uint32_t> order;
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < desc.buses.size(); ++i)
    if (pendingIn[i] == 0) ready.push_back(i);
  while (!ready.empty()) {
    const uint32_t b = ready.back();
    ready.pop_back();
    order.push_back(b);
    const int32_t o = desc.buses[b].output;
    if (o >= 0 && --pendingIn[static_cast<size_t>(o)] == 0) ready.push_back(static_cast<uint32_t>(o));
  }
  if (order.size() != desc.buses.size()) return fail("bus routing has a cycle");
  // Kahn above releases a bus once all its BUS inputs are done; keep a stable order of that.
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

  // ---- latency bookkeeping ----
  const size_t nt = g->tracks_.size();
  g->trackArrival_.resize(nt);
  for (size_t i = 0; i < nt; ++i) g->trackArrival_[i] = g->tracks_[i]->latency();
  std::vector<uint32_t> busMaxIn(desc.buses.size(), 0);
  g->busArrival_.assign(desc.buses.size(), 0);
  uint32_t masterMaxIn = 0;
  auto feed = [&](int32_t dest, uint32_t arrival) {
    if (dest < 0) masterMaxIn = std::max(masterMaxIn, arrival);
    else busMaxIn[static_cast<size_t>(dest)] = std::max(busMaxIn[static_cast<size_t>(dest)], arrival);
  };
  for (size_t i = 0; i < nt; ++i) feed(g->trackDest_[i], g->trackArrival_[i]);
  for (uint32_t b : order) {
    g->busArrival_[b] = busMaxIn[b] + g->buses_[b]->latency();
    feed(g->busDest_[b], g->busArrival_[b]);
  }
  g->latency_ = masterMaxIn + g->master_->latency();
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

  // ---- fast lane: zero-latency clones of each track's inserts ----
  g->fastChains_.resize(nt);
  uint32_t maxTrackCh = 1;
  for (size_t i = 0; i < nt; ++i) {
    const TrackDesc& td = desc.tracks[i];
    const uint32_t ch = g->tracks_[i]->channels();
    maxTrackCh = std::max(maxTrackCh, ch);
    for (auto& proto : td.inserts) {
      if (!proto) continue;
      ProcessorPtr p = proto->clone();
      if (!p->prepare(g->sr_, cfg.maxBlock, ch)) return fail("fast lane prepare failed");
      if (p->latency() == 0) g->fastChains_[i].add(std::move(p));
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

void Graph::sumInto(int32_t dest, float* const* src, uint32_t ch, uint32_t frames) noexcept {
  if (dest < 0) master_->addInput(src, ch, frames);
  else buses_[static_cast<size_t>(dest)]->addInput(src, ch, frames);
}

void Graph::process(const ProcessContext& ctx, float* const* out, uint32_t outChannels, bool lowLatencyMonitoring,
                    bool metronome) noexcept {
  const uint32_t n = ctx.frames;
  if (n == 0) return;
  ProcessContext c2 = ctx;
  if (!ctx.timelineExplicit) c2.timelineFrame = ctx.outputFrame + static_cast<int64_t>(latency_);
  c2.midi = nullptr;
  c2.midiCount = 0;

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
    trackComp_[i].process(to, ch, n);
    sumInto(trackDest_[i], to, ch, n);
  }
  // buses
  for (uint32_t b : busOrder_) {
    float* const* bo = busOutPtrs_[b].data();
    const uint32_t ch = static_cast<uint32_t>(busOutPtrs_[b].size());
    buses_[b]->process(c2, bo, ch);
    meterBlock(meters_, busMeter_[b], bo, ch, n);
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
    const float vol = t.audible() ? t.volume() : 0.0f;
    applyFaderPan(fb, ch, fo, oc, n, vol, t.pan(), &fastGain_[i * kMaxChannels], &fastGainInit_[i]);
    const float mv = masterVolume_;
    for (uint32_t c = 0; c < oc; ++c)
      for (uint32_t k = 0; k < n; ++k) out[c][k] += fo[c][k] * mv;
  }
}

}  // namespace sq::engine::audio
