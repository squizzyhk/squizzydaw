// pluginhost-clap: load the adapter through the real loader, scan + instantiate the CLAP test
// gain plugin in-process, process audio, check latency, parameters and state round trip.
#include <cmath>
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
  void SQ_CALL onLatencyChanged(sq::IPluginInstance*) override { ++latency; }
  void SQ_CALL onParamsChanged(sq::IPluginInstance*) override { ++params; }
  void SQ_CALL onParamEdited(sq::IPluginInstance*, const sq_param_change*) override { ++edits; }
  void SQ_CALL onRestartRequested(sq::IPluginInstance*) override { ++restarts; }
  void SQ_CALL onStateDirty(sq::IPluginInstance*) override { ++dirty; }
  sq_bool SQ_CALL onEditorResize(sq::IPluginInstance*, uint32_t, uint32_t) override { return SQ_FALSE; }
};

std::string gainPath() { return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "testplugins"), "sq-test-gain.clap"); }

// Runs `blocks` x 64 frames of a constant 0.5 signal; returns the last output sample (left).
float run(sq::IPluginInstance* p, int blocks, std::vector<sq_param_change> params = {}) {
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
    p->process(&pr);
    last = outL[n - 1];
  }
  return last;
}

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
