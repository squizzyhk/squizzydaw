// Theme packs, theme:// resolution, colors/metrics and @Nx variant selection. See theme.hpp.
#include "sq/ui/theme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <functional>
#include <new>

#include "sq/cpp/attributes.hpp"
#include "sq/cpp/bytestream.hpp"
#include "sq/platform.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace sq::ui {

namespace {

constexpr uint32_t kPackVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kEntrySize = 32;
constexpr uint32_t kMaxPackEntries = 1u << 20;
constexpr uint64_t kMaxDirectoryBytes = 512ull * 1024 * 1024;  // addDirectory budget
constexpr int kMaxChain = 16;

void putLe32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putLe64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }
uint64_t le64(const uint8_t* p) { return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32); }
size_t align8(size_t v) { return (v + 7) & ~size_t{7}; }

bool err(std::string* e, std::string msg) {
  if (e) *e = std::move(msg);
  return false;
}

bool endsWith(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != std::tolower(static_cast<unsigned char>(suffix[i])))
      return false;
  return true;
}

std::string formatNumber(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

std::string hexColor(uint32_t rgba) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "#%08X", rgba);
  return buf;
}

// Copies `s` into a caller buffer (contract 4.3).
sq_result copyOut(const std::string& s, char* buf, uint32_t bufSize, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(s.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, s.c_str(), need);
  return SQ_OK;
}

}  // namespace

// ---- paths / variants -------------------------------------------------------------------------------

int themeResourceKind(std::string_view path) {
  std::string_view file = path.substr(path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1);
  if (file == "theme.xml" || endsWith(file, ".colors.xml")) return SQ_THEME_RES_COLORS;
  if (endsWith(file, ".skin.xml")) return SQ_THEME_RES_SKIN;
  for (const char* ext : {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".webp"})
    if (endsWith(file, ext)) return SQ_THEME_RES_IMAGE;
  for (const char* ext : {".ttf", ".otf", ".woff", ".woff2"})
    if (endsWith(file, ext)) return SQ_THEME_RES_FONT;
  return SQ_THEME_RES_OTHER;
}

void splitDensity(std::string_view path, std::string& base, float& density) {
  base.assign(path);
  density = 1.0f;
  size_t slash = path.rfind('/');
  size_t start = slash == std::string_view::npos ? 0 : slash + 1;
  size_t dot = path.rfind('.');
  if (dot == std::string_view::npos || dot < start) dot = path.size();
  std::string_view stem = path.substr(start, dot - start);
  size_t at = stem.rfind('@');
  if (at == std::string_view::npos || at == 0 || stem.size() < at + 3 || stem.back() != 'x') return;
  std::string num(stem.substr(at + 1, stem.size() - at - 2));
  if (num.empty() || num.find_first_not_of("0123456789.") != std::string::npos) return;
  char* end = nullptr;
  double d = std::strtod(num.c_str(), &end);
  if (!end || *end != '\0' || !(d > 0) || d > 16) return;
  density = static_cast<float>(d);
  base.assign(path.substr(0, start + at));
  base.append(path.substr(dot));
}

size_t chooseDensity(const std::vector<float>& densities, float scale) {
  if (!(scale > 0) || !std::isfinite(scale)) scale = 1.0f;
  size_t best = densities.size(), largest = 0;
  for (size_t i = 0; i < densities.size(); ++i) {
    if (densities[i] > densities[largest]) largest = i;
    if (densities[i] + 1e-4f >= scale && (best == densities.size() || densities[i] < densities[best])) best = i;
  }
  return best == densities.size() ? largest : best;
}

bool validThemePath(std::string_view p) {
  if (p.empty() || p.front() == '/' || p.back() == '/' || p.find('\\') != std::string_view::npos || p.find(':') != std::string_view::npos ||
      p.find('\0') != std::string_view::npos || p.find('?') != std::string_view::npos)
    return false;
  size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string_view::npos) j = p.size();
    std::string_view seg = p.substr(i, j - i);
    if (seg.empty() || seg == "." || seg == "..") return false;
    i = j + 1;
  }
  return true;
}

