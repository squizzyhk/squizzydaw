// time-stretch through the real module loader + ITimeStretcher.
#include <cmath>
#include <string>
#include <vector>

#include "dsp_test_signals.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/interfaces/dsp.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
std::string modulePath(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}
}  // namespace

SQ_TEST("time-stretch service: loads, stretches, unloads") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  auto lr = loader.load(modulePath("time-stretch"));
  REQUIRE(lr.ok());
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE(reg.findClass(&sq::dsp::kTimeStretchClassId, &ci) == SQ_OK);
  CHECK_EQ(std::string(ci.category), std::string("TimeStretch"));
  {
    auto ts = sq::createInstance<sq::ITimeStretcher>(&reg, sq::dsp::kTimeStretchClassId);
    REQUIRE(ts);
    CHECK(ts->setRatios(1.0, 0.0) == SQ_E_FAIL);  // not configured
    sq_stretch_config cfg{};
    cfg.struct_size = sizeof cfg;
    cfg.sample_rate = 48000.0;
    cfg.channels = 2;
    cfg.max_block_frames = 256;
    cfg.quality = SQ_STRETCH_QUALITY_STANDARD;
    cfg.max_time_ratio = 2.0;
    REQUIRE(ts->configure(&cfg) == SQ_OK);
    REQUIRE(ts->setRatios(2.0, 0.0) == SQ_OK);
    CHECK(ts->getLatencyFrames() > 0);

    const std::vector<float> l = sqdsptest::sine(500.0, 48000.0, 24064, 0.5f);
    const std::vector<float> r = sqdsptest::sine(750.0, 48000.0, 24064, 0.5f);
    std::vector<float> outL, outR, bl(256), br(256);
    float* outPtrs[2] = {bl.data(), br.data()};
    for (size_t pos = 0; pos < l.size(); pos += 256) {
      const float* inPtrs[2] = {l.data() + pos, r.data() + pos};
      sq_audio_block in{const_cast<float* const*>(inPtrs), 2, 256, static_cast<sq_sample_time>(pos)};
      REQUIRE(ts->push(&in) == SQ_OK);
      for (;;) {
        sq_audio_block out{outPtrs, 2, 256, 0};
        uint32_t produced = 0;
        REQUIRE(SQ_SUCCEEDED(ts->pull(&out, &produced)));
        outL.insert(outL.end(), bl.begin(), bl.begin() + produced);
        outR.insert(outR.end(), br.begin(), br.begin() + produced);
        if (produced < 256) break;
      }
    }
    REQUIRE(ts->flush() == SQ_OK);
    sq_result last = SQ_OK;
    for (int i = 0; i < 1000 && last != SQ_S_FALSE; ++i) {
      sq_audio_block out{outPtrs, 2, 256, 0};
      uint32_t produced = 0;
      last = ts->pull(&out, &produced);
      outL.insert(outL.end(), bl.begin(), bl.begin() + produced);
      outR.insert(outR.end(), br.begin(), br.begin() + produced);
    }
    CHECK(last == SQ_S_FALSE);
    CHECK(std::fabs(double(outL.size()) - 2.0 * 24064) <= 2.0);
    CHECK(std::fabs(sqdsptest::zeroCrossingFreq(outL, 48000.0, 6000, 42000) - 500.0) < 2.0);
    CHECK(std::fabs(sqdsptest::zeroCrossingFreq(outR, 48000.0, 6000, 42000) - 750.0) < 3.0);
    // wrong channel count is rejected
    const float* one[1] = {l.data()};
    sq_audio_block bad{const_cast<float* const*>(one), 1, 16, 0};
    ts->reset();
    CHECK(ts->push(&bad) == SQ_E_INVALIDARG);
    // a null channel pointer is rejected, not dereferenced
    const float* holey[2] = {l.data(), nullptr};
    sq_audio_block holeyBlock{const_cast<float* const*>(holey), 2, 16, 0};
    CHECK(ts->push(&holeyBlock) == SQ_E_INVALIDARG);
    float* outHoley[2] = {bl.data(), nullptr};
    sq_audio_block outBlock{outHoley, 2, 16, 0};
    uint32_t produced = 7;
    CHECK(ts->pull(&outBlock, &produced) == SQ_E_INVALIDARG);
    CHECK_EQ(produced, 0u);
  }
  CHECK(loader.unload("time-stretch") == SQ_OK);
}
