// analysis-stems: tensor type, model spec parsing, chunked separation with a fake model, the
// "no runtime -> unavailable" path, the dynamic ONNX Runtime seam, and a fake INNRuntime service.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dsp_test_signals.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/dsp.h"
#include "sq/nn/tensor.hpp"
#include "sq/platform.hpp"
#include "sqtest.hpp"
#include "stem_separator.hpp"

namespace {

// Fake model: output stem s = input * gain[s] (shape [1, S, C, L]).
class FakeModel final : public sq::Object<FakeModel, sq::INNModel> {
 public:
  std::vector<float> gains{0.25f, 0.5f, 0.25f};
  uint32_t channels = 2, chunk = 4096;
  int runs = 0;
  uint32_t SQ_CALL getInputCount() override { return 1; }
  sq_result SQ_CALL getInputInfo(uint32_t i, sq_tensor_info* out) override {
    if (i || !out) return SQ_E_NOTFOUND;
    *out = sq_tensor_info{};
    out->struct_size = sizeof *out;
    std::strcpy(out->name, "mix");
    out->dtype = SQ_TENSOR_F32;
    out->rank = 3;
    out->shape[0] = 1, out->shape[1] = channels, out->shape[2] = -1;
    return SQ_OK;
  }
  uint32_t SQ_CALL getOutputCount() override { return 1; }
  sq_result SQ_CALL getOutputInfo(uint32_t i, sq_tensor_info* out) override { return getInputInfo(i, out); }
  sq_result SQ_CALL run(const sq_tensor* in, uint32_t ni, sq_tensor* out, uint32_t no) override {
    if (ni != 1 || no != 1 || in[0].dtype != SQ_TENSOR_F32 || in[0].rank != 3) return SQ_E_INVALIDARG;
    ++runs;
    const int64_t C = in[0].shape[1], L = in[0].shape[2], S = static_cast<int64_t>(gains.size());
    const uint64_t need = static_cast<uint64_t>(S * C * L) * 4;
    out[0].dtype = SQ_TENSOR_F32;
    out[0].rank = 4;
    out[0].shape[0] = 1, out[0].shape[1] = S, out[0].shape[2] = C, out[0].shape[3] = L;
    if (out[0].byte_size < need) {
      out[0].byte_size = need;
      return SQ_E_BUFFER_TOO_SMALL;
    }
    const float* x = static_cast<const float*>(in[0].data);
    float* y = static_cast<float*>(out[0].data);
    for (int64_t s = 0; s < S; ++s)
      for (int64_t i = 0; i < C * L; ++i) y[s * C * L + i] = x[i] * gains[static_cast<size_t>(s)];
    return SQ_OK;
  }
};

class FakeRuntime final : public sq::Object<FakeRuntime, sq::INNRuntime> {
 public:
  sq_result SQ_CALL getRuntimeInfo(char* buf, uint32_t size, uint32_t* needed) override {
    const char t[] = "fake";
    if (needed) *needed = sizeof t;
    if (!buf || size < sizeof t) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, t, sizeof t);
    return SQ_OK;
  }
  sq_result SQ_CALL loadModel(const char* path, sq::INNModel** out) override {
    if (!out || !sq_fs_is_file(path)) return SQ_E_NOTFOUND;
    *out = new FakeModel();
    return SQ_OK;
  }
  sq_result SQ_CALL setThreadCount(uint32_t) override { return SQ_OK; }
};

const char* kSpec =
    R"({"format":"sq-stems-1","sample_rate":44100,"channels":2,"stems":["vocals","drums","other"],)"
    R"("chunk_frames":4096,"overlap_frames":512})";

std::string modulePath(const char* name) {
  return sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(name) + sq_dylib_suffix());
}

std::string tempModel() {
  std::string dir = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sq_stems_test");
  sq_fs_create_dirs(dir.c_str());
  std::string model = sq::joinPath(dir, "fake.onnx");
  sq::writeFile(model, "not a real model");
  sq::writeFile(model + ".json", kSpec);
  return model;
}

}  // namespace