bool validThemeName(std::string_view name) {
  return validThemePath(name) && name.find('/') == std::string_view::npos && name != "current";
}

// ---- pack writer / reader -----------------------------------------------------------------------------

bool writeThemePack(std::vector<ThemePackEntry> entries, std::string& out, std::string* error) {
  try {
    std::sort(entries.begin(), entries.end(), [](const ThemePackEntry& a, const ThemePackEntry& b) { return a.path < b.path; });
    if (entries.size() > kMaxPackEntries) return err(error, "too many entries");
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& p = entries[i].path;
      if (!validThemePath(p) || p.find('/') == std::string::npos) return err(error, "invalid pack path '" + p + "'");
      if (i && entries[i - 1].path == p) return err(error, "duplicate pack path '" + p + "'");
      if (entries[i].data.size() > 0xFFFFFFFFull || p.size() > 0xFFFF) return err(error, "entry too large: " + p);
    }
    const size_t n = entries.size();
    size_t strStart = kHeaderSize + kEntrySize * n, strSize = 0;
    for (const auto& e : entries) strSize += e.path.size() + 1;
    const size_t dataStart = align8(strStart + strSize);
    std::vector<uint64_t> dataOff(n);
    size_t cursor = 0;
    for (size_t i = 0; i < n; ++i) {
      dataOff[i] = cursor;
      cursor = align8(cursor + entries[i].data.size());
    }
    const size_t total = dataStart + cursor;
    if (total > 0xFFFFFFFFull) return err(error, "pack exceeds 4 GB");
    out.clear();
    out.reserve(total);
    out.append(kThemePackMagic, 8);
    putLe32(out, kPackVersion);
    putLe32(out, static_cast<uint32_t>(n));
    putLe64(out, dataStart);
    putLe64(out, total);
    size_t pathOff = strStart;
    for (size_t i = 0; i < n; ++i) {
      const auto& e = entries[i];
      putLe64(out, dataOff[i]);
      putLe64(out, e.data.size());
      putLe32(out, static_cast<uint32_t>(pathOff));
      putLe32(out, static_cast<uint32_t>(e.path.size()));
      putLe32(out, sq_crc32(0, e.data.data(), e.data.size()));
      putLe32(out, static_cast<uint32_t>(themeResourceKind(std::string_view(e.path).substr(e.path.find('/') + 1))));
      pathOff += e.path.size() + 1;
    }
    for (const auto& e : entries) {
      out += e.path;
      out.push_back('\0');
    }
    out.resize(dataStart, '\0');
    for (size_t i = 0; i < n; ++i) {
      out += entries[i].data;
      out.resize(dataStart + align8(dataOff[i] + entries[i].data.size()), '\0');
    }
    return out.size() == total || err(error, "internal pack size mismatch");
  } catch (const std::bad_alloc&) {
    return err(error, "out of memory writing the pack");
  }
}

