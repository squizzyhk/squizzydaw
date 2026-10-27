// pluginhost-vst3: load the adapter through the real loader, scan + instantiate the original VST3
// test plugin (bin/testplugins-vst3/sq-test-gain.vst3 bundle) in-process and check module
// entry, class enumeration, buses incl. sidechain, process (audio, automation, events, MIDI-CC
// mapping, output params, silence), latency/tail, component <-> controller connection, component
// handler edits, state round trip, and unload.
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/cpp/bytestream.hpp"
#include "sq/interfaces/pluginhost.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {

constexpr const char* kGainId = "6D2F880BA9134121A830C04A1A7D830E";
constexpr uint32_t kGain = 1, kScMix = 3, kLatencyMode = 4, kPeak = 5, kDrive = 6, kHostRate = 100, kEditProbe = 101;

struct Sink final : sq::Object<Sink, sq::IPluginScanSink> {
  std::vector<sq_plugin_desc> found;
  void SQ_CALL onPlugin(const sq_plugin_desc* d) override { found.push_back(*d); }
};

struct HostCb final : sq::Object<HostCb, sq::IPluginHostCallback> {
  int latency = 0, params = 0, restarts = 0, dirty = 0;
  std::vector<sq_param_change> edits;
  void SQ_CALL onLatencyChanged(sq::IPluginInstance*) override { ++latency; }
  void SQ_CALL onParamsChanged(sq::IPluginInstance*) override { ++params; }
  void SQ_CALL onParamEdited(sq::IPluginInstance*, const sq_param_change* c) override { edits.push_back(*c); }
  void SQ_CALL onRestartRequested(sq::IPluginInstance*) override { ++restarts; }
  void SQ_CALL onStateDirty(sq::IPluginInstance*) override { ++dirty; }
  sq_bool SQ_CALL onEditorResize(sq::IPluginInstance*, uint32_t, uint32_t) override { return SQ_FALSE; }
};

std::string pluginDir() { return sq::joinPath(sq::specialDir(SQ_DIR_EXE), "testplugins-vst3"); }
std::string bundlePath() { return sq::joinPath(pluginDir(), "sq-test-gain.vst3"); }

// One 64-frame block with constant main input `in` and sidechain `sc`.
struct Block {
  static constexpr uint32_t n = 64;
  std::vector<float> inL = std::vector<float>(n), inR = std::vector<float>(n), scL = std::vector<float>(n), scR = std::vector<float>(n);
  std::vector<float> outL = std::vector<float>(n), outR = std::vector<float>(n);
  std::vector<sq_param_change> paramsIn, paramsOut = std::vector<sq_param_change>(32);
  std::vector<sq_midi_event> midiIn, midiOut = std::vector<sq_midi_event>(32);
  sq_plugin_process pr{};
  sq_result run(sq::IPluginInstance* p, float in, float sc) {
    std::fill(inL.begin(), inL.end(), in);
    std::fill(inR.begin(), inR.end(), in);
    std::fill(scL.begin(), scL.end(), sc);
    std::fill(scR.begin(), scR.end(), sc);
    float* inCh[2] = {inL.data(), inR.data()};
    float* scCh[2] = {scL.data(), scR.data()};
    float* outCh[2] = {outL.data(), outR.data()};
    sq_audio_block ins[2] = {{inCh, 2, n, 0}, {scCh, 2, n, 0}};
    sq_audio_block outs[1] = {{outCh, 2, n, 0}};
    pr = sq_plugin_process{};
    pr.struct_size = sizeof pr;
    pr.frames = n;
    pr.inputs = ins;
    pr.input_bus_count = 2;
    pr.outputs = outs;
    pr.output_bus_count = 1;
    pr.params_in = paramsIn.empty() ? nullptr : paramsIn.data();
    pr.params_in_count = static_cast<uint32_t>(paramsIn.size());
    pr.params_out = paramsOut.data();
    pr.params_out_capacity = static_cast<uint32_t>(paramsOut.size());
    pr.midi_in = midiIn.empty() ? nullptr : midiIn.data();
    pr.midi_in_count = static_cast<uint32_t>(midiIn.size());
    pr.midi_out = midiOut.data();
    pr.midi_out_capacity = static_cast<uint32_t>(midiOut.size());
    sq_result r = p->process(&pr);
    paramsIn.clear();
    midiIn.clear();
    return r;
  }
  // Runs `blocks` blocks; returns the last left output sample.
  float settle(sq::IPluginInstance* p, float in, float sc, int blocks) {
    for (int i = 0; i < blocks; ++i) run(p, in, sc);
    return outL[n - 1];
  }
};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

