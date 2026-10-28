// Squizzy Studio engine/audio - source nodes.
#include "sq/engine/audio/nodes.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sq::engine::audio {

// ---- media loading (control / worker thread) ----
sq_result loadMedia(IAudioFileReader* reader, const char* path, MediaPtr* out) {
  if (!reader || !path || !out) return SQ_E_INVALIDARG;
  sq_result r = reader->open(path);
  if (SQ_FAILED(r)) return r;
  sq_audio_file_format fmt{};
  fmt.struct_size = sizeof fmt;
  r = reader->getFormat(&fmt);
  if (SQ_FAILED(r)) return r;
  if (fmt.channels == 0 || fmt.channels > kMaxChannels) return SQ_E_UNSUPPORTED;
  auto m = std::make_shared<MediaBuffer>();
  m->sampleRate = fmt.sample_rate;
  m->channels = fmt.channels;
  m->path = path;
  m->data.resize(fmt.channels);
  if (fmt.frame_count > 0)
    for (auto& d : m->data) d.reserve(static_cast<size_t>(fmt.frame_count));
  constexpr uint32_t kChunk = 8192;
  std::vector<float> tmp(static_cast<size_t>(kChunk) * fmt.channels);
  std::vector<float*> ptrs(fmt.channels);
  for (uint32_t c = 0; c < fmt.channels; ++c) ptrs[c] = &tmp[static_cast<size_t>(c) * kChunk];
  for (;;) {
    sq_audio_block blk{ptrs.data(), fmt.channels, kChunk, 0};
    uint32_t got = 0;
    r = reader->read(&blk, &got);
    if (SQ_FAILED(r)) return r;
    for (uint32_t c = 0; c < fmt.channels; ++c) m->data[c].insert(m->data[c].end(), ptrs[c], ptrs[c] + got);
    if (got < kChunk) break;
  }
  m->frames = static_cast<int64_t>(m->data[0].size());
  *out = std::move(m);
  return SQ_OK;
}

static std::string extensionOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string e = path.substr(dot + 1);
  for (char& ch : e) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
  return e;
}

sq_result loadMediaFile(IClassRegistry* reg, const char* path, MediaPtr* out) {
  if (!reg || !path || !out) return SQ_E_INVALIDARG;
  const std::string ext = extensionOf(path);
  sq::Ptr<ICodecService> codec = sq::Ptr<ICodecService>::adopt(codec::findCodec(reg, ext.c_str(), nullptr, SQ_CODEC_READ));
  if (!codec) return SQ_E_UNSUPPORTED;
  sq::Ptr<IAudioFileReader> reader;
  sq_result r = codec->createReader(reader.put());
  if (SQ_FAILED(r)) return r;
  return loadMedia(reader.get(), path, out);
}

// ---- MediaNode ----
void MediaNode::render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept {
  if (!media_ || media_->channels == 0) return;
  const int64_t t0 = ctx.timelineFrame;
  const double graphRate = ctx.sampleRate > 0 ? ctx.sampleRate : 48000.0;
  const double ratio = media_->sampleRate > 0 ? media_->sampleRate / graphRate : 1.0;
  if (std::fabs(ratio - 1.0) > 1e-9) {
    // Linear-interpolating sample-rate conversion so imported media at any rate plays at the
    // right pitch/speed through a graph that runs at the device rate. t0 and ctx.frames are
    // graph frames; the source position is t0 * ratio.
    const double last = static_cast<double>(media_->frames - 1);
    for (uint32_t c = 0; c < channels; ++c) {
      uint32_t sc = c;
      if (sc >= media_->channels) {
        if (media_->channels != 1) continue;
        sc = 0;
      }
      const float* src = media_->channel(sc);
      float* dst = out[c];
      for (uint32_t i = 0; i < ctx.frames; ++i) {
        const double p = (static_cast<double>(t0) + static_cast<double>(i)) * ratio;
        if (p < 0 || p > last) continue;
        const int64_t i0 = static_cast<int64_t>(p);
        const double frac = p - static_cast<double>(i0);
        const float a = src[i0];
        const float b = i0 + 1 < media_->frames ? src[i0 + 1] : a;
        dst[i] += static_cast<float>(a + (b - a) * frac);
      }
    }
    return;
  }
  const int64_t s = std::max<int64_t>(t0, 0);
  const int64_t e = std::min<int64_t>(t0 + ctx.frames, media_->frames);
  if (s >= e) return;
  const size_t off = static_cast<size_t>(s - t0);
  const size_t n = static_cast<size_t>(e - s);
  for (uint32_t c = 0; c < channels; ++c) {
    uint32_t sc = c;
    if (sc >= media_->channels) {
      if (media_->channels != 1) continue;
      sc = 0;
    }
    const float* src = media_->channel(sc) + s;
    float* dst = out[c] + off;
    for (size_t i = 0; i < n; ++i) dst[i] += src[i];
  }
}

