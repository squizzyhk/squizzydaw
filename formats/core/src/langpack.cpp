#include "sq/formats/core/langpack.hpp"

#include "sq/formats/core/zip.hpp"

namespace sq::formats {

namespace {
constexpr std::string_view kCatalogDir = "catalogs/";
constexpr std::string_view kPropsDir = "properties/";

Status checkMeta(const MetaInfo& m) {
  if (!isSafeIdentifier(m.get(meta::kId))) return Status::error(SQ_E_INVALIDARG, "langpack: missing or invalid Package.Id");
  std::string lang = m.get(meta::kLanguage);
  if (lang.empty() || lang.size() > 35) return Status::error(SQ_E_INVALIDARG, "langpack: missing Language.Code");
  for (char c : lang)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return Status::error(SQ_E_INVALIDARG, "langpack: Language.Code must be a BCP 47 tag");
  return {};
}
}  // namespace

Status langPackWrite(const LangPack& lp, std::string& zipOut) {
  SQF_TRY(checkMeta(lp.meta));
  ZipArchive zip;
  zip.add(kMetaInfoFile, metaInfoToXml(lp.meta));
  for (const auto& [module, cat] : lp.catalogs) {
    if (!isSafeIdentifier(module)) return Status::error(SQ_E_INVALIDARG, "langpack: invalid module name '" + module + "'");
    std::string mo;
    SQF_TRY(moWrite(cat, mo));
    zip.add(std::string(kCatalogDir) + module + ".mo", std::move(mo));
  }
  for (const auto& [name, kv] : lp.properties) {
    if (!isSafeIdentifier(name)) return Status::error(SQ_E_INVALIDARG, "langpack: invalid properties name '" + name + "'");
    zip.add(std::string(kPropsDir) + name + ".properties", propertiesWrite(kv));
  }
  return zipWrite(zip, zipOut);
}

Status langPackRead(std::string_view zipBytes, LangPack& out) {
  out = LangPack{};
  ZipArchive zip;
  SQF_TRY(zipRead(zipBytes, zip, ZipLimits::metadata()));
  const std::string* mi = zip.find(kMetaInfoFile);
  if (!mi) return Status::error(SQ_E_PARSE, "langpack: missing metainfo.xml");
  SQF_TRY(metaInfoFromXml(*mi, out.meta));
  SQF_TRY(checkMeta(out.meta));
  for (const auto& e : zip.entries) {
    std::string_view n = e.name;
    if (n.substr(0, kCatalogDir.size()) == kCatalogDir && fileExtension(n) == "mo") {
      std::string module = fileStem(n);
      if (n.substr(kCatalogDir.size()).find('/') != std::string_view::npos || !isSafeIdentifier(module))
        return Status::error(SQ_E_PARSE, "langpack: unexpected catalog path '" + e.name + "'");
      Status s = moRead(e.data, out.catalogs[module]);
      if (!s) return Status::error(s.code, "langpack: " + e.name + ": " + s.message);
    } else if (n.substr(0, kPropsDir.size()) == kPropsDir && fileExtension(n) == "properties") {
      std::string name = fileStem(n);
      if (n.substr(kPropsDir.size()).find('/') != std::string_view::npos || !isSafeIdentifier(name))
        return Status::error(SQ_E_PARSE, "langpack: unexpected properties path '" + e.name + "'");
      Status s = propertiesParse(e.data, out.properties[name]);
      if (!s) return Status::error(s.code, "langpack: " + e.name + ": " + s.message);
    }
    // other members (README, licence text ...) are ignored
  }
  return {};
}

}  // namespace sq::formats