double param(sq::IPluginInstance* p, uint32_t id) {
  double v = -12345;
  p->getParamValue(id, &v);
  return v;
}

}  // namespace

SQ_TEST("pluginhost-vst3: scan, instantiate, buses, process, events, params, latency, state") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("pluginhost-vst3") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  REQUIRE(sq_fs_is_dir(bundlePath().c_str()));
  {
    auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, sq::pluginhost::kVst3FormatClassId);
    REQUIRE(fmt);
    sq_plugin_format_info fi{};
    fi.struct_size = sizeof fi;
    REQUIRE_OK(fmt->getFormatInfo(&fi));
    CHECK_EQ(std::string(fi.format), std::string("vst3"));
    CHECK_EQ(std::string(fi.file_extension), std::string("vst3"));
    REQUIRE(fmt->getSearchPathCount() >= 1);
    char sp[2048];
    uint32_t need = 0;
    REQUIRE_OK(fmt->getSearchPath(0, sp, sizeof sp, &need));
    CHECK(std::strstr(sp, "VST3") != nullptr || std::strstr(sp, "vst3") != nullptr);

    // ---- scanner side: class enumeration (the controller class is not reported) ----
    auto sink = sq::Ptr<Sink>::adopt(new Sink());
    REQUIRE_OK(fmt->scanFile(bundlePath().c_str(), sink.get()));
    REQUIRE_EQ(sink->found.size(), size_t{1});
    sq_plugin_desc d = sink->found[0];
    CHECK_EQ(std::string(d.format), std::string("vst3"));
    CHECK_EQ(std::string(d.id), std::string(kGainId));
    CHECK_EQ(std::string(d.name), std::string("Squizzy Test Gain"));
    CHECK_EQ(std::string(d.vendor), std::string("Squizzy Studio Tests"));
    CHECK_EQ(std::string(d.version), std::string("1.0.0"));
    CHECK_EQ(std::string(d.category), std::string("Fx|Tools"));
    CHECK_EQ(d.status, SQ_PLUGIN_STATUS_OK);
    CHECK(d.flags & SQ_PLUGIN_EFFECT);
    CHECK(d.flags & SQ_PLUGIN_SIDECHAIN);
    CHECK(!(d.flags & SQ_PLUGIN_HAS_EDITOR));
    CHECK_EQ(d.audio_inputs, 2u);
    CHECK_EQ(d.audio_outputs, 2u);
    CHECK_EQ(d.aux_inputs, 1u);
    CHECK_EQ(d.midi_inputs, 1u);
    CHECK_EQ(d.midi_outputs, 1u);
    CHECK_EQ(d.sub_index, 0u);
    CHECK(d.file_size > 0);
    CHECK(fmt->scanFile(sq::joinPath(pluginDir(), "does-not-exist.vst3").c_str(), sink.get()) < 0);

    // ---- instance ----
    auto cb = sq::Ptr<HostCb>::adopt(new HostCb());
    sq::Ptr<sq::IPluginInstance> inst;
    REQUIRE_OK(fmt->instantiate(&d, cb.get(), inst.put()));
    REQUIRE(inst);
    sq_plugin_desc live{};
    live.struct_size = sizeof live;
    REQUIRE_OK(inst->getDesc(&live));
    CHECK_EQ(std::string(live.id), std::string(kGainId));
    CHECK_EQ(inst->hasEditor(), SQ_FALSE);
    uint32_t ew = 0, eh = 0;
    int dummyWindow = 0;
    CHECK_EQ(inst->openEditor(&dummyWindow, "win32", &ew, &eh), SQ_E_UNSUPPORTED);

    // Buses: main in/out active, sidechain aux inactive by default.
    REQUIRE_EQ(inst->getBusCount(SQ_TRUE), 2u);
    REQUIRE_EQ(inst->getBusCount(SQ_FALSE), 1u);
    sq_plugin_bus_info bi{};
    bi.struct_size = sizeof bi;
    REQUIRE_OK(inst->getBusInfo(SQ_TRUE, 1, &bi));
    CHECK_EQ(std::string(bi.name), std::string("Sidechain"));
    CHECK_EQ(bi.channels, 2u);
    CHECK(bi.flags & SQ_BUS_AUX);
    CHECK(!(bi.flags & SQ_BUS_ACTIVE));
    REQUIRE_OK(inst->getBusInfo(SQ_TRUE, 0, &bi));
    CHECK(bi.flags & SQ_BUS_MAIN);
    CHECK(bi.flags & SQ_BUS_ACTIVE);
    REQUIRE_OK(inst->setBusActive(SQ_TRUE, 1, SQ_TRUE));
    REQUIRE_OK(inst->getBusInfo(SQ_TRUE, 1, &bi));
    CHECK(bi.flags & SQ_BUS_ACTIVE);

    // Parameters are plain values.
    REQUIRE_EQ(inst->getParamCount(), 611u);  // 11 named + 600 hidden "Bank" params
    sq_param_info pi{};
    pi.struct_size = sizeof pi;
    REQUIRE_OK(inst->getParamInfo(0, &pi));
    CHECK_EQ(pi.id, kGain);
    CHECK_EQ(std::string(pi.name), std::string("Gain"));
    CHECK_EQ(std::string(pi.unit), std::string("dB"));
    CHECK(near(pi.min_value, -60.0, 1e-9));
    CHECK(near(pi.max_value, 12.0, 1e-9));
    CHECK(near(pi.default_value, 0.0, 1e-9));
    CHECK(pi.flags & SQ_PARAM_AUTOMATABLE);
    REQUIRE_OK(inst->getParamInfo(1, &pi));
    CHECK(pi.flags & SQ_PARAM_BYPASS);
    CHECK(pi.flags & SQ_PARAM_STEPPED);
    CHECK_EQ(pi.step_count, 1u);
    REQUIRE_OK(inst->getParamInfo(6, &pi));
    CHECK_EQ(pi.id, kHostRate);
    CHECK(pi.flags & SQ_PARAM_HIDDEN);
    CHECK(pi.flags & SQ_PARAM_READONLY);
    char txt[64];
    REQUIRE_OK(inst->paramValueToText(kGain, -6.0, txt, sizeof txt, &need));
    CHECK_EQ(std::string(txt), std::string("-6.00 dB"));
    CHECK(near(param(inst.get(), kGain), 0.0, 1e-9));
    CHECK(near(param(inst.get(), kHostRate), 0.0, 1e-9));

    // ---- activate: latency, and the processor's activation message reached the controller
    // through the host's connection proxies; the controller answered through the handler ----
    sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 64, 0};
    REQUIRE_OK(inst->activate(&setup));
    CHECK_EQ(inst->getLatencyFrames(), 64u);
    CHECK_EQ(inst->getTailFrames(), 256u);
    CHECK(near(param(inst.get(), kHostRate), 48000.0, 1e-6));
    CHECK(cb->params >= 1);
    REQUIRE_EQ(cb->edits.size(), size_t{3});
    CHECK_EQ(cb->edits[0].param_id, kEditProbe);
    CHECK_EQ(cb->edits[0].flags, SQ_PARAM_GESTURE_BEGIN);
    CHECK_EQ(cb->edits[1].flags, 0u);
    CHECK(near(cb->edits[1].value, 0.75, 1e-9));
    CHECK_EQ(cb->edits[2].flags, SQ_PARAM_GESTURE_END);

    Block b;
    // Unity gain: after the 64-frame latency the output equals the input.
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.5, 1e-5));
    CHECK_EQ(b.outR[10], b.outL[10]);
    // Output parameter: the processor reports the input peak.
    bool peakSeen = false;
    for (uint32_t i = 0; i < b.pr.params_out_count; ++i)
      if (b.paramsOut[i].param_id == kPeak) peakSeen = near(b.paramsOut[i].value, 0.5, 1e-6);
    CHECK(peakSeen);
    // Sample-accurate automation (plain dB): -6.0206 dB halves the level.
    b.paramsIn.push_back(sq_param_change{kGain, 0, -6.0206, 0});
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.25, 1e-4));
    // The controller follows automation and the processor's output parameters (via pump()).
    CHECK(near(param(inst.get(), kGain), -6.0206, 1e-6));
    CHECK(near(param(inst.get(), kPeak), 0.5, 1e-6));
    // Non-linear parameter through the RT conversion table: Drive 2.5 (plain).
    b.paramsIn.push_back(sq_param_change{kDrive, 0, 2.5, 0});
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.25 * 2.5, 2e-3));
    b.paramsIn.push_back(sq_param_change{kDrive, 0, 1.0, 0});
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.25, 1e-4));
    // Sidechain bus (activated above) mixed in by a main-thread parameter set.
    REQUIRE_OK(inst->setParamValue(kScMix, 1.0));
    CHECK(near(b.settle(inst.get(), 0.5f, 0.25f, 3), 0.5, 1e-4));
    REQUIRE_OK(inst->setParamValue(kScMix, 0.0));
    CHECK(near(b.settle(inst.get(), 0.5f, 0.25f, 3), 0.25, 1e-4));

    // ---- events: notes echoed an octave up at the same offset, sysex echoed ----
    static const uint8_t sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
    b.midiIn.push_back(sq_midi_event{3, {0x91, 60, 100, 0}, 3, nullptr});
    b.midiIn.push_back(sq_midi_event{9, {0x81, 60, 0, 0}, 3, nullptr});
    b.midiIn.push_back(sq_midi_event{12, {0xF0, 0, 0, 0}, sizeof sysex, sysex});
    REQUIRE_OK(b.run(inst.get(), 0.5f, 0.0f));
    REQUIRE_EQ(b.pr.midi_out_count, 3u);
    CHECK_EQ(b.midiOut[0].frame_offset, 3u);
    CHECK_EQ(b.midiOut[0].bytes[0], 0x91);
    CHECK_EQ(b.midiOut[0].bytes[1], 72);
    CHECK_EQ(b.midiOut[0].bytes[2], 100);
    CHECK_EQ(b.midiOut[1].frame_offset, 9u);
    CHECK_EQ(b.midiOut[1].bytes[0], 0x81);
    CHECK_EQ(b.midiOut[1].bytes[1], 72);
    CHECK_EQ(b.midiOut[2].size, static_cast<uint32_t>(sizeof sysex));
    REQUIRE(b.midiOut[2].data != nullptr);
    CHECK(std::memcmp(b.midiOut[2].data, sysex, sizeof sysex) == 0);
    CHECK(b.midiOut[2].data != sysex);  // copied into the adapter's arena
    // MIDI CC 7 is mapped to Gain by the controller (IMidiMapping): 127 -> +12 dB.
    b.midiIn.push_back(sq_midi_event{0, {0xB0, 7, 127, 0}, 3, nullptr});
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.5 * std::pow(10.0, 12.0 / 20.0), 1e-3));
    b.paramsIn.push_back(sq_param_change{kGain, 0, 0.0, 0});
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.5, 1e-4));

    // ---- reset clears the delay line; silence is reported once the tail has drained ----
    inst->reset();
    REQUIRE_OK(b.run(inst.get(), 0.5f, 0.0f));
    CHECK_EQ(b.outL[0], 0.0f);
    CHECK_EQ(b.outL[63], 0.0f);
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 2), 0.5, 1e-5));
    CHECK_EQ(b.run(inst.get(), 0.0f, 0.0f), SQ_OK);  // still emitting the delayed signal
    sq_result silent = SQ_OK;
    for (int i = 0; i < 4 && silent == SQ_OK; ++i) silent = b.run(inst.get(), 0.0f, 0.0f);
    CHECK_EQ(silent, SQ_S_FALSE);

    // ---- latency change: controller -> restartComponent(kLatencyChanged) -> host callback ----
    int latBefore = cb->latency;
    REQUIRE_OK(inst->setParamValue(kLatencyMode, 1.0));
    inst->getLatencyFrames();  // main-thread call delivers pending requests
    CHECK(cb->latency > latBefore);
    b.run(inst.get(), 0.0f, 0.0f);  // the processor receives the new mode
    REQUIRE_OK(inst->deactivate());
    REQUIRE_OK(inst->activate(&setup));
    CHECK_EQ(inst->getLatencyFrames(), 128u);
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.5, 1e-5));

    // ---- state: component + controller state round trip ----
    REQUIRE_OK(inst->setParamValue(kGain, -12.0));
    b.run(inst.get(), 0.5f, 0.0f);
    auto st = sq::makeMemoryStream();
    REQUIRE_OK(inst->saveState(st.get()));
    REQUIRE_OK(inst->setParamValue(kGain, 3.0));
    b.run(inst.get(), 0.5f, 0.0f);
    CHECK(near(param(inst.get(), kGain), 3.0, 1e-6));
    st->seek(0, SQ_SEEK_SET, nullptr);
    REQUIRE_OK(inst->loadState(st.get()));
    CHECK(near(param(inst.get(), kGain), -12.0, 1e-6));
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 4), 0.5 * std::pow(10.0, -12.0 / 20.0), 1e-4));
    REQUIRE_OK(inst->deactivate());
    CHECK_EQ(inst->deactivate(), SQ_S_FALSE);

    // A second, never activated instance takes the saved state, controller state included.
    sq::Ptr<sq::IPluginInstance> inst2;
    REQUIRE_OK(fmt->instantiate(&d, nullptr, inst2.put()));
    CHECK(near(param(inst2.get(), kHostRate), 0.0, 1e-9));
    st->seek(0, SQ_SEEK_SET, nullptr);
    REQUIRE_OK(inst2->loadState(st.get()));
    CHECK(near(param(inst2.get(), kGain), -12.0, 1e-6));
    CHECK(near(param(inst2.get(), kHostRate), 48000.0, 1e-6));
    CHECK(near(param(inst2.get(), kLatencyMode), 1.0, 1e-9));
    // A bare component state (no adapter container) is accepted too.
    uint32_t hdr[2] = {0x47545153u, 1};
    double vals[5] = {(-30.0 + 60.0) / 72.0, 0.0, 0.0, 0.0, 0.0};
    auto bare = sq::makeMemoryStream();
    uint32_t w = 0;
    bare->write(hdr, sizeof hdr, &w);
    bare->write(vals, sizeof vals, &w);
    bare->seek(0, SQ_SEEK_SET, nullptr);
    REQUIRE_OK(inst2->loadState(bare.get()));
    CHECK(near(param(inst2.get(), kGain), -30.0, 1e-6));
    auto junk = sq::makeMemoryStream("nope", 4, false);
    CHECK(inst2->loadState(junk.get()) < 0);
    // Sets while inactive are coalesced (no ring overflow on large preset loads) and reach the
    // processor on activation. No host callback: the plugin's calls back must not crash.
    int busy = 0;
    for (int i = 0; i < 3000; ++i) busy += inst2->setParamValue(kGain, -60.0 + (i % 72)) != SQ_OK;
    CHECK_EQ(busy, 0);
    REQUIRE_OK(inst2->setParamValue(kGain, -6.0206));
    REQUIRE_OK(inst2->activate(&setup));
    Block b2;
    CHECK(near(b2.settle(inst2.get(), 0.5f, 0.0f, 3), 0.25, 1e-4));
    REQUIRE_OK(inst2->deactivate());
    inst2 = nullptr;
    inst = nullptr;

    // ---- legacy single-file module layout ----
    std::string tmp = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-vst3-" + std::to_string(sq_process_id()));
    sq_fs_create_dirs(tmp.c_str());
    std::string flat = sq::joinPath(tmp, std::string("sq-test-gain-flat") + (std::strcmp(sq_dylib_suffix(), ".dll") == 0 ? ".vst3" : sq_dylib_suffix()));
    std::string inner = sq::joinPath(sq::joinPath(sq::joinPath(bundlePath(), "Contents"), SQ_VST3_TEST_ARCH), std::string("sq-test-gain") + SQ_VST3_TEST_BINEXT);
    REQUIRE_OK(sq_fs_copy_file(inner.c_str(), flat.c_str(), SQ_TRUE));
    auto sink2 = sq::Ptr<Sink>::adopt(new Sink());