std::shared_ptr<const ThemePack> ThemePack::fromMemory(const void* data, size_t size, bool copy, std::string* error) {
  try {
    auto pack = std::shared_ptr<ThemePack>(new ThemePack());
    if (!data || size < kHeaderSize) {
      err(error, "not a theme pack (too small)");
      return nullptr;
    }
    if (copy) {
      pack->owned_.assign(static_cast<const char*>(data), size);
      pack->blob_ = pack->owned_;
    } else {
      pack->blob_ = std::string_view(static_cast<const char*>(data), size);
    }
    const auto* p = reinterpret_cast<const uint8_t*>(pack->blob_.data());
    if (std::memcmp(p, kThemePackMagic, 8) != 0) {
      err(error, "not a theme pack (bad magic)");
      return nullptr;
    }
    if (le32(p + 8) != kPackVersion) {
      err(error, "unsupported theme pack version " + std::to_string(le32(p + 8)));
      return nullptr;
    }
    const uint32_t n = le32(p + 12);
    const uint64_t dataStart = le64(p + 16), total = le64(p + 24);
    if (total != size || n > kMaxPackEntries || kHeaderSize + uint64_t{kEntrySize} * n > dataStart || dataStart > size) {
      err(error, "corrupt theme pack header");
      return nullptr;
    }
    const uint64_t strStart = kHeaderSize + uint64_t{kEntrySize} * n;
    pack->entries_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* e = p + kHeaderSize + size_t{kEntrySize} * i;
      uint64_t off = le64(e), len = le64(e + 8);
      uint32_t pathOff = le32(e + 16), pathLen = le32(e + 20), crc = le32(e + 24), kind = le32(e + 28);
      if (pathOff < strStart || uint64_t{pathOff} + pathLen + 1 > dataStart || p[pathOff + pathLen] != 0) {
        err(error, "corrupt theme pack path table");
        return nullptr;
      }
      if (off > size - dataStart || len > size - dataStart - off) {
        err(error, "corrupt theme pack entry bounds");
        return nullptr;
      }
      std::string_view path(reinterpret_cast<const char*>(p + pathOff), pathLen);
      std::string_view bytes(reinterpret_cast<const char*>(p + dataStart + off), static_cast<size_t>(len));
      if (!validThemePath(path) || (i && !(pack->entries_.back().path < path))) {
        err(error, "theme pack paths invalid or not sorted");
        return nullptr;
      }
      if (sq_crc32(0, bytes.data(), bytes.size()) != crc) {
        err(error, "theme pack CRC mismatch for '" + std::string(path) + "'");
        return nullptr;
      }
      pack->entries_.push_back(Entry{path, bytes, static_cast<int>(kind)});
    }
    return pack;
  } catch (const std::bad_alloc&) {
    err(error, "out of memory reading the pack");
    return nullptr;
  }
}

std::shared_ptr<const ThemePack> ThemePack::fromFile(const std::string& path, std::string* error) {
  auto bytes = readFile(path);
  if (!bytes) {
    err(error, "cannot read '" + path + "'");
    return nullptr;
  }
  return fromMemory(bytes->data(), bytes->size(), true, error);
}

std::shared_ptr<const ThemePack> ThemePack::fromResource(const char* name, void* module, std::string* error) {
#ifdef _WIN32
  if (!name) name = kThemePackResourceName;
  std::wstring wname;
  for (const char* c = name; *c; ++c) wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
  HMODULE mod = module ? static_cast<HMODULE>(module) : GetModuleHandleW(nullptr);
  HRSRC res = FindResourceW(mod, wname.c_str(), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
  if (!res) {
    err(error, std::string("no RCDATA resource '") + name + "' in the module");
    return nullptr;
  }
  HGLOBAL h = LoadResource(mod, res);
  const void* data = h ? LockResource(h) : nullptr;
  DWORD size = SizeofResource(mod, res);
  if (!data || size == 0) {
    err(error, "cannot load the theme resource");
    return nullptr;
  }
  // Resource memory lives as long as the module; module=nullptr is the EXE, so reference it.
  return fromMemory(data, size, module != nullptr, error);
#else
  (void)name;
  (void)module;
  err(error, "embedded theme resources are only supported on Windows");
  return nullptr;
#endif
}

std::optional<size_t> ThemePack::find(std::string_view path) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), path, [](const Entry& e, std::string_view p) { return e.path < p; });
  if (it == entries_.end() || it->path != path) return std::nullopt;
  return static_cast<size_t>(it - entries_.begin());
}

// ---- resolver ---------------------------------------------------------------------------------------------

