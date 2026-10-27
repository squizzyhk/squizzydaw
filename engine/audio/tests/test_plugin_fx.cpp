// engine/audio - PluginFx: IPluginInstance as an insert processor (fake plugin, no module).
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/plugin_fx.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

std::atomic<int> g_alive{0}, g_active{0};
// Thread probe: lifecycle calls (create/activate/deactivate/destroy) must stay on g_main.
std::atomic<bool> g_checkThreads{false};
std::thread::id g_main;
std::atomic<int> g_lifecycleOffMain{0}, g_processOffMain{0};
void lifecycleCall() {
  if (g_checkThreads.load() && std::this_thread::get_id() != g_main) ++g_lifecycleOffMain;
}

// Stereo gain x0.5 with 32 frames of reported latency, a stereo sidechain bus that must arrive
// silent, and a fixed tail.
class FakeGain final : public sq::Object<FakeGain, sq::IPluginInstance> {
 public:
  FakeGain() {
    lifecycleCall();
    ++g_alive;
  }
  ~FakeGain() override {
    lifecycleCall();
    --g_alive;
  }
  bool sidechainSilent = true;
  sq_result SQ_CALL getDesc(sq_plugin_desc*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getBusCount(sq_bool input) override { return input ? 2 : 1; }
  sq_result SQ_CALL getBusInfo(sq_bool, uint32_t, sq_plugin_bus_info* out) override {
    out->channels = 2;
    return SQ_OK;
  }
  sq_result SQ_CALL setBusActive(sq_bool, uint32_t, sq_bool) override { return SQ_OK; }
  sq_result SQ_CALL activate(const sq_plugin_setup* s) override {
    if (!s || s->max_block_frames == 0) return SQ_E_INVALIDARG;
    lifecycleCall();
    ++g_active;
    return SQ_OK;
  }
  sq_result SQ_CALL deactivate() override {
    lifecycleCall();
    --g_active;
    return SQ_OK;
  }
  sq_result SQ_CALL process(sq_plugin_process* d) override {
    if (d->input_bus_count != 2 || d->output_bus_count != 1) return SQ_E_INVALIDARG;
    if (g_checkThreads.load() && std::this_thread::get_id() != g_main) ++g_processOffMain;
    for (uint32_t c = 0; c < 2; ++c)
      for (uint32_t i = 0; i < d->frames; ++i) {
        if (d->inputs[1].channels[c][i] != 0.0f) sidechainSilent = false;
        d->outputs[0].channels[c][i] = d->inputs[0].channels[c][i] * (c == 0 ? 0.5f : 0.25f);
      }
    return SQ_OK;
  }
  void SQ_CALL reset() override {}
  uint32_t SQ_CALL getLatencyFrames() override { return 32; }
  uint32_t SQ_CALL getTailFrames() override { return 100; }
  uint32_t SQ_CALL getParamCount() override { return 0; }
  sq_result SQ_CALL getParamInfo(uint32_t, sq_param_info*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL getParamValue(uint32_t, double*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL setParamValue(uint32_t, double) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL paramValueToText(uint32_t, double, char*, uint32_t, uint32_t*) override { return SQ_E_NOTFOUND; }
  sq_result SQ_CALL saveState(sq::IByteStream*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL loadState(sq::IByteStream*) override { return SQ_E_NOTIMPL; }
  sq_bool SQ_CALL hasEditor() override { return SQ_FALSE; }
  sq_result SQ_CALL openEditor(void*, const char*, uint32_t*, uint32_t*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL closeEditor() override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL setEditorSize(uint32_t, uint32_t) override { return SQ_E_NOTIMPL; }
};

}  // namespace

SQ_TEST("plugin_fx: clone instantiates, prepare activates, process maps buses, release deactivates") {
  int made = 0;
  auto proto = std::make_shared<PluginFx>([&](std::string*) {
    ++made;
    return sq::Ptr<sq::IPluginInstance>::adopt(new FakeGain());
  }, "fake");
  CHECK_EQ(made, 0);  // prototypes never own an instance
  {
    ProcessorPtr a = proto->clone();
    ProcessorPtr b = proto->clone();
    CHECK_EQ(made, 2);
    CHECK_EQ(g_alive.load(), 2);
    REQUIRE(a->prepare(48000.0, 64, 2));
    CHECK_EQ(a->latency(), 32u);
    CHECK_EQ(a->tail(), 100u);
    CHECK_EQ(g_active.load(), 1);

    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    float* io[2] = {l.data(), r.data()};
    ProcessContext ctx;
    ctx.frames = 64;
    a->process(ctx, io, 2);
    CHECK_EQ(l[10], 0.5f);
    CHECK_EQ(r[10], 0.25f);

    // Mono track: both plugin inputs get the mono signal, the first output comes back.
    REQUIRE(b->prepare(48000.0, 64, 1));
    std::vector<float> m(48, 1.0f);
    float* mio[1] = {m.data()};
    ctx.frames = 48;
    b->process(ctx, mio, 1);
    CHECK_EQ(m[0], 0.5f);
    auto* fb = static_cast<FakeGain*>(static_cast<PluginFx*>(b.get())->instance());
    CHECK(fb->sidechainSilent);
    // Oversized blocks are ignored (dry), never overrun the prepared buffers.
    std::vector<float> big(128, 1.0f);
    float* bio[1] = {big.data()};
    ctx.frames = 128;
    b->process(ctx, bio, 1);
    CHECK_EQ(big[100], 1.0f);
  }
  CHECK_EQ(g_alive.load(), 0);
  CHECK_EQ(g_active.load(), 0);
}

SQ_TEST("plugin_fx: a failing factory yields a pass-through processor with an error") {
  auto proto = std::make_shared<PluginFx>([](std::string* e) {
    *e = "not installed";
    return sq::Ptr<sq::IPluginInstance>();
  });
  ProcessorPtr p = proto->clone();
  CHECK_EQ(static_cast<PluginFx*>(p.get())->error(), std::string("not installed"));
  REQUIRE(p->prepare(48000.0, 64, 2));
  CHECK_EQ(p->latency(), 0u);
  std::vector<float> l(64, 0.75f);
  float* io[1] = {l.data()};
  ProcessContext ctx;
  ctx.frames = 64;
  p->process(ctx, io, 1);
  CHECK_EQ(l[3], 0.75f);
}

// Regression (pluginhost.h: IPluginInstance lifecycle on the main thread only): offline renders
// used to instantiate, activate, deactivate and destroy their insert instances on the detached
// render worker. Now the engine compiles the offline graphs in startRender (main thread) and
// releases them in idle(); the worker only calls process().
SQ_TEST("plugin_fx: offline renders keep plugin create/activate/deactivate/destroy on the main thread") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  g_main = std::this_thread::get_id();
  g_lifecycleOffMain = 0;
  g_processOffMain = 0;
  g_checkThreads = true;
  {
    sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
    GraphDesc d;
    TrackDesc t;
    t.id = fakes::newGuid();
    t.name = "fx";
    t.channels = 2;
    t.inserts.push_back(std::make_shared<PluginFx>([](std::string*) { return sq::Ptr<sq::IPluginInstance>::adopt(new FakeGain()); }, "fake"));
    d.tracks.push_back(t);
    REQUIRE_OK(eng->setGraphDesc(d));
    const int liveInstances = g_alive.load();
    CHECK(liveInstances >= 1);
    for (uint32_t flags : {0u, static_cast<uint32_t>(SQ_RENDER_NORMALIZE)}) {
      sq_render_request rq{};
      rq.struct_size = sizeof rq;
      rq.kind = SQ_RENDER_MIXDOWN;
      rq.flags = flags;
      rq.start = 0.0;
      rq.end = 2.0;
      rq.output = flags ? "mem://pfx/norm.tst" : "mem://pfx/mix.tst";
      rq.file_extension = "tst";
      sq::Ptr<sq::IRenderTask> task;
      REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
      REQUIRE_OK(task->wait(20000));
      REQUIRE_OK(task->getResult());
      for (int i = 0; i < 2000 && eng->pendingOfflineGraphs() > 0; ++i) {
        eng->idle();  // releases the finished render's graphs (and plugin instances) here
        sq_thread_sleep_ms(1);
      }
      CHECK_EQ(eng->pendingOfflineGraphs(), size_t(0));
      CHECK_EQ(g_alive.load(), liveInstances);  // only the live graph's instances remain
    }
    CHECK(g_processOffMain.load() > 0);  // the render really ran on the worker
  }
  g_checkThreads = false;
  CHECK_EQ(g_lifecycleOffMain.load(), 0);
  CHECK_EQ(g_alive.load(), 0);
  CHECK_EQ(g_active.load(), 0);
}

SQ_TEST("plugin_fx: an engine destroyed while an offline render runs cancels it and frees its graphs") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  g_main = std::this_thread::get_id();
  g_lifecycleOffMain = 0;
  g_checkThreads = true;
  sq::Ptr<sq::IRenderTask> task;
  {
    sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
    GraphDesc d;
    TrackDesc t;
    t.id = fakes::newGuid();
    t.inserts.push_back(std::make_shared<PluginFx>([](std::string*) { return sq::Ptr<sq::IPluginInstance>::adopt(new FakeGain()); }, "fake"));
    d.tracks.push_back(t);
    REQUIRE_OK(eng->setGraphDesc(d));
    sq_render_request rq{};
    rq.struct_size = sizeof rq;
    rq.kind = SQ_RENDER_MIXDOWN;
    rq.start = 0.0;
    rq.end = 20000.0;  // long: still running when the engine goes away
    rq.output = "mem://pfx/long.tst";
    rq.file_extension = "tst";
    REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  }
  REQUIRE_OK(task->wait(20000));
  CHECK_EQ(task->getResult(), SQ_E_CLOSED);
  g_checkThreads = false;
  CHECK_EQ(g_lifecycleOffMain.load(), 0);
  CHECK_EQ(g_alive.load(), 0);
}
