// pluginhost-vst3: OPT-IN smoke test against VST3 plugins installed on this machine.
// Set SQ_VST3_SMOKE_DIR to a folder (e.g. "C:\Program Files\Common Files\VST3"); every .vst3
// below it is scanned, and every audio module class is instantiated, activated, fed a few blocks
// of audio + a note, state-saved/-restored and destroyed, in-process. Without the variable the case
// is reported SKIPPED (exit 77), never passed: third-party plugins are not part of the build.
#include <cmath>
#include <cstdio>
#include <string>
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

void collect(const std::string& dir, int depth, std::vector<std::string>& out) {
  if (depth > 4) return;
  std::vector<std::pair<std::string, bool>> entries;
  sq_fs_list_dir(
      dir.c_str(),
      [](void* user, const char* name, sq_bool isDir) -> sq_bool {
        static_cast<std::vector<std::pair<std::string, bool>>*>(user)->emplace_back(name, isDir != 0);
        return SQ_TRUE;
      },
      &entries);
  for (const auto& [name, isDir] : entries) {
    std::string full = sq::joinPath(dir, name);
    if (name.size() > 5 && sq_ascii_casecmp(name.substr(name.size() - 5).c_str(), ".vst3") == 0) out.push_back(full);
    else if (isDir) collect(full, depth + 1, out);
  }
}

}  // namespace

SQ_TEST("pluginhost-vst3: installed plugins smoke (opt-in via SQ_VST3_SMOKE_DIR)") {
  sq::Buffer env;
  if (sq_env_get("SQ_VST3_SMOKE_DIR", env.out()) != SQ_OK || env.str().empty())
    SQ_SKIP("SQ_VST3_SMOKE_DIR not set (opt-in test against locally installed VST3 plugins)");
  std::vector<std::string> files;
  collect(env.str(), 0, files);
  if (files.empty()) SQ_SKIP("no .vst3 found under SQ_VST3_SMOKE_DIR");

  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("pluginhost-vst3") + sq_dylib_suffix());
  REQUIRE(loader.load(mod).ok());
  int instantiated = 0;
  {
    auto fmt = sq::createInstance<sq::IPluginFormat>(&reg, sq::pluginhost::kVst3FormatClassId);
    REQUIRE(fmt);
    for (const std::string& f : files) {
      std::printf("  scanning %s\n", f.c_str());
      std::fflush(stdout);
      auto sink = sq::Ptr<Sink>::adopt(new Sink());
      sq_result sr = fmt->scanFile(f.c_str(), sink.get());
      std::printf("  scan %s -> %d, %zu class(es)\n", f.c_str(), static_cast<int>(sr), sink->found.size());
      for (const sq_plugin_desc& d : sink->found) {
        if (d.status != SQ_PLUGIN_STATUS_OK) continue;
        sq::Ptr<sq::IPluginInstance> inst;
        sq_result ir = fmt->instantiate(&d, nullptr, inst.put());
        std::printf("    %-40s %-24s in %u out %u aux %u midi %u/%u flags 0x%x -> %d\n", d.name, d.vendor, d.audio_inputs, d.audio_outputs,
                    d.aux_inputs, d.midi_inputs, d.midi_outputs, d.flags, static_cast<int>(ir));
        std::fflush(stdout);
        if (SQ_FAILED(ir) || !inst) continue;
        sq_plugin_setup setup{sizeof(sq_plugin_setup), 48000.0, 1, 512, 0};
        CHECK_OK(inst->activate(&setup));
        const uint32_t n = 512;
        std::vector<std::vector<float>> bufs(32, std::vector<float>(n, 0.0f));
        std::vector<float*> ptrs(32);
        for (size_t i = 0; i < ptrs.size(); ++i) ptrs[i] = bufs[i].data();
        uint32_t inBuses = inst->getBusCount(SQ_TRUE), outBuses = inst->getBusCount(SQ_FALSE);
        std::vector<sq_audio_block> ins, outs;
        size_t next = 0;
        auto blockFor = [&](sq_bool input, uint32_t b) {
          sq_plugin_bus_info bi{};
          bi.struct_size = sizeof bi;
          inst->getBusInfo(input, b, &bi);
          uint32_t ch = bi.channels > 8 ? 8 : bi.channels;
          sq_audio_block blk{ptrs.data() + (next < 24 ? next : 24), ch, n, 0};
          next += ch;
          return blk;
        };
        for (uint32_t b = 0; b < inBuses; ++b) ins.push_back(blockFor(SQ_TRUE, b));
        for (uint32_t b = 0; b < outBuses; ++b) outs.push_back(blockFor(SQ_FALSE, b));
        sq_midi_event note{0, {0x90, 60, 100, 0}, 3, nullptr};
        bool finite = true;
        for (int blk = 0; blk < 16; ++blk) {
          for (uint32_t c = 0; c < 8; ++c)
            for (uint32_t i = 0; i < n; ++i) bufs[c][i] = 0.1f * std::sin(0.05f * static_cast<float>(i + blk * n) + static_cast<float>(c));
          sq_plugin_process pr{};
          pr.struct_size = sizeof pr;
          pr.frames = n;
          pr.inputs = ins.data();
          pr.input_bus_count = inBuses;
          pr.outputs = outs.data();
          pr.output_bus_count = outBuses;
          pr.midi_in = blk == 0 ? &note : nullptr;
          pr.midi_in_count = blk == 0 ? 1 : 0;
          CHECK(SQ_SUCCEEDED(inst->process(&pr)));
          for (const sq_audio_block& o : outs)
            for (uint32_t c = 0; c < o.channel_count; ++c)
              for (uint32_t i = 0; i < n; ++i) finite = finite && std::isfinite(o.channels[c][i]);
        }
        CHECK(finite);
        auto st = sq::makeMemoryStream();
        CHECK_OK(inst->saveState(st.get()));
        st->seek(0, SQ_SEEK_SET, nullptr);
        CHECK_OK(inst->loadState(st.get()));
        std::printf("      latency %u tail %u params %u editor %d\n", inst->getLatencyFrames(), inst->getTailFrames(), inst->getParamCount(),
                    static_cast<int>(inst->hasEditor()));
        CHECK_OK(inst->deactivate());
        ++instantiated;
      }
    }
  }
  std::printf("  %d plugin class(es) exercised\n", instantiated);
  CHECK(loader.unload("pluginhost-vst3") == SQ_OK);
}