struct ThemeResolver::Theme {
  std::string name, parent, title;
  std::map<std::string, std::string, std::less<>> colors;  // raw: "#..." or "@Alias"
  std::map<std::string, double, std::less<>> metrics;
  struct File {
    std::shared_ptr<const void> keep;
    std::string_view bytes;
  };
  std::map<std::string, File, std::less<>> files;
  // base path -> variants (density, file path), ascending density
  std::map<std::string, std::vector<std::pair<float, std::string>>, std::less<>> variants;

  void rebuildVariants() {
    variants.clear();
    for (const auto& [path, f] : files) {
      std::string base;
      float d = 1;
      splitDensity(path, base, d);
      variants[base].emplace_back(d, path);
    }
    for (auto& [b, v] : variants) std::sort(v.begin(), v.end());
  }
};

struct ThemeResolver::Hit {
  const Theme* theme = nullptr;  // only valid under mu_
  std::string themeName;
  std::string path;
  float density = 1;
  std::string_view bytes;
  std::shared_ptr<const void> keep;
};

ThemeResolver::ThemeResolver() = default;
ThemeResolver::~ThemeResolver() = default;

sq::Ptr<ThemeResolver> ThemeResolver::create() { return sq::Ptr<ThemeResolver>::adopt(new ThemeResolver()); }

bool ThemeResolver::mergeTheme(const std::string& name,
                               std::map<std::string, std::pair<std::shared_ptr<const void>, std::string_view>>& files, std::string* error) {
  if (!validThemeName(name)) return err(error, "invalid theme name '" + name + "' (one path segment, not \"current\")");
  auto xmlIt = files.find("theme.xml");
  if (xmlIt == files.end()) return err(error, "theme '" + name + "' has no theme.xml");
  std::string perr;
  auto doc = xmlParse(xmlIt->second.second, &perr);
  if (!doc) return err(error, "theme '" + name + "': theme.xml: " + perr);
  XmlNode root = xmlRoot(doc);
  if (root.name() != "Theme") return err(error, "theme '" + name + "': theme.xml root must be <Theme>");
  if (root.attr("name") != name)
    return err(error, "theme '" + name + "': theme.xml says name=\"" + root.attr("name") + "\" (must match its folder)");
  std::string fmt = root.attr("format", "1");
  if (fmt != "1") return err(error, "theme '" + name + "': unsupported format " + fmt);
  std::string parent = root.attr("inherits");
  if (parent == name) return err(error, "theme '" + name + "' inherits itself");
  std::map<std::string, std::string, std::less<>> colors;
  std::map<std::string, double, std::less<>> metrics;
  for (XmlNode cs : root.children("Colors"))
    for (XmlNode c : cs.children("Color")) {
      std::string cn = c.attr("name"), v = c.attr("value");
      Color tmp;
      if (cn.empty()) return err(error, "theme '" + name + "': <Color> without name");
      if (!(v.size() > 1 && v[0] == '@') && !Skin::parseColor(v, tmp))
        return err(error, "theme '" + name + "': bad color value '" + v + "' for " + cn);
      colors[cn] = v;
    }
  for (XmlNode ms : root.children("Metrics"))
    for (XmlNode m : ms.children("Metric")) {
      std::string mn = m.attr("name"), v = m.attr("value");
      char* end = nullptr;
      double d = std::strtod(v.c_str(), &end);
      if (mn.empty() || v.empty() || !end || *end != '\0' || !std::isfinite(d))
        return err(error, "theme '" + name + "': bad metric '" + mn + "' = '" + v + "'");
      metrics[mn] = d;
    }
  std::unique_lock lock(mu_);
  auto& slot = themes_[name];
  if (!slot) slot = std::make_unique<Theme>();
  Theme& t = *slot;
  t.name = name;
  if (root.hasAttr("inherits") || t.parent.empty()) t.parent = parent;
  if (root.hasAttr("title")) t.title = root.attr("title");
  for (auto& [k, v] : colors) t.colors[k] = v;
  for (auto& [k, v] : metrics) t.metrics[k] = v;
  for (auto& [path, f] : files) t.files[path] = Theme::File{f.first, f.second};
  t.rebuildVariants();
  if (active_.empty()) active_ = name;
  // Invalidate decoded images while still holding mu_: an image() that found its bytes before
  // this merge sees the generation change and does not cache the stale decode.
  std::lock_guard cl(cacheMu_);
  cache_.clear();
  ++cacheGen_;
  return true;
}

