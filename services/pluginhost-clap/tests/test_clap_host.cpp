// pluginhost-clap: load the adapter through the real loader, scan + instantiate the CLAP test
// gain plugin in-process, process audio, check latency, parameters and state round trip.
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/cpp/bytestream.hpp"
#include "sq/interfaces/pluginhost.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {

struct Sink final : sq::Object<Sink, sq::IPluginScanSink> {
  std::vector<sq_plugin_desc> found;
  void SQ_CALL onPlugin(const sq_plugin_desc* d) override { found.push_back(*d); }
};

struct HostCb final : sq::Object<HostCb, sq::IPluginHostCallback> {
  int latency = 0, params = 0, edits = 0, restarts = 0, dirty = 0;
  std::vector<sq_param_change> changes;  // every onParamEdited, in order
  void SQ_CALL onLatencyChanged(sq::IPluginInstance*) override { ++latency; }
  void SQ_CALL onParamsChanged(sq::IPluginInstance*) override { ++params; }
  void SQ_CALL onParamEdited(sq::IPluginInstance*, const sq_param_change* c) override {
    ++edits;
    changes.push_back(*c);
  }
  void SQ_CALL onRestartRequested(sq::IPluginInstance*) override { ++restarts; }
  void SQ_CALL onStateDirty(sq::IPluginInstance*) override { ++dirty; }
  sq_bool SQ_CALL onEditorResize(sq::IPluginInstance*, uint32_t, uint32_t) override { return SQ_FALSE; }
};

std::string gainPath() { return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "testplugins"), "sq-test-gain.clap"); }

// Runs `blocks` x 64 frames of a constant 0.5 signal; returns the last output sample (left).
// `params` / `midi` go into the first block; `paramsOut` (when given and non-empty) is the first
// block's params_out with its size as the capacity, resized to the reported count afterwards.
float run(sq::IPluginInstance* p, int blocks, std::vector<sq_param_change> params = {},
          std::vector<sq_midi_event> midi = {}, std::vector<sq_param_change>* paramsOut = nullptr) {
  const uint32_t n = 64;
  std::vector<float> inL(n, 0.5f), inR(n, 0.5f), scL(n, 0.0f), scR(n, 0.0f), outL(n), outR(n);
  float* inCh[2] = {inL.data(), inR.data()};
  float* scCh[2] = {scL.data(), scR.data()};
  float* outCh[2] = {outL.data(), outR.data()};
  sq_audio_block ins[2] = {{inCh, 2, n, 0}, {scCh, 2, n, 0}};
  sq_audio_block outs[1] = {{outCh, 2, n, 0}};
  float last = 0;
  for (int b = 0; b < blocks; ++b) {
    sq_plugin_process pr{};
    pr.struct_size = sizeof pr;
    pr.frames = n;
    pr.inputs = ins;
    pr.input_bus_count = p->getBusCount(SQ_TRUE) > 1 ? 2 : 1;
    pr.outputs = outs;
    pr.output_bus_count = 1;
    if (b == 0 && !params.empty()) {
      pr.params_in = params.data();
      pr.params_in_count = static_cast<uint32_t>(params.size());
    }
    if (b == 0 && !midi.empty()) {
      pr.midi_in = midi.data();
      pr.midi_in_count = static_cast<uint32_t>(midi.size());
    }
    if (b == 0 && paramsOut && !paramsOut->empty()) {
      pr.params_out = paramsOut->data();
      pr.params_out_capacity = static_cast<uint32_t>(paramsOut->size());
    }
    p->process(&pr);
    if (b == 0 && paramsOut) paramsOut->resize(pr.params_out_count);
    last = outL[n - 1];
  }
  return last;
}

// One 64-frame block through `inputBuses` (1 or 2) input buses that calls nothing but process(),
// as an audio thread does.
void audioBlock(sq::IPluginInstance* p, uint32_t inputBuses) {
  const uint32_t n = 64;
  std::vector<float> in(n, 0.5f), sc(n, 0.0f), outL(n), outR(n);
  float* inCh[2] = {in.data(), in.data()};
  float* scCh[2] = {sc.data(), sc.data()};
  float* outCh[2] = {outL.data(), outR.data()};
  sq_audio_block ins[2] = {{inCh, 2, n, 0}, {scCh, 2, n, 0}};
  sq_audio_block outs[1] = {{outCh, 2, n, 0}};
  sq_plugin_process pr{};
  pr.struct_size = sizeof pr;
  pr.frames = n;
  pr.inputs = ins;
  pr.input_bus_count = inputBuses;
  pr.outputs = outs;
  pr.output_bus_count = 1;
  p->process(&pr);
}

