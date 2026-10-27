// Squizzy Studio formats - .dsppreset: JSON preset for hardware DSP (interface mixers, onboard
// effects). Schema: SCHEMAS.md ".dsppreset".
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

inline constexpr int kDspPresetVersion = 1;

struct DspValue {
  enum class Type { Number, Bool, String } type = Type::Number;
  double number = 0.0;
  bool flag = false;
  std::string text;

  static DspValue num(double v) { DspValue d; d.number = v; return d; }
  static DspValue boolean(bool v) { DspValue d; d.type = Type::Bool; d.flag = v; return d; }
  static DspValue str(std::string v) { DspValue d; d.type = Type::String; d.text = std::move(v); return d; }
  bool operator==(const DspValue& o) const {
    return type == o.type && number == o.number && flag == o.flag && text == o.text;
  }
};

struct DspBlock {
  std::string id;    // unique within the preset
  std::string type;  // "eq", "comp", "reverb", "mixer" ... (device defined)
  bool enabled = true;
  std::vector<std::pair<std::string, DspValue>> params;  // ordered
};

struct DspPreset {
  std::string name;
  std::string deviceVendor;
  std::string deviceModel;
  std::string firmwareMin;  // optional, device-defined version string
  KeyValues info;           // free-form strings (author, comment ...)
  std::vector<DspBlock> blocks;
};

Status dspPresetWrite(const DspPreset& p, std::string& jsonOut);
Status dspPresetRead(std::string_view json, DspPreset& out);

}  // namespace sq::formats
