#include "sq/formats/core/jsonutil.hpp"

#include "sq/platform.hpp"

namespace sq::formats {

Status Json::parse(std::string_view text, Json& out) {
  sq_json* v = nullptr;
  char err[256] = {};
  if (sq_json_parse(text.data(), text.size(), &v, err, sizeof err) != SQ_OK)
    return Status::error(SQ_E_PARSE, std::string("JSON error: ") + err);
  out = Json(v, true);
  return {};
}

std::string Json::str() const {
  if (!isString()) return {};
  size_t len = 0;
  const char* s = sq_json_get_string(v_, &len);
  return std::string(s, len);
}

std::string Json::keyAt(size_t i) const {
  if (!isObject() || i >= size()) return {};
  const char* k = sq_json_key_at(v_, i);
  return k ? std::string(k) : std::string();
}

void Json::set(const char* key, Json&& child) {
  if (!isObject() || !child.owned_ || !child.v_) return;
  if (sq_json_set(v_, key, child.v_) == SQ_OK) {
    child.v_ = nullptr;
    child.owned_ = false;
  }
}

void Json::append(Json&& child) {
  if (!isArray() || !child.owned_ || !child.v_) return;
  if (sq_json_append(v_, child.v_) == SQ_OK) {
    child.v_ = nullptr;
    child.owned_ = false;
  }
}

std::string Json::write(bool pretty) const { return v_ ? jsonWrite(v_, pretty) : std::string("null"); }

}  // namespace sq::formats
