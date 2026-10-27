// analysis-stems service:
//  - INNRuntime (registered with the host) backed by ONNX Runtime, *only if* an ONNX Runtime
//    shared library is found at INIT: setting Services.analysis-stems.OnnxRuntimePath, env
//    SQ_ONNXRUNTIME_PATH, or onnxruntime.dll next to the executable / in services/. It is opened
//    lazily on first use. Nothing is registered if another module already registered INNRuntime.
//  - IAnalyzer kind "stems" (kStemSeparatorClassId): uses whatever INNRuntime the host has
//    (prepare() fails with SQ_E_UNSUPPORTED when there is none = "unavailable").
//    Options: "model" (string path; default setting Services.analysis-stems.Model),
//             "stems" (int, merge down to this many stems). Model convention: stem_separator.hpp.
#include <cstdlib>
#include <string>
#include <vector>

#include "ort_runtime.hpp"
#include "sq/engine/simd/analysis_result.hpp"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"
#include "stem_separator.hpp"

namespace {

constexpr const char* kModelKey = "Services.analysis-stems.Model";
constexpr const char* kOrtPathKey = "Services.analysis-stems.OnnxRuntimePath";

std::string settingString(const char* key) {
  sq::IHostContext* h = sq::host();
  if (!h) return {};
  uint32_t needed = 0;
  if (h->getSettings()->getString(key, nullptr, 0, &needed) != SQ_E_BUFFER_TOO_SMALL || needed == 0) return {};
  std::string s(needed, '\0');
  if (h->getSettings()->getString(key, s.data(), needed, &needed) != SQ_OK) return {};
  s.resize(needed - 1);
  return s;
}

std::string attrString(sq::IAttributes* a, const char* key) {
  if (!a || a->getType(key) != SQ_VALUE_STRING) return {};
  uint32_t needed = 0;
  a->getString(key, nullptr, 0, &needed);
  if (!needed) return {};
  std::string s(needed, '\0');
  if (a->getString(key, s.data(), needed, &needed) != SQ_OK) return {};
  s.resize(needed - 1);
  return s;
}

class StemAnalyzer final : public sq::Object<StemAnalyzer, sq::IAnalyzer> {
 public:
  sq_result SQ_CALL getInfo(sq_analyzer_info* out) override {
    return sq::dspkit::fillInfo(out, "stems", "Squizzy Stem Separator (neural)", SQ_ANALYZER_OFFLINE);
  }
  sq_result SQ_CALL prepare(double sampleRate, uint32_t channels, uint32_t maxBlockFrames, sq::IAttributes* options) override {
    if (!(sampleRate >= 4000.0) || channels == 0 || maxBlockFrames == 0) return SQ_E_INVALIDARG;
    model_ = nullptr;
    sq::IHostContext* h = sq::host();
    if (!h) return SQ_E_FAIL;
    sq::Ptr<sq::INNRuntime> rt;
    if (h->queryService(&sq::INNRuntime::iid, rt.putVoid()) != SQ_OK || !rt) return SQ_E_UNSUPPORTED;
    std::string path = attrString(options, "model");
    if (path.empty()) path = settingString(kModelKey);
    if (path.empty()) return SQ_E_UNSUPPORTED;
    try {
      auto text = sq::readFile(path + ".json");
      if (!text) return SQ_E_NOTFOUND;
      std::string err;
      if (!sq::stems::parseModelSpec(*text, spec_, err)) {
        sq::logf(SQ_LOG_WARN, "%s.json: %s", path.c_str(), err.c_str());
        return SQ_E_PARSE;
      }
      if (std::abs(spec_.sampleRate - sampleRate) > 0.5) return SQ_E_UNSUPPORTED;  // no resampling here
      sq_result r = rt->loadModel(path.c_str(), model_.put());
      if (SQ_FAILED(r)) return r;
      wantStems_ = options ? static_cast<uint32_t>(options->getInt("stems", 0)) : 0u;
      input_.assign(channels, {});
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL process(const sq_audio_block* block) override {
    if (!model_) return SQ_E_FAIL;
    if (!sq::dspkit::blockReadable(block) || block->channel_count != input_.size()) return SQ_E_INVALIDARG;
    if (!block->frames) return SQ_OK;
    try {
      for (uint32_t c = 0; c < block->channel_count; ++c)
        input_[c].insert(input_[c].end(), block->channels[c], block->channels[c] + block->frames);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL finish(sq::IAnalysisResult** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!model_) return SQ_E_FAIL;
    try {
      sq::stems::Separated sep;
      sq_result r = sq::stems::separate(model_.get(), spec_, input_, wantStems_, sep);
      if (SQ_FAILED(r)) return r;
      auto* res = new sq::dspkit::AnalysisResult();
      for (size_t i = 0; i < sep.names.size(); ++i) res->stems.push_back({sep.names[i], std::move(sep.audio[i])});
      *out = res;
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL getRealtimeValue(sq_tuner_value*) override { return SQ_E_UNSUPPORTED; }
  void SQ_CALL reset() override {
    for (auto& c : input_) c.clear();
  }

 private:
  sq::Ptr<sq::INNModel> model_;
  sq::stems::ModelSpec spec_;
  uint32_t wantStems_ = 0;
  std::vector<std::vector<float>> input_;
};

sq::INNRuntime* g_runtime = nullptr;

sq_result onInit(sq::IHostContext* host) {
  sq::ISettings* s = host->getSettings();
  if (!s->has(kModelKey)) s->setString(kModelKey, "");
  if (!s->has(kOrtPathKey)) s->setString(kOrtPathKey, "");
  g_runtime = nullptr;
  std::string explicitPath = settingString(kOrtPathKey);
  if (explicitPath.empty()) {
    sq::Buffer env;
    if (sq_env_get("SQ_ONNXRUNTIME_PATH", env.out()) == SQ_OK) explicitPath = env.str();
  }
  const std::string lib = sq::stems::findOrtLibrary(explicitPath);
  if (lib.empty()) {
    sq::logf(SQ_LOG_INFO, "no ONNX Runtime library found: neural inference unavailable");
    return SQ_OK;
  }
  sq::Ptr<sq::INNRuntime> existing;
  if (host->queryService(&sq::INNRuntime::iid, existing.putVoid()) == SQ_OK && existing) {
    sq::logf(SQ_LOG_INFO, "an INNRuntime is already registered; not registering ONNX Runtime");
    return SQ_OK;
  }
  g_runtime = sq::stems::createOrtRuntime(lib);
  sq_result r = host->registerService(&sq::INNRuntime::iid, g_runtime);
  if (SQ_FAILED(r)) {
    g_runtime->release();
    g_runtime = nullptr;
  }
  return SQ_OK;
}

void onShutdown() {
  if (g_runtime) {
    if (sq::IHostContext* h = sq::host()) h->unregisterService(&sq::INNRuntime::iid, g_runtime);
    g_runtime->release();
    g_runtime = nullptr;
  }
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<StemAnalyzer>(sq::dsp::kStemSeparatorClassId, sq::dsp::kAnalysisCategory,
                                 "Squizzy Stem Separator (neural)"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('S', 'T', 'E', 'M'),
    .name = "analysis-stems",
    .description = "Neural stem separation + ONNX Runtime prediction dispatcher (optional)",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
