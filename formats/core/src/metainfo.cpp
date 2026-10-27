#include "sq/formats/core/metainfo.hpp"

#include "sq/formats/core/xmlutil.hpp"
#include "sq/platform.hpp"

namespace sq::formats {

std::string metaInfoToXml(const MetaInfo& m) {
  auto doc = xmlCreate("MetaInfo");
  XmlNode root = xmlRoot(doc);
  root.setAttr("format", std::to_string(kMetaInfoFormat));
  for (const auto& [k, v] : m.attrs) {
    XmlNode a = root.append("Attribute");
    a.setAttr("key", k);
    a.setAttr("value", v);
  }
  return xmlWrite(doc);
}

Status metaInfoFromXml(std::string_view xml, MetaInfo& out) {
  out.attrs.clear();
  XmlDocPtr doc;
  XmlNode root;
  SQF_TRY(parseVersionedXml(xml, "MetaInfo", kMetaInfoFormat, doc, root));
  for (XmlNode a : root.children("Attribute")) {
    std::string key = a.attr("key");
    if (key.empty()) return Status::error(SQ_E_PARSE, "metainfo: attribute without key");
    if (findValue(out.attrs, key)) return Status::error(SQ_E_PARSE, "metainfo: duplicate key '" + key + "'");
    out.attrs.emplace_back(std::move(key), a.attr("value"));
  }
  return {};
}

}  // namespace sq::formats
