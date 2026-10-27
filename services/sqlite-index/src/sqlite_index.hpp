// services/sqlite-index internal C++ API (shared by the module and the tests).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sq/cpp/ptr.hpp"
#include "sq/interfaces/common.h"
#include "sq/interfaces/index.h"
#include "sq/sqlite_index/index_scanner.h"

namespace sq::sqlite_index {

inline constexpr uint32_t kSchemaVersion = 1;

// Factories (refcount 1).
Ptr<IContentIndex> createContentIndex();
Ptr<IIndexScanner> createIndexScanner();

// Registry used by the scanner to find codec services for formats it does not parse itself
// (set by the module in INIT, cleared in SHUTDOWN; null = built-in parsers only).
void setClassRegistry(IClassRegistry* reg);
IClassRegistry* classRegistry();

// ---- helpers (exposed for tests) ----------------------------------------------------
// ';'-list -> lowercase (foldTag), trimmed, de-duplicated, non-empty tags (order of first appearance).
std::vector<std::string> splitTags(const char* list);
std::string joinTags(const std::vector<std::string>& tags);
// Lowercase fold used for tags (ASCII + Latin/Greek/Cyrillic/Armenian/fullwidth, 1:1 mappings).
std::string foldTag(const std::string& tag);
// Free text -> FTS5 MATCH expression (every word as a quoted prefix term, AND-ed); "" if none.
std::string ftsExpression(const char* text);
// Smallest string greater than every string starting with `prefix` ("" if none exists).
std::string prefixUpperBound(const std::string& prefix);

// Metadata extracted from one file (metadata only, no decoding).
struct MetaValue {
  std::string key;
  int32_t type = SQ_VALUE_NONE;  // INT, DOUBLE, BOOL or STRING
  int64_t i = 0;
  double d = 0.0;
  std::string s;
};
struct FileInfo {
  std::string kind;   // "audio" | "midi" | "preset"
  std::string title;  // may be empty -> file stem
  std::vector<std::string> tags;
  std::vector<MetaValue> meta;
  void setInt(const std::string& k, int64_t v);
  void setDouble(const std::string& k, double v);
  void setBool(const std::string& k, bool v);
  void setString(const std::string& k, const std::string& v);
  const MetaValue* find(const std::string& k) const;
};
// "audio" / "midi" / "preset" for a lowercase extension, or nullptr if not indexable.
const char* kindForExtension(const std::string& ext);
// Reads the metadata of `path`. false only if the file cannot be read at all; a file whose
// content is not understood still yields kind + file info.
bool extractFileInfo(const std::string& path, FileInfo& out);

}  // namespace sq::sqlite_index
