// INNRuntime / INNModel over the ONNX Runtime C API, resolved dynamically (see ort_runtime.hpp).
#include "ort_runtime.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "onnxruntime_c_api.h"
#include "sq/interfaces/dsp.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"

namespace sq::stems {

namespace {

constexpr uint32_t kMinApiVersion = 11;  // ORT 1.11: every call used here exists since then

int32_t fromOrtType(ONNXTensorElementDataType t) {
  switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return SQ_TENSOR_F32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return SQ_TENSOR_I64;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return SQ_TENSOR_I32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return SQ_TENSOR_U8;
    default: return 0;
  }
}
ONNXTensorElementDataType toOrtType(int32_t t) {
  switch (t) {
    case SQ_TENSOR_F32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case SQ_TENSOR_I64: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    case SQ_TENSOR_I32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    case SQ_TENSOR_U8: return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
    default: return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  }
}
uint32_t elemSize(int32_t t) { return t == SQ_TENSOR_I64 ? 8u : t == SQ_TENSOR_U8 ? 1u : t ? 4u : 0u; }

sq_result copyString(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed) {
  const uint32_t need = static_cast<uint32_t>(s.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, s.c_str(), need);
  return SQ_OK;
}

class OrtRuntime;

class OrtModel final : public sq::Object<OrtModel, sq::INNModel> {
 public:
  OrtModel(OrtRuntime* rt, const OrtApi* api, OrtSession* s);
  ~OrtModel() override;
  bool init(std::string& err);

  uint32_t SQ_CALL getInputCount() override { return static_cast<uint32_t>(inInfo_.size()); }
  sq_result SQ_CALL getInputInfo(uint32_t index, sq_tensor_info* out) override { return info(inInfo_, index, out); }
  uint32_t SQ_CALL getOutputCount() override { return static_cast<uint32_t>(outInfo_.size()); }
  sq_result SQ_CALL getOutputInfo(uint32_t index, sq_tensor_info* out) override { return info(outInfo_, index, out); }
  sq_result SQ_CALL run(const sq_tensor* inputs, uint32_t inputCount, sq_tensor* outputs, uint32_t outputCount) override {
    try {  // no exception may cross the ABI
      return runImpl(inputs, inputCount, outputs, outputCount);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
  }

 private:
  static sq_result info(const std::vector<sq_tensor_info>& v, uint32_t i, sq_tensor_info* out) {
    if (!out || out->struct_size < sizeof(sq_tensor_info)) return SQ_E_INVALIDARG;
    if (i >= v.size()) return SQ_E_NOTFOUND;
    *out = v[i];
    return SQ_OK;
  }
  sq_result runImpl(const sq_tensor* inputs, uint32_t inputCount, sq_tensor* outputs, uint32_t outputCount);
  bool readInfo(bool input, size_t i, sq_tensor_info& out, std::string& err);
  bool check(OrtStatus* st, std::string* err = nullptr) {
    if (!st) return true;
    if (err) *err = api_->GetErrorMessage(st);
    api_->ReleaseStatus(st);
    return false;
  }

  sq::Ptr<sq::INNRuntime> keep_;  // the runtime (and the ORT library) outlive every model
  const OrtApi* api_;
  OrtSession* session_;
  OrtMemoryInfo* mem_ = nullptr;
  std::vector<std::string> inNames_, outNames_;
  std::vector<sq_tensor_info> inInfo_, outInfo_;
};

class OrtRuntime final : public sq::Object<OrtRuntime, sq::INNRuntime> {
 public:
  explicit OrtRuntime(std::string path) : path_(std::move(path)) {}
  ~OrtRuntime() override {
    if (api_ && env_) api_->ReleaseEnv(env_);
    if (lib_) sq_dylib_close(lib_);
  }