bool ThemeResolver::addPack(const std::shared_ptr<const ThemePack>& pack, std::string* error) {
  if (!pack) return err(error, "null pack");
  try {
    std::map<std::string, std::map<std::string, std::pair<std::shared_ptr<const void>, std::string_view>>> byTheme;
    for (size_t i = 0; i < pack->count(); ++i) {
      std::string_view p = pack->path(i);
      size_t slash = p.find('/');
      if (slash == std::string_view::npos) return err(error, "pack entry '" + std::string(p) + "' has no theme prefix");
      byTheme[std::string(p.substr(0, slash))][std::string(p.substr(slash + 1))] = {pack, pack->data(i)};
    }
    for (auto& [name, files] : byTheme)
      if (!mergeTheme(name, files, error)) return false;
    return true;
  } catch (const std::bad_alloc&) {
    return err(error, "out of memory adding the pack");
  }
}

namespace {
struct DirWalk {
  uint64_t total = 0;
  std::string error;
  // rel path -> bytes
  std::map<std::string, std::shared_ptr<const std::string>> files;

  bool walk(const std::string& abs, const std::string& rel, int depth) {
    if (depth > 32) {
      error = "directory nesting too deep at " + abs;
      return false;
    }
    std::vector<std::pair<std::string, bool>> list;
    auto cb = [](void* user, const char* name, sq_bool isDir) -> sq_bool {
      static_cast<std::vector<std::pair<std::string, bool>>*>(user)->emplace_back(name, isDir != 0);
      return SQ_TRUE;
    };
    if (sq_fs_list_dir(abs.c_str(), cb, &list) != SQ_OK) {
      error = "cannot list " + abs;
      return false;
    }
    std::sort(list.begin(), list.end());
    for (const auto& [name, isDir] : list) {
      if (name.empty() || name[0] == '.') continue;
      std::string a = joinPath(abs, name), r = rel.empty() ? name : rel + "/" + name;
      if (isDir) {
        if (!walk(a, r, depth + 1)) return false;
        continue;
      }
      auto bytes = readFile(a);
      if (!bytes) {
        error = "cannot read " + a;
        return false;
      }
      total += bytes->size();
      if (total > kMaxDirectoryBytes) {
        error = "theme directory exceeds the size budget";
        return false;
      }
      files[r] = std::make_shared<const std::string>(std::move(*bytes));
    }
    return true;
  }
};
}  // namespace

bool ThemeResolver::addDirectory(const std::string& root, std::string* error) {
  try {
    if (!sq_fs_is_dir(root.c_str())) return err(error, "not a directory: " + root);
    auto addOne = [&](const std::string& dir, const std::string& name) {
      DirWalk w;
      if (!w.walk(dir, "", 0)) return err(error, w.error);
      std::map<std::string, std::pair<std::shared_ptr<const void>, std::string_view>> files;
      for (auto& [rel, bytes] : w.files) files[rel] = {bytes, std::string_view(*bytes)};
      return mergeTheme(name, files, error);
    };
    std::string single = joinPath(root, "theme.xml");
    if (sq_fs_is_file(single.c_str())) {
      auto bytes = readFile(single);
      auto doc = bytes ? xmlParse(*bytes) : nullptr;
      std::string name = doc ? xmlRoot(doc).attr("name") : std::string();
      if (name.empty()) return err(error, single + ": cannot read the theme name");
      return addOne(root, name);
    }
    std::vector<std::string> subs;
    auto cb = [](void* user, const char* name, sq_bool isDir) -> sq_bool {
      if (isDir && name[0] != '.') static_cast<std::vector<std::string>*>(user)->emplace_back(name);
      return SQ_TRUE;
    };
    sq_fs_list_dir(root.c_str(), cb, &subs);
    std::sort(subs.begin(), subs.end());
    bool any = false;
    for (const auto& s : subs) {
      std::string dir = joinPath(root, s);
      if (!sq_fs_is_file(joinPath(dir, "theme.xml").c_str())) continue;
      if (!addOne(dir, s)) return false;
      any = true;
    }
    return any || err(error, "no themes (no */theme.xml) under " + root);
  } catch (const std::bad_alloc&) {
    return err(error, "out of memory reading themes");
  }
}

