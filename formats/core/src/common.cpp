#include "sq/formats/core/common.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "sq/platform.hpp"

namespace sq::formats {

const std::string* findValue(const KeyValues& kv, std::string_view key) {
  for (const auto& p : kv)
    if (p.first == key) return &p.second;
  return nullptr;
}

void setValue(KeyValues& kv, std::string_view key, std::string value) {
  for (auto& p : kv) {
    if (p.first == key) {
      p.second = std::move(value);
      return;
    }
  }
  kv.emplace_back(std::string(key), std::move(value));
}

bool isSafeRelativePath(std::string_view p) {
  if (p.empty() || p.size() > 1024) return false;
  if (p.front() == '/') return false;
  if (p.size() >= 2 && p[1] == ':') return false;
  size_t start = 0;
  while (start <= p.size()) {
    size_t end = p.find('/', start);
    if (end == std::string_view::npos) end = p.size();
    std::string_view seg = p.substr(start, end - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    for (char c : seg) {
      auto u = static_cast<unsigned char>(c);
      if (u < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        return false;
    }
    if (end == p.size()) break;
    start = end + 1;
  }
  return true;
}

bool isSafeIdentifier(std::string_view s) {
  if (s.empty() || s.size() > 128 || s == "." || s == "..") return false;
  for (char c : s) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string fileBaseName(std::string_view path) {
  size_t s = path.find_last_of("/\\");
  return std::string(s == std::string_view::npos ? path : path.substr(s + 1));
}

std::string fileExtension(std::string_view path) {
  std::string base = fileBaseName(path);
  size_t d = base.rfind('.');
  if (d == std::string::npos || d == 0) return {};
  std::string e = base.substr(d + 1);
  for (char& c : e)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return e;
}

std::string fileStem(std::string_view path) {
  std::string base = fileBaseName(path);
  size_t d = base.rfind('.');
  return (d == std::string::npos || d == 0) ? base : base.substr(0, d);
}

Status readFileBytes(const std::string& path, std::string& out) {
  Buffer b;
  sq_result r = sq_fs_read_file(path.c_str(), b.out());
  if (r != SQ_OK) return Status::error(r == SQ_E_NOTFOUND ? SQ_E_NOTFOUND : SQ_E_IO, "cannot read '" + path + "'");
  out = b.str();
  return {};
}

Status writeFileBytes(const std::string& path, std::string_view data) {
  sq_result r = sq_fs_write_file(path.c_str(), data.data(), data.size());
  if (r != SQ_OK) return Status::error(SQ_E_IO, "cannot write '" + path + "'");
  return {};
}

namespace {
struct ListCtx {
  std::vector<std::pair<std::string, bool>> items;
};
sq_bool listCb(void* user, const char* name, sq_bool isDir) {
  static_cast<ListCtx*>(user)->items.emplace_back(name, isDir != 0);
  return SQ_TRUE;
}
Status listRec(const std::string& root, const std::string& rel, std::vector<std::string>& out, int depth) {
  if (depth > 64) return Status::error(SQ_E_FAIL, "directory nesting too deep under '" + root + "'");
  ListCtx ctx;
  std::string dir = rel.empty() ? root : joinPath(root, rel);
  if (sq_fs_list_dir(dir.c_str(), &listCb, &ctx) != SQ_OK) return Status::error(SQ_E_IO, "cannot list '" + dir + "'");
  for (auto& [name, isDir] : ctx.items) {
    std::string r = rel.empty() ? name : rel + "/" + name;
    if (isDir) {
      SQF_TRY(listRec(root, r, out, depth + 1));
    } else {
      out.push_back(r);
    }
  }
  return {};
}
}  // namespace

Status listFilesRecursive(const std::string& dir, std::vector<std::string>& out) {
  out.clear();
  if (!sq_fs_is_dir(dir.c_str())) return Status::error(SQ_E_NOTFOUND, "not a directory: '" + dir + "'");
  SQF_TRY(listRec(dir, "", out, 0));
  std::sort(out.begin(), out.end());
  return {};
}

Status copyTree(const std::string& src, const std::string& dst) {
  std::vector<std::string> files;
  SQF_TRY(listFilesRecursive(src, files));
  if (sq_fs_create_dirs(dst.c_str()) != SQ_OK) return Status::error(SQ_E_IO, "cannot create '" + dst + "'");
  for (const auto& f : files) {
    std::string to = joinPath(dst, f);
    size_t slash = to.find_last_of('/');
    if (slash != std::string::npos) sq_fs_create_dirs(to.substr(0, slash).c_str());
    if (sq_fs_copy_file(joinPath(src, f).c_str(), to.c_str(), SQ_TRUE) != SQ_OK)
      return Status::error(SQ_E_IO, "cannot copy '" + f + "'");
  }
  return {};
}

void putU16(std::string& out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void putU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint16_t getU16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t getU32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool parseInt(std::string_view s, int64_t& out) {
  if (s.empty()) return false;
  const char* b = s.data();
  const char* e = b + s.size();
  if (*b == '+') ++b;
  auto r = std::from_chars(b, e, out);
  return r.ec == std::errc() && r.ptr == e;
}

bool parseAbi(std::string_view s, uint32_t& out) {
  size_t dot = s.find('.');
  if (dot == std::string_view::npos) return false;
  int64_t maj = 0, min = 0;
  if (!parseInt(s.substr(0, dot), maj) || maj < 0 || maj > 0xFFFF) return false;
  std::string_view m = s.substr(dot + 1);
  if (m == "*") {
    min = 0xFFFF;
  } else if (!parseInt(m, min) || min < 0 || min > 0xFFFF) {
    return false;
  }
  out = SQ_ABI_MAKE(maj, min);
  return true;
}

std::string formatAbi(uint32_t abi) {
  return std::to_string(SQ_ABI_GET_MAJOR(abi)) + "." + std::to_string(SQ_ABI_GET_MINOR(abi));
}

std::string formatDouble(double v) {
  if (!std::isfinite(v)) return "0";
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

bool parseDouble(std::string_view s, double& out) {
  if (s.empty()) return false;
  const char* b = s.data();
  const char* e = b + s.size();
  if (*b == '+') ++b;
  auto r = std::from_chars(b, e, out);
  return r.ec == std::errc() && r.ptr == e && std::isfinite(out);
}

bool parseColor(std::string_view s, uint32_t& rgba) {
  if (s.size() != 7 && s.size() != 9) return false;
  if (s[0] != '#') return false;
  uint32_t v = 0;
  for (size_t i = 1; i < s.size(); ++i) {
    char c = s[i];
    int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (h < 0) return false;
    v = (v << 4) | static_cast<uint32_t>(h);
  }
  rgba = s.size() == 7 ? ((v << 8) | 0xFFu) : v;
  return true;
}

std::string formatColor(uint32_t rgba, bool withAlpha) {
  char buf[16];
  if (withAlpha)
    std::snprintf(buf, sizeof buf, "#%08X", rgba);
  else
    std::snprintf(buf, sizeof buf, "#%06X", rgba >> 8);
  return buf;
}

}  // namespace sq::formats