// The test plugin's test-only exports (testplugins/test_gain.cpp), through a handle of our own on
// the library the adapter loads (the same module); the destructor resets them.
struct GainHooks {
  sq_dylib* lib = nullptr;
  void (*setProcessHook)(void (*fn)(void*), void* ctx) = nullptr;
  void (*setEditSteps)(uint32_t steps) = nullptr;
  GainHooks() {
    char err[512] = {};
    if (sq_dylib_open(gainPath().c_str(), &lib, err, sizeof err) != SQ_OK) return;
    setProcessHook = reinterpret_cast<void (*)(void (*)(void*), void*)>(sq_dylib_symbol(lib, "sq_test_gain_set_process_hook"));
    setEditSteps = reinterpret_cast<void (*)(uint32_t)>(sq_dylib_symbol(lib, "sq_test_gain_set_edit_steps"));
  }
  ~GainHooks() {
    if (setProcessHook) setProcessHook(nullptr, nullptr);
    if (setEditSteps) setEditSteps(1);
    if (lib) sq_dylib_close(lib);
  }
  GainHooks(const GainHooks&) = delete;
  GainHooks& operator=(const GainHooks&) = delete;
};

}  // namespace

SQ_TEST("pluginhost-clap: scan, instantiate, process, latency, params, state") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("pluginhost-clap") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, sq::pluginhost::kClapFormatClassId);
    REQUIRE(fmt);
    sq_plugin_format_info fi{};
    fi.struct_size = sizeof fi;
    REQUIRE_OK(fmt->getFormatInfo(&fi));
    CHECK_EQ(std::string(fi.format), std::string("clap"));
    CHECK(fmt->getSearchPathCount() >= 1);

    auto sink = sq::Ptr<Sink>::adopt(new Sink());
    REQUIRE_OK(fmt->scanFile(gainPath().c_str(), sink.get()));
    REQUIRE_EQ(sink->found.size(), size_t{1});
    sq_plugin_desc d = sink->found[0];
    CHECK_EQ(std::string(d.id), std::string("studio.squizzy.test.gain"));
    CHECK_EQ(d.status, SQ_PLUGIN_STATUS_OK);
    CHECK(d.flags & SQ_PLUGIN_EFFECT);
    CHECK(d.flags & SQ_PLUGIN_SIDECHAIN);
    CHECK_EQ(d.audio_inputs, 2u);
    CHECK(fmt->scanFile("does/not/exist.clap", sink.get()) < 0);

    auto cb = sq::Ptr<HostCb>::adopt(new HostCb());
    sq::Ptr<sq::IPluginInstance> inst;
    REQUIRE_OK(fmt->instantiate(&d, cb.get(), inst.put()));
    REQUIRE(inst);
    CHECK(inst->getParamCount() >= 3);
    sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 64, 0};
    REQUIRE_OK(inst->activate(&setup));
    CHECK_EQ(inst->getLatencyFrames(), 64u);

    // Unity gain: after the latency the output equals the input.
    CHECK(std::fabs(run(inst.get(), 4) - 0.5f) < 1e-5f);
    // -6.0206 dB halves the level (sample-accurate automation in the first block).
    sq_param_change pc{1, 0, -6.0206, 0};
    float half = run(inst.get(), 4, {pc});
    CHECK(std::fabs(half - 0.25f) < 1e-3f);
    double v = 0;
    REQUIRE_OK(inst->getParamValue(1, &v));
    CHECK(std::fabs(v + 6.0206) < 1e-6);

    auto st = sq::makeMemoryStream();
    REQUIRE_OK(inst->saveState(st.get()));
    REQUIRE_OK(inst->setParamValue(1, 0.0));
    run(inst.get(), 1);
    st->seek(0, SQ_SEEK_SET, nullptr);
    REQUIRE_OK(inst->loadState(st.get()));
    REQUIRE_OK(inst->getParamValue(1, &v));
    CHECK(std::fabs(v + 6.0206) < 1e-6);
    REQUIRE_OK(inst->deactivate());
  }
  CHECK(loader.unload("pluginhost-clap") == SQ_OK);
}

