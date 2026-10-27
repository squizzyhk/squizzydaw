#include "sq/formats/core/gettext.hpp"

#include <vector>

#include "sq/platform/text.h"

namespace sq::formats {

namespace {
constexpr uint32_t kMoMagic = 0x950412DEu;
constexpr uint32_t kMoMagicSwapped = 0xDE120495u;

uint32_t bswap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
}  // namespace

const std::string* Catalog::find(std::string_view msgid, std::string_view context) const {
  std::string key;
  if (!context.empty()) {
    key.assign(context);
    key.push_back('\x04');
  }
  key.append(msgid);
  auto it = messages.find(key);
  if (it == messages.end()) {
    // plural entries are keyed "singular\0plural": match on the singular part
    auto lb = messages.lower_bound(key);
    if (lb != messages.end() && lb->first.size() > key.size() && lb->first.compare(0, key.size(), key) == 0 &&
        lb->first[key.size()] == '\0')
      it = lb;
  }
  if (it == messages.end() || it->second.empty()) return nullptr;
  return &it->second;
}

Status moWrite(const Catalog& c, std::string& out) {
  const uint32_t n = static_cast<uint32_t>(c.messages.size());
  const uint32_t origTab = 28, transTab = origTab + 8 * n, strings = transTab + 8 * n;
  std::string ids, strs;
  std::vector<std::pair<uint32_t, uint32_t>> idPos, strPos;
  for (const auto& [k, v] : c.messages) {
    idPos.emplace_back(static_cast<uint32_t>(k.size()), static_cast<uint32_t>(ids.size()));
    ids += k;
    ids.push_back('\0');
  }
  for (const auto& [k, v] : c.messages) {
    strPos.emplace_back(static_cast<uint32_t>(v.size()), static_cast<uint32_t>(strs.size()));
    strs += v;
    strs.push_back('\0');
  }
  if (static_cast<uint64_t>(strings) + ids.size() + strs.size() > 0xFFFFFFF0ull)
    return Status::error(SQ_E_INVALIDARG, "mo: catalog too large");
  out.clear();
  putU32(out, kMoMagic);
  putU32(out, 0);  // revision
  putU32(out, n);
  putU32(out, origTab);
  putU32(out, transTab);
  putU32(out, 0);        // hash table size
  putU32(out, strings);  // hash table offset (unused)
  for (auto [len, off] : idPos) {
    putU32(out, len);
    putU32(out, strings + off);
  }
  const uint32_t strBase = strings + static_cast<uint32_t>(ids.size());
  for (auto [len, off] : strPos) {
    putU32(out, len);
    putU32(out, strBase + off);
  }
  out += ids;
  out += strs;
  return {};
}

Status moRead(std::string_view b, Catalog& out) {
  out.messages.clear();
  if (b.size() < 28) return Status::error(SQ_E_PARSE, "mo: file too short");
  auto p = reinterpret_cast<const unsigned char*>(b.data());
  bool swap = false;
  uint32_t magic = getU32(p);
  if (magic == kMoMagicSwapped) swap = true;
  else if (magic != kMoMagic) return Status::error(SQ_E_PARSE, "mo: bad magic");
  auto rd = [&](size_t off) { uint32_t v = getU32(p + off); return swap ? bswap(v) : v; };
  uint32_t rev = rd(4);
  if ((rev >> 16) > 1) return Status::error(SQ_E_UNSUPPORTED, "mo: unsupported revision");
  uint32_t n = rd(8), o = rd(12), t = rd(16);
  if (n > b.size() / 16 || static_cast<uint64_t>(o) + 8ull * n > b.size() || static_cast<uint64_t>(t) + 8ull * n > b.size())
    return Status::error(SQ_E_PARSE, "mo: tables out of range");
  auto str = [&](uint32_t tab, uint32_t i, std::string& s) -> bool {
    uint32_t len = rd(tab + 8 * i), off = rd(tab + 8 * i + 4);
    if (static_cast<uint64_t>(off) + len + 1 > b.size() || p[off + len] != 0) return false;
    s.assign(b.substr(off, len));
    return true;
  };
  for (uint32_t i = 0; i < n; ++i) {
    std::string k, v;
    if (!str(o, i, k) || !str(t, i, v)) return Status::error(SQ_E_PARSE, "mo: string out of range");
    out.messages[std::move(k)] = std::move(v);
  }
  return {};
}

// ---- .po ------------------------------------------------------------------------------

namespace {
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool unquote(std::string_view s, std::string& out) {
  s = trim(s);
  if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
  s = s.substr(1, s.size() - 2);
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (++i >= s.size()) return false;
    switch (s[i]) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case 'a': out.push_back('\a'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'v': out.push_back('\v'); break;
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      default: return false;
    }
  }
  return true;
}

std::string quote(std::string_view s) {
  std::string o = "\"";
  for (char c : s) {
    switch (c) {
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      default: o.push_back(c);
    }
  }
  o.push_back('"');
  return o;
}

struct PoEntry {
  bool fuzzy = false, hasId = false, hasCtx = false, hasPlural = false;
  std::string ctx, id, plural;
  std::vector<std::string> strs;
};
}  // namespace

