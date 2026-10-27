// cloud-client: small helpers over the platform JSON DOM.
#pragma once

#include <cstdint>
#include <string>

#include "sq/platform.hpp"

namespace sq::cloud {

inline std::string jsonString(const sq_json* obj, const char* key) {
  const sq_json* v = obj ? sq_json_get(obj, key) : nullptr;
  if (!v || sq_json_type_of(v) != SQ_JSON_STRING) return {};
  size_t n = 0;
  const char* s = sq_json_get_string(v, &n);
  return std::string(s, n);
}

inline int64_t jsonInt(const sq_json* obj, const char* key, int64_t def) {
  const sq_json* v = obj ? sq_json_get(obj, key) : nullptr;
  if (!v || sq_json_type_of(v) != SQ_JSON_NUMBER) return def;
  return sq_json_get_int(v);
}

inline sq_json* jsonStr(const std::string& s) { return sq_json_new_string(s.data(), s.size()); }
inline sq_json* jsonNum(int64_t v) { return sq_json_new_number(static_cast<double>(v)); }

}  // namespace sq::cloud