SQ_TEST("stems: tensor type") {
  sq::nn::Tensor t = sq::nn::Tensor::of<float>({1, 2, 3});
  CHECK_EQ(t.rank(), 3u);
  CHECK_EQ(t.elementCount(), uint64_t(6));
  CHECK_EQ(t.byteSize(), uint64_t(24));
  REQUIRE(t.data<float>() != nullptr);
  CHECK(t.data<int64_t>() == nullptr);
  t.at({0, 1, 2}) = 5.0f;
  CHECK_EQ(t.data<float>()[5], 5.0f);
  CHECK_EQ(t.shapeString(), std::string("[1,2,3]"));
  sq_tensor v = t.view();
  CHECK_EQ(v.rank, 3u);
  CHECK_EQ(v.shape[2], int64_t(3));
  CHECK(v.byte_size >= 24);
  v.shape[1] = 3, v.shape[2] = 2;
  CHECK(t.adoptShape(v));
  CHECK_EQ(t.shapeString(), std::string("[1,3,2]"));
  v.shape[2] = 100;
  CHECK(!t.adoptShape(v));
  sq::nn::Tensor d(SQ_TENSOR_I64, {4, -1});
  CHECK_EQ(d.elementCount(), uint64_t(0));
}

SQ_TEST("stems: model spec parsing") {
  sq::stems::ModelSpec s;
  std::string err;
  REQUIRE(sq::stems::parseModelSpec(kSpec, s, err));
  CHECK_EQ(s.stems.size(), size_t(3));
  CHECK_EQ(s.stems[1], std::string("drums"));
  CHECK_EQ(s.chunkFrames, 4096u);
  CHECK(!sq::stems::parseModelSpec("[]", s, err));
  CHECK(!sq::stems::parseModelSpec(R"({"format":"other","stems":["a"],"chunk_frames":100})", s, err));
  CHECK(!sq::stems::parseModelSpec(R"({"stems":["a"],"chunk_frames":100,"overlap_frames":60})", s, err));
}

SQ_TEST("stems: chunked separation reconstructs exactly across chunk seams") {
  sq::stems::ModelSpec spec;
  std::string err;
  REQUIRE(sq::stems::parseModelSpec(kSpec, spec, err));
  FakeModel* m = new FakeModel();
  sq::Ptr<sq::INNModel> model = sq::Ptr<sq::INNModel>::adopt(m);
  const size_t n = 4096 * 5 + 777;
  std::vector<std::vector<float>> in = {sqdsptest::sine(440.0, 44100.0, n, 0.5f), sqdsptest::sine(660.0, 44100.0, n, 0.5f)};
  sq::stems::Separated out;
  REQUIRE(sq::stems::separate(model.get(), spec, in, 0, out) == SQ_OK);
  CHECK(m->runs >= 6);
  REQUIRE(out.names.size() == 3);
  CHECK_EQ(out.names[0], std::string("vocals"));
  double err0 = 0.0;
  for (size_t s = 0; s < 3; ++s)
    for (size_t c = 0; c < 2; ++c) {
      REQUIRE(out.audio[s][c].size() == n);
      for (size_t i = 0; i < n; ++i) err0 = std::max(err0, double(std::fabs(out.audio[s][c][i] - m->gains[s] * in[c][i])));
    }
  CHECK(err0 < 1e-6);
  // merge to 2 stems: vocals + accompaniment (= drums + other)
  REQUIRE(sq::stems::separate(model.get(), spec, in, 2, out) == SQ_OK);
  REQUIRE(out.names.size() == 2);
  CHECK_EQ(out.names[1], std::string("accompaniment"));
  CHECK(std::fabs(out.audio[1][0][1000] - 0.75f * in[0][1000]) < 1e-6f);
  // mono input through a stereo model
  std::vector<std::vector<float>> mono = {in[0]};
  REQUIRE(sq::stems::separate(model.get(), spec, mono, 0, out) == SQ_OK);
  REQUIRE(out.audio[0].size() == 1);
  CHECK(std::fabs(out.audio[1][0][5000] - 0.5f * mono[0][5000]) < 1e-6f);
}

