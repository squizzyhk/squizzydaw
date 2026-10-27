// Squizzy Studio formats - metainfo.xml: the ordered attribute list carried by every package
// (presets, langpacks, extensions). Schema: formats/SCHEMAS.md "metainfo.xml".
#pragma once

#include <string>
#include <string_view>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

namespace meta {
// Well-known keys (dotted, case-sensitive). Any other key is preserved as-is.
inline constexpr const char* kId = "Package.Id";
inline constexpr const char* kName = "Package.Name";
inline constexpr const char* kVersion = "Package.Version";
inline constexpr const char* kVendor = "Package.Vendor";
inline constexpr const char* kDescription = "Package.Description";
inline constexpr const char* kCategory = "Package.Category";
inline constexpr const char* kCreated = "Package.Created";      // ISO 8601
inline constexpr const char* kLanguage = "Language.Code";        // BCP 47, langpacks
inline constexpr const char* kLanguageName = "Language.Name";
inline constexpr const char* kPresetKind = "Preset.Kind";        // preset | multipreset | instrument | trackpreset
inline constexpr const char* kPresetClass = "Preset.ClassId";    // braced GUID of the target processor
}  // namespace meta

inline constexpr int kMetaInfoFormat = 1;
inline constexpr const char* kMetaInfoFile = "metainfo.xml";

struct MetaInfo {
  KeyValues attrs;

  std::string get(std::string_view key, std::string_view def = {}) const {
    const std::string* v = findValue(attrs, key);
    return v ? *v : std::string(def);
  }
  bool has(std::string_view key) const { return findValue(attrs, key) != nullptr; }
  void set(std::string_view key, std::string value) { setValue(attrs, key, std::move(value)); }
};

std::string metaInfoToXml(const MetaInfo& m);
// Rejects a newer major format and malformed documents.
Status metaInfoFromXml(std::string_view xml, MetaInfo& out);

}  // namespace sq::formats
