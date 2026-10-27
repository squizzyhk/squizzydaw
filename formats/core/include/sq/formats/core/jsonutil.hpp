// Squizzy Studio formats - thin RAII wrapper over the platform JSON DOM (sq/platform/json.h).
// A Json either owns a root value or is a borrowed view into a container (from get/at).
// Accessors on a missing value return neutral defaults ("" / 0 / false), so readers can
// validate with explicit type checks where it matters.
#pragma once

#include <string>
#include <string_view>

#include "sq/formats/core/common.hpp"
#include "sq/platform/json.h"

namespace sq::formats {

class Json {
 public:
  Json() = default;
  Json(Json&& o) noexcept : v_(o.v_), owned_(o.owned_) { o.v_ = nullptr; o.owned_ = false; }
  Json& operator=(Json&& o) noexcept {
    if (this != &o) {
      reset();
      v_ = o.v_;
      owned_ = o.owned_;
      o.v_ = nullptr;
      o.owned_ = false;
    }
    return *this;
  }
  Json(const Json&) = delete;
  Json& operator=(const Json&) = delete;
  ~Json() { reset(); }

  static Json object() { return Json(sq_json_new_object(), true); }
  static Json array() { return Json(sq_json_new_array(), true); }
  static Json string(std::string_view s) { return Json(sq_json_new_string(s.data(), s.size()), true); }
  static Json number(double d) { return Json(sq_json_new_number(d), true); }
  static Json boolean(bool b) { return Json(sq_json_new_bool(b ? SQ_TRUE : SQ_FALSE), true); }
  static Json null() { return Json(sq_json_new_null(), true); }
  static Status parse(std::string_view text, Json& out);

  bool valid() const { return v_ != nullptr; }
  sq_json_type type() const { return v_ ? sq_json_type_of(v_) : SQ_JSON_NULL; }
  bool isObject() const { return type() == SQ_JSON_OBJECT; }
  bool isArray() const { return type() == SQ_JSON_ARRAY; }
  bool isString() const { return v_ && type() == SQ_JSON_STRING; }
  bool isNumber() const { return v_ && type() == SQ_JSON_NUMBER; }
  bool isBool() const { return v_ && type() == SQ_JSON_BOOL; }

  std::string str() const;
  double number() const { return isNumber() ? sq_json_get_number(v_) : 0.0; }
  int64_t integer() const { return isNumber() ? sq_json_get_int(v_) : 0; }
  bool boolean() const { return isBool() && sq_json_get_bool(v_) != 0; }

  size_t size() const { return (isObject() || isArray()) ? sq_json_size(v_) : 0; }
  Json at(size_t i) const { return Json(i < size() ? sq_json_at(v_, i) : nullptr, false); }
  std::string keyAt(size_t i) const;
  Json get(const char* key) const { return Json(isObject() ? sq_json_get(v_, key) : nullptr, false); }
  bool has(const char* key) const { return isObject() && sq_json_get(v_, key) != nullptr; }

  // Takes ownership of an owned child (a view or empty child is ignored).
  void set(const char* key, Json&& child);
  void append(Json&& child);

  std::string write(bool pretty = false) const;

 private:
  Json(sq_json* v, bool owned) : v_(v), owned_(owned) {}
  void reset() {
    if (owned_ && v_) sq_json_free(v_);
    v_ = nullptr;
    owned_ = false;
  }
  sq_json* v_ = nullptr;
  bool owned_ = false;
};

}  // namespace sq::formats