sq_result FileNode::load(IClassRegistry* reg, const char* path, std::unique_ptr<FileNode>* out) {
  if (!out) return SQ_E_INVALIDARG;
  MediaPtr m;
  sq_result r = loadMediaFile(reg, path, &m);
  if (SQ_FAILED(r)) return r;
  *out = std::make_unique<FileNode>(std::move(m));
  return SQ_OK;
}

// ---- helpers ----
static void allocScratch(std::vector<float>& buf, std::vector<float*>& ptrs, uint32_t channels, uint32_t frames) {
  buf.assign(static_cast<size_t>(channels) * frames, 0.0f);
  ptrs.assign(channels, nullptr);
  for (uint32_t c = 0; c < channels; ++c) ptrs[c] = &buf[static_cast<size_t>(c) * frames];
}

// ---- ClipNode ----
bool ClipNode::prepare(double sr, uint32_t maxFrames, uint32_t channels) {
  child_ = childProto_ ? childProto_->clone() : nullptr;
  if (child_ && !child_->prepare(sr, maxFrames, channels)) return false;
  maxFrames_ = maxFrames;
  allocScratch(scratch_, ptrs_, channels, maxFrames);
  return true;
}
void ClipNode::reset() noexcept {
  if (child_) child_->reset();
}
void ClipNode::render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept {
  if (!child_) return;
  const int64_t t0 = ctx.timelineFrame;
  const int64_t s = std::max<int64_t>(t0, p_.start);
  const int64_t e = std::min<int64_t>(t0 + ctx.frames, p_.start + p_.length);
  if (s >= e) return;
  const uint32_t off = static_cast<uint32_t>(s - t0);
  const uint32_t cnt = static_cast<uint32_t>(e - s);
  const uint32_t nch = std::min<uint32_t>(channels, static_cast<uint32_t>(ptrs_.size()));
  for (uint32_t c = 0; c < nch; ++c) std::memset(ptrs_[c], 0, sizeof(float) * cnt);
  ProcessContext cc = ctx;
  cc.timelineFrame = s - p_.start + p_.sourceOffset;
  cc.frames = cnt;
  child_->render(cc, ptrs_.data(), nch);
  const bool fades = p_.fadeIn > 0 || p_.fadeOut > 0;
  for (uint32_t c = 0; c < nch; ++c) {
    const float* src = ptrs_[c];
    float* dst = out[c] + off;
    if (!fades) {
      for (uint32_t i = 0; i < cnt; ++i) dst[i] += src[i] * p_.gain;
      continue;
    }
    for (uint32_t i = 0; i < cnt; ++i) {
      const int64_t rel = s + i - p_.start;
      float g = p_.gain;
      if (p_.fadeIn > 0 && rel < p_.fadeIn) g *= static_cast<float>(static_cast<double>(rel) / static_cast<double>(p_.fadeIn));
      if (p_.fadeOut > 0 && rel >= p_.length - p_.fadeOut)
        g *= static_cast<float>(static_cast<double>(p_.length - rel) / static_cast<double>(p_.fadeOut));
      dst[i] += src[i] * g;
    }
  }
}

