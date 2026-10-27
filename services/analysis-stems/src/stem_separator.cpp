// Stem separation driver (see stem_separator.hpp).
#include "stem_separator.hpp"

#include <algorithm>
#include <cmath>

#include "sq/nn/tensor.hpp"
#include "sq/platform.hpp"

namespace sq::stems {

bool parseModelSpec(const std::string& json, ModelSpec& out, std::string& err) {
  std::string perr;
  sq::JsonPtr root = sq::jsonParse(json, &perr);
  if (!root || sq_json_type_of(root.get()) != SQ_JSON_OBJECT) {
    err = "model spec is not a JSON object " + perr;
    return false;
  }
  const sq_json* fmt = sq_json_get(root.get(), "format");
  if (fmt) {
    size_t len = 0;
    const char* s = sq_json_get_string(fmt, &len);
    if (std::string(s, len) != "sq-stems-1") {
      err = "unknown model format";
      return false;
    }
  }
  if (fmt && sq_json_type_of(fmt) != SQ_JSON_STRING) {
    err = "unknown model format";
    return false;
  }
  ModelSpec spec;
  // Integers are range-checked as doubles BEFORE narrowing: a negative or huge value must not wrap
  // into a plausible uint32 (e.g. chunk_frames -1 -> 4 G frames -> a 32 GB tensor).
  bool numbersOk = true;
  auto intField = [&](const char* key, uint32_t& dst, double lo, double hi) {
    const sq_json* v = sq_json_get(root.get(), key);
    if (!v) return;
    const double d = sq_json_get_number(v);
    if (sq_json_type_of(v) != SQ_JSON_NUMBER || !(d >= lo && d <= hi) || d != std::floor(d)) {
      numbersOk = false;
      return;
    }
    dst = static_cast<uint32_t>(d);
  };
  if (const sq_json* v = sq_json_get(root.get(), "sample_rate")) spec.sampleRate = sq_json_get_number(v);
  intField("channels", spec.channels, 1, 16);
  intField("chunk_frames", spec.chunkFrames, 64, kMaxChunkFrames);
  intField("overlap_frames", spec.overlapFrames, 0, kMaxChunkFrames);
  if (const sq_json* v = sq_json_get(root.get(), "stems"); v && sq_json_type_of(v) == SQ_JSON_ARRAY) {
    for (size_t i = 0; i < sq_json_size(v); ++i) {
      const sq_json* e = sq_json_at(v, i);
      size_t len = 0;
      const char* s = sq_json_get_string(e, &len);
      if (sq_json_type_of(e) != SQ_JSON_STRING || len == 0) {
        err = "model spec: stem names must be non-empty strings";
        return false;
      }
      spec.stems.emplace_back(s, len);
    }
  }
  if (!numbersOk || !(spec.sampleRate >= 4000.0 && spec.sampleRate <= 1.0e6) || spec.channels == 0 ||
      spec.channels > 16 || spec.stems.empty() || spec.stems.size() > 32 || spec.chunkFrames < 64 ||
      spec.overlapFrames * 2 >= spec.chunkFrames ||
      static_cast<uint64_t>(spec.stems.size()) * spec.channels * spec.chunkFrames > kMaxOutputElements) {
    err = "model spec: invalid sample_rate/channels/stems/chunk_frames/overlap_frames";
    return false;
  }
  out = std::move(spec);
  return true;
}

sq_result separate(sq::INNModel* model, const ModelSpec& spec, const std::vector<std::vector<float>>& input,
                   uint32_t wantStems, Separated& out) {
  if (!model || input.empty()) return SQ_E_INVALIDARG;
  for (const auto& ch : input)
    if (ch.size() != input[0].size()) return SQ_E_INVALIDARG;  // ragged channels
  if (model->getInputCount() < 1 || model->getOutputCount() < 1) return SQ_E_UNSUPPORTED;
  const uint32_t ci = static_cast<uint32_t>(input.size());
  const uint32_t cm = spec.channels;
  const uint32_t ns = static_cast<uint32_t>(spec.stems.size());
  const uint32_t L = spec.chunkFrames, O = spec.overlapFrames, hop = L - O;
  const size_t frames = input[0].size();

  std::vector<std::vector<std::vector<float>>> acc(ns, std::vector<std::vector<float>>(cm, std::vector<float>(frames, 0.0f)));
  sq::nn::Tensor in(SQ_TENSOR_F32, {1, cm, L});
  sq::nn::Tensor res(SQ_TENSOR_F32, {1, ns, cm, L});
  float* x = in.data<float>();
  for (size_t start = 0; start < std::max<size_t>(frames, 1); start += hop) {
    const size_t len = std::min<size_t>(L, frames > start ? frames - start : 0);
    // model input channels
    for (uint32_t m = 0; m < cm; ++m) {
      float* dst = x + static_cast<size_t>(m) * L;
      std::fill(dst, dst + L, 0.0f);
      if (cm == 1 && ci > 1) {
        for (uint32_t c = 0; c < ci; ++c)
          for (size_t i = 0; i < len; ++i) dst[i] += input[c][start + i] / static_cast<float>(ci);
      } else {
        const std::vector<float>& src = input[m % ci];
        std::copy(src.begin() + static_cast<std::ptrdiff_t>(start), src.begin() + static_cast<std::ptrdiff_t>(start + len), dst);
      }
    }
    sq_tensor tin = in.view();
    sq_tensor tout = res.view();
    sq_result r = model->run(&tin, 1, &tout, 1);
    if (r == SQ_E_BUFFER_TOO_SMALL) return SQ_E_UNSUPPORTED;  // output larger than the spec says
    if (SQ_FAILED(r)) return r;
    if (tout.dtype != SQ_TENSOR_F32 || tout.rank != 4 || tout.shape[1] != ns || tout.shape[2] != cm || tout.shape[3] != L)
      return SQ_E_UNSUPPORTED;
    const float* y = res.data<float>();
    const bool first = start == 0, last = start + L >= frames;
    for (size_t i = 0; i < len; ++i) {
      float w = 1.0f;
      if (!first && i < O) w = static_cast<float>(i) / static_cast<float>(O);
      if (!last && i >= hop) w = static_cast<float>(L - i) / static_cast<float>(O);
      for (uint32_t s = 0; s < ns; ++s)
        for (uint32_t m = 0; m < cm; ++m) acc[s][m][start + i] += w * y[(static_cast<size_t>(s) * cm + m) * L + i];
    }
    if (last) break;
  }

  // map model channels to the input's channel count
  auto toInputChannels = [&](const std::vector<std::vector<float>>& stem) {
    std::vector<std::vector<float>> o(ci, std::vector<float>(frames, 0.0f));
    for (uint32_t c = 0; c < ci; ++c) {
      if (ci == cm) {
        o[c] = stem[c];
      } else if (ci == 1) {
        for (uint32_t m = 0; m < cm; ++m)
          for (size_t i = 0; i < frames; ++i) o[0][i] += stem[m][i] / static_cast<float>(cm);
      } else {
        o[c] = stem[c % cm];
      }
    }
    return o;
  };
  out = Separated{};
  const uint32_t keep = (wantStems > 0 && wantStems < ns) ? wantStems - 1 : ns;
  for (uint32_t s = 0; s < keep; ++s) {
    out.names.push_back(spec.stems[s]);
    out.audio.push_back(toInputChannels(acc[s]));
  }
  if (keep < ns) {
    std::vector<std::vector<float>> rest(cm, std::vector<float>(frames, 0.0f));
    for (uint32_t s = keep; s < ns; ++s)
      for (uint32_t m = 0; m < cm; ++m)
        for (size_t i = 0; i < frames; ++i) rest[m][i] += acc[s][m][i];
    out.names.push_back(wantStems == 2 ? "accompaniment" : "other");
    out.audio.push_back(toInputChannels(rest));
  }
  return SQ_OK;
}

}  // namespace sq::stems
