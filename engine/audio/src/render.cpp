// Squizzy Studio engine/audio - offline rendering + render tasks.
#include "sq/engine/audio/render.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>

#include "sq/platform.h"

namespace sq::engine::audio {

namespace {

std::condition_variable& taskCv() {
  static std::condition_variable cv;
  return cv;
}
std::mutex& taskCvMutex() {
  static std::mutex m;
  return m;
}

using BlockFn = std::function<sq_result(Graph& g, float* const* master, uint32_t skip, uint32_t use)>;

GraphConfig renderConfig(const RenderJob& job) {
  GraphConfig cfg;
  cfg.maxBlock = job.blockFrames;
  cfg.outputChannels = job.outputChannels;
  cfg.mix64 = job.mix64;
  cfg.stemTaps = job.kind == SQ_RENDER_STEMS || job.kind == SQ_RENDER_BOUNCE_IN_PLACE;
  cfg.stemPreFader = job.kind == SQ_RENDER_BOUNCE_IN_PLACE || (job.flags & SQ_RENDER_PRE_FADER) != 0;
  return cfg;
}

sq_result runRender(Graph& graphRef, const RenderJob& job, const BlockFn& onBlock, const std::function<void(double)>& progress,
                    const std::atomic<bool>* cancel) {
  if (job.end <= job.start || job.blockFrames == 0 || job.outputChannels == 0) return SQ_E_INVALIDARG;
  Graph* graph = &graphRef;
  if (graph->maxBlock() < job.blockFrames) return SQ_E_INVALIDARG;
  for (int32_t t : job.tracks)
    if (t < 0 || static_cast<size_t>(t) >= graph->trackCount()) return SQ_E_INVALIDARG;

  std::vector<float> mbuf(static_cast<size_t>(job.outputChannels) * job.blockFrames, 0.0f);
  std::vector<float*> mptr(job.outputChannels);
  for (uint32_t c = 0; c < job.outputChannels; ++c) mptr[c] = &mbuf[static_cast<size_t>(c) * job.blockFrames];

  const int64_t total = (job.end - job.start) + static_cast<int64_t>(job.tailFrames);
  int64_t discard = graph->latency();
  int64_t pos = job.start - discard;
  int64_t written = 0;
  const bool click = job.kind == kRenderClick;
  while (written < total) {
    if (cancel && cancel->load()) return SQ_E_CLOSED;
    const int64_t want = discard + (total - written);
    const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(job.blockFrames, want));
    ProcessContext ctx;
    ctx.sampleRate = graph->sampleRate();
    ctx.frames = n;
    ctx.outputFrame = pos;
    ctx.playing = true;
    {
      rt::Scope scope;
      graph->process(ctx, mptr.data(), job.outputChannels, false, click);
    }
    const uint32_t skip = static_cast<uint32_t>(std::min<int64_t>(discard, n));
    discard -= skip;
    const uint32_t useFrames = n - skip;
    if (useFrames > 0) {
      const sq_result r = onBlock(*graph, mptr.data(), skip, useFrames);
      if (SQ_FAILED(r)) return r;
    }
    written += useFrames;
    pos += n;
    if (progress) progress(static_cast<double>(written) / static_cast<double>(total));
  }
  return SQ_OK;
}

}  // namespace

std::unique_ptr<Graph> compileRenderGraph(const GraphDesc& desc, const RenderJob& job, std::string* error) {
  if (job.blockFrames == 0 || job.outputChannels == 0) {
    if (error) *error = "invalid render job";
    return nullptr;
  }
  const GraphConfig cfg = renderConfig(job);
  std::string err;
  std::unique_ptr<Graph> g;
  if (job.kind == kRenderClick) {
    GraphDesc clickDesc;
    clickDesc.sampleRate = desc.sampleRate;
    clickDesc.tempo = desc.tempo;
    clickDesc.numerator = desc.numerator;
    clickDesc.denominator = desc.denominator;
    g = Graph::compile(clickDesc, cfg, &err);
  } else {
    g = Graph::compile(desc, cfg, &err);
  }
  if (!g && error) *error = err;
  return g;
}

sq_result compileRenderGraphs(const GraphDesc& desc, const RenderJob& job, OfflineGraphs* out) {
  if (!out) return SQ_E_INVALIDARG;
  out->main = compileRenderGraph(desc, job);
  if (!out->main) return SQ_E_INVALIDARG;
  if (job.flags & SQ_RENDER_NORMALIZE) {
    out->analysis = compileRenderGraph(desc, job);  // own instances: the passes start from silence
    if (!out->analysis) {
      out->main.reset();
      return SQ_E_INVALIDARG;
    }
  }
  return SQ_OK;
}