Status poParse(std::string_view text, Catalog& out) {
  out.messages.clear();
  PoEntry e;
  bool sawStr = false, pendingFuzzy = false;
  std::string* field = nullptr;
  int lineNo = 0;
  auto flush = [&]() {
    if (e.hasId && !(e.fuzzy && !e.id.empty())) {
      bool any = false;
      for (auto& s : e.strs) any = any || !s.empty();
      if (any) {
        std::string key = e.hasCtx ? e.ctx + '\x04' + e.id : e.id;
        std::string val;
        if (e.hasPlural) {
          key.push_back('\0');
          key += e.plural;
          for (size_t i = 0; i < e.strs.size(); ++i) {
            if (i) val.push_back('\0');
            val += e.strs[i];
          }
        } else {
          val = e.strs.empty() ? std::string() : e.strs[0];
        }
        out.messages[key] = val;
      }
    }
    e = PoEntry{};
    sawStr = false;
    field = nullptr;
  };
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view line = trim(text.substr(pos, nl - pos));
    pos = nl + 1;
    ++lineNo;
    auto fail = [&](const char* m) { return Status::error(SQ_E_PARSE, "po line " + std::to_string(lineNo) + ": " + m); };
    if (line.empty()) {
      if (nl == text.size()) break;
      continue;
    }
    if (line[0] == '#') {
      if (line.size() > 1 && line[1] == ',' && line.find("fuzzy") != std::string_view::npos) {
        if (sawStr) flush();
        pendingFuzzy = true;
      }
      if (nl == text.size()) break;
      continue;
    }
    if (line[0] == '"') {
      if (!field) return fail("continuation without keyword");
      if (!unquote(line, *field)) return fail("bad string");
      continue;
    }
    size_t sp = line.find_first_of(" \t");
    if (sp == std::string_view::npos) return fail("keyword without string");
    std::string_view kw = line.substr(0, sp), rest = line.substr(sp + 1);
    if (kw == "msgctxt" || (kw == "msgid" && (sawStr || e.hasId))) {
      if (sawStr || e.hasId) flush();
    }
    if (pendingFuzzy) {
      e.fuzzy = true;
      pendingFuzzy = false;
    }
    if (kw == "msgctxt") {
      e.hasCtx = true;
      field = &e.ctx;
    } else if (kw == "msgid") {
      e.hasId = true;
      field = &e.id;
    } else if (kw == "msgid_plural") {
      e.hasPlural = true;
      field = &e.plural;
    } else if (kw == "msgstr") {
      sawStr = true;
      e.strs.assign(1, std::string());
      field = &e.strs[0];
    } else if (kw.size() > 8 && kw.substr(0, 7) == "msgstr[" && kw.back() == ']') {
      int64_t idx = 0;
      if (!parseInt(kw.substr(7, kw.size() - 8), idx) || idx < 0 || idx > 64) return fail("bad plural index");
      sawStr = true;
      if (e.strs.size() <= static_cast<size_t>(idx)) e.strs.resize(static_cast<size_t>(idx) + 1);
      field = &e.strs[static_cast<size_t>(idx)];
    } else {
      return fail("unknown keyword");
    }
    if (!unquote(rest, *field)) return fail("bad string");
    if (nl == text.size()) break;
  }
  flush();
  return {};
}

std::string poWrite(const Catalog& c) {
  std::string o;
  for (const auto& [k, v] : c.messages) {
    std::string_view key = k;
    std::string ctx;
    bool hasCtx = false;
    size_t eot = key.find('\x04');
    if (eot != std::string_view::npos) {
      hasCtx = true;
      ctx.assign(key.substr(0, eot));
      key.remove_prefix(eot + 1);
    }
    size_t nul = key.find('\0');
    if (hasCtx) o += "msgctxt " + quote(ctx) + "\n";
    if (nul == std::string_view::npos) {
      o += "msgid " + quote(key) + "\nmsgstr " + quote(v) + "\n\n";
      continue;
    }
    o += "msgid " + quote(key.substr(0, nul)) + "\nmsgid_plural " + quote(key.substr(nul + 1)) + "\n";
    size_t start = 0;
    for (int i = 0;; ++i) {
      size_t z = v.find('\0', start);
      o += "msgstr[" + std::to_string(i) + "] " + quote(std::string_view(v).substr(start, z == std::string::npos ? std::string::npos : z - start)) + "\n";
      if (z == std::string::npos) break;
      start = z + 1;
    }
    o += "\n";
  }
  return o;
}

