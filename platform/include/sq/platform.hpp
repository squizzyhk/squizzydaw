// Squizzy Studio platform layer - header-only C++ conveniences over the C ABI.
// These are inline wrappers compiled into the caller; the ABI remains the C functions.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "sq/platform.h"

namespace sq {

// ---- buffers / strings -------------------------------------------------------------
class Buffer {
 public:
  Buffer() = default;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer() { sq_buffer_free(&b_); }
  sq_buffer* out() {
    sq_buffer_free(&b_);
    return &b_;
  }
  const char* data() const { return b_.data ? b_.data : ""; }
  size_t size() const { return b_.size; }
  std::string str() const { return b_.data ? std::string(b_.data, b_.size) : std::string(); }

 private:
  sq_buffer b_{nullptr, 0};
};

inline std::string specialDir(sq_special_dir which) {
  Buffer b;
  return sq_special_dir_get(which, b.out()) == SQ_OK ? b.str() : std::string();
}

inline std::optional<std::string> readFile(const std::string& path) {
  Buffer b;
  if (sq_fs_read_file(path.c_str(), b.out()) != SQ_OK) return std::nullopt;
  return b.str();
}

inline sq_result writeFile(const std::string& path, std::string_view data) {
  return sq_fs_write_file(path.c_str(), data.data(), data.size());
}

inline std::string joinPath(std::string a, std::string_view b) {
  if (!a.empty() && a.back() != '/' && a.back() != '\\') a += '/';
  a.append(b);
  return a;
}

// ---- XML -----------------------------------------------------------------------------
class XmlNode {
 public:
  XmlNode() = default;
  explicit XmlNode(sq_xml_node* n) : n_(n) {}
  explicit operator bool() const { return n_ != nullptr; }
  sq_xml_node* raw() const { return n_; }

  std::string_view name() const { return n_ ? sq_xml_name(n_) : ""; }
  std::string_view text() const { return n_ ? sq_xml_text(n_) : ""; }
  void setText(const std::string& t) { sq_xml_set_text(n_, t.c_str()); }
  std::string attr(const char* name, const std::string& def = {}) const {
    const char* v = n_ ? sq_xml_attr(n_, name) : nullptr;
    return v ? std::string(v) : def;
  }
  bool hasAttr(const char* name) const { return n_ && sq_xml_attr(n_, name) != nullptr; }
  void setAttr(const char* name, const std::string& v) { sq_xml_set_attr(n_, name, v.c_str()); }
  XmlNode child(const char* name = nullptr) const { return XmlNode(n_ ? sq_xml_first_child(n_, name) : nullptr); }
  XmlNode next(const char* name = nullptr) const { return XmlNode(n_ ? sq_xml_next_sibling(n_, name) : nullptr); }
  XmlNode parent() const { return XmlNode(n_ ? sq_xml_parent(n_) : nullptr); }
  XmlNode append(const char* name) { return XmlNode(sq_xml_append_child(n_, name)); }

  // for (XmlNode c : node.children("Section")) ...
  struct Range {
    sq_xml_node* first;
    const char* filter;
    struct It {
      sq_xml_node* n;
      const char* f;
      XmlNode operator*() const { return XmlNode(n); }
      It& operator++() {
        n = sq_xml_next_sibling(n, f);
        return *this;
      }
      bool operator!=(const It& o) const { return n != o.n; }
    };
    It begin() const { return It{first, filter}; }
    It end() const { return It{nullptr, filter}; }
  };
  Range children(const char* name = nullptr) const { return Range{n_ ? sq_xml_first_child(n_, name) : nullptr, name}; }

 private:
  sq_xml_node* n_ = nullptr;
};

struct XmlDocDeleter {
  void operator()(sq_xml_doc* d) const { sq_xml_free(d); }
};
using XmlDocPtr = std::unique_ptr<sq_xml_doc, XmlDocDeleter>;

inline XmlDocPtr xmlParse(std::string_view s, std::string* err = nullptr) {
  sq_xml_doc* d = nullptr;
  char e[256] = {};
  if (sq_xml_parse(s.data(), s.size(), &d, e, sizeof e) != SQ_OK) {
    if (err) *err = e;
    return nullptr;
  }
  return XmlDocPtr(d);
}
inline XmlDocPtr xmlCreate(const char* root) { return XmlDocPtr(sq_xml_create(root)); }
inline XmlNode xmlRoot(const XmlDocPtr& d) { return XmlNode(d ? sq_xml_root(d.get()) : nullptr); }
inline std::string xmlWrite(const XmlDocPtr& d, bool pretty = true) {
  Buffer b;
  return d && sq_xml_write(d.get(), pretty ? SQ_TRUE : SQ_FALSE, b.out()) == SQ_OK ? b.str() : std::string();
}

// ---- JSON ----------------------------------------------------------------------------
struct JsonDeleter {
  void operator()(sq_json* v) const { sq_json_free(v); }
};
using JsonPtr = std::unique_ptr<sq_json, JsonDeleter>;

inline JsonPtr jsonParse(std::string_view s, std::string* err = nullptr) {
  sq_json* v = nullptr;
  char e[256] = {};
  if (sq_json_parse(s.data(), s.size(), &v, e, sizeof e) != SQ_OK) {
    if (err) *err = e;
    return nullptr;
  }
  return JsonPtr(v);
}
inline std::string jsonWrite(const sq_json* v, bool pretty = false) {
  Buffer b;
  return v && sq_json_write(v, pretty ? SQ_TRUE : SQ_FALSE, b.out()) == SQ_OK ? b.str() : std::string();
}

// ---- hashing -------------------------------------------------------------------------
inline std::string sha256Hex(std::string_view data) {
  uint8_t d[32];
  sq_sha256(data.data(), data.size(), d);
  char hex[65];
  sq_hex_encode(d, 32, hex);
  return std::string(hex, 64);
}

}  // namespace sq
