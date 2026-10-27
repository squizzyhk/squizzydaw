// Minimal XML DOM: own recursive-descent parser + writer.
#include <string>
#include <utility>
#include <vector>

#include "internal.h"

struct sq_xml_node {
  std::string name;
  std::string text;
  std::vector<std::pair<std::string, std::string>> attrs;
  sq_xml_node* parent = nullptr;
  sq_xml_node* first = nullptr;
  sq_xml_node* last = nullptr;
  sq_xml_node* next = nullptr;
  sq_xml_node* prev = nullptr;

  ~sq_xml_node() {
    sq_xml_node* c = first;
    while (c) {
      sq_xml_node* n = c->next;
      delete c;
      c = n;
    }
  }
  void append(sq_xml_node* c) {
    c->parent = this;
    c->prev = last;
    c->next = nullptr;
    if (last) last->next = c;
    else first = c;
    last = c;
  }
  void unlink(sq_xml_node* c) {
    if (c->prev) c->prev->next = c->next;
    else first = c->next;
    if (c->next) c->next->prev = c->prev;
    else last = c->prev;
    c->parent = c->next = c->prev = nullptr;
  }
};

struct sq_xml_doc {
  sq_xml_node* root = nullptr;
  ~sq_xml_doc() { delete root; }
};

namespace {

constexpr int kMaxDepth = 256;

bool isNameStart(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80; }
bool isNameChar(unsigned char c) { return isNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool validName(const char* s) {
  if (!s || !isNameStart(static_cast<unsigned char>(*s))) return false;
  for (++s; *s; ++s)
    if (!isNameChar(static_cast<unsigned char>(*s))) return false;
  return true;
}

class Parser {
 public:
  Parser(const char* d, size_t n) : p_(d), end_(d + n) {}

  sq_xml_node* parseDocument() {
    if (end_ - p_ >= 3 && static_cast<unsigned char>(p_[0]) == 0xEF && static_cast<unsigned char>(p_[1]) == 0xBB &&
        static_cast<unsigned char>(p_[2]) == 0xBF)
      p_ += 3;
    skipMisc();
    if (failed_) return nullptr;
    if (p_ >= end_ || *p_ != '<') return fail("expected root element");
    sq_xml_node* root = parseElement(0);
    if (!root) return nullptr;
    skipMisc();
    if (!failed_ && p_ < end_) {
      delete root;
      return fail("content after root element");
    }
    if (failed_) {
      delete root;
      return nullptr;
    }
    return root;
  }
  const std::string& error() const { return err_; }

 private:
  const char* p_;
  const char* end_;
  int line_ = 1;
  bool failed_ = false;
  std::string err_;

  sq_xml_node* fail(const char* msg) {
    if (!failed_) {
      failed_ = true;
      err_ = "line " + std::to_string(line_) + ": " + msg;
    }
    return nullptr;
  }
  bool startsWith(const char* s) const {
    size_t n = std::strlen(s);
    return static_cast<size_t>(end_ - p_) >= n && std::memcmp(p_, s, n) == 0;
  }
  void advance(size_t n = 1) {
    for (size_t i = 0; i < n && p_ < end_; ++i, ++p_)
      if (*p_ == '\n') ++line_;
  }
  void skipSpace() {
    while (p_ < end_ && isSpace(*p_)) advance();
  }
  bool skipUntil(const char* terminator) {
    while (p_ < end_ && !startsWith(terminator)) advance();
    if (p_ >= end_) return false;
    advance(std::strlen(terminator));
    return true;
  }
  // Prolog / epilog: whitespace, comments, PIs, DOCTYPE.
  void skipMisc() {
    for (;;) {
      skipSpace();
      if (startsWith("<?")) {
        if (!skipUntil("?>")) {
          fail("unterminated processing instruction");
          return;
        }
      } else if (startsWith("<!--")) {
        if (!skipUntil("-->")) {
          fail("unterminated comment");
          return;
        }
      } else if (startsWith("<!DOCTYPE")) {
        int depth = 0;
        while (p_ < end_) {
          char c = *p_;
          advance();
          if (c == '[') ++depth;
          else if (c == ']') --depth;
          else if (c == '>' && depth <= 0) break;
        }
      } else {
        return;
      }
    }
  }
  bool parseName(std::string& out) {
    if (p_ >= end_ || !isNameStart(static_cast<unsigned char>(*p_))) return false;
    const char* s = p_;
    while (p_ < end_ && isNameChar(static_cast<unsigned char>(*p_))) ++p_;
    out.assign(s, p_);
    return true;
  }
  // Decodes an entity at p_ ('&'), appending to out.
  bool parseEntity(std::string& out) {
    const char* semi = p_;
    while (semi < end_ && *semi != ';' && semi - p_ < 12) ++semi;
    if (semi >= end_ || *semi != ';') return false;
    std::string ent(p_ + 1, semi);
    if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "amp") out += '&';
    else if (ent == "quot") out += '"';
    else if (ent == "apos") out += '\'';
    else if (ent.size() > 1 && ent[0] == '#') {
      uint32_t cp = 0;
      bool hex = ent[1] == 'x' || ent[1] == 'X';
      size_t i = hex ? 2 : 1;
      if (i >= ent.size()) return false;
      for (; i < ent.size(); ++i) {
        char c = ent[i];
        int v = (c >= '0' && c <= '9') ? c - '0' : hex && (c >= 'a' && c <= 'f') ? c - 'a' + 10 : hex && (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(v);
        if (cp > 0x10FFFF) return false;
      }
      char buf[4];
      size_t k = sq_utf8_encode(cp, buf);
      if (!k || cp == 0) return false;
      out.append(buf, k);
    } else {
      return false;
    }
    p_ = semi + 1;
    return true;
  }
  bool parseAttrValue(std::string& out) {
    if (p_ >= end_ || (*p_ != '"' && *p_ != '\'')) return false;
    char q = *p_;
    advance();
    while (p_ < end_ && *p_ != q) {
      if (*p_ == '<') return false;
      if (*p_ == '&') {
        if (!parseEntity(out)) return false;
      } else {
        char c = *p_;
        // attribute-value normalization: whitespace chars become spaces
        out += (c == '\n' || c == '\t' || c == '\r') ? ' ' : c;
        advance();
      }
    }
    if (p_ >= end_) return false;
    advance();
    return true;
  }

  sq_xml_node* parseElement(int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    advance();  // '<'
    auto* node = new sq_xml_node();
    if (!parseName(node->name)) {
      delete node;
      return fail("expected element name");
    }
    // attributes
    for (;;) {
      bool hadSpace = p_ < end_ && isSpace(*p_);
      skipSpace();
      if (p_ >= end_) {
        delete node;
        return fail("unexpected end in start tag");
      }
      if (*p_ == '/') {
        if (!startsWith("/>")) {
          delete node;
          return fail("expected '/>'");
        }
        advance(2);
        return node;
      }
      if (*p_ == '>') {
        advance();
        break;
      }
      std::string an, av;
      if (!hadSpace || !parseName(an)) {
        delete node;
        return fail("malformed attribute");
      }
      skipSpace();
      if (p_ >= end_ || *p_ != '=') {
        delete node;
        return fail("expected '=' after attribute name");
      }
      advance();
      skipSpace();
      if (!parseAttrValue(av)) {
        delete node;
        return fail("malformed attribute value");
      }
      for (auto& a : node->attrs)
        if (a.first == an) {
          delete node;
          return fail("duplicate attribute");
        }
      node->attrs.emplace_back(std::move(an), std::move(av));
    }
    // content
    for (;;) {
      if (p_ >= end_) {
        delete node;
        return fail("unexpected end of document (unclosed element)");
      }
      if (*p_ == '<') {
        if (startsWith("</")) {
          advance(2);
          std::string close;
          if (!parseName(close) || close != node->name) {
            delete node;
            return fail("mismatched closing tag");
          }
          skipSpace();
          if (p_ >= end_ || *p_ != '>') {
            delete node;
            return fail("expected '>'");
          }
          advance();
          break;
        } else if (startsWith("<!--")) {
          if (!skipUntil("-->")) {
            delete node;
            return fail("unterminated comment");
          }
        } else if (startsWith("<![CDATA[")) {
          advance(9);
          const char* s = p_;
          while (p_ < end_ && !startsWith("]]>")) advance();
          if (p_ >= end_) {
            delete node;
            return fail("unterminated CDATA");
          }
          node->text.append(s, p_);
          advance(3);
        } else if (startsWith("<?")) {
          if (!skipUntil("?>")) {
            delete node;
            return fail("unterminated processing instruction");
          }
        } else {
          sq_xml_node* child = parseElement(depth + 1);
          if (!child) {
            delete node;
            return nullptr;
          }
          node->append(child);
        }
      } else if (*p_ == '&') {
        if (!parseEntity(node->text)) {
          delete node;
          return fail("bad entity reference");
        }
      } else {
        node->text += *p_;
        advance();
      }
    }
    if (node->first) {
      bool allSpace = true;
      for (char c : node->text)
        if (!isSpace(c)) {
          allSpace = false;
          break;
        }
      if (allSpace) node->text.clear();
    }
    return node;
  }
};

void escapeInto(std::string& out, const std::string& s, bool attr) {
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"':
        if (attr) out += "&quot;";
        else out += c;
        break;
      case '\n':
        if (attr) out += "&#10;";
        else out += c;
        break;
      case '\t':
        if (attr) out += "&#9;";
        else out += c;
        break;
      case '\r': out += "&#13;"; break;
      default: out += c;
    }
  }
}