sq_result renderOffline(OfflineGraphs& graphs, const RenderJob& job, std::vector<RenderOutput>& outputs,
                        const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
  if (!graphs.main) return SQ_E_INVALIDARG;
  std::vector<float> gains(outputs.size(), 1.0f);
  if (job.flags & SQ_RENDER_NORMALIZE) {
    if (!graphs.analysis) return SQ_E_INVALIDARG;
    // analysis pass: peak per output
    std::vector<float> peaks(outputs.size(), 0.0f);
    sq_result r = runRender(
        *graphs.analysis, job,
        [&](Graph& g, float* const* master, uint32_t skip, uint32_t use) -> sq_result {
          for (size_t o = 0; o < outputs.size(); ++o) {
            const RenderOutput& ro = outputs[o];
            float* const* src = ro.track < 0 ? master : g.stem(static_cast<size_t>(ro.track));
            const uint32_t srcCh = ro.track < 0 ? job.outputChannels : g.stemChannels(static_cast<size_t>(ro.track));
            for (uint32_t c = 0; c < ro.channels && c < srcCh; ++c)
              for (uint32_t i = 0; i < use; ++i) peaks[o] = std::max(peaks[o], std::fabs(src[c][skip + i]));
          }
          return SQ_OK;
        },
        nullptr, cancel);
    if (SQ_FAILED(r)) return r;
    for (size_t o = 0; o < outputs.size(); ++o) gains[o] = peaks[o] > 0.0f ? 1.0f / peaks[o] : 1.0f;
  }
  std::vector<float> scaled;
  return runRender(
      *graphs.main, job,
      [&](Graph& g, float* const* master, uint32_t skip, uint32_t use) -> sq_result {
        for (size_t o = 0; o < outputs.size(); ++o) {
          RenderOutput& ro = outputs[o];
          if (!ro.writer) continue;
          float* const* src = ro.track < 0 ? master : g.stem(static_cast<size_t>(ro.track));
          const uint32_t srcCh = ro.track < 0 ? job.outputChannels : g.stemChannels(static_cast<size_t>(ro.track));
          float* ptrs[kMaxChannels];
          if (gains[o] != 1.0f) {
            scaled.resize(static_cast<size_t>(ro.channels) * use);
            for (uint32_t c = 0; c < ro.channels; ++c) {
              ptrs[c] = &scaled[static_cast<size_t>(c) * use];
              const float* s = src[c < srcCh ? c : srcCh - 1];
              for (uint32_t i = 0; i < use; ++i) ptrs[c][i] = s[skip + i] * gains[o];
            }
          } else {
            for (uint32_t c = 0; c < ro.channels; ++c) ptrs[c] = src[c < srcCh ? c : srcCh - 1] + skip;
          }
          sq_audio_block blk{ptrs, ro.channels, use, 0};
          const sq_result r = ro.writer->write(&blk);
          if (SQ_FAILED(r)) return SQ_E_FAIL;
        }
        return SQ_OK;
      },
      progress, cancel);
}

sq_result renderOffline(const GraphDesc& desc, const RenderJob& job, std::vector<RenderOutput>& outputs,
                        const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
  if (job.end <= job.start || job.blockFrames == 0 || job.outputChannels == 0) return SQ_E_INVALIDARG;
  OfflineGraphs graphs;
  const sq_result r = compileRenderGraphs(desc, job, &graphs);
  if (SQ_FAILED(r)) return r;
  return renderOffline(graphs, job, outputs, progress, cancel);
}

sq_result renderToMemory(const GraphDesc& desc, const RenderJob& job, std::vector<std::vector<float>>* master,
                         std::vector<std::vector<std::vector<float>>>* stems) {
  if (master) master->assign(job.outputChannels, {});
  if (stems) stems->clear();
  if (job.end <= job.start || job.blockFrames == 0 || job.outputChannels == 0) return SQ_E_INVALIDARG;
  std::unique_ptr<Graph> graph = compileRenderGraph(desc, job);
  if (!graph) return SQ_E_INVALIDARG;
  return runRender(
      *graph, job,
      [&](Graph& g, float* const* m, uint32_t skip, uint32_t use) -> sq_result {
        if (master)
          for (uint32_t c = 0; c < job.outputChannels; ++c) (*master)[c].insert((*master)[c].end(), m[c] + skip, m[c] + skip + use);
        if (stems && (job.kind == SQ_RENDER_STEMS || job.kind == SQ_RENDER_BOUNCE_IN_PLACE)) {
          if (stems->empty()) {
            stems->resize(g.trackCount());
            for (size_t t = 0; t < g.trackCount(); ++t) (*stems)[t].resize(g.stemChannels(t));
          }
          for (size_t t = 0; t < g.trackCount(); ++t)
            for (uint32_t c = 0; c < g.stemChannels(t); ++c)
              (*stems)[t][c].insert((*stems)[t][c].end(), g.stem(t)[c] + skip, g.stem(t)[c] + skip + use);
        }
        return SQ_OK;
      },
      nullptr, nullptr);
}