std::vector<ThemeInfo> ThemeResolver::themes() const {
  std::shared_lock lock(mu_);
  std::vector<ThemeInfo> v;
  for (const auto& [n, t] : themes_) v.push_back(ThemeInfo{t->name, t->parent, t->title});
  return v;
}

bool ThemeResolver::hasTheme(std::string_view name) const {
  std::shared_lock lock(mu_);
  return themes_.find(name) != themes_.end();
}

std::string ThemeResolver::activeTheme() const {
  std::shared_lock lock(mu_);
  return active_;
}

std::vector<const ThemeResolver::Theme*> ThemeResolver::chainLocked(std::string_view name) const {
  std::vector<const Theme*> chain;
  std::string cur(name);
  while (!cur.empty() && static_cast<int>(chain.size()) < kMaxChain) {
    auto it = themes_.find(cur);
    if (it == themes_.end()) break;
    if (std::find(chain.begin(), chain.end(), it->second.get()) != chain.end()) break;  // cycle
    chain.push_back(it->second.get());
    cur = it->second->parent;
  }
  return chain;
}

namespace {
// "theme://<theme>/<path>[?scale=N]" -> theme, path, scale (if given).
bool parseThemeUrl(std::string_view url, std::string& theme, std::string& path, float* scale) {
  constexpr std::string_view kPrefix = "theme://";
  if (url.substr(0, kPrefix.size()) != kPrefix) return false;
  url.remove_prefix(kPrefix.size());
  size_t q = url.find('?');
  if (q != std::string_view::npos) {
    std::string_view query = url.substr(q + 1);
    url = url.substr(0, q);
    if (scale && query.substr(0, 6) == "scale=") {
      std::string v(query.substr(6));
      char* end = nullptr;
      double d = std::strtod(v.c_str(), &end);
      if (end && *end == '\0' && d > 0 && d <= 16) *scale = static_cast<float>(d);
    }
  }
  size_t slash = url.find('/');
  if (slash == std::string_view::npos || slash == 0) return false;
  theme.assign(url.substr(0, slash));
  path.assign(url.substr(slash + 1));
  return validThemePath(path);
}
}  // namespace

bool ThemeResolver::findLocked(std::string_view url, float scale, Hit& hit) const {
  std::string theme, path;
  if (!parseThemeUrl(url, theme, path, nullptr)) return false;
  if (theme == "current") theme = active_;
  for (const Theme* t : chainLocked(theme)) {
    if (auto v = t->variants.find(path); v != t->variants.end() && !v->second.empty()) {
      std::vector<float> ds;
      for (const auto& [d, p] : v->second) ds.push_back(d);
      const auto& chosen = v->second[chooseDensity(ds, scale)];
      const auto& f = t->files.at(chosen.second);
      hit = Hit{t, t->name, chosen.second, chosen.first, f.bytes, f.keep};
      return true;
    }
    if (auto f = t->files.find(path); f != t->files.end()) {  // explicit variant name
      std::string base;
      float d = 1;
      splitDensity(path, base, d);
      hit = Hit{t, t->name, path, d, f->second.bytes, f->second.keep};
      return true;
    }
  }
  return false;
}