SQ_TEST("pluginhost-clap: editor edits of an active plugin reach onParamEdited from the next main-thread call") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("pluginhost-clap") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, sq::pluginhost::kClapFormatClassId);
    REQUIRE(fmt);
    auto sink = sq::Ptr<Sink>::adopt(new Sink());
    REQUIRE_OK(fmt->scanFile(gainPath().c_str(), sink.get()));
    REQUIRE_EQ(sink->found.size(), size_t{1});
    auto cb = sq::Ptr<HostCb>::adopt(new HostCb());
    sq::Ptr<sq::IPluginInstance> inst;
    REQUIRE_OK(fmt->instantiate(&sink->found[0], cb.get(), inst.put()));
    sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 64, 0};
    REQUIRE_OK(inst->activate(&setup));
    const uint32_t kGain = 1, kScMix = 2, kNotes = 4, kEditProbe = 5;  // test_gain.cpp

    // A main-thread set is visible before the audio thread has applied it.
    REQUIRE_OK(inst->setParamValue(kEditProbe, 0.25));
    double v = -1;
    REQUIRE_OK(inst->getParamValue(kEditProbe, &v));
    CHECK_EQ(v, 0.25);

    // The probe makes the plugin report an editor drag of "Sidechain Mix" from process(). The
    // engine passes no params_out: the edit waits for the next main-thread call into the instance.
    cb->changes.clear();
    run(inst.get(), 1);
    CHECK(cb->changes.empty());
    inst->getLatencyFrames();
    REQUIRE_EQ(cb->changes.size(), size_t{3});
    CHECK_EQ(cb->changes[0].param_id, kScMix);
    CHECK_EQ(cb->changes[0].flags, uint32_t{SQ_PARAM_GESTURE_BEGIN});
    CHECK_EQ(cb->changes[1].param_id, kScMix);
    CHECK_EQ(cb->changes[1].flags, 0u);
    CHECK_EQ(cb->changes[1].value, 0.25);
    CHECK_EQ(cb->changes[2].param_id, kScMix);
    CHECK_EQ(cb->changes[2].flags, uint32_t{SQ_PARAM_GESTURE_END});
    CHECK_EQ(cb->changes[2].value, 0.25);
    REQUIRE_OK(inst->getParamValue(kScMix, &v));
    CHECK_EQ(v, 0.25);
    REQUIRE_OK(inst->getParamValue(kEditProbe, &v));
    CHECK_EQ(v, 0.25);

    // Read-only parameters are outputs, not edits: the note counter's output event is not reported.
    sq_midi_event on{};
    on.bytes[0] = 0x90;
    on.bytes[1] = 60;
    on.bytes[2] = 100;
    on.size = 3;
    cb->changes.clear();
    run(inst.get(), 1, {}, {on});
    inst->getLatencyFrames();
    CHECK(cb->changes.empty());
    REQUIRE_OK(inst->getParamValue(kNotes, &v));
    CHECK_EQ(v, 1.0);

    // A caller that provides params_out gets the edits there instead.
    REQUIRE_OK(inst->setParamValue(kEditProbe, 0.5));
    std::vector<sq_param_change> out(8);
    run(inst.get(), 1, {}, {}, &out);
    inst->getLatencyFrames();
    CHECK(cb->changes.empty());
    REQUIRE_EQ(out.size(), size_t{3});
    CHECK_EQ(out[1].param_id, kScMix);
    CHECK_EQ(out[1].value, 0.5);

    // A set no process() call picked up still reaches the plugin when it is deactivated.
    REQUIRE_OK(inst->setParamValue(kGain, -12.0));
    REQUIRE_OK(inst->getParamValue(kGain, &v));
    CHECK_EQ(v, -12.0);
    REQUIRE_OK(inst->deactivate());
    REQUIRE_OK(inst->getParamValue(kGain, &v));
    CHECK_EQ(v, -12.0);
  }
  CHECK(loader.unload("pluginhost-clap") == SQ_OK);
}

