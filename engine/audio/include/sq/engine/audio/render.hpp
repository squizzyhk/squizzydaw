// Squizzy Studio engine/audio - offline render tasks (mixdown, stems, click, bounce-in-place).
//
// Every task gets its OWN Graph compiled from a GraphDesc snapshot (on the main thread, see
// OfflineGraphs) and runs it on a worker thread as fast as the CPU allows (offline, faster than
// realtime); the live graph is never touched.
// Alignment: the graph is started `latency()` frames before the range and that pre-roll is
// discarded, so file frame 0 == timeline frame `start` regardless of plugin delay.
// Blocks run inside rt::Scope like the live audio thread (same RT rules, testable).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/interfaces/audioengine.h"
#include "sq/interfaces/codec.h"
#include "sq/engine/audio/graph.hpp"

namespace sq::engine::audio {

// Engine-private render kind (not in audioengine.h): metronome only, to one file.
inline constexpr int32_t kRenderClick = 100;

struct RenderJob {
  int32_t kind = SQ_RENDER_MIXDOWN;
  uint32_t flags = 0;               // SQ_RENDER_*
  int64_t start = 0;                // timeline frames
  int64_t end = 0;
  uint32_t tailFrames = 0;
  std::vector<int32_t> tracks;      // stems / bounce: track indices (empty = all, stems)
  uint32_t blockFrames = 1024;
  bool mix64 = false;
  uint32_t outputChannels = 2;
};

// One destination: a writer receiving `channels` channels.
struct RenderOutput {
  int32_t track = -1;               // -1 = master / click
  uint32_t channels = 2;
  std::string path;
  sq::Ptr<IAudioFileWriter> writer; // opened, not finalized
};

// Threading (pluginhost.h: plugin instances are created, activated, deactivated and destroyed on
// the MAIN thread only): a Graph's plugin inserts are instantiated by Graph::compile and released
// by ~Graph, so the engine compiles offline graphs on the main thread (compileRenderGraphs), runs
// only process()/reset() on the render worker, and destroys them again on the main thread
// (AudioEngine::idle). The GraphDesc overloads below compile on the calling thread: use them only
// where that thread may own plugin instances (tests, tools without a separate main thread).
struct OfflineGraphs {
  std::unique_ptr<Graph> main;      // the rendering pass
  std::unique_ptr<Graph> analysis;  // SQ_RENDER_NORMALIZE: the peak-analysis pass (own instances)
};
std::unique_ptr<Graph> compileRenderGraph(const GraphDesc& desc, const RenderJob& job, std::string* error = nullptr);
sq_result compileRenderGraphs(const GraphDesc& desc, const RenderJob& job, OfflineGraphs* out);
// Runs precompiled graphs (any thread; no graph is created or destroyed here).
sq_result renderOffline(OfflineGraphs& graphs, const RenderJob& job, std::vector<RenderOutput>& outputs,
                        const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Runs the render loop synchronously on the calling thread. progress(0..1) may be null.
// cancel (nullable) is polled once per block. Writers are written but NOT finalized.
// SQ_E_FAIL if a writer fails, SQ_E_CLOSED if cancelled.
sq_result renderOffline(const GraphDesc& desc, const RenderJob& job, std::vector<RenderOutput>& outputs,
                        const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Same loop into memory (planar). Used by tests and by normalization's analysis pass.
sq_result renderToMemory(const GraphDesc& desc, const RenderJob& job, std::vector<std::vector<float>>* master,
                         std::vector<std::vector<std::vector<float>>>* stems = nullptr);

class RenderTask final : public sq::Object<RenderTask, IRenderTask> {
 public:
  // work runs on the worker thread and returns the result; outputs are published by it via
  // addOutputPath(). If needsMainThread, the task stays "running" after work() until
  // completeOnMainThread() is called (bounce-in-place applies its document edit there).
  RenderTask(std::function<sq_result(RenderTask&)> work, sq::Ptr<IRenderObserver> observer, bool needsMainThread = false);
  ~RenderTask() override;

  void startWorker();
  void addOutputPath(const std::string& p);
  void reportProgress(double p);
  const std::atomic<bool>& cancelFlag() const noexcept { return cancel_; }
  bool awaitingMainThread() const noexcept { return awaitingMain_.load(); }
  sq_result workResult() const noexcept { return workResult_.load(); }
  // Main thread: finish a needsMainThread task with `result`.
  void completeOnMainThread(sq_result result);
  std::vector<std::string> outputPaths() const;
  void* userData = nullptr;           // engine bookkeeping (bounce plan)

  // ---- IRenderTask ----
  double SQ_CALL getProgress() override { return progress_.load(); }
  sq_result SQ_CALL cancel() override;
  sq_result SQ_CALL wait(uint32_t timeoutMs) override;
  sq_result SQ_CALL getResult() override;
  uint32_t SQ_CALL getOutputCount() override;
  sq_result SQ_CALL getOutputPath(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override;

 private:
  void finish(sq_result r);
  std::function<sq_result(RenderTask&)> work_;
  sq::Ptr<IRenderObserver> observer_;
  bool needsMain_;
  std::thread thread_;
  std::atomic<double> progress_{0.0};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> done_{false};
  std::atomic<bool> awaitingMain_{false};
  std::atomic<sq_result> result_{SQ_E_BUSY};
  std::atomic<sq_result> workResult_{SQ_E_BUSY};
  mutable std::mutex m_;
  std::vector<std::string> paths_;
};

// Opens a writer for `path` through the codec registry (extension picks the codec).
sq_result openRenderWriter(IClassRegistry* reg, const std::string& path, const std::string& extension, double sampleRate,
                           uint32_t channels, sq_sample_format format, IAttributes* options, sq::Ptr<IAudioFileWriter>* out);

}  // namespace sq::engine::audio