#if defined(_WIN32)
    REQUIRE_OK(fmt->scanFile(flat.c_str(), sink2.get()));
    CHECK_EQ(sink2->found.size(), size_t{1});
#endif
    sink2 = nullptr;
    sq_fs_remove(flat.c_str());
    sq_fs_remove_all(tmp.c_str());
  }
  // Every plugin object is gone: the adapter unloads cleanly.
  CHECK(loader.unload("pluginhost-vst3") == SQ_OK);
}

// ---- verifier regressions ------------------------------------------------------------------------
namespace {

struct CountingCb final : sq::Object<CountingCb, sq::IPluginHostCallback> {
  int restarts = 0, dirty = 0, latency = 0;
  void SQ_CALL onLatencyChanged(sq::IPluginInstance*) override { ++latency; }
  void SQ_CALL onParamsChanged(sq::IPluginInstance*) override {}
  void SQ_CALL onParamEdited(sq::IPluginInstance*, const sq_param_change*) override {}
  void SQ_CALL onRestartRequested(sq::IPluginInstance*) override { ++restarts; }
  void SQ_CALL onStateDirty(sq::IPluginInstance*) override { ++dirty; }
  sq_bool SQ_CALL onEditorResize(sq::IPluginInstance*, uint32_t, uint32_t) override { return SQ_FALSE; }
};

constexpr uint32_t kExtraOut = 8, kBankSum = 7, kDirtyProbe = 102, kBankFirst = 1000, kBankCount = 600;

}  // namespace