SQ_TEST("pluginhost-clap: a set on an active plugin never reads back as the old value while the plugin takes it") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("pluginhost-clap") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, sq::pluginhost::kClapFormatClassId);
    REQUIRE(fmt);
    auto sink = sq::Ptr<Sink>::adopt(new Sink());
    REQUIRE_OK(fmt->scanFile(gainPath().c_str(), sink.get()));
    REQUIRE_EQ(sink->found.size(), size_t{1});
    GainHooks hooks;
    REQUIRE(hooks.setProcessHook);
    auto cb = sq::Ptr<HostCb>::adopt(new HostCb());
    sq::Ptr<sq::IPluginInstance> inst;
    REQUIRE_OK(fmt->instantiate(&sink->found[0], cb.get(), inst.put()));
    sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 64, 0};
    REQUIRE_OK(inst->activate(&setup));
    const uint32_t kGain = 1;  // test_gain.cpp (0 dB until set)
    const uint32_t buses = inst->getBusCount(SQ_TRUE) > 1 ? 2 : 1;
    run(inst.get(), 1);

    // An audio thread stops inside the plugin's process call: the adapter has taken the set from its
    // queue and handed it over, the plugin has not read it yet. The main thread asks meanwhile.
    struct Gate {
      std::atomic<int> stage{0};  // 1: audio thread inside the plugin, 2: main thread done
    } gate;
    hooks.setProcessHook(
        [](void* ctx) {
          auto* g = static_cast<Gate*>(ctx);
          int idle = 0;
          if (!g->stage.compare_exchange_strong(idle, 1)) return;  // the main thread gave up
          while (g->stage.load() != 2) std::this_thread::yield();
        },
        &gate);
    REQUIRE_OK(inst->setParamValue(kGain, -12.0));
    std::thread audio([&] { audioBlock(inst.get(), buses); });
    bool inside = false;
    for (int i = 0; i < 10000 && !inside; ++i) {
      inside = gate.stage.load() == 1;
      if (!inside) sq_thread_sleep_ms(1);
    }
    double during = 1.0;
    const sq_result r = inside ? inst->getParamValue(kGain, &during) : SQ_E_FAIL;
    gate.stage.store(2);
    audio.join();
    hooks.setProcessHook(nullptr, nullptr);
    REQUIRE(inside);
    CHECK_EQ(r, SQ_OK);
    CHECK_EQ(during, -12.0);
    double after = 1.0;
    REQUIRE_OK(inst->getParamValue(kGain, &after));
    CHECK_EQ(after, -12.0);
    REQUIRE_OK(inst->deactivate());
  }
  CHECK(loader.unload("pluginhost-clap") == SQ_OK);
}

