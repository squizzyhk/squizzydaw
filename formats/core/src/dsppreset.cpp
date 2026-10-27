#include "sq/formats/core/dsppreset.hpp"

#include <set>

#include "sq/formats/core/jsonutil.hpp"

namespace sq::formats {

namespace {
constexpr const char* kTag = "squizzy.dsppreset";
}

Status dspPresetWrite(const DspPreset& p, std::string& jsonOut) {
  if (p.deviceModel.empty()) return Status::error(SQ_E_INVALIDARG, "dsppreset: device model is required");
  Json root = Json::object();
  root.set("format", Json::string(kTag));
  root.set("version", Json::number(kDspPresetVersion));
  root.set("name", Json::string(p.name));
  Json dev = Json::object();
  dev.set("vendor", Json::string(p.deviceVendor));
  dev.set("model", Json::string(p.deviceModel));
  if (!p.firmwareMin.empty()) dev.set("firmwareMin", Json::string(p.firmwareMin));
  root.set("device", std::move(dev));
  if (!p.info.empty()) {
    Json info = Json::object();
    for (const auto& [k, v] : p.info) info.set(k.c_str(), Json::string(v));
    root.set("info", std::move(info));
  }
  Json blocks = Json::array();
  std::set<std::string> ids;
  for (const auto& b : p.blocks) {
    if (b.id.empty() || !ids.insert(b.id).second) return Status::error(SQ_E_INVALIDARG, "dsppreset: empty or duplicate block id '" + b.id + "'");
    if (b.type.empty()) return Status::error(SQ_E_INVALIDARG, "dsppreset: block '" + b.id + "' has no type");
    Json jb = Json::object();
    jb.set("id", Json::string(b.id));
    jb.set("type", Json::string(b.type));
    jb.set("enabled", Json::boolean(b.enabled));
    Json params = Json::object();
    std::set<std::string> keys;
    for (const auto& [k, v] : b.params) {
      if (k.empty() || !keys.insert(k).second) return Status::error(SQ_E_INVALIDARG, "dsppreset: empty or duplicate parameter '" + k + "'");
      switch (v.type) {
        case DspValue::Type::Number: params.set(k.c_str(), Json::number(v.number)); break;
        case DspValue::Type::Bool: params.set(k.c_str(), Json::boolean(v.flag)); break;
        case DspValue::Type::String: params.set(k.c_str(), Json::string(v.text)); break;
      }
    }
    jb.set("params", std::move(params));
    blocks.append(std::move(jb));
  }
  root.set("blocks", std::move(blocks));
  jsonOut = root.write(true);
  return {};
}

Status dspPresetRead(std::string_view json, DspPreset& out) {
  out = DspPreset{};
  Json root;
  SQF_TRY(Json::parse(json, root));
  if (!root.isObject() || root.get("format").str() != kTag) return Status::error(SQ_E_PARSE, "dsppreset: wrong format tag");
  if (!root.get("version").isNumber() || root.get("version").integer() < 1) return Status::error(SQ_E_PARSE, "dsppreset: invalid version");
  if (root.get("version").integer() > kDspPresetVersion) return Status::error(SQ_E_UNSUPPORTED, "dsppreset: newer version than supported");
  out.name = root.get("name").str();
  Json dev = root.get("device");
  out.deviceVendor = dev.get("vendor").str();
  out.deviceModel = dev.get("model").str();
  out.firmwareMin = dev.get("firmwareMin").str();
  if (out.deviceModel.empty()) return Status::error(SQ_E_PARSE, "dsppreset: device.model missing");
  Json info = root.get("info");
  for (size_t i = 0; i < info.size(); ++i) out.info.emplace_back(info.keyAt(i), info.at(i).str());
  Json blocks = root.get("blocks");
  if (!blocks.isArray()) return Status::error(SQ_E_PARSE, "dsppreset: 'blocks' must be an array");
  std::set<std::string> ids;
  for (size_t i = 0; i < blocks.size(); ++i) {
    Json jb = blocks.at(i);
    DspBlock b;
    b.id = jb.get("id").str();
    b.type = jb.get("type").str();
    b.enabled = jb.has("enabled") ? jb.get("enabled").boolean() : true;
    if (b.id.empty() || !ids.insert(b.id).second) return Status::error(SQ_E_PARSE, "dsppreset: empty or duplicate block id");
    Json params = jb.get("params");
    for (size_t k = 0; k < params.size(); ++k) {
      Json v = params.at(k);
      DspValue dv;
      if (v.isNumber()) dv = DspValue::num(v.number());
      else if (v.isBool()) dv = DspValue::boolean(v.boolean());
      else if (v.isString()) dv = DspValue::str(v.str());
      else return Status::error(SQ_E_PARSE, "dsppreset: parameter '" + params.keyAt(k) + "' must be number, bool or string");
      b.params.emplace_back(params.keyAt(k), std::move(dv));
    }
    out.blocks.push_back(std::move(b));
  }
  return {};
}

}  // namespace sq::formats