  sq_result SQ_CALL getRuntimeInfo(char* buf, uint32_t bufSize, uint32_t* needed) override {
    try {
      return runtimeInfo(buf, bufSize, needed);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
  }
  sq_result SQ_CALL loadModel(const char* pathOrUrl, sq::INNModel** out) override {
    if (out) *out = nullptr;
    try {
      return loadModelImpl(pathOrUrl, out);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
  }
  sq_result SQ_CALL setThreadCount(uint32_t threads) override {
    try {
      std::lock_guard<std::mutex> g(m_);
      threads_ = threads;
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

 private:
  sq_result runtimeInfo(char* buf, uint32_t bufSize, uint32_t* needed) {
    std::string s;
    bool ok = false;
    {
      std::lock_guard<std::mutex> g(m_);
      ok = ensureLoaded();
      s = ok ? "onnxruntime " + version_ : "unavailable: " + error_;
    }
    sq_result r = copyString(s, buf, bufSize, needed);
    return SQ_FAILED(r) ? r : (ok ? SQ_OK : SQ_S_FALSE);
  }

  sq_result loadModelImpl(const char* pathOrUrl, sq::INNModel** out) {
    if (!out || !pathOrUrl || !*pathOrUrl) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (std::strncmp(pathOrUrl, "package://", 10) == 0) return SQ_E_UNSUPPORTED;  // needs IUrlResolver
    std::lock_guard<std::mutex> g(m_);
    if (!ensureLoaded()) return SQ_E_UNSUPPORTED;
    if (!sq_fs_is_file(pathOrUrl)) return SQ_E_NOTFOUND;
    OrtSessionOptions* opts = nullptr;
    OrtSession* session = nullptr;
    std::string err;
    bool ok = check(api_->CreateSessionOptions(&opts), &err);
    if (ok && threads_) ok = check(api_->SetIntraOpNumThreads(opts, static_cast<int>(threads_)), &err);
    if (ok) ok = check(api_->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL), &err);
    if (ok) {
#ifdef _WIN32
      std::vector<uint16_t> w(std::strlen(pathOrUrl) + 1, 0);
      size_t wl = 0;
      ok = sq_utf8_to_utf16(pathOrUrl, std::strlen(pathOrUrl), w.data(), w.size(), &wl) == SQ_OK;
      if (ok) {
        w.resize(wl + 1);
        w[wl] = 0;
        ok = check(api_->CreateSession(env_, reinterpret_cast<const wchar_t*>(w.data()), opts, &session), &err);
      }
#else
      ok = check(api_->CreateSession(env_, pathOrUrl, opts, &session), &err);
#endif
    }
    if (opts) api_->ReleaseSessionOptions(opts);
    if (!ok || !session) {
      sq::logf(SQ_LOG_WARN, "onnxruntime: cannot load model %s: %s", pathOrUrl, err.c_str());
      return SQ_E_PARSE;
    }
    auto* model = new (std::nothrow) OrtModel(this, api_, session);
    if (!model) {
      api_->ReleaseSession(session);
      return SQ_E_OUTOFMEMORY;
    }
    bool inited = false;
    try {
      inited = model->init(err);
    } catch (...) {
      err = "out of memory";
    }
    if (!inited) {
      sq::logf(SQ_LOG_WARN, "onnxruntime: model %s: %s", pathOrUrl, err.c_str());
      model->release();  // releases the session too
      return SQ_E_UNSUPPORTED;
    }
    *out = model;
    return SQ_OK;
  }

  bool check(OrtStatus* st, std::string* err) {
    if (!st) return true;
    if (err) *err = api_->GetErrorMessage(st);
    api_->ReleaseStatus(st);
    return false;
  }
  bool ensureLoaded() {
    if (api_ && env_) return true;
    if (tried_) return false;
    tried_ = true;
    char msg[512] = {};
    if (sq_dylib_open(path_.c_str(), &lib_, msg, sizeof msg) != SQ_OK || !lib_) {
      error_ = std::string("cannot open ") + path_ + ": " + msg;
      lib_ = nullptr;
      return false;
    }
    using GetBase = const OrtApiBase*(ORT_API_CALL*)(void);
    auto getBase = reinterpret_cast<GetBase>(sq_dylib_symbol(lib_, "OrtGetApiBase"));
    const OrtApiBase* base = getBase ? getBase() : nullptr;
    if (!base) {
      error_ = path_ + " is not an ONNX Runtime library (no OrtGetApiBase)";
      return false;
    }
    for (uint32_t v = ORT_API_VERSION; v >= kMinApiVersion && !api_; --v) api_ = base->GetApi(v);
    if (!api_) {
      error_ = "ONNX Runtime too old (need C API >= 11)";
      return false;
    }
    version_ = base->GetVersionString ? base->GetVersionString() : "?";
    std::string err;
    if (!check(api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "squizzy", &env_), &err)) {
      error_ = "CreateEnv failed: " + err;
      env_ = nullptr;
      return false;
    }
    sq::logf(SQ_LOG_INFO, "onnxruntime %s loaded from %s", version_.c_str(), path_.c_str());
    return true;
  }

  std::mutex m_;
  std::string path_;
  bool tried_ = false;
  sq_dylib* lib_ = nullptr;
  const OrtApi* api_ = nullptr;
  OrtEnv* env_ = nullptr;
  std::string version_, error_;
  uint32_t threads_ = 0;
};

OrtModel::OrtModel(OrtRuntime* rt, const OrtApi* api, OrtSession* s) : keep_(rt), api_(api), session_(s) {}

OrtModel::~OrtModel() {
  if (mem_) api_->ReleaseMemoryInfo(mem_);
  if (session_) api_->ReleaseSession(session_);
}

bool OrtModel::readInfo(bool input, size_t i, sq_tensor_info& out, std::string& err) {
  OrtAllocator* alloc = nullptr;
  if (!check(api_->GetAllocatorWithDefaultOptions(&alloc), &err)) return false;
  char* name = nullptr;
  if (!check(input ? api_->SessionGetInputName(session_, i, alloc, &name) : api_->SessionGetOutputName(session_, i, alloc, &name), &err))
    return false;
  out = sq_tensor_info{};
  out.struct_size = sizeof out;
  std::strncpy(out.name, name ? name : "", sizeof out.name - 1);
  (input ? inNames_ : outNames_).push_back(name ? name : "");
  if (name) (void)check(api_->AllocatorFree(alloc, name));
  OrtTypeInfo* ti = nullptr;
  if (!check(input ? api_->SessionGetInputTypeInfo(session_, i, &ti) : api_->SessionGetOutputTypeInfo(session_, i, &ti), &err))
    return false;
  const OrtTensorTypeAndShapeInfo* tsi = nullptr;
  bool ok = check(api_->CastTypeInfoToTensorInfo(ti, &tsi), &err) && tsi;
  if (ok) {
    ONNXTensorElementDataType et{};
    size_t rank = 0;
    ok = check(api_->GetTensorElementType(tsi, &et), &err) && check(api_->GetDimensionsCount(tsi, &rank), &err);
    if (ok && rank <= SQ_TENSOR_MAX_RANK) {
      out.dtype = fromOrtType(et);
      out.rank = static_cast<uint32_t>(rank);
      ok = check(api_->GetDimensions(tsi, out.shape, rank), &err);
    } else if (ok) {
      err = "tensor rank too large";
      ok = false;
    }
  } else if (err.empty()) {
    err = "non-tensor input/output";
  }
  api_->ReleaseTypeInfo(ti);
  return ok;
}

bool OrtModel::init(std::string& err) {
  size_t ni = 0, no = 0;
  if (!check(api_->SessionGetInputCount(session_, &ni), &err) || !check(api_->SessionGetOutputCount(session_, &no), &err))
    return false;
  inInfo_.resize(ni);
  outInfo_.resize(no);
  for (size_t i = 0; i < ni; ++i)
    if (!readInfo(true, i, inInfo_[i], err)) return false;
  for (size_t i = 0; i < no; ++i)
    if (!readInfo(false, i, outInfo_[i], err)) return false;
  return check(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_), &err);
}

sq_result OrtModel::runImpl(const sq_tensor* inputs, uint32_t inputCount, sq_tensor* outputs, uint32_t outputCount) {
  if (inputCount != inInfo_.size() || outputCount != outInfo_.size() || (inputCount && !inputs) || (outputCount && !outputs))
    return SQ_E_INVALIDARG;
  std::vector<OrtValue*> in(inputCount, nullptr), out(outputCount, nullptr);
  struct ValueGuard {  // releases every OrtValue on any exit path (incl. a throwing allocation)
    const OrtApi* api;
    std::vector<OrtValue*>& a;
    std::vector<OrtValue*>& b;
    ~ValueGuard() {
      for (OrtValue*& v : a)
        if (v) api->ReleaseValue(v), v = nullptr;
      for (OrtValue*& v : b)
        if (v) api->ReleaseValue(v), v = nullptr;
    }
  } guard{api_, in, out};
  std::vector<const char*> inN(inputCount), outN(outputCount);
  sq_result result = SQ_OK;
  std::string err;
  for (uint32_t i = 0; i < inputCount && result == SQ_OK; ++i) {
    const sq_tensor& t = inputs[i];
    uint64_t count = 1;
    for (uint32_t d = 0; d < t.rank && d < SQ_TENSOR_MAX_RANK; ++d) count *= static_cast<uint64_t>(t.shape[d] < 0 ? 0 : t.shape[d]);
    const uint64_t bytes = count * elemSize(t.dtype);
    if (t.struct_size < sizeof(sq_tensor) || !elemSize(t.dtype) || t.rank > SQ_TENSOR_MAX_RANK || bytes > t.byte_size ||
        (bytes && !t.data)) {
      result = SQ_E_INVALIDARG;
      break;
    }
    if (!check(api_->CreateTensorWithDataAsOrtValue(mem_, t.data, static_cast<size_t>(bytes), t.shape, t.rank,
                                                    toOrtType(t.dtype), &in[i]), &err))
      result = SQ_E_INVALIDARG;
    inN[i] = inNames_[i].c_str();
  }
  for (uint32_t i = 0; i < outputCount; ++i) outN[i] = outNames_[i].c_str();
  if (result == SQ_OK &&
      !check(api_->Run(session_, nullptr, inN.data(), in.data(), inputCount, outN.data(), outputCount, out.data()), &err)) {
    sq::logf(SQ_LOG_WARN, "onnxruntime: run failed: %s", err.c_str());
    result = SQ_E_FAIL;
  }
  for (uint32_t i = 0; i < outputCount && SQ_SUCCEEDED(result); ++i) {
    OrtTensorTypeAndShapeInfo* tsi = nullptr;
    if (!out[i] || !check(api_->GetTensorTypeAndShape(out[i], &tsi), &err)) {
      result = SQ_E_FAIL;
      break;
    }
    ONNXTensorElementDataType et{};
    size_t rank = 0;
    sq_tensor& o = outputs[i];
    bool ok = check(api_->GetTensorElementType(tsi, &et)) && check(api_->GetDimensionsCount(tsi, &rank)) &&
              rank <= SQ_TENSOR_MAX_RANK && check(api_->GetDimensions(tsi, o.shape, rank));
    api_->ReleaseTensorTypeAndShapeInfo(tsi);
    if (!ok || !fromOrtType(et)) {
      result = SQ_E_UNSUPPORTED;
      break;
    }
    o.dtype = fromOrtType(et);
    o.rank = static_cast<uint32_t>(rank);
    uint64_t count = 1;
    for (size_t d = 0; d < rank; ++d) count *= static_cast<uint64_t>(o.shape[d] < 0 ? 0 : o.shape[d]);
    const uint64_t bytes = count * elemSize(o.dtype);
    if (o.byte_size < bytes || (bytes && !o.data)) {
      o.byte_size = bytes;
      result = SQ_E_BUFFER_TOO_SMALL;
      continue;
    }
    void* src = nullptr;
    if (!check(api_->GetTensorMutableData(out[i], &src)) || (bytes && !src)) {
      result = SQ_E_FAIL;
      break;
    }
    if (bytes) std::memcpy(o.data, src, static_cast<size_t>(bytes));
  }
  return result;
}

}  // namespace

const char* ortLibraryName() {
#if defined(_WIN32)
  return "onnxruntime.dll";
#elif defined(__APPLE__)
  return "libonnxruntime.dylib";
#else
  return "libonnxruntime.so";
#endif
}

std::string findOrtLibrary(const std::string& explicitPath) {
  if (!explicitPath.empty()) return sq_fs_is_file(explicitPath.c_str()) ? explicitPath : std::string();
  const std::string exe = sq::specialDir(SQ_DIR_EXE);
  for (const std::string& dir : {exe, sq::joinPath(exe, "services")}) {
    std::string p = sq::joinPath(dir, ortLibraryName());
    if (sq_fs_is_file(p.c_str())) return p;
  }
  return {};
}

sq::INNRuntime* createOrtRuntime(const std::string& libraryPath) { return new OrtRuntime(libraryPath); }

}  // namespace sq::stems
