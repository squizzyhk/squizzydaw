// Squizzy Studio engine/audio - PluginFx / PluginSynth (see plugin_fx.hpp).
#include "sq/engine/audio/plugin_fx.hpp"

#include "sq/cpp/bytestream.hpp"

#include <algorithm>
#include <cstring>

namespace sq::engine::audio {

// ---- PluginHostCore ----
PluginHostCore::~PluginHostCore() {
  // An editor still open on this instance is not closed here: whoever opened it holds its own
  // reference and closes it (the GUI's PluginEditorHost, at its next pump, outside any plugin call),
  // and the adapters close a view that is still open when they are destroyed. A graph can be retired
  // from a message loop the plugin runs inside one of its own editor calls (setEditorSize,
  // closeEditor), where closing that view would pull it out from under the running call.
  if (inst_ && active_) inst_->deactivate();
  inst_ = nullptr;
}

namespace {
std::string pluginIdOf(IPluginInstance* inst) {
  sq_plugin_desc d{};
  d.struct_size = sizeof d;
  if (!inst || inst->getDesc(&d) != SQ_OK) return {};
  d.id[sizeof d.id - 1] = 0;
  return d.id;
}
}  // namespace

void PluginHostCore::loadState(const PluginState& state) {
  if (!inst_ || state.empty()) return;
  if (!state.pluginId.empty() && pluginIdOf(inst_.get()) != state.pluginId) return;
  if (!state.bytes.empty()) {
    auto in = sq::makeMemoryStream(state.bytes.data(), state.bytes.size(), false);
    inst_->loadState(in.get());
  }
  // The document's "i=v" values win over the carried bytes (PluginState::params): main thread,
  // before activation, so no process() runs in between. parameter() reports them too.
  for (const auto& [id, v] : state.params) {
    inst_->setParamValue(id, v);
    const float f = static_cast<float>(v);
    bool known = false;
    for (uint32_t i = 0; i < valueCount_; ++i)
      if (values_[i].first == id) {
        values_[i].second = f;
        known = true;
      }
    if (!known && valueCount_ < kMaxParamChanges) values_[valueCount_++] = {id, f};
  }
}

bool PluginHostCore::saveState(PluginState* out) const {
  if (!inst_ || !out) return false;
  auto mem = sq::Ptr<sq::MemoryStream>::adopt(new sq::MemoryStream());
  if (SQ_FAILED(inst_->saveState(mem.get())) || mem->bytes().empty()) return false;
  out->pluginId = pluginIdOf(inst_.get());
  out->bytes = mem->bytes();
  return true;
}

void PluginHostCore::instantiate() {
  if (!factory_) {
    error_ = "no plugin factory";
    return;
  }
  std::string err;
  inst_ = factory_(&err);
  if (!inst_) error_ = err.empty() ? "plugin instantiation failed" : err;
}

bool PluginHostCore::activate(double sampleRate, uint32_t maxFrames) {
  if (!inst_) return false;  // a failed instance becomes a pass-through / silent synth
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
    return false;
  }
  active_ = true;
  maxFrames_ = maxFrames;
  latency_ = inst_->getLatencyFrames();
  const uint32_t t = inst_->getTailFrames();
  tail_ = t == UINT32_MAX ? static_cast<uint32_t>(sampleRate * 2) : t;
  auto makeBuses = [&](sq_bool input, std::vector<Bus>& buses, std::vector<sq_audio_block>& blocks) {
    const uint32_t n = inst_->getBusCount(input);
    buses.assign(n, Bus{});
    blocks.assign(n, sq_audio_block{});
    for (uint32_t b = 0; b < n; ++b) {
      sq_plugin_bus_info bi{};
      bi.struct_size = sizeof bi;
      const uint32_t ch = SQ_SUCCEEDED(inst_->getBusInfo(input, b, &bi)) ? bi.channels : 2;
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

void PluginHostCore::reset() noexcept {
  if (inst_ && active_) inst_->reset();
}

void PluginHostCore::setParameter(uint32_t id, float value) noexcept {
  bool known = false;
  for (uint32_t i = 0; i < valueCount_; ++i)
    if (values_[i].first == id) {
      values_[i].second = value;
      known = true;
    }
  if (!known && valueCount_ < kMaxParamChanges) values_[valueCount_++] = {id, value};
  for (uint32_t i = 0; i < pendingCount_; ++i)
    if (pending_[i].param_id == id) {
      pending_[i].value = value;
      return;
    }
  if (pendingCount_ >= kMaxParamChanges) return;  // more distinct changes than one block carries: dropped
  sq_param_change& c = pending_[pendingCount_++];
  c.param_id = id;
  c.frame_offset = 0;
  c.value = value;
  c.flags = 0;
}

float PluginHostCore::parameter(uint32_t id) const noexcept {
  for (uint32_t i = 0; i < valueCount_; ++i)
    if (values_[i].first == id) return values_[i].second;
  return 0.0f;
}

bool PluginHostCore::run(const ProcessContext& ctx, const float* const* in, uint32_t inChannels) noexcept {
  if (!inst_ || !active_ || outs_.empty() || ctx.frames == 0 || ctx.frames > maxFrames_) return false;
  const uint32_t n = ctx.frames;
  // Main input bus <- track buffers (or silence); aux buses stay silent.
  if (!ins_.empty()) {
    Bus& b = ins_[0];
    for (uint32_t c = 0; c < b.channels; ++c) {
      if (in && inChannels) std::memcpy(b.ptrs[c], in[std::min(c, inChannels - 1)], sizeof(float) * n);
      else std::memset(b.ptrs[c], 0, sizeof(float) * n);
    }
  }
  for (auto& blk : inBlocks_) {
    blk.frames = n;
    blk.position = ctx.outputFrame;
  }
  for (auto& blk : outBlocks_) {
    blk.frames = n;
    blk.position = ctx.outputFrame;
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
  pr.midi_in = ctx.midiCount ? ctx.midi : nullptr;
  pr.midi_in_count = ctx.midiCount;
  pr.params_in = pendingCount_ ? pending_ : nullptr;
  pr.params_in_count = pendingCount_;
  pr.transport = &tr;
  const sq_result r = inst_->process(&pr);
  pendingCount_ = 0;
  return SQ_SUCCEEDED(r);
}

const float* const* PluginHostCore::output(uint32_t* channels) const noexcept {
  if (outs_.empty() || outs_[0].channels == 0) {
    *channels = 0;
    return nullptr;
  }
  *channels = outs_[0].channels;
  return outs_[0].ptrs.data();
}

// ---- PluginFx ----
PluginFx::PluginFx(PluginFactory factory, std::string name) : core_(std::move(factory), std::move(name)) {}
PluginFx::~PluginFx() = default;

ProcessorPtr PluginFx::clone() const {
  auto p = std::make_unique<PluginFx>(core_.factory(), core_.name());
  p->core_.instantiate();
  return p;
}

bool PluginFx::prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) {
  (void)channels;
  core_.activate(sampleRate, maxFrames);
  return true;  // a failed instance becomes a pass-through (the graph still plays)
}

void PluginFx::reset() noexcept { core_.reset(); }

void PluginFx::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  if (channels == 0) return;
  ProcessContext c = ctx;
  c.midi = nullptr;
  c.midiCount = 0;
  if (!core_.run(c, io, channels)) return;  // leave the dry signal on failure
  uint32_t och = 0;
  const float* const* out = core_.output(&och);
  if (!out) return;
  for (uint32_t ch = 0; ch < channels; ++ch) std::memcpy(io[ch], out[std::min(ch, och - 1)], sizeof(float) * ctx.frames);
}

// ---- PluginSynth ----
PluginSynth::PluginSynth(PluginFactory factory, std::string name) : core_(std::move(factory), std::move(name)) {}
PluginSynth::~PluginSynth() = default;

ProcessorPtr PluginSynth::clone() const {
  auto p = std::make_unique<PluginSynth>(core_.factory(), core_.name());
  p->core_.instantiate();
  return p;
}

bool PluginSynth::prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) {
  (void)channels;
  core_.activate(sampleRate, maxFrames);
  return true;  // a failed instance stays silent
}

void PluginSynth::reset() noexcept { core_.reset(); }

void PluginSynth::process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept {
  if (channels == 0 || !core_.run(ctx, nullptr, 0)) return;
  uint32_t och = 0;
  const float* const* out = core_.output(&och);
  if (!out) return;
  for (uint32_t ch = 0; ch < channels; ++ch) {
    const float* s = out[std::min(ch, och - 1)];
    float* d = io[ch];
    for (uint32_t i = 0; i < ctx.frames; ++i) d[i] += s[i];
  }
}

}  // namespace sq::engine::audio