SQ_TEST("pluginhost-clap: editor edits beyond the edit queue keep their final value and gesture end") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("pluginhost-clap") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, sq::pluginhost::kClapFormatClassId);
    REQUIRE(fmt);
    auto sink = sq::Ptr<Sink>::adopt(new Sink());
    REQUIRE_OK(fmt->scanFile(gainPath().c_str(), sink.get()));
    REQUIRE_EQ(sink->found.size(), size_t{1});
    GainHooks hooks;
    REQUIRE(hooks.setEditSteps);
    auto cb = sq::Ptr<HostCb>::adopt(new HostCb());
    sq::Ptr<sq::IPluginInstance> inst;
    REQUIRE_OK(fmt->instantiate(&sink->found[0], cb.get(), inst.put()));
    sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 64, 0};
    REQUIRE_OK(inst->activate(&setup));
    const uint32_t kScMix = 2, kEditProbe = 5;  // test_gain.cpp
    // Counts the gesture events in cb->changes; false when a value goes back against the drag.
    struct Seen {
      int begins = 0, ends = 0;
      bool monotonic = true;
    };
    const auto survey = [&](bool rising) {
      Seen s;
      for (size_t i = 0; i < cb->changes.size(); ++i) {
        const sq_param_change& c = cb->changes[i];
        CHECK_EQ(c.param_id, kScMix);
        if (c.flags & SQ_PARAM_GESTURE_BEGIN) ++s.begins;
        if (c.flags & SQ_PARAM_GESTURE_END) ++s.ends;
        if (i && (rising ? c.value < cb->changes[i - 1].value : c.value > cb->changes[i - 1].value)) s.monotonic = false;
      }
      return s;
    };

    // One editor drag of "Sidechain Mix" from 0 to 1 in 1500 steps inside one block: more edits
    // than the queue to the main thread holds (1024). Later blocks deliver what it had no room for.
    hooks.setEditSteps(1500);
    cb->changes.clear();
    REQUIRE_OK(inst->setParamValue(kEditProbe, 1.0));
    run(inst.get(), 1);
    inst->getLatencyFrames();
    run(inst.get(), 1);
    inst->getLatencyFrames();
    REQUIRE(!cb->changes.empty());
    Seen s = survey(true);
    CHECK_EQ(s.begins, 1);
    CHECK_EQ(s.ends, 1);
    CHECK(s.monotonic);  // never an older value after a newer one
    CHECK_EQ(cb->changes.front().flags, uint32_t{SQ_PARAM_GESTURE_BEGIN});
    CHECK_EQ(cb->changes.back().flags, uint32_t{SQ_PARAM_GESTURE_END});
    CHECK_EQ(cb->changes.back().value, 1.0);
    double v = -1;
    REQUIRE_OK(inst->getParamValue(kScMix, &v));
    CHECK_EQ(v, 1.0);

    // Afterwards a drag arrives exactly as reported again.
    hooks.setEditSteps(1);
    cb->changes.clear();
    REQUIRE_OK(inst->setParamValue(kEditProbe, 0.5));
    run(inst.get(), 1);
    inst->getLatencyFrames();
    REQUIRE_EQ(cb->changes.size(), size_t{3});
    CHECK_EQ(cb->changes[0].flags, uint32_t{SQ_PARAM_GESTURE_BEGIN});
    CHECK_EQ(cb->changes[1].flags, 0u);
    CHECK_EQ(cb->changes[1].value, 0.5);
    CHECK_EQ(cb->changes[2].flags, uint32_t{SQ_PARAM_GESTURE_END});
    CHECK_EQ(cb->changes[2].value, 0.5);

    // What no later process() call delivers arrives when the plugin is deactivated.
    hooks.setEditSteps(1500);
    cb->changes.clear();
    REQUIRE_OK(inst->setParamValue(kEditProbe, 0.0));
    run(inst.get(), 1);
    inst->getLatencyFrames();
    REQUIRE_OK(inst->deactivate());
    REQUIRE(!cb->changes.empty());
    s = survey(false);
    CHECK_EQ(s.begins, 1);
    CHECK_EQ(s.ends, 1);
    CHECK(s.monotonic);
    CHECK_EQ(cb->changes.back().flags, uint32_t{SQ_PARAM_GESTURE_END});
    CHECK_EQ(cb->changes.back().value, 0.0);

    // An instance deactivated off its main thread (the engine releases a stuck offline render's
    // graph on the render worker) reports nothing there: the callback is main-thread only.
    REQUIRE_OK(inst->activate(&setup));
    cb->changes.clear();
    REQUIRE_OK(inst->setParamValue(kEditProbe, 1.0));
    run(inst.get(), 1);
    inst->getLatencyFrames();  // the queue's edits; the rest is held
    REQUIRE(!cb->changes.empty());
    CHECK(cb->changes.back().flags != uint32_t{SQ_PARAM_GESTURE_END});
    cb->changes.clear();
    sq_result offMain = SQ_E_FAIL;
    std::thread worker([&] { offMain = inst->deactivate(); });
    worker.join();
    CHECK_EQ(offMain, SQ_OK);
    CHECK(cb->changes.empty());
    inst->getLatencyFrames();
    CHECK(cb->changes.empty());

    // Nothing of it is left over: the next activation reports a drag exactly again.
    hooks.setEditSteps(1);
    REQUIRE_OK(inst->activate(&setup));
    CHECK(cb->changes.empty());
    REQUIRE_OK(inst->setParamValue(kEditProbe, 0.5));
    run(inst.get(), 1);
    inst->getLatencyFrames();
    REQUIRE_EQ(cb->changes.size(), size_t{3});
    CHECK_EQ(cb->changes[0].flags, uint32_t{SQ_PARAM_GESTURE_BEGIN});
    CHECK_EQ(cb->changes[2].flags, uint32_t{SQ_PARAM_GESTURE_END});
    CHECK_EQ(cb->changes[2].value, 0.5);
    REQUIRE_OK(inst->deactivate());
  }
  CHECK(loader.unload("pluginhost-clap") == SQ_OK);
}