std::optional<std::string> ThemeResolver::read(const std::string& url, float scale, sq_theme_resource* info) const {
  Hit hit;
  {
    std::shared_lock lock(mu_);
    if (!findLocked(url, scale, hit)) return std::nullopt;
  }
  if (info) {
    std::memset(info, 0, sizeof *info);
    info->struct_size = sizeof *info;
    info->kind = themeResourceKind(hit.path);
    info->scale = hit.density;
    info->size = static_cast<int64_t>(hit.bytes.size());
    if (info->kind == SQ_THEME_RES_IMAGE) imageSize(hit.bytes.data(), hit.bytes.size(), &info->width, &info->height);
  }
  return std::string(hit.bytes);
}

ImagePtr ThemeResolver::image(const std::string& url, float scale, std::string* error) const {
  Hit hit;
  std::string key;
  uint64_t gen = 0;
  {
    std::shared_lock lock(mu_);
    if (!findLocked(url, scale, hit)) {
      err(error, "no theme resource " + url);
      return nullptr;
    }
    key = hit.themeName + "/" + hit.path;
    std::lock_guard cl(cacheMu_);
    gen = cacheGen_;
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
  }
  ImagePtr img = decodeImage(hit.bytes.data(), hit.bytes.size(), hit.density, error);
  if (img) {
    std::lock_guard cl(cacheMu_);
    if (gen == cacheGen_) {
      if (cache_.size() > 4096) cache_.clear();
      cache_[key] = img;
    }
  }
  return img;
}

std::function<ImagePtr(const std::string&, float)> ThemeResolver::imageProvider() {
  sq::Ptr<ThemeResolver> self(this);
  return [self](const std::string& url, float scale) { return self->image(url, scale); };
}

SkinDefines ThemeResolver::definesLocked(std::string_view theme) const {
  SkinDefines d;
  auto chain = chainLocked(theme);
  if (chain.empty()) return d;
  d["theme.name"] = chain.front()->name;
  std::set<std::string, std::less<>> colorNames;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (const auto& [k, v] : (*it)->colors) colorNames.insert(k);
    for (const auto& [k, v] : (*it)->metrics) d["theme.metric." + k] = formatNumber(v);
  }
  for (const auto& k : colorNames) {
    uint32_t rgba = 0;
    if (colorLocked(chain.front()->name, k, rgba)) d["theme.color." + k] = hexColor(rgba);
  }
  return d;
}

SkinDefines ThemeResolver::skinDefines() const {
  std::shared_lock lock(mu_);
  return definesLocked(active_);
}

SkinDefines ThemeResolver::skinDefinesFor(std::string_view theme) const {
  std::shared_lock lock(mu_);
  return definesLocked(theme);
}

bool ThemeResolver::colorLocked(const std::string& start, std::string_view name, uint32_t& rgba) const {
  auto chain = chainLocked(start);
  std::string cur(name);
  for (int hops = 0; hops < 8; ++hops) {
    const std::string* raw = nullptr;
    for (const Theme* t : chain)
      if (auto it = t->colors.find(cur); it != t->colors.end()) {
        raw = &it->second;
        break;
      }
    if (!raw) return false;
    if (raw->size() > 1 && (*raw)[0] == '@') {
      cur = raw->substr(1);
      continue;
    }
    Color c;
    if (!Skin::parseColor(*raw, c)) return false;
    rgba = c.toRgba();
    return true;
  }
  return false;  // alias loop
}