SQ_TEST("analysis-stems service: unavailable without a runtime, works with a registered one") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  REQUIRE(loader.load(modulePath("analysis-stems")).ok());
  {
    sq::Ptr<sq::INNRuntime> rt;
    const bool haveOrt = host.queryService(&sq::INNRuntime::iid, rt.putVoid()) == SQ_OK;
    std::printf("  ONNX Runtime present on this machine: %s\n", haveOrt ? "yes" : "no");
    auto an = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kStemSeparatorClassId);
    REQUIRE(an);
    sq_analyzer_info info{};
    info.struct_size = sizeof info;
    REQUIRE(an->getInfo(&info) == SQ_OK);
    CHECK_EQ(std::string(info.kind), std::string("stems"));
    if (!haveOrt) CHECK(an->prepare(44100.0, 2, 512, nullptr) == SQ_E_UNSUPPORTED);
  }
  CHECK(loader.unload("analysis-stems") == SQ_OK);

  // A runtime registered by someone else (here: the test) is used by the separator.
  sq::Ptr<sq::INNRuntime> fake = sq::Ptr<sq::INNRuntime>::adopt(new FakeRuntime());
  REQUIRE(host.registerService(&sq::INNRuntime::iid, fake.get()) == SQ_OK);
  REQUIRE(loader.load(modulePath("analysis-stems")).ok());
  {
    sq::Ptr<sq::INNRuntime> rt;
    REQUIRE(host.queryService(&sq::INNRuntime::iid, rt.putVoid()) == SQ_OK);
    CHECK(rt.get() == fake.get());  // the module did not replace it
    auto an = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kStemSeparatorClassId);
    REQUIRE(an);
    auto opts = sq::makeAttributes();
    CHECK(an->prepare(44100.0, 2, 512, opts.get()) == SQ_E_UNSUPPORTED);  // no model configured
    const std::string model = tempModel();
    opts->setString("model", model.c_str());
    CHECK(an->prepare(48000.0, 2, 512, opts.get()) == SQ_E_UNSUPPORTED);  // sample-rate mismatch
    REQUIRE(an->prepare(44100.0, 2, 512, opts.get()) == SQ_OK);
    const size_t n = 10000;
    auto l = sqdsptest::sine(220.0, 44100.0, n, 0.5f), r = sqdsptest::sine(330.0, 44100.0, n, 0.5f);
    for (size_t pos = 0; pos < n; pos += 512) {
      uint32_t k = static_cast<uint32_t>(std::min<size_t>(512, n - pos));
      float* ch[2] = {l.data() + pos, r.data() + pos};
      sq_audio_block b{ch, 2, k, static_cast<sq_sample_time>(pos)};
      REQUIRE(an->process(&b) == SQ_OK);
    }
    sq::Ptr<sq::IAnalysisResult> res;
    REQUIRE(an->finish(res.put()) == SQ_OK);
    REQUIRE(res->getStemCount() == 3u);
    CHECK_EQ(res->getEventCount(), 0u);
    char name[32];
    uint32_t needed = 0;
    REQUIRE(res->getStemName(1, name, sizeof name, &needed) == SQ_OK);
    CHECK_EQ(std::string(name), std::string("drums"));
    std::vector<float> a(600), b(600);
    float* dst[2] = {a.data(), b.data()};
    sq_audio_block blk{dst, 2, 600, 0};
    uint32_t got = 0;
    REQUIRE(res->readStem(1, 9500, &blk, &got) == SQ_OK);
    CHECK_EQ(got, 500u);
    CHECK(std::fabs(a[10] - 0.5f * l[9510]) < 1e-6f);
    CHECK(std::fabs(b[10] - 0.5f * r[9510]) < 1e-6f);
    CHECK_EQ(a[550], 0.0f);
    CHECK(res->readStem(1, 20000, &blk, &got) == SQ_S_FALSE);
    CHECK(res->readStem(7, 0, &blk, &got) == SQ_E_NOTFOUND);
  }
  CHECK(loader.unload("analysis-stems") == SQ_OK);
  host.unregisterService(&sq::INNRuntime::iid, fake.get());
}