// ---- .properties ---------------------------------------------------------------------------

Status propertiesParse(std::string_view text, KeyValues& out) {
  out.clear();
  // Join logical lines first (a line ending in an odd number of '\' continues).
  std::vector<std::string> logical;
  {
    std::string cur;
    bool cont = false;
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string_view::npos) nl = text.size();
      std::string_view line = text.substr(pos, nl - pos);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      pos = nl + 1;
      if (cont) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\f')) line.remove_prefix(1);
      } else {
        std::string_view t = line;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\f')) t.remove_prefix(1);
        if (t.empty() || t.front() == '#' || t.front() == '!') {
          if (nl == text.size()) break;
          continue;
        }
        line = t;
      }
      size_t bs = 0;
      while (bs < line.size() && line[line.size() - 1 - bs] == '\\') ++bs;
      if (bs % 2 == 1) {
        cur.append(line.substr(0, line.size() - 1));
        cont = true;
      } else {
        cur.append(line);
        logical.push_back(std::move(cur));
        cur.clear();
        cont = false;
      }
      if (nl == text.size()) break;
    }
    if (cont) logical.push_back(std::move(cur));
  }
  auto unescape = [](std::string_view s, std::string& o) -> bool {
    for (size_t i = 0; i < s.size(); ++i) {
      char c = s[i];
      if (c != '\\') {
        o.push_back(c);
        continue;
      }
      if (++i >= s.size()) return true;
      char e = s[i];
      switch (e) {
        case 't': o.push_back('\t'); break;
        case 'n': o.push_back('\n'); break;
        case 'r': o.push_back('\r'); break;
        case 'f': o.push_back('\f'); break;
        case 'u': {
          auto hex4 = [&](size_t at, uint32_t& v) {
            if (at + 4 > s.size()) return false;
            v = 0;
            for (size_t k = at; k < at + 4; ++k) {
              char h = s[k];
              int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
              if (d < 0) return false;
              v = (v << 4) | static_cast<uint32_t>(d);
            }
            return true;
          };
          uint32_t cp = 0;
          if (!hex4(i + 1, cp)) return false;
          i += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
            uint32_t lo = 0;
            if (hex4(i + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              i += 6;
            }
          }
          char buf[4];
          size_t n = sq_utf8_encode(cp, buf);
          if (n == 0) {
            buf[0] = '\xEF', buf[1] = '\xBF', buf[2] = '\xBD';
            n = 3;
          }
          o.append(buf, n);
          break;
        }
        default: o.push_back(e);  // \\ \= \: \# \! and unknown escapes map to the char
      }
    }
    return true;
  };
  for (const std::string& l : logical) {
    size_t i = 0;
    // key ends at an unescaped '=', ':' or whitespace
    while (i < l.size()) {
      char c = l[i];
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '=' || c == ':' || c == ' ' || c == '\t' || c == '\f') break;
      ++i;
    }
    if (i > l.size()) i = l.size();
    std::string_view rawKey = std::string_view(l).substr(0, i);
    size_t j = i;
    while (j < l.size() && (l[j] == ' ' || l[j] == '\t' || l[j] == '\f')) ++j;
    if (j < l.size() && (l[j] == '=' || l[j] == ':')) ++j;
    while (j < l.size() && (l[j] == ' ' || l[j] == '\t' || l[j] == '\f')) ++j;
    std::string key, value;
    if (!unescape(rawKey, key) || !unescape(std::string_view(l).substr(j), value))
      return Status::error(SQ_E_PARSE, "properties: bad \\u escape");
    if (!sq_utf8_validate(key.data(), key.size()) || !sq_utf8_validate(value.data(), value.size()))
      return Status::error(SQ_E_PARSE, "properties: invalid UTF-8");
    setValue(out, key, std::move(value));
  }
  return {};
}

std::string propertiesWrite(const KeyValues& kv) {
  auto esc = [](std::string_view s, bool isKey) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
      char c = s[i];
      switch (c) {
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\f': o += "\\f"; break;
        case '=': case ':': o += isKey ? std::string("\\") + c : std::string(1, c); break;
        case '#': case '!': o += (i == 0) ? std::string("\\") + c : std::string(1, c); break;
        case ' ': o += (isKey || i == 0) ? "\\ " : " "; break;
        default: o.push_back(c);
      }
    }
    return o;
  };
  std::string o;
  for (const auto& [k, v] : kv) o += esc(k, true) + "=" + esc(v, false) + "\n";
  return o;
}

}  // namespace sq::formats