// ---- ModificationNode ----
ModificationNode::ModificationNode(SourceProto child, std::vector<GainPoint> points)
    : childProto_(std::move(child)), points_(std::move(points)) {
  std::sort(points_.begin(), points_.end(), [](const GainPoint& a, const GainPoint& b) { return a.frame < b.frame; });
}
bool ModificationNode::prepare(double sr, uint32_t maxFrames, uint32_t channels) {
  child_ = childProto_ ? childProto_->clone() : nullptr;
  if (child_ && !child_->prepare(sr, maxFrames, channels)) return false;
  maxFrames_ = maxFrames;
  allocScratch(scratch_, ptrs_, channels, maxFrames);
  return true;
}
void ModificationNode::reset() noexcept {
  if (child_) child_->reset();
}
int64_t ModificationNode::endFrame() const noexcept { return child_ ? child_->endFrame() : (childProto_ ? childProto_->endFrame() : 0); }
float ModificationNode::gainAt(int64_t frame) const noexcept {
  if (points_.empty()) return 1.0f;
  if (frame <= points_.front().frame) return points_.front().gain;
  if (frame >= points_.back().frame) return points_.back().gain;
  // binary search for the segment
  size_t lo = 0, hi = points_.size() - 1;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (points_[mid].frame <= frame) lo = mid;
    else hi = mid;
  }
  const GainPoint& a = points_[lo];
  const GainPoint& b = points_[hi];
  const double t = static_cast<double>(frame - a.frame) / static_cast<double>(b.frame - a.frame);
  return static_cast<float>(a.gain + (b.gain - a.gain) * t);
}
void ModificationNode::render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept {
  if (!child_) return;
  const uint32_t nch = std::min<uint32_t>(channels, static_cast<uint32_t>(ptrs_.size()));
  for (uint32_t c = 0; c < nch; ++c) std::memset(ptrs_[c], 0, sizeof(float) * ctx.frames);
  child_->render(ctx, ptrs_.data(), nch);
  for (uint32_t i = 0; i < ctx.frames; ++i) {
    const float g = gainAt(ctx.timelineFrame + i);
    for (uint32_t c = 0; c < nch; ++c) out[c][i] += ptrs_[c][i] * g;
  }
}

// ---- EventNode ----
EventNode::EventNode(std::vector<TimedMidi> events, ProcessorProto synth)
    : events_(std::move(events)), synthProto_(std::move(synth)) {
  std::stable_sort(events_.begin(), events_.end(), [](const TimedMidi& a, const TimedMidi& b) { return a.frame < b.frame; });
}
bool EventNode::prepare(double sr, uint32_t maxFrames, uint32_t channels) {
  synth_ = synthProto_ ? synthProto_->clone() : nullptr;
  if (synth_ && !synth_->prepare(sr, maxFrames, channels)) return false;
  blockEvents_.assign(std::min<size_t>(events_.size(), 4096) + 1, sq_midi_event{});
  maxFrames_ = maxFrames;
  allocScratch(scratch_, ptrs_, channels, maxFrames);
  expectNext_ = INT64_MIN;
  cursor_ = 0;
  return true;
}
void EventNode::reset() noexcept {
  if (synth_) synth_->reset();
  expectNext_ = INT64_MIN;
  cursor_ = 0;
}
int64_t EventNode::endFrame() const noexcept { return events_.empty() ? 0 : events_.back().frame + 1; }
void EventNode::render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept {
  if (!synth_) return;
  const int64_t t0 = ctx.timelineFrame;
  if (t0 != expectNext_) {
    synth_->reset();
    // first event at or after t0
    size_t lo = 0, hi = events_.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (events_[mid].frame < t0) lo = mid + 1;
      else hi = mid;
    }
    cursor_ = lo;
  }
  expectNext_ = t0 + ctx.frames;
  uint32_t n = 0;
  const size_t cap = blockEvents_.size();
  while (cursor_ < events_.size() && events_[cursor_].frame < t0 + ctx.frames) {
    const TimedMidi& e = events_[cursor_++];
    if (n >= cap) continue;  // more events than capacity in one block: dropped
    sq_midi_event& me = blockEvents_[n++];
    me.frame_offset = static_cast<uint32_t>(e.frame - t0);
    me.bytes[0] = e.bytes[0];
    me.bytes[1] = e.bytes[1];
    me.bytes[2] = e.bytes[2];
    me.bytes[3] = 0;
    me.size = e.size;
    me.data = nullptr;
  }
  const uint32_t nch = std::min<uint32_t>(channels, static_cast<uint32_t>(ptrs_.size()));
  for (uint32_t c = 0; c < nch; ++c) std::memset(ptrs_[c], 0, sizeof(float) * ctx.frames);
  ProcessContext sc = ctx;
  sc.midi = blockEvents_.data();
  sc.midiCount = n;
  synth_->process(sc, ptrs_.data(), nch);
  for (uint32_t c = 0; c < nch; ++c)
    for (uint32_t i = 0; i < ctx.frames; ++i) out[c][i] += ptrs_[c][i];
}

// ---- InputNode ----
void InputNode::render(const ProcessContext& ctx, float* const* out, uint32_t channels) noexcept {
  if (!ctx.input || channels_.empty()) return;
  for (uint32_t c = 0; c < channels; ++c) {
    const uint32_t src = channels_[c < channels_.size() ? c : channels_.size() - 1];
    if (src >= ctx.inputChannels || !ctx.input[src]) continue;
    for (uint32_t i = 0; i < ctx.frames; ++i) out[c][i] += ctx.input[src][i];
  }
}

}  // namespace sq::engine::audio