// Opt-in integration with a real ONNX Runtime: set SQ_TEST_ORT_LIB (path to onnxruntime.dll) and
// SQ_TEST_ORT_MODEL (an sq-stems-1 model whose sidecar declares 3 stems with gains 0.25/0.5/0.25,
// e.g. Unsqueeze(mix, 1) * [0.25, 0.5, 0.25]). Skipped (passes) when unset.
SQ_TEST("analysis-stems service: real ONNX Runtime (opt-in)") {
  sq::Buffer lib, modelPath;
  if (sq_env_get("SQ_TEST_ORT_LIB", lib.out()) != SQ_OK || sq_env_get("SQ_TEST_ORT_MODEL", modelPath.out()) != SQ_OK ||
      lib.str().empty() || modelPath.str().empty()) {
    std::printf("  skipped: SQ_TEST_ORT_LIB / SQ_TEST_ORT_MODEL not set\n");
    return;
  }
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  settings.setString("Services.analysis-stems.OnnxRuntimePath", lib.str().c_str());
  REQUIRE(loader.load(modulePath("analysis-stems")).ok());
  {
    sq::Ptr<sq::INNRuntime> rt;
    REQUIRE(host.queryService(&sq::INNRuntime::iid, rt.putVoid()) == SQ_OK);
    char info[256];
    uint32_t needed = 0;
    REQUIRE(rt->getRuntimeInfo(info, sizeof info, &needed) == SQ_OK);
    std::printf("  runtime: %s\n", info);
    REQUIRE(rt->setThreadCount(1) == SQ_OK);
    sq::Ptr<sq::INNModel> m;
    REQUIRE(rt->loadModel(modelPath.str().c_str(), m.put()) == SQ_OK);
    REQUIRE(m->getInputCount() == 1u);
    sq_tensor_info ti{};
    ti.struct_size = sizeof ti;
    REQUIRE(m->getInputInfo(0, &ti) == SQ_OK);
    std::printf("  input %s rank %u [%lld,%lld,%lld]\n", ti.name, ti.rank, (long long)ti.shape[0], (long long)ti.shape[1],
                (long long)ti.shape[2]);
    CHECK_EQ(ti.dtype, SQ_TENSOR_F32);
    CHECK_EQ(ti.shape[2], int64_t(-1));
    // too-small output buffer reports the needed size
    sq::nn::Tensor x(SQ_TENSOR_F32, {1, 2, 8});
    for (int i = 0; i < 16; ++i) x.data<float>()[i] = static_cast<float>(i);
    sq::nn::Tensor small(SQ_TENSOR_F32, {4});
    sq_tensor tin = x.view(), tout = small.view();
    CHECK(m->run(&tin, 1, &tout, 1) == SQ_E_BUFFER_TOO_SMALL);
    CHECK_EQ(tout.byte_size, uint64_t(3 * 2 * 8 * 4));
    sq::nn::Tensor y(SQ_TENSOR_F32, {1, 3, 2, 8});
    tout = y.view();
    REQUIRE(m->run(&tin, 1, &tout, 1) == SQ_OK);
    CHECK_EQ(tout.rank, 4u);
    CHECK_EQ(y.data<float>()[16 + 5], 0.5f * 5.0f);

    auto an = sq::createInstance<sq::IAnalyzer>(&reg, sq::dsp::kStemSeparatorClassId);
    auto opts = sq::makeAttributes();
    opts->setString("model", modelPath.str().c_str());
    REQUIRE(an->prepare(44100.0, 2, 512, opts.get()) == SQ_OK);
    auto l = sqdsptest::sine(220.0, 44100.0, 9000, 0.5f), r = sqdsptest::sine(330.0, 44100.0, 9000, 0.5f);
    float* ch[2] = {l.data(), r.data()};
    sq_audio_block b{ch, 2, 9000, 0};
    REQUIRE(an->process(&b) == SQ_OK);
    sq::Ptr<sq::IAnalysisResult> res;
    REQUIRE(an->finish(res.put()) == SQ_OK);
    REQUIRE(res->getStemCount() == 3u);
    std::vector<float> a(100), c(100);
    float* dst[2] = {a.data(), c.data()};
    sq_audio_block blk{dst, 2, 100, 0};
    uint32_t got = 0;
    REQUIRE(res->readStem(1, 4000, &blk, &got) == SQ_OK);
    CHECK(std::fabs(a[7] - 0.5f * l[4007]) < 1e-6f);
    CHECK(std::fabs(c[7] - 0.5f * r[4007]) < 1e-6f);
  }
  CHECK(loader.unload("analysis-stems") == SQ_OK);
}

SQ_TEST("analysis-stems service: dynamic ONNX Runtime seam rejects a non-ORT library") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::app::ModuleLoader loader(host, reg);
  // Point the seam at a library that exists but is not ONNX Runtime.
  // A real shared library that is not ONNX Runtime: the stems module itself (exists on every OS).
  const std::string bogus = modulePath("analysis-stems");
  settings.setString("Services.analysis-stems.OnnxRuntimePath", bogus.c_str());
  REQUIRE(loader.load(modulePath("analysis-stems")).ok());
  {
    sq::Ptr<sq::INNRuntime> rt;
    REQUIRE(host.queryService(&sq::INNRuntime::iid, rt.putVoid()) == SQ_OK);
    char info[512];
    uint32_t needed = 0;
    CHECK(rt->getRuntimeInfo(info, sizeof info, &needed) == SQ_S_FALSE);
    std::printf("  runtime info: %s\n", info);
    CHECK(std::string(info).rfind("unavailable", 0) == 0);
    sq::Ptr<sq::INNModel> m;
    CHECK(rt->loadModel(tempModel().c_str(), m.put()) == SQ_E_UNSUPPORTED);
    CHECK(rt->loadModel("package://x/model.onnx", m.put()) == SQ_E_UNSUPPORTED);
    CHECK(rt->setThreadCount(2) == SQ_OK);
  }
  CHECK(loader.unload("analysis-stems") == SQ_OK);
  sq::Ptr<sq::INNRuntime> after;
  CHECK(host.queryService(&sq::INNRuntime::iid, after.putVoid()) != SQ_OK);  // unregistered at shutdown
}