SQ_TEST("pluginhost-vst3: malformed input, param ids, burst sets, IO change, teardown callbacks") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("pluginhost-vst3") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  {
    auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, sq::pluginhost::kVst3FormatClassId);
    REQUIRE(fmt);
    auto sink = sq::Ptr<Sink>::adopt(new Sink());
    REQUIRE_OK(fmt->scanFile(bundlePath().c_str(), sink.get()));
    REQUIRE_EQ(sink->found.size(), size_t{1});
    const sq_plugin_desc d = sink->found[0];

    // ---- malformed descs are rejected without reading past their fields ----
    sq::Ptr<sq::IPluginInstance> bad;
    sq_plugin_desc m = d;
    std::memset(m.id, 'A', sizeof m.id);  // no terminator
    CHECK_EQ(fmt->instantiate(&m, nullptr, bad.put()), SQ_E_INVALIDARG);
    m = d;
    std::memset(m.path, 'B', sizeof m.path);
    CHECK_EQ(fmt->instantiate(&m, nullptr, bad.put()), SQ_E_INVALIDARG);
    m = d;
    m.struct_size = 8;
    CHECK_EQ(fmt->instantiate(&m, nullptr, bad.put()), SQ_E_INVALIDARG);
    CHECK(!bad);
    {
      // Junk "module" file and an empty bundle directory: scan fails cleanly, reports nothing.
      std::string tmp = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-vst3-junk-" + std::to_string(sq_process_id()));
      sq_fs_create_dirs(tmp.c_str());
      std::string junk = sq::joinPath(tmp, "junk.vst3");
      const char text[] = "this is not a module";
      REQUIRE_OK(sq_fs_write_file(junk.c_str(), text, sizeof text - 1));
      std::string emptyBundle = sq::joinPath(tmp, "empty.vst3");
      sq_fs_create_dirs(sq::joinPath(emptyBundle, "Contents").c_str());
      auto s2 = sq::Ptr<Sink>::adopt(new Sink());
      CHECK(fmt->scanFile(junk.c_str(), s2.get()) < 0);
      CHECK(fmt->scanFile(emptyBundle.c_str(), s2.get()) < 0);
      CHECK_EQ(s2->found.size(), size_t{0});
      sq_fs_remove_all(tmp.c_str());
    }

    auto cb = sq::Ptr<CountingCb>::adopt(new CountingCb());
    sq::Ptr<sq::IPluginInstance> inst;
    REQUIRE_OK(fmt->instantiate(&d, cb.get(), inst.put()));

    // ---- unknown parameter ids and non-finite values ----
    double v = 0;
    CHECK_EQ(inst->getParamValue(9999, &v), SQ_E_NOTFOUND);
    CHECK_EQ(inst->setParamValue(9999, 0.5), SQ_E_NOTFOUND);
    char txt[64];
    uint32_t need = 0;
    CHECK_EQ(inst->paramValueToText(9999, 0.5, txt, sizeof txt, &need), SQ_E_NOTFOUND);
    CHECK_EQ(inst->setParamValue(kGain, std::nan("")), SQ_E_INVALIDARG);
    CHECK_EQ(inst->setParamValue(kGain, std::numeric_limits<double>::infinity()), SQ_E_INVALIDARG);
    REQUIRE_OK(inst->getParamValue(kBankFirst + 599, &v));

    sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 64, 0};
    REQUIRE_OK(inst->activate(&setup));
    Block b;
    CHECK(near(b.settle(inst.get(), 0.5f, 0.0f, 3), 0.5, 1e-5));

    // ---- NaN automation never reaches the plugin; an offset past the block is clamped to the
    // last frame (not wrapped to a negative sample offset) ----
    b.paramsIn.push_back(sq_param_change{kGain, 0, std::nan(""), 0});
    b.run(inst.get(), 0.5f, 0.0f);
    b.run(inst.get(), 0.5f, 0.0f);
    CHECK(std::isfinite(b.outL[0]) && std::isfinite(b.outL[63]));
    CHECK(near(b.outL[63], 0.5, 1e-5));
    b.paramsIn.push_back(sq_param_change{kGain, 0xFFFFFFFFu, -6.0206, 0});
    b.run(inst.get(), 0.5f, 0.0f);
    b.run(inst.get(), 0.5f, 0.0f);  // shows the previous block (64-frame latency)
    CHECK(near(b.outL[0], 0.5, 1e-5));
    CHECK(near(b.outL[62], 0.5, 1e-5));
    CHECK(near(b.outL[63], 0.25, 1e-4));
    REQUIRE_OK(inst->setParamValue(kGain, 0.0));

    // ---- a burst of main-thread sets on more distinct ids than one block's queues hold is
    // delivered over several blocks, none lost ----
    int failed = 0;
    for (uint32_t i = 0; i < kBankCount; ++i) failed += inst->setParamValue(kBankFirst + i, 1.0) != SQ_OK;
    CHECK_EQ(failed, 0);
    b.settle(inst.get(), 0.5f, 0.0f, 4);
    CHECK(near(param(inst.get(), kBankSum), 1.0, 1e-9));

    // ---- IO change: kIoChanged -> onRestartRequested; after deactivate the new bus layout is
    // visible and used by activate/process ----
    int restartsBefore = cb->restarts;
    REQUIRE_OK(inst->setParamValue(kExtraOut, 1.0));
    b.run(inst.get(), 0.5f, 0.0f);  // the processor receives the request
    inst->getLatencyFrames();
    CHECK(cb->restarts > restartsBefore);
    REQUIRE_OK(inst->deactivate());
    REQUIRE_EQ(inst->getBusCount(SQ_FALSE), 2u);
    sq_plugin_bus_info bi{};
    bi.struct_size = sizeof bi;
    REQUIRE_OK(inst->getBusInfo(SQ_FALSE, 1, &bi));
    CHECK_EQ(std::string(bi.name), std::string("Aux Out"));
    CHECK(bi.flags & SQ_BUS_AUX);
    CHECK(!(bi.flags & SQ_BUS_ACTIVE));
    REQUIRE_OK(inst->getBusInfo(SQ_TRUE, 1, &bi));
    CHECK(!(bi.flags & SQ_BUS_ACTIVE));  // the sidechain keeps its (default) inactive state
    REQUIRE_OK(inst->setBusActive(SQ_FALSE, 1, SQ_TRUE));
    sq_plugin_desc live{};
    live.struct_size = sizeof live;
    REQUIRE_OK(inst->getDesc(&live));
    REQUIRE_OK(inst->activate(&setup));
    {
      constexpr uint32_t n = 64;
      std::vector<float> inL(n, 0.5f), inR(n, 0.5f), oL(n), oR(n), aL(n, -1.0f), aR(n, -1.0f);
      float* inCh[2] = {inL.data(), inR.data()};
      float* outCh[2] = {oL.data(), oR.data()};
      float* auxCh[2] = {aL.data(), aR.data()};
      sq_audio_block ins[1] = {{inCh, 2, n, 0}};
      sq_audio_block outs[2] = {{outCh, 2, n, 0}, {auxCh, 2, n, 0}};
      sq_plugin_process pr{};
      pr.struct_size = sizeof pr;
      pr.frames = n;
      pr.inputs = ins;
      pr.input_bus_count = 1;
      pr.outputs = outs;
      pr.output_bus_count = 2;
      for (int i = 0; i < 3; ++i) REQUIRE(SQ_SUCCEEDED(inst->process(&pr)));
      CHECK(near(oL[63], 0.5, 1e-5));
      CHECK(near(aL[63], 0.5, 1e-5));
      CHECK(near(aR[0], 0.5, 1e-5));
    }

    // ---- plugin-initiated requests: delivered by the next main-thread call, but never to the
    // host callback from inside the instance's destructor ----
    int dirty0 = cb->dirty;
    REQUIRE_OK(inst->getParamValue(kDirtyProbe, &v));  // the controller calls setDirty here
    inst->getLatencyFrames();
    CHECK_EQ(cb->dirty, dirty0 + 1);
    REQUIRE_OK(inst->getParamValue(kDirtyProbe, &v));  // pending again, instance still active
    int dirty1 = cb->dirty;
    inst = nullptr;  // destroys the active instance
    CHECK_EQ(cb->dirty, dirty1);
  }
  CHECK(loader.unload("pluginhost-vst3") == SQ_OK);
}