sq_result openRenderWriter(IClassRegistry* reg, const std::string& path, const std::string& extension, double sampleRate,
                           uint32_t channels, sq_sample_format format, IAttributes* options, sq::Ptr<IAudioFileWriter>* out) {
  if (!reg || !out) return SQ_E_INVALIDARG;
  sq::Ptr<ICodecService> codec = sq::Ptr<ICodecService>::adopt(codec::findCodec(reg, extension.c_str(), nullptr, SQ_CODEC_WRITE));
  if (!codec) return SQ_E_UNSUPPORTED;
  sq::Ptr<IAudioFileWriter> w;
  sq_result r = codec->createWriter(extension.c_str(), w.put());
  if (SQ_FAILED(r)) return r;
  sq_audio_file_format fmt{};
  fmt.struct_size = sizeof fmt;
  fmt.sample_rate = sampleRate;
  fmt.channels = channels;
  fmt.sample_format = format == SQ_SAMPLE_UNKNOWN ? SQ_SAMPLE_F32 : format;
  switch (fmt.sample_format) {
    case SQ_SAMPLE_I16: fmt.bits_per_sample = 16; break;
    case SQ_SAMPLE_I24: case SQ_SAMPLE_I24_IN32: fmt.bits_per_sample = 24; break;
    case SQ_SAMPLE_F64: fmt.bits_per_sample = 64; break;
    default: fmt.bits_per_sample = 32; break;
  }
  fmt.frame_count = -1;
  const size_t n = std::min(extension.size(), sizeof fmt.container - 1);
  std::memcpy(fmt.container, extension.data(), n);
  r = w->open(path.c_str(), &fmt, options);
  if (SQ_FAILED(r)) return r;
  *out = std::move(w);
  return SQ_OK;
}

// ---- RenderTask ----
RenderTask::RenderTask(std::function<sq_result(RenderTask&)> work, sq::Ptr<IRenderObserver> observer, bool needsMainThread)
    : work_(std::move(work)), observer_(std::move(observer)), needsMain_(needsMainThread) {}

RenderTask::~RenderTask() = default;

void RenderTask::startWorker() {
  addRef();  // held by the worker until it finishes
  std::thread([this] {
    sq_thread_set_current_name("RenderTask");
    sq_result r = SQ_E_FAIL;
    try {
      r = work_(*this);
    } catch (...) {
      r = SQ_E_FAIL;
    }
    if (cancel_.load() && SQ_SUCCEEDED(r)) r = SQ_E_CLOSED;
    workResult_.store(r);
    if (needsMain_ && SQ_SUCCEEDED(r)) {
      awaitingMain_.store(true);
    } else {
      finish(r);
    }
    release();
  }).detach();
}

void RenderTask::addOutputPath(const std::string& p) {
  std::lock_guard<std::mutex> lk(m_);
  paths_.push_back(p);
}

void RenderTask::reportProgress(double p) {
  progress_.store(p);
  if (observer_) observer_->onRenderProgress(this, p);
}

void RenderTask::completeOnMainThread(sq_result result) {
  awaitingMain_.store(false);
  finish(result);
}

std::vector<std::string> RenderTask::outputPaths() const {
  std::lock_guard<std::mutex> lk(m_);
  return paths_;
}

void RenderTask::finish(sq_result r) {
  if (SQ_SUCCEEDED(r)) progress_.store(1.0);
  if (observer_) observer_->onRenderFinished(this, r);
  observer_ = nullptr;  // audioengine.h: the observer is add-ref'd only until the task finished
  {
    std::lock_guard<std::mutex> lk(taskCvMutex());
    result_.store(r);
    done_.store(true);
  }
  taskCv().notify_all();
}

sq_result RenderTask::cancel() {
  cancel_.store(true);
  return SQ_OK;
}

sq_result RenderTask::wait(uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lk(taskCvMutex());
  if (done_.load()) return SQ_OK;
  if (timeoutMs == 0) return SQ_E_TIMEOUT;
  if (taskCv().wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return done_.load(); })) return SQ_OK;
  return SQ_E_TIMEOUT;
}

sq_result RenderTask::getResult() { return done_.load() ? result_.load() : SQ_E_BUSY; }

uint32_t RenderTask::getOutputCount() {
  std::lock_guard<std::mutex> lk(m_);
  return static_cast<uint32_t>(paths_.size());
}

sq_result RenderTask::getOutputPath(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) {
  std::lock_guard<std::mutex> lk(m_);
  if (index >= paths_.size()) return SQ_E_NOTFOUND;
  const std::string& p = paths_[index];
  const uint32_t need = static_cast<uint32_t>(p.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, p.c_str(), need);
  return SQ_OK;
}

}  // namespace sq::engine::audio
