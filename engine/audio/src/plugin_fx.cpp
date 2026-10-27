// Squizzy Studio engine/audio - PluginFx (see plugin_fx.hpp).
#include "sq/engine/audio/plugin_fx.hpp"

#include <algorithm>
#include <cstring>

namespace sq::engine::audio {

PluginFx::PluginFx(PluginFactory factory, std::string name) : factory_(std::move(factory)), name_(std::move(name)) {}

PluginFx::~PluginFx() {
  if (inst_ && active_) inst_->deactivate();
  inst_ = nullptr;
}

ProcessorPtr PluginFx::clone() const {
  auto p = std::make_unique<PluginFx>(factory_, name_);
  if (factory_) {
    std::string err;
    p->inst_ = factory_(&err);
    if (!p->inst_) p->error_ = err.empty() ? "plugin instantiation failed" : err;
  } else {
    p->error_ = "no plugin factory";
  }
  return p;
}

bool PluginFx::prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) {
  (void)channels;
  if (!inst_) return true;  // a failed instance becomes a pass-through (the graph still plays)
  if (active_) {
    inst_->deactivate();
    active_ = false;
  }
  sq_plugin_setup setup{};
  setup.struct_size = sizeof setup;
  setup.sample_rate = sampleRate;
  setup.min_block_frames = 1;
  setup.max_block_frames = maxFrames;
  if (SQ_FAILED(inst_->activate(&setup))) {
    error_ = "activate failed";
    inst_ = nullptr;
    return true;
  }
  active_ = true;
  maxFrames_ = maxFrames;
  latency_ = inst_->getLatencyFrames();
  uint32_t t = inst_->getTailFrames();
  tail_ = t == UINT32_MAX ? static_cast<uint32_t>(sampleRate * 2) : t;
  auto makeBuses = [&](sq_bool input, std::vector<Bus>& buses, std::vector<sq_audio_block>& blocks) {
    uint32_t n = inst_->getBusCount(input);
    buses.assign(n, Bus{});
    blocks.assign(n, sq_audio_block{});
    for (uint32_t b = 0; b < n; ++b) {
      sq_plugin_bus_info bi{};
      bi.struct_size = sizeof bi;
      uint32_t ch = SQ_SUCCEEDED(inst_->getBusInfo(input, b, &bi)) ? bi.channels : 2;
      Bus& bus = buses[b];
      bus.channels = ch;
      bus.data.assign(static_cast<size_t>(ch) * maxFrames, 0.0f);
      bus.ptrs.resize(ch);
      for (uint32_t c = 0; c < ch; ++c) bus.ptrs[c] = bus.data.data() + static_cast<size_t>(c) * maxFrames;
      blocks[b] = sq_audio_block{bus.ptrs.data(), ch, maxFrames, 0};
    }
  };
  makeBuses(SQ_TRUE, ins_, inBlocks_);
  makeBuses(SQ_FALSE, outs_, outBlocks_);
  return true;
}

void PluginFx::reset() noexcept {
  if (inst_ && active_) inst_->reset();
}

void PluginFx::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  if (!inst_ || !active_ || outs_.empty() || ctx.frames == 0 || ctx.frames > maxFrames_ || channels == 0) return;
  const uint32_t n = ctx.frames;
  // Main input bus <- track buffers; aux buses stay silent.
  if (!ins_.empty()) {
    Bus& in = ins_[0];
    for (uint32_t c = 0; c < in.channels; ++c) std::memcpy(in.ptrs[c], io[std::min(c, channels - 1)], sizeof(float) * n);
  }
  for (size_t b = 0; b < inBlocks_.size(); ++b) {
    inBlocks_[b].frames = n;
    inBlocks_[b].position = ctx.outputFrame;
  }
  for (auto& b : outBlocks_) {
    b.frames = n;
    b.position = ctx.outputFrame;
  }
  sq_plugin_transport tr{};
  tr.struct_size = sizeof tr;
  tr.flags = ctx.playing ? 0x01u : 0u;  // SQ_TRANSPORT_PLAYING
  tr.frames = ctx.outputFrame;
  tr.numerator = 4;
  tr.denominator = 4;
  sq_plugin_process pr{};
  pr.struct_size = sizeof pr;
  pr.frames = n;
  pr.inputs = inBlocks_.empty() ? nullptr : inBlocks_.data();
  pr.input_bus_count = static_cast<uint32_t>(inBlocks_.size());
  pr.outputs = outBlocks_.data();
  pr.output_bus_count = static_cast<uint32_t>(outBlocks_.size());
  pr.transport = &tr;
  if (SQ_FAILED(inst_->process(&pr))) return;  // leave the dry signal on failure
  const Bus& out = outs_[0];
  if (out.channels == 0) return;
  for (uint32_t c = 0; c < channels; ++c) std::memcpy(io[c], out.ptrs[std::min(c, out.channels - 1)], sizeof(float) * n);
}

}  // namespace sq::engine::audio
