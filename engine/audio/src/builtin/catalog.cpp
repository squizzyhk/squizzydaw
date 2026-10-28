// Squizzy Studio engine/audio - built-in device catalog, factories and spec helpers.
#include <charconv>
#include <cstdlib>

#include "device.hpp"
#include "meter.hpp"

namespace sq::engine::audio {

namespace {
using Factory = ProcessorPtr (*)();
struct Entry {
  DeviceInfo (*info)();
  Factory make;
};
// Stable catalog order (instruments first).
const Entry kEntries[] = {
    {builtin::SynthInfo, builtin::makeSynth},           {builtin::DrumsInfo, builtin::makeDrums},
    {builtin::KeysInfo, builtin::makeKeys},             {builtin::BassInfo, builtin::makeBass},
    {builtin::SamplerInfo, builtin::makeSampler},       {builtin::EqInfo, builtin::makeEq},
    {builtin::CompressorInfo, builtin::makeCompressor}, {builtin::LimiterInfo, builtin::makeLimiter},
    {builtin::GateInfo, builtin::makeGate},             {builtin::ReverbInfo, builtin::makeReverb},
    {builtin::DelayInfo, builtin::makeDelay},           {builtin::ChorusInfo, builtin::makeChorus},
    {builtin::FlangerInfo, builtin::makeFlanger},       {builtin::PhaserInfo, builtin::makePhaser},
    {builtin::DistortionInfo, builtin::makeDistortion}, {builtin::FilterInfo, builtin::makeFilter},
    {builtin::UtilityInfo, builtin::makeUtility},       {builtin::TremoloInfo, builtin::makeTremolo},
};

Factory factoryOf(const std::string& id) {
  const auto& all = builtinDevices();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].id == id) return kEntries[i].make;
  return nullptr;
}

void appendNumber(std::string* s, double v) {
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  s->append(buf, r.ptr);
}

ProcessorProto finish(ProcessorPtr p, const DeviceInfo& info, const std::vector<std::pair<uint32_t, double>>& params) {
  for (const auto& [index, value] : params) {
    if (index >= info.params.size()) continue;
    p->setParameter(index, builtin::clampParam(info.params[index], value));
  }
  return ProcessorProto(p.release());
}
}  // namespace

const std::vector<DeviceInfo>& builtinDevices() {
  static const std::vector<DeviceInfo> all = [] {
    std::vector<DeviceInfo> v;
    for (const Entry& e : kEntries) v.push_back(e.info());
    return v;
  }();
  return all;
}

const DeviceInfo* findBuiltin(const std::string& id) {
  for (const DeviceInfo& d : builtinDevices())
    if (d.id == id) return &d;
  return nullptr;
}

namespace builtin {
const DeviceInfo& infoOf(const char* id) {
  const DeviceInfo* d = findBuiltin(id);
  return *d;
}
}  // namespace builtin

ProcessorProto makeBuiltin(const std::string& id, const std::vector<std::pair<uint32_t, double>>& params, std::string* error) {
  const DeviceInfo* info = findBuiltin(id);
  Factory f = factoryOf(id);
  if (!info || !f) {
    if (error) *error = "unknown built-in device '" + id + "'";
    return nullptr;
  }
  return finish(f(), *info, params);
}

ProcessorProto makeBuiltinWithMedia(const std::string& id, MediaPtr media, const std::vector<std::pair<uint32_t, double>>& params,
                                    std::string* error) {
  if (id != "sampler") return makeBuiltin(id, params, error);
  return finish(builtin::makeSamplerWithMedia(std::move(media)), *findBuiltin(id), params);
}

float builtinMeterValue(const AudioProcessor* processor) noexcept {
  const auto* m = dynamic_cast<const builtin::MeterSource*>(processor);
  return m ? m->meterValue() : 0.0f;
}

std::string builtinSpec(const std::string& id, const std::vector<float>& values) {
  std::string s = "builtin|" + id + "|";
  const DeviceInfo* info = findBuiltin(id);
  for (size_t i = 0; i < values.size(); ++i) {
    if (info && i < info->params.size() && values[i] == info->params[i].def) continue;
    s += '|';
    s += std::to_string(i);
    s += '=';
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, values[i]);
    s.append(buf, r.ptr);
  }
  return s;
}

bool parseSpec(const std::string& spec, SpecParts* out) {
  std::vector<std::string> parts;
  size_t p = 0;
  for (;;) {
    const size_t e = spec.find('|', p);
    parts.push_back(spec.substr(p, e == std::string::npos ? std::string::npos : e - p));
    if (e == std::string::npos) break;
    p = e + 1;
  }
  if (parts.size() < 3 || parts[0].empty() || parts[1].empty()) return false;
  SpecParts r;
  r.format = parts[0];
  r.id = parts[1];
  r.path = parts[2];
  for (size_t i = 3; i < parts.size(); ++i) {
    const std::string& kv = parts[i];
    const size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    char* end = nullptr;
    const unsigned long idx = std::strtoul(kv.c_str(), &end, 10);
    if (end != kv.c_str() + eq) continue;
    double v = 0.0;
    const char* vb = kv.c_str() + eq + 1;
    const char* ve = kv.c_str() + kv.size();
    auto res = std::from_chars(vb, ve, v);
    if (res.ec != std::errc() || res.ptr != ve) {
      v = std::strtod(vb, &end);  // tolerate "+1" and similar
      if (end == vb) continue;
    }
    r.params.emplace_back(static_cast<uint32_t>(idx), v);
  }
  if (out) *out = std::move(r);
  return true;
}

std::string formatSpec(const SpecParts& parts) {
  std::string s = parts.format + "|" + parts.id + "|" + parts.path;
  for (const auto& [index, value] : parts.params) {
    s += '|';
    s += std::to_string(index);
    s += '=';
    appendNumber(&s, value);
  }
  return s;
}

}  // namespace sq::engine::audio