sq_result SQ_CALL ThemeResolver::getActiveTheme(char* buf, uint32_t bufSize, uint32_t* needed) {
  try {
    return copyOut(activeTheme(), buf, bufSize, needed);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ThemeResolver::setActiveTheme(const char* name) {
  if (!name || !*name) return SQ_E_INVALIDARG;
  try {
    std::string previous;
    {
      std::unique_lock lock(mu_);
      if (themes_.find(std::string_view(name)) == themes_.end()) return SQ_E_NOTFOUND;
      if (active_ == name) return SQ_S_FALSE;
      previous = active_;
      active_ = name;
    }
    std::vector<sq::Ptr<IObserver>> obs;
    {
      std::lock_guard ol(obsMu_);
      obs = observers_;
    }
    if (!obs.empty()) {
      auto args = sq::makeAttributes();
      args->setString("theme", name);
      args->setString("previous", previous.c_str());
      for (auto& o : obs) o->onNotify(static_cast<IThemeResolver*>(this), "theme.changed", args.get());
    }
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ThemeResolver::resolve(const char* url, float scale, sq_theme_resource* out) {
  if (!url || !out) return SQ_E_INVALIDARG;
  try {
    sq_theme_resource info{};
    if (!read(url, scale, &info)) return SQ_E_NOTFOUND;
    *out = info;
    return SQ_OK;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ThemeResolver::openResource(const char* url, float scale, IByteStream** out) {
  if (!url || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  try {
    auto bytes = read(url, scale, nullptr);
    if (!bytes) return SQ_E_NOTFOUND;
    *out = sq::makeMemoryStream(bytes->data(), bytes->size(), false).detach();
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result SQ_CALL ThemeResolver::getColor(const char* name, uint32_t* rgba) {
  if (!name || !rgba) return SQ_E_INVALIDARG;
  try {
    std::shared_lock lock(mu_);
    return colorLocked(active_, name, *rgba) ? SQ_OK : SQ_E_NOTFOUND;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ThemeResolver::getMetric(const char* name, double* value) {
  if (!name || !value) return SQ_E_INVALIDARG;
  try {
    std::shared_lock lock(mu_);
    for (const Theme* t : chainLocked(active_))
      if (auto it = t->metrics.find(std::string_view(name)); it != t->metrics.end()) {
        *value = it->second;
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ThemeResolver::addObserver(IObserver* observer) {
  if (!observer) return SQ_E_INVALIDARG;
  try {
    std::lock_guard ol(obsMu_);
    for (auto& o : observers_)
      if (o.get() == observer) return SQ_S_FALSE;
    observers_.emplace_back(observer);
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result SQ_CALL ThemeResolver::removeObserver(IObserver* observer) {
  std::lock_guard ol(obsMu_);
  for (auto it = observers_.begin(); it != observers_.end(); ++it)
    if (it->get() == observer) {
      observers_.erase(it);
      return SQ_OK;
    }
  return SQ_S_FALSE;
}

// ---- theme:// URL resolver ---------------------------------------------------------------------------------

namespace {
class ThemeUrlResolver final : public sq::Object<ThemeUrlResolver, IUrlResolver> {
 public:
  explicit ThemeUrlResolver(sq::Ptr<ThemeResolver> t) : themes_(std::move(t)) {}
  sq_result SQ_CALL resolve(const char* url, const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    sq::Ptr<IByteStream> s;
    sq_result r = openStream(url, s.put());
    if (SQ_FAILED(r)) return r;
    return s->queryInterface(riid, out);
  }
  sq_result SQ_CALL openStream(const char* url, IByteStream** out) override {
    if (!url || !out) return SQ_E_INVALIDARG;
    std::string theme, path;
    float scale = 1.0f;
    if (!parseThemeUrl(url, theme, path, &scale)) return SQ_E_INVALIDARG;
    std::string plain = "theme://" + theme + "/" + path;
    return themes_->openResource(plain.c_str(), scale, out);
  }

 private:
  sq::Ptr<ThemeResolver> themes_;
};
}  // namespace

sq::Ptr<IUrlResolver> ThemeResolver::urlResolver() {
  return sq::Ptr<IUrlResolver>::adopt(new ThemeUrlResolver(sq::Ptr<ThemeResolver>(this)));
}

}  // namespace sq::ui
