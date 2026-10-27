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

class PluginFx final : public FxProcessor {
 public:
  // Prototype constructor: no instance yet; clone() creates one.
  explicit PluginFx(PluginFactory factory, std::string name = {});
  ~PluginFx() override;

  ProcessorPtr clone() const override;
  bool prepare(double sampleRate, uint32_t maxFrames, uint32_t channels) override;
  void reset() noexcept override;
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override;
  uint32_t latency() const noexcept override { return latency_; }
  uint32_t tail() const noexcept override { return tail_; }

  IPluginInstance* instance() const noexcept { return inst_.get(); }
  const std::string& name() const noexcept { return name_; }
  const std::string& error() const noexcept { return error_; }

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
};

}  // namespace sq::engine::audio
