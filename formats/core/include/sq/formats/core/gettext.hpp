// Squizzy Studio formats - gettext catalogs (.mo binary, .po source subset) and .properties maps.
// Implemented from the publicly documented GNU gettext MO layout; no gettext code is used.
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

// Message catalog. Keys follow the MO convention: "ctx\x04msgid" for a context, plural forms
// as "msgid\0msgid_plural" -> "str0\0str1...". The header is the entry with the empty key.
struct Catalog {
  std::map<std::string, std::string> messages;

  // Lookup helpers (return nullptr if missing or untranslated).
  const std::string* find(std::string_view msgid, std::string_view context = {}) const;
};

Status moWrite(const Catalog& c, std::string& out);           // little endian, no hash table
Status moRead(std::string_view bytes, Catalog& out);          // either byte order
// .po subset: msgctxt, msgid, msgid_plural, msgstr, msgstr[n], "..." continuation lines,
// C escapes (\n \t \r \" \\ \a \b \f \v), comments. Fuzzy entries (#, fuzzy) are dropped and
// empty translations are skipped, matching what msgfmt emits by default.
Status poParse(std::string_view text, Catalog& out);
std::string poWrite(const Catalog& c);

// .properties: "key = value" / "key: value" / "key value", '#' and '!' comments, trailing '\'
// line continuation, escapes \t \n \r \f \\ \= \: \# \! \uXXXX (surrogate pairs combined).
// Text is UTF-8. Duplicate keys: last one wins (position of the first is kept).
Status propertiesParse(std::string_view text, KeyValues& out);
std::string propertiesWrite(const KeyValues& kv);  // escapes as needed; UTF-8 kept as-is

}  // namespace sq::formats
