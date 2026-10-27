// analysis-stems: hostile model sidecars, misbehaving models and result readers.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sq/engine/simd/analysis_result.hpp"
#include "sq/interfaces/dsp.h"
#include "sqtest.hpp"
#include "stem_separator.hpp"

namespace {

// A model that lies: reports a valid output shape but a wrong rank / dtype, or fails.
class LyingModel final : public sq::Object<LyingModel, sq::INNModel> {
 public:
  int mode = 0;  // 0 wrong rank, 1 wrong dtype, 2 run error, 3 shape mismatch
  uint32_t SQ_CALL getInputCount() override { return 1; }
  sq_result SQ_CALL getInputInfo(uint32_t, sq_tensor_info*) override { return SQ_E_NOTIMPL; }
  uint32_t SQ_CALL getOutputCount() override { return 1; }
  sq_result SQ_CALL getOutputInfo(uint32_t, sq_tensor_info*) override { return SQ_E_NOTIMPL; }
  sq_result SQ_CALL run(const sq_tensor*, uint32_t, sq_tensor* out, uint32_t) override {
    if (mode == 2) return SQ_E_FAIL;
    out[0].dtype = mode == 1 ? SQ_TENSOR_I32 : SQ_TENSOR_F32;
    out[0].rank = mode == 0 ? 3 : 4;
    out[0].shape[0] = 1, out[0].shape[1] = 2, out[0].shape[2] = 2, out[0].shape[3] = mode == 3 ? 7 : 256;
    return SQ_OK;
  }
};

}  // namespace

SQ_TEST("stems adversarial: sidecar with absurd sizes or non-string stems is rejected") {
  sq::stems::ModelSpec s;
  std::string err;
  const char* bad[] = {
      R"({"stems":["a","b"],"chunk_frames":-1,"overlap_frames":0})",
      R"({"stems":["a","b"],"chunk_frames":4294967295,"overlap_frames":0})",
      R"({"stems":["a","b"],"chunk_frames":1e12,"overlap_frames":0})",
      R"({"stems":["a","b"],"chunk_frames":4096,"overlap_frames":-5})",
      R"({"stems":["a","b"],"chunk_frames":4096,"channels":-1})",
      R"({"stems":["a","b"],"chunk_frames":4096,"channels":4294967298})",
      R"({"stems":[1,2],"chunk_frames":4096})",
      R"({"stems":["a",""],"chunk_frames":4096})",
      R"({"stems":["a","b"],"chunk_frames":4096,"sample_rate":1e309})",
      R"({"format":5,"stems":["a"],"chunk_frames":4096})",
  };
  for (const char* j : bad) {
    const bool ok = sq::stems::parseModelSpec(j, s, err);
    if (ok) std::printf("  accepted: %s (chunk %u overlap %u ch %u)\n", j, s.chunkFrames, s.overlapFrames, s.channels);
    CHECK(!ok);
  }
  REQUIRE(sq::stems::parseModelSpec(R"({"stems":["a","b"],"chunk_frames":4096,"overlap_frames":256})", s, err));
}

SQ_TEST("stems adversarial: a model returning the wrong tensor is reported, not trusted") {
  sq::stems::ModelSpec spec;
  std::string err;
  REQUIRE(sq::stems::parseModelSpec(R"({"stems":["a","b"],"channels":2,"chunk_frames":256,"overlap_frames":32})", spec, err));
  std::vector<std::vector<float>> in(2, std::vector<float>(1000, 0.1f));
  for (int mode = 0; mode < 4; ++mode) {
    auto* m = new LyingModel();
    m->mode = mode;
    sq::Ptr<sq::INNModel> model = sq::Ptr<sq::INNModel>::adopt(m);
    sq::stems::Separated out;
    const sq_result r = sq::stems::separate(model.get(), spec, in, 0, out);
    CHECK(SQ_FAILED(r));
    CHECK(out.names.empty());
  }
  sq::stems::Separated out;
  CHECK(sq::stems::separate(nullptr, spec, in, 0, out) == SQ_E_INVALIDARG);
  // ragged input channels are rejected instead of read out of bounds
  std::vector<std::vector<float>> ragged = {std::vector<float>(1000), std::vector<float>(10)};
  auto* m = new LyingModel();
  sq::Ptr<sq::INNModel> model = sq::Ptr<sq::INNModel>::adopt(m);
  CHECK(sq::stems::separate(model.get(), spec, ragged, 0, out) == SQ_E_INVALIDARG);
}

SQ_TEST("stems adversarial: result reader validates its arguments") {
  sq::Ptr<sq::dspkit::AnalysisResult> res = sq::Ptr<sq::dspkit::AnalysisResult>::adopt(new sq::dspkit::AnalysisResult());
  res->stems.push_back({"x", {std::vector<float>(10, 1.0f)}});
  float a[4];
  float* ch[2] = {a, nullptr};
  sq_audio_block blk{ch, 2, 4, 0};
  uint32_t got = 99;
  CHECK(res->readStem(0, -1, &blk, &got) == SQ_E_INVALIDARG);
  CHECK_EQ(got, 0u);
  CHECK(res->readStem(0, 8, &blk, &got) == SQ_OK);  // null channel pointer is skipped
  CHECK_EQ(got, 2u);
  CHECK_EQ(a[0], 1.0f);
  CHECK_EQ(a[3], 0.0f);
  sq_audio_block noArray{nullptr, 2, 0, 0};
  CHECK(res->readStem(0, 0, &noArray, &got) == SQ_E_INVALIDARG);
  char small[1];
  uint32_t need = 0;
  CHECK(res->getStemName(0, small, sizeof small, &need) == SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(need, 2u);
  sq_analysis_event e{};
  e.struct_size = 4;
  CHECK(res->getEvent(0, &e) == SQ_E_INVALIDARG);
}
