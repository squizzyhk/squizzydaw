#include "sq/formats/core/xmlutil.hpp"

namespace sq::formats {

Status parseVersionedXml(std::string_view xml, const char* rootName, int maxFormat, XmlDocPtr& doc, XmlNode& root,
                         int* formatOut) {
  std::string err;
  doc = xmlParse(xml, &err);
  if (!doc) return Status::error(SQ_E_PARSE, std::string(rootName) + ": XML error: " + err);
  root = xmlRoot(doc);
  if (root.name() != rootName)
    return Status::error(SQ_E_PARSE, std::string("expected root <") + rootName + ">, found <" + std::string(root.name()) + ">");
  int64_t fmt = 0;
  if (!parseInt(root.attr("format"), fmt) || fmt < 1)
    return Status::error(SQ_E_PARSE, std::string(rootName) + ": missing or invalid format attribute");
  if (fmt > maxFormat)
    return Status::error(SQ_E_UNSUPPORTED, std::string(rootName) + ": format " + std::to_string(fmt) +
                                               " is newer than supported (" + std::to_string(maxFormat) + ")");
  if (formatOut) *formatOut = static_cast<int>(fmt);
  return {};
}

double xmlAttrDouble(const XmlNode& n, const char* name, double def) {
  double v = 0;
  return parseDouble(n.attr(name), v) ? v : def;
}

int64_t xmlAttrInt(const XmlNode& n, const char* name, int64_t def) {
  int64_t v = 0;
  return parseInt(n.attr(name), v) ? v : def;
}

bool xmlAttrBool(const XmlNode& n, const char* name, bool def) {
  std::string v = n.attr(name);
  if (v == "true" || v == "1") return true;
  if (v == "false" || v == "0") return false;
  return def;
}

}  // namespace sq::formats
