// JSON DOM: own parser (RFC 8259) + writer.
#include <charconv>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "internal.h"

struct sq_json {
  sq_json_type type = SQ_JSON_NULL;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<sq_json*> arr;                           // array elements
  std::vector<std::pair<std::string, sq_json*>> obj;  // object members (ordered)
  sq_json* parent = nullptr;

  ~sq_json() {
    for (auto* c : arr) delete c;
    for (auto& m : obj) delete m.second;
  }
};

namespace {

constexpr int kMaxDepth = 512;

class Parser {
 public:
  Parser(const char* d, size_t n) : s_(d), p_(d), end_(d + n) {}

  sq_json* parseDocument() {
    ws();
    sq_json* v = value(0);
    if (!v) return nullptr;
    ws();
    if (p_ != end_) {
      delete v;
      return fail("trailing characters");
    }
    return v;
  }
  const std::string& error() const { return err_; }

 private:
  const char* s_;
  const char* p_;
  const char* end_;
  std::string err_;

  sq_json* fail(const char* msg) {
    if (err_.empty()) err_ = "offset " + std::to_string(p_ - s_) + ": " + msg;
    return nullptr;
  }
  void ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }
  bool lit(const char* w) {
    size_t n = std::strlen(w);
    if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, w, n) != 0) return false;
    p_ += n;
    return true;
  }
  bool hex4(uint32_t& v) {
    if (end_ - p_ < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = *p_++;
      int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      v = (v << 4) | static_cast<uint32_t>(d);
    }
    return true;
  }
  bool string(std::string& out) {
    ++p_;  // opening quote
    while (p_ < end_) {
      unsigned char c = static_cast<unsigned char>(*p_);
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20) return false;
      if (c == '\\') {
        ++p_;
        if (p_ >= end_) return false;
        char e = *p_++;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            uint32_t cp;
            if (!hex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              uint32_t lo;
              if (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                p_ += 2;
                if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                cp = 0xFFFD;
              }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              cp = 0xFFFD;
            }
            char buf[4];
            out.append(buf, sq_utf8_encode(cp, buf));
            break;
          }
          default: return false;
        }
      } else {
        out += static_cast<char>(c);
        ++p_;
      }
    }
    return false;
  }
  sq_json* number() {
    const char* start = p_;
    if (p_ < end_ && *p_ == '-') ++p_;
    if (p_ >= end_) return fail("bad number");
    if (*p_ == '0') {
      ++p_;
    } else if (*p_ >= '1' && *p_ <= '9') {
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    } else {
      return fail("bad number");
    }
    if (p_ < end_ && *p_ == '.') {
      ++p_;
      if (p_ >= end_ || *p_ < '0' || *p_ > '9') return fail("bad fraction");
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
      if (p_ >= end_ || *p_ < '0' || *p_ > '9') return fail("bad exponent");
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    double d = 0;
    if (!sqp::parseDouble(start, p_, d)) {
      // Syntax was validated above, so a failure here is an out-of-range magnitude.
      bool nonzero = false;
      for (const char* q = start; q < p_ && *q != 'e' && *q != 'E'; ++q)
        if (*q >= '1' && *q <= '9') nonzero = true;
      const char* ex = start;
      while (ex < p_ && *ex != 'e' && *ex != 'E') ++ex;
      bool negExp = ex + 1 < p_ && ex[1] == '-';
      if (!nonzero || negExp) d = 0.0;
      else d = (*start == '-') ? -HUGE_VAL : HUGE_VAL;
    }
    auto* v = new sq_json();
    v->type = SQ_JSON_NUMBER;
    v->num = d;
    return v;
  }
  sq_json* value(int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (p_ >= end_) return fail("unexpected end");
    char c = *p_;
    if (c == '{') {
      ++p_;
      auto* o = new sq_json();
      o->type = SQ_JSON_OBJECT;
      ws();
      if (p_ < end_ && *p_ == '}') {
        ++p_;
        return o;
      }
      for (;;) {
        ws();
        std::string key;
        if (p_ >= end_ || *p_ != '"' || !string(key)) {
          delete o;
          return fail("expected object key");
        }
        ws();
        if (p_ >= end_ || *p_ != ':') {
          delete o;
          return fail("expected ':'");
        }
        ++p_;
        ws();
        sq_json* v = value(depth + 1);
        if (!v) {
          delete o;
          return nullptr;
        }
        v->parent = o;
        bool replaced = false;
        for (auto& m : o->obj)
          if (m.first == key) {  // duplicate key: last one wins
            delete m.second;
            m.second = v;
            replaced = true;
          }
        if (!replaced) o->obj.emplace_back(std::move(key), v);
        ws();
        if (p_ < end_ && *p_ == ',') {
          ++p_;
          continue;
        }
        if (p_ < end_ && *p_ == '}') {
          ++p_;
          return o;
        }
        delete o;
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++p_;
      auto* a = new sq_json();
      a->type = SQ_JSON_ARRAY;
      ws();
      if (p_ < end_ && *p_ == ']') {
        ++p_;
        return a;
      }
      for (;;) {
        ws();
        sq_json* v = value(depth + 1);
        if (!v) {
          delete a;
          return nullptr;
        }
        v->parent = a;
        a->arr.push_back(v);
        ws();
        if (p_ < end_ && *p_ == ',') {
          ++p_;
          continue;
        }
        if (p_ < end_ && *p_ == ']') {
          ++p_;
          return a;
        }
        delete a;
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      auto* v = new sq_json();
      v->type = SQ_JSON_STRING;
      if (!string(v->str)) {
        delete v;
        return fail("bad string");
      }
      return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return number();
    if (lit("true")) {
      auto* v = new sq_json();
      v->type = SQ_JSON_BOOL;
      v->b = true;
      return v;
    }
    if (lit("false")) {
      auto* v = new sq_json();
      v->type = SQ_JSON_BOOL;
      return v;
    }
    if (lit("null")) return new sq_json();
    return fail("unexpected character");
  }
};

void writeString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void writeNumber(std::string& out, double d) {
  if (!std::isfinite(d)) {
    out += "null";  // JSON has no NaN/Inf
    return;
  }
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, d);
  out.append(buf, res.ptr);
}

void indent(std::string& out, bool pretty, int depth) {
  if (!pretty) return;
  out += '\n';
  out.append(static_cast<size_t>(depth) * 2, ' ');
}

void writeValue(std::string& out, const sq_json* v, bool pretty, int depth) {
  switch (v->type) {
    case SQ_JSON_NULL: out += "null"; break;
    case SQ_JSON_BOOL: out += v->b ? "true" : "false"; break;
    case SQ_JSON_NUMBER: writeNumber(out, v->num); break;
    case SQ_JSON_STRING: writeString(out, v->str); break;
    case SQ_JSON_ARRAY:
      out += '[';
      for (size_t i = 0; i < v->arr.size(); ++i) {
        if (i) out += ',';
        indent(out, pretty, depth + 1);
        writeValue(out, v->arr[i], pretty, depth + 1);
      }
      if (!v->arr.empty()) indent(out, pretty, depth);
      out += ']';
      break;
    case SQ_JSON_OBJECT:
      out += '{';
      for (size_t i = 0; i < v->obj.size(); ++i) {
        if (i) out += ',';
        indent(out, pretty, depth + 1);
        writeString(out, v->obj[i].first);
        out += pretty ? ": " : ":";
        writeValue(out, v->obj[i].second, pretty, depth + 1);
      }
      if (!v->obj.empty()) indent(out, pretty, depth);
      out += '}';
      break;
  }
}

sq_json* make(sq_json_type t) {
  auto* v = new (std::nothrow) sq_json();
  if (v) v->type = t;
  return v;
}

}  // namespace

extern "C" {

sq_result sq_json_parse(const char* data, size_t len, sq_json** out, char* err, size_t err_size) {
  if (!out || (!data && len)) return SQ_E_INVALIDARG;
  *out = nullptr;
  try {
    Parser p(data, len);
    sq_json* v = p.parseDocument();
    if (!v) {
      sqp::copyErr(err, err_size, p.error());
      return SQ_E_PARSE;
    }
    *out = v;
    return SQ_OK;
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result sq_json_write(const sq_json* v, sq_bool pretty, sq_buffer* out) {
  if (!v || !out) return SQ_E_INVALIDARG;
  std::string s;
  writeValue(s, v, pretty != 0, 0);
  return sqp::toBuffer(s, out);
}

void sq_json_free(sq_json* v) {
  if (v && !v->parent) delete v;  // owned values are freed by their container
}

sq_json* sq_json_new_null(void) { return make(SQ_JSON_NULL); }
sq_json* sq_json_new_bool(sq_bool b) {
  sq_json* v = make(SQ_JSON_BOOL);
  if (v) v->b = b != 0;
  return v;
}
sq_json* sq_json_new_number(double d) {
  sq_json* v = make(SQ_JSON_NUMBER);
  if (v) v->num = d;
  return v;
}
sq_json* sq_json_new_string(const char* s, size_t len) {
  sq_json* v = make(SQ_JSON_STRING);
  if (v && s) v->str.assign(s, len == static_cast<size_t>(-1) ? std::strlen(s) : len);
  return v;
}
sq_json* sq_json_new_array(void) { return make(SQ_JSON_ARRAY); }
sq_json* sq_json_new_object(void) { return make(SQ_JSON_OBJECT); }

sq_json_type sq_json_type_of(const sq_json* v) { return v ? v->type : SQ_JSON_NULL; }
sq_bool sq_json_get_bool(const sq_json* v) { return v && v->type == SQ_JSON_BOOL && v->b ? SQ_TRUE : SQ_FALSE; }
double sq_json_get_number(const sq_json* v) { return v && v->type == SQ_JSON_NUMBER ? v->num : 0.0; }
int64_t sq_json_get_int(const sq_json* v) {
  if (!v || v->type != SQ_JSON_NUMBER || !std::isfinite(v->num)) return 0;
  if (v->num >= 9.2233720368547758e18) return INT64_MAX;
  if (v->num <= -9.2233720368547758e18) return INT64_MIN;
  return static_cast<int64_t>(v->num);
}
const char* sq_json_get_string(const sq_json* v, size_t* len) {
  if (!v || v->type != SQ_JSON_STRING) {
    if (len) *len = 0;
    return "";
  }
  if (len) *len = v->str.size();
  return v->str.c_str();
}

size_t sq_json_size(const sq_json* v) {
  if (!v) return 0;
  return v->type == SQ_JSON_ARRAY ? v->arr.size() : v->type == SQ_JSON_OBJECT ? v->obj.size() : 0;
}
sq_json* sq_json_at(const sq_json* v, size_t i) {
  if (!v) return nullptr;
  if (v->type == SQ_JSON_ARRAY) return i < v->arr.size() ? v->arr[i] : nullptr;
  if (v->type == SQ_JSON_OBJECT) return i < v->obj.size() ? v->obj[i].second : nullptr;
  return nullptr;
}
const char* sq_json_key_at(const sq_json* v, size_t i) {
  return v && v->type == SQ_JSON_OBJECT && i < v->obj.size() ? v->obj[i].first.c_str() : nullptr;
}
sq_json* sq_json_get(const sq_json* obj, const char* key) {
  if (!obj || obj->type != SQ_JSON_OBJECT || !key) return nullptr;
  for (auto& m : obj->obj)
    if (m.first == key) return m.second;
  return nullptr;
}

sq_result sq_json_append(sq_json* arr, sq_json* child) {
  if (!arr || !child || arr->type != SQ_JSON_ARRAY || child->parent || child == arr) return SQ_E_INVALIDARG;
  arr->arr.push_back(child);
  child->parent = arr;
  return SQ_OK;
}
sq_result sq_json_set(sq_json* obj, const char* key, sq_json* child) {
  if (!obj || !key || !child || obj->type != SQ_JSON_OBJECT || child->parent || child == obj) return SQ_E_INVALIDARG;
  child->parent = obj;
  for (auto& m : obj->obj)
    if (m.first == key) {
      delete m.second;
      m.second = child;
      return SQ_OK;
    }
  obj->obj.emplace_back(key, child);
  return SQ_OK;
}
sq_result sq_json_remove(sq_json* obj, const char* key) {
  if (!obj || !key || obj->type != SQ_JSON_OBJECT) return SQ_E_INVALIDARG;
  for (auto it = obj->obj.begin(); it != obj->obj.end(); ++it)
    if (it->first == key) {
      delete it->second;
      obj->obj.erase(it);
      return SQ_OK;
    }
  return SQ_E_NOTFOUND;
}

}  // extern "C"