void writeNode(std::string& out, const sq_xml_node* n, bool pretty, int depth) {
  if (pretty) out.append(static_cast<size_t>(depth) * 2, ' ');
  out += '<';
  out += n->name;
  for (auto& a : n->attrs) {
    out += ' ';
    out += a.first;
    out += "=\"";
    escapeInto(out, a.second, true);
    out += '"';
  }
  if (!n->first && n->text.empty()) {
    out += "/>";
    if (pretty) out += '\n';
    return;
  }
  out += '>';
  escapeInto(out, n->text, false);
  if (n->first) {
    if (pretty) out += '\n';
    for (const sq_xml_node* c = n->first; c; c = c->next) writeNode(out, c, pretty, depth + 1);
    if (pretty) out.append(static_cast<size_t>(depth) * 2, ' ');
  }
  out += "</";
  out += n->name;
  out += '>';
  if (pretty) out += '\n';
}

}  // namespace

extern "C" {

sq_result sq_xml_parse(const char* data, size_t len, sq_xml_doc** out, char* err, size_t err_size) {
  if (!out || (!data && len)) return SQ_E_INVALIDARG;
  *out = nullptr;
  try {
    Parser p(data, len);
    sq_xml_node* root = p.parseDocument();
    if (!root) {
      sqp::copyErr(err, err_size, p.error());
      return SQ_E_PARSE;
    }
    auto* d = new sq_xml_doc();
    d->root = root;
    *out = d;
    return SQ_OK;
  } catch (const std::bad_alloc&) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result sq_xml_load_file(const char* path, sq_xml_doc** out, char* err, size_t err_size) {
  sq_buffer b{nullptr, 0};
  sq_result r = sq_fs_read_file(path, &b);
  if (r != SQ_OK) {
    sqp::copyErr(err, err_size, sq_result_string(r));
    return r;
  }
  r = sq_xml_parse(b.data, b.size, out, err, err_size);
  sq_buffer_free(&b);
  return r;
}

sq_xml_doc* sq_xml_create(const char* root_name) {
  if (!validName(root_name)) return nullptr;
  auto* d = new (std::nothrow) sq_xml_doc();
  if (!d) return nullptr;
  d->root = new (std::nothrow) sq_xml_node();
  if (!d->root) {
    delete d;
    return nullptr;
  }
  d->root->name = root_name;
  return d;
}

void sq_xml_free(sq_xml_doc* doc) { delete doc; }
sq_xml_node* sq_xml_root(sq_xml_doc* doc) { return doc ? doc->root : nullptr; }

sq_result sq_xml_write(const sq_xml_doc* doc, sq_bool pretty, sq_buffer* out) {
  if (!doc || !doc->root || !out) return SQ_E_INVALIDARG;
  std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  s += pretty ? "\n" : "";
  writeNode(s, doc->root, pretty != 0, 0);
  return sqp::toBuffer(s, out);
}

sq_result sq_xml_save_file(const sq_xml_doc* doc, const char* path, sq_bool pretty) {
  sq_buffer b{nullptr, 0};
  sq_result r = sq_xml_write(doc, pretty, &b);
  if (r != SQ_OK) return r;
  r = sq_fs_write_file(path, b.data, b.size);
  sq_buffer_free(&b);
  return r;
}

const char* sq_xml_name(const sq_xml_node* n) { return n ? n->name.c_str() : ""; }
sq_result sq_xml_set_name(sq_xml_node* n, const char* name) {
  if (!n || !validName(name)) return SQ_E_INVALIDARG;
  n->name = name;
  return SQ_OK;
}
const char* sq_xml_text(const sq_xml_node* n) { return n ? n->text.c_str() : ""; }
sq_result sq_xml_set_text(sq_xml_node* n, const char* text) {
  if (!n) return SQ_E_INVALIDARG;
  n->text = text ? text : "";
  return SQ_OK;
}

uint32_t sq_xml_attr_count(const sq_xml_node* n) { return n ? static_cast<uint32_t>(n->attrs.size()) : 0; }
const char* sq_xml_attr_name(const sq_xml_node* n, uint32_t i) { return n && i < n->attrs.size() ? n->attrs[i].first.c_str() : nullptr; }
const char* sq_xml_attr_value(const sq_xml_node* n, uint32_t i) { return n && i < n->attrs.size() ? n->attrs[i].second.c_str() : nullptr; }
const char* sq_xml_attr(const sq_xml_node* n, const char* name) {
  if (!n || !name) return nullptr;
  for (auto& a : n->attrs)
    if (a.first == name) return a.second.c_str();
  return nullptr;
}
sq_result sq_xml_set_attr(sq_xml_node* n, const char* name, const char* value) {
  if (!n || !validName(name)) return SQ_E_INVALIDARG;
  for (auto& a : n->attrs)
    if (a.first == name) {
      a.second = value ? value : "";
      return SQ_OK;
    }
  n->attrs.emplace_back(name, value ? value : "");
  return SQ_OK;
}
sq_result sq_xml_remove_attr(sq_xml_node* n, const char* name) {
  if (!n || !name) return SQ_E_INVALIDARG;
  for (auto it = n->attrs.begin(); it != n->attrs.end(); ++it)
    if (it->first == name) {
      n->attrs.erase(it);
      return SQ_OK;
    }
  return SQ_E_NOTFOUND;
}

sq_xml_node* sq_xml_parent(const sq_xml_node* n) { return n ? n->parent : nullptr; }
sq_xml_node* sq_xml_first_child(const sq_xml_node* n, const char* name) {
  if (!n) return nullptr;
  for (sq_xml_node* c = n->first; c; c = c->next)
    if (!name || c->name == name) return c;
  return nullptr;
}
sq_xml_node* sq_xml_next_sibling(const sq_xml_node* n, const char* name) {
  if (!n) return nullptr;
  for (sq_xml_node* c = n->next; c; c = c->next)
    if (!name || c->name == name) return c;
  return nullptr;
}
uint32_t sq_xml_child_count(const sq_xml_node* n) {
  uint32_t k = 0;
  if (n)
    for (sq_xml_node* c = n->first; c; c = c->next) ++k;
  return k;
}
sq_xml_node* sq_xml_append_child(sq_xml_node* parent, const char* name) {
  if (!parent || !validName(name)) return nullptr;
  auto* c = new (std::nothrow) sq_xml_node();
  if (!c) return nullptr;
  c->name = name;
  parent->append(c);
  return c;
}
sq_result sq_xml_remove_child(sq_xml_node* parent, sq_xml_node* child) {
  if (!parent || !child || child->parent != parent) return SQ_E_INVALIDARG;
  parent->unlink(child);
  delete child;
  return SQ_OK;
}

}  // extern "C"
