// Squizzy Studio formats - small helpers shared by the XML-based schemas.
#pragma once

#include <string>
#include <string_view>

#include "sq/formats/core/common.hpp"
#include "sq/platform.hpp"

namespace sq::formats {

// Parses xml, checks the root element name and its integer `format` attribute
// (1 <= format <= maxFormat). Every schema in SCHEMAS.md is versioned this way.
Status parseVersionedXml(std::string_view xml, const char* rootName, int maxFormat, XmlDocPtr& doc, XmlNode& root,
                         int* formatOut = nullptr);

double xmlAttrDouble(const XmlNode& n, const char* name, double def);
int64_t xmlAttrInt(const XmlNode& n, const char* name, int64_t def);
bool xmlAttrBool(const XmlNode& n, const char* name, bool def);

}  // namespace sq::formats
