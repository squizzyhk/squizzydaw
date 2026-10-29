// Squizzy Studio engine/audio - plugin inserts: IPluginInstance (pluginhost.h) as an FxProcessor.
//
// Document model: a track's insert chain is the user property "user.inserts" (string), one insert
// spec per line. The engine does not interpret specs itself; the host installs an InsertResolver
// (AudioEngine::setInsertResolver) that turns a spec into a processor prototype. The app's resolver
// understands  "<format>|<plugin id>|<path>[|<paramId>=<plain value>]..."  (see app/shell.cpp).
//
// PluginFx
//   - The prototype holds a factory (creates + configures a fresh IPluginInstance). Every compiled
//     graph clones its own processor, and clone() instantiates a new plugin instance on the
//     compiling thread, which is always the MAIN thread (pluginhost.h): the live graph is compiled
//     there, and AudioEngine::startRender compiles offline-render graphs there too.
//   - prepare() activates the instance (sample rate, max block) and allocates every buffer; process()
//     is RT safe (no allocation, no locks) and calls IPluginInstance::process with the track buffers
//     as the main bus and silent aux (sidechain) buses. Mono tracks feed both plugin inputs and take
//     the first output channel back.
//   - latency() = the plugin's reported latency after activation (PDC uses it).
//   - The instance is deactivated and released when the processor dies (control thread: graphs are
//     destroyed by collectGarbage / AudioEngine::idle on the main thread; the render worker only
//     calls process()/reset()).
//   - Live parameters: setParameter(id, value) takes the plugin's PARAMETER ID as the index (the
//     spec grammar writes "|<paramId>=<value>"); changes are queued RT-safely and handed to the
//     plugin as sq_plugin_process::params_in at the next process(). parameter(id) returns the last
//     value set through the engine (or the spec value), parameterCount() is 0 (unknown to the engine).
//
// PluginSynth (instrument tracks, spec in track property "user.instrument")
//   Same lifetime/threading as PluginFx. process() sends ctx.midi to the plugin's event input
//   (midi_in), feeds silence to its audio inputs and ADDS its main output into the track buffer.
//
// InstrumentResolver (AudioEngine::setInstrumentResolver): same signature as InsertResolver,
// turns a "user.instrument" spec into a SynthProcessor prototype.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/interfaces/pluginhost.h"
#include "sq/engine/audio/processor.hpp"

namespace sq::engine::audio {

using PluginFactory = std::function<sq::Ptr<IPluginInstance>(std::string* error)>;

// Returns a processor prototype for one insert spec, or nullptr + error.
using InsertResolver = std::function<ProcessorProto(const std::string& spec, std::string* error)>;
using InstrumentResolver = InsertResolver;

// Shared plugin-instance plumbing of PluginFx / PluginSynth.
class PluginHostCore {
 public:
  static constexpr uint32_t kMaxParamChanges = 256;
  PluginHostCore(PluginFactory factory, std::string name) : factory_(std::move(factory)), name_(std::move(name)) {}
  ~PluginHostCore();
  void instantiate();  // clone(): new instance from the factory (main thread)
  bool activate(double sampleRate, uint32_t maxFrames);
  void reset() noexcept;
  // RT: run one block. in = main input channels (null = silence), midi optional.
  bool run(const ProcessContext& ctx, const float* const* in, uint32_t inChannels) noexcept;
  const float* const* output(uint32_t* channels) const noexcept;
  void setParameter(uint32_t id, float value) noexcept;
  float parameter(uint32_t id) const noexcept;
  IPluginInstance* instance() const noexcept { return inst_.get(); }
  // Main thread, before activate(): IPluginInstance::loadState when the plugin id matches.
  void loadState(const PluginState& state);
  // Main thread: captures the instance state (false when there is no instance / no state).
  bool saveState(PluginState* out) const;
  const PluginFactory& factory() const noexcept { return factory_; }
  const std::string& name() const noexcept { return name_; }
  const std::string& error() const noexcept { return error_; }
  uint32_t latency() const noexcept { return latency_; }
  uint32_t tail() const noexcept { return tail_; }
  uint32_t maxFrames() const noexcept { return maxFrames_; }
  bool active() const noexcept { return inst_ && active_; }

 private:
  struct Bus {
    uint32_t channels = 0;
    std::vector<float> data;
    std::vector<float*> ptrs;
  };
  PluginFactory factory_;
  std::string name_;
  std::string error_;
  sq::Ptr<IPluginInstance> inst_;
  bool active_ = false;
  uint32_t latency_ = 0, tail_ = 0, maxFrames_ = 0;
  std::vector<Bus> ins_, outs_;
  std::vector<sq_audio_block> inBlocks_, outBlocks_;
  sq_param_change pending_[kMaxParamChanges] = {};
  uint32_t pendingCount_ = 0;
  std::pair<uint32_t, float> values_[kMaxParamChanges] = {};  // last values set (parameter())
  uint32_t valueCount_ = 0;
};

class PluginFx final : public FxProcessor {
 public:
  // Prototype constructor: no instance yet; clone() creates one.
  explicit PluginFx(PluginFactory factory, std::string name = {});
  ~PluginFx() override;

  ProcessorPtr clone() const override;
  bool prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return core_.latency(); }
  uint32_t tail() const noexcept override { return core_.tail(); }
  void setParameter(uint32_t id, float value) noexcept override { core_.setParameter(id, value); }
  float parameter(uint32_t id) const noexcept override { return core_.parameter(id); }

  IPluginInstance* instance() const noexcept { return core_.instance(); }
  IPluginInstance* pluginInstance() const noexcept override { return core_.instance(); }
  void loadInitialState(const PluginState& state) override { core_.loadState(state); }
  const std::string& name() const noexcept { return core_.name(); }
  const std::string& error() const noexcept { return core_.error(); }

 private:
  PluginHostCore core_;
};

class PluginSynth final : public SynthProcessor {
 public:
  explicit PluginSynth(PluginFactory factory, std::string name = {});
  ~PluginSynth() override;

  ProcessorPtr clone() const override;
  bool prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return core_.latency(); }
  uint32_t tail() const noexcept override { return core_.tail(); }
  void setParameter(uint32_t id, float value) noexcept override { core_.setParameter(id, value); }
  float parameter(uint32_t id) const noexcept override { return core_.parameter(id); }

  IPluginInstance* instance() const noexcept { return core_.instance(); }
  IPluginInstance* pluginInstance() const noexcept override { return core_.instance(); }
  void loadInitialState(const PluginState& state) override { core_.loadState(state); }
  const std::string& name() const noexcept { return core_.name(); }
  const std::string& error() const noexcept { return core_.error(); }

 private:
  PluginHostCore core_;
};

}  // namespace sq::engine::audio
