// Squizzy Studio formats - .langpack: ZIP with metainfo.xml, one gettext catalog per module
// (catalogs/<module>.mo) and named properties maps (properties/<name>.properties).
// Schema: SCHEMAS.md ".langpack".
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "sq/formats/core/gettext.hpp"
#include "sq/formats/core/metainfo.hpp"

namespace sq::formats {

struct LangPack {
  MetaInfo meta;                               // requires Package.Id and Language.Code
  std::map<std::string, Catalog> catalogs;     // module name -> catalog
  std::map<std::string, KeyValues> properties; // map name -> key/values
};

Status langPackWrite(const LangPack& lp, std::string& zipOut);
Status langPackRead(std::string_view zipBytes, LangPack& out);

}  // namespace sq::formats
