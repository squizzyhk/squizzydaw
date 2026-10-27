// sq-skinc implementation. See include/sq/tools/skin_compiler/skinc.hpp.
#include "sq/tools/skin_compiler/skinc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#include "sq/platform.hpp"
#include "sq/ui/image.hpp"
#include "sq/ui/remote_pages.hpp"
#include "sq/ui/skin.hpp"
#include "sq/ui/theme.hpp"

namespace sq::tools::skinc {

using namespace sq::ui;

size_t Result::errors() const {
  return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) { return d.error; }));
}
size_t Result::warnings() const { return diagnostics.size() - errors(); }

namespace {

constexpr uint64_t kMaxInputBytes = 512ull * 1024 * 1024;

struct Ctx {
  const Options& opt;
  Result& r;
  void error(const std::string& file, const std::string& msg) { r.diagnostics.push_back(Diagnostic{true, file, msg}); }
  void warn(const std::string& file, const std::string& msg) { r.diagnostics.push_back(Diagnostic{false, file, msg}); }
};

// ---- input collection --------------------------------------------------------------------------------

bool walkDir(Ctx& c, const std::string& abs, const std::string& rel, std::map<std::string, std::string>& files, uint64_t& total, int depth) {
  if (depth > 32) {
    c.error(abs, "directory nesting deeper than 32 levels");
    return false;
  }
  std::vector<std::pair<std::string, bool>> list;
  auto cb = [](void* user, const char* name, sq_bool isDir) -> sq_bool {
    static_cast<std::vector<std::pair<std::string, bool>>*>(user)->emplace_back(name, isDir != 0);
    return SQ_TRUE;
  };
  if (sq_fs_list_dir(abs.c_str(), cb, &list) != SQ_OK) {
    c.error(abs, "cannot list directory");
    return false;
  }
  std::sort(list.begin(), list.end());
  for (const auto& [name, isDir] : list) {
    if (name.empty() || name[0] == '.') continue;  // hidden files / VCS metadata
    std::string a = joinPath(abs, name), r = rel + "/" + name;
    if (isDir) {
      if (!walkDir(c, a, r, files, total, depth + 1)) return false;
      continue;
    }
    auto bytes = readFile(a);
    if (!bytes) {
      c.error(a, "cannot read file");
      return false;
    }
    total += bytes->size();
    if (total > kMaxInputBytes) {
      c.error(a, "inputs exceed 512 MB");
      return false;
    }
    if (!validThemePath(r)) {
      c.error(a, "file name cannot be packed (use plain relative names)");
      continue;
    }
    files[r] = std::move(*bytes);
  }
  return true;
}

std::string themeNameOf(const std::string& themeXml) {
  auto doc = xmlParse(themeXml);
  return doc ? xmlRoot(doc).attr("name") : std::string();
}

// path -> bytes for every input theme; false on IO errors.
bool collect(Ctx& c, std::map<std::string, std::string>& files) {
  uint64_t total = 0;
  std::set<std::string> seen;
  // folder: the folder name when dir came from a folder of themes (ThemeResolver::addDirectory
  // then requires name= to match it, so the pack must not accept what the directory load refuses).
  auto addTheme = [&](const std::string& dir, const std::string& folder) {
    auto xml = readFile(joinPath(dir, "theme.xml"));
    std::string name = xml ? themeNameOf(*xml) : std::string();
    if (!validThemeName(name)) {
      c.error(joinPath(dir, "theme.xml"), "cannot read a valid <Theme name=...> (one path segment, not \"current\")");
      return false;
    }
    if (!folder.empty() && folder != name) {
      c.error(joinPath(dir, "theme.xml"), "theme name '" + name + "' must match its folder '" + folder + "'");
      return true;  // reported; keep checking the other themes (the run fails anyway)
    }
    if (!seen.insert(name).second) {
      c.error(dir, "theme '" + name + "' is given twice");
      return false;
    }
    return walkDir(c, dir, name, files, total, 0);
  };
  if (c.opt.inputs.empty()) {
    c.error("", "no input folders");
    return false;
  }
  for (const auto& in : c.opt.inputs) {
    if (!sq_fs_is_dir(in.c_str())) {
      c.error(in, "not a directory");
      return false;
    }
    if (sq_fs_is_file(joinPath(in, "theme.xml").c_str())) {
      if (!addTheme(in, std::string())) return false;
      continue;
    }
    std::vector<std::string> subs;
    auto cb = [](void* user, const char* name, sq_bool isDir) -> sq_bool {
      if (isDir && name[0] != '.') static_cast<std::vector<std::string>*>(user)->emplace_back(name);
      return SQ_TRUE;
    };
    sq_fs_list_dir(in.c_str(), cb, &subs);
    std::sort(subs.begin(), subs.end());
    bool any = false;
    for (const auto& s : subs) {
      std::string dir = joinPath(in, s);
      if (!sq_fs_is_file(joinPath(dir, "theme.xml").c_str())) {
        c.warn(dir, "folder has no theme.xml and is ignored");
        continue;
      }
      if (!addTheme(dir, s)) return false;
      any = true;
    }
    if (!any) {
      c.error(in, "no themes found (expected theme.xml or <name>/theme.xml)");
      return false;
    }
  }
  return true;
}

// ---- checks ----------------------------------------------------------------------------------------

struct ThemeFacts {
  std::string parent;
  std::vector<std::string> colorNames;
};

void checkThemeXml(Ctx& c, const std::string& theme, const std::string& xml, ThemeFacts& facts) {
  const std::string file = theme + "/theme.xml";
  auto doc = xmlParse(xml);
  if (!doc) return;  // the resolver load reports the parse error
  XmlNode root = xmlRoot(doc);
  facts.parent = root.attr("inherits");
  std::set<std::string> colors, metrics;
  for (XmlNode n : root.children()) {
    std::string_view el = n.name();
    if (el == "Colors") {
      for (XmlNode k : n.children()) {
        if (k.name() != "Color") c.warn(file, "<Colors> ignores <" + std::string(k.name()) + ">");
        else if (!colors.insert(k.attr("name")).second) c.warn(file, "color '" + k.attr("name") + "' is defined twice (last wins)");
        else facts.colorNames.push_back(k.attr("name"));
      }
    } else if (el == "Metrics") {
      for (XmlNode k : n.children()) {
        if (k.name() != "Metric") c.warn(file, "<Metrics> ignores <" + std::string(k.name()) + ">");
        else if (!metrics.insert(k.attr("name")).second) c.warn(file, "metric '" + k.attr("name") + "' is defined twice (last wins)");
      }
    } else {
      c.warn(file, "unknown element <" + std::string(el) + "> in theme.xml is ignored");
    }
  }
}

std::vector<std::string> chainOf(const std::map<std::string, ThemeFacts>& facts, const std::string& theme) {
  std::vector<std::string> chain;
  std::string cur = theme;
  while (!cur.empty() && facts.count(cur) && std::find(chain.begin(), chain.end(), cur) == chain.end() && chain.size() < 16) {
    chain.push_back(cur);
    cur = facts.at(cur).parent;
  }
  return chain;
}

// Leaf themes (no theme inherits from them) whose chain contains `owner`; `owner` itself if none.
std::vector<std::string> leavesUnder(const std::map<std::string, ThemeFacts>& facts, const std::string& owner) {
  std::set<std::string> parents;
  for (const auto& [n, f] : facts)
    if (!f.parent.empty()) parents.insert(f.parent);
  std::vector<std::string> leaves;
  for (const auto& [n, f] : facts) {
    if (parents.count(n)) continue;
    auto ch = chainOf(facts, n);
    if (std::find(ch.begin(), ch.end(), owner) != ch.end()) leaves.push_back(n);
  }
  if (leaves.empty()) leaves.push_back(owner);
  return leaves;
}

void checkImages(Ctx& c, const std::map<std::string, std::string>& files) {
  // theme/base path -> density -> (w, h)
  std::map<std::string, std::map<float, std::pair<int, int>>> variants;
  for (const auto& [path, bytes] : files) {
    if (themeResourceKind(path) != SQ_THEME_RES_IMAGE) continue;
    std::string err;
    ImagePtr img = decodeImage(bytes.data(), bytes.size(), 1.0f, &err);
    if (!img) {
      c.error(path, "image does not decode: " + err);
      continue;
    }
    std::string base;
    float d = 1;
    splitDensity(path, base, d);
    variants[base][d] = {img->width(), img->height()};
  }
  for (const auto& [base, byDensity] : variants) {
    auto one = byDensity.find(1.0f);
    if (one == byDensity.end()) {
      c.warn(base, "has @Nx variants but no 1x image");
      continue;
    }
    for (const auto& [d, wh] : byDensity) {
      if (d == 1.0f) continue;
      const double ew = one->second.first * d, eh = one->second.second * d;
      if (std::abs(wh.first - ew) > 1.0 || std::abs(wh.second - eh) > 1.0) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "@%gx variant is %dx%d, expected about %.0fx%.0f", static_cast<double>(d), wh.first, wh.second, ew, eh);
        c.warn(base, buf);
      }
    }
  }
}

void checkSkins(Ctx& c, ThemeResolver& themes, const std::map<std::string, ThemeFacts>& facts,
                const std::map<std::string, std::string>& files) {
  for (const auto& [path, bytes] : files) {
    if (themeResourceKind(path) != SQ_THEME_RES_SKIN) continue;
    const std::string owner = path.substr(0, path.find('/'));
    const std::vector<std::string> leaves = leavesUnder(facts, owner);
    SkinValidateOptions vo;
    vo.customElements = c.opt.customElements;
    std::vector<std::string> missingIn;
    vo.assetExists = [&](const std::string& url) {
      constexpr std::string_view kCurrent = "theme://current/";
      if (url.rfind(kCurrent, 0) != 0) return themes.read(url).has_value();
      std::string rest = url.substr(kCurrent.size());
      bool all = true;
      for (const auto& leaf : leaves)
        if (!themes.read("theme://" + leaf + "/" + rest)) {
          all = false;
          missingIn.push_back(leaf);
        }
      return all;
    };
    std::vector<SkinIssue> issues;
    Skin::validate(bytes, issues, vo);
    for (const auto& i : issues) c.r.diagnostics.push_back(Diagnostic{i.error, path, i.where + ": " + i.message});
    if (!missingIn.empty()) {
      std::sort(missingIn.begin(), missingIn.end());
      missingIn.erase(std::unique(missingIn.begin(), missingIn.end()), missingIn.end());
      std::string list;
      for (const auto& m : missingIn) list += (list.empty() ? "" : ", ") + m;
      c.error(path, "theme://current/ assets are missing in theme(s): " + list);
    }
    // Remote pages are top-level instantiations: build them for real under every leaf theme.
    std::string perr;
    auto skin = Skin::parse(bytes, &perr);
    if (!skin || skin->remotePages().empty()) continue;
    for (const auto& leaf : leaves) {
      std::vector<RemotePageSpec> pages;
      std::string err;
      if (!buildRemotePages(*skin, themes.skinDefinesFor(leaf), "object://skinc/context", pages, &err))
        c.error(path, "remote pages fail under theme '" + leaf + "': " + err);
    }
  }
}

// ---- outputs -----------------------------------------------------------------------------------------

std::string jsonString(std::string_view s) {
  std::string o = "\"";
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') {
      o += '\\';
      o += static_cast<char>(ch);
    } else if (ch < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", ch);
      o += b;
    } else {
      o += static_cast<char>(ch);
    }
  }
  return o + "\"";
}

const char* kindName(int k) {
  switch (k) {
    case SQ_THEME_RES_IMAGE: return "image";
    case SQ_THEME_RES_SKIN: return "skin";
    case SQ_THEME_RES_FONT: return "font";
    case SQ_THEME_RES_COLORS: return "colors";
    default: return "other";
  }
}

std::string absolutePath(const std::string& p) {
  Buffer b;
  std::string a = sq_fs_absolute(p.c_str(), b.out()) == SQ_OK ? b.str() : p;
  std::replace(a.begin(), a.end(), '\\', '/');
  return a;
}

bool validResourceName(const std::string& n) {
  if (n.empty() || n.size() > 64 || std::isdigit(static_cast<unsigned char>(n[0]))) return false;
  return std::all_of(n.begin(), n.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; });
}

bool writeOutputs(Ctx& c, const ThemePack& pack) {
  const Options& o = c.opt;
  auto put = [&](const std::string& path, const std::string& data) {
    if (path.empty()) return true;
    if (writeFile(path, data) != SQ_OK) {
      c.error(path, "cannot write");
      return false;
    }
    return true;
  };
  if (!put(o.out, c.r.pack)) return false;
  if (!o.rc.empty()) {
    std::string rc = "// Generated by sq-skinc - do not edit.\n// pack sha256 " + c.r.sha256 + ", " + std::to_string(c.r.pack.size()) +
                     " bytes, " + std::to_string(pack.count()) + " files\n" + o.resourceName + " RCDATA \"" + absolutePath(o.out) + "\"\n";
    if (!put(o.rc, rc)) return false;
  }
  if (!o.header.empty()) {
    std::string h = "/* Generated by sq-skinc - do not edit. Theme pack embedded as RCDATA (see sq/ui/theme.hpp). */\n#pragma once\n";
    h += "#define SQ_THEME_PACK_RESOURCE \"" + o.resourceName + "\"\n";
    h += "#define SQ_THEME_PACK_SIZE " + std::to_string(c.r.pack.size()) + "u\n";
    h += "#define SQ_THEME_PACK_FILES " + std::to_string(pack.count()) + "u\n";
    h += "#define SQ_THEME_PACK_SHA256 \"" + c.r.sha256 + "\"\n";
    if (!put(o.header, h)) return false;
  }
  if (!o.manifest.empty()) {
    std::string j = "{\n  \"format\": 1,\n  \"size\": " + std::to_string(c.r.pack.size()) + ",\n  \"sha256\": \"" + c.r.sha256 +
                    "\",\n  \"themes\": [";
    for (size_t i = 0; i < c.r.themes.size(); ++i) j += (i ? ", " : "") + jsonString(c.r.themes[i]);
    j += "],\n  \"entries\": [\n";
    for (size_t i = 0; i < pack.count(); ++i) {
      j += "    {\"path\": " + jsonString(pack.path(i)) + ", \"size\": " + std::to_string(pack.data(i).size()) + ", \"kind\": \"" +
           kindName(pack.kind(i)) + "\", \"sha256\": \"" + sha256Hex(pack.data(i)) + "\"}" + (i + 1 < pack.count() ? ",\n" : "\n");
    }
    j += "  ]\n}\n";
    if (!put(o.manifest, j)) return false;
  }
  return true;
}

}  // namespace

Result compile(const Options& options) {
  Result r;
  Ctx c{options, r};
  try {
    if (!validResourceName(options.resourceName)) {
      c.error("", "resource name must be 1..64 of [A-Za-z0-9_] not starting with a digit");
      return r;
    }
    if (!options.validateOnly && options.out.empty() && (!options.rc.empty() || !options.header.empty() || !options.manifest.empty())) {
      c.error("", "--rc / --header / --manifest need --out");
      return r;
    }
    std::map<std::string, std::string> files;
    if (!collect(c, files)) return r;
    r.files = files.size();

    // Per-theme load through the real resolver (theme.xml syntax, names, colors, metrics).
    std::map<std::string, std::vector<ThemePackEntry>> byTheme;
    for (const auto& [path, bytes] : files) byTheme[path.substr(0, path.find('/'))].push_back(ThemePackEntry{path, bytes});
    auto themes = ThemeResolver::create();
    std::map<std::string, ThemeFacts> facts;
    for (auto& [name, entries] : byTheme) {
      r.themes.push_back(name);
      std::string blob, err;
      if (!writeThemePack(entries, blob, &err)) {
        c.error(name, err);
        continue;
      }
      auto sub = ThemePack::fromMemory(blob.data(), blob.size(), true, &err);
      if (!sub || !themes->addPack(sub, &err)) {
        c.error(name + "/theme.xml", err);
        continue;
      }
      checkThemeXml(c, name, files.at(name + "/theme.xml"), facts[name]);
    }
    // Inheritance: known parents, no cycles; colors (and their aliases) resolve in every theme.
    for (const auto& [name, f] : facts) {
      if (f.parent.empty()) continue;
      if (!facts.count(f.parent)) c.warn(name + "/theme.xml", "inherits '" + f.parent + "', which is not in this pack (must come from another source)");
      auto ch = chainOf(facts, name);
      std::string last = ch.empty() ? std::string() : facts.at(ch.back()).parent;
      if (!last.empty() && std::find(ch.begin(), ch.end(), last) != ch.end()) c.error(name + "/theme.xml", "inheritance cycle through '" + last + "'");
    }
    for (const auto& [name, f] : facts) {
      if (themes->setActiveTheme(name.c_str()) < 0) continue;
      for (const auto& member : chainOf(facts, name))
        for (const auto& cn : facts.at(member).colorNames) {
          uint32_t rgba = 0;
          if (themes->getColor(cn.c_str(), &rgba) != SQ_OK)
            c.error(member + "/theme.xml", "color '" + cn + "' does not resolve in theme '" + name + "' (alias to an unknown or looping color)");
        }
    }
    checkImages(c, files);
    checkSkins(c, *themes, facts, files);

    std::sort(r.diagnostics.begin(), r.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
      return std::tie(a.file, a.message) < std::tie(b.file, b.message);
    });
    r.diagnostics.erase(std::unique(r.diagnostics.begin(), r.diagnostics.end(),
                                    [](const Diagnostic& a, const Diagnostic& b) { return a.error == b.error && a.file == b.file && a.message == b.message; }),
                        r.diagnostics.end());
    if (r.errors() || (options.strict && r.warnings())) return r;

    std::vector<ThemePackEntry> all;
    all.reserve(files.size());
    for (auto& [path, bytes] : files) all.push_back(ThemePackEntry{path, std::move(bytes)});
    std::string err;
    if (!writeThemePack(std::move(all), r.pack, &err)) {
      c.error("", "packing failed: " + err);
      return r;
    }
    r.sha256 = sha256Hex(r.pack);
    // Self check: the pack must load exactly like the folders did.
    auto pack = ThemePack::fromMemory(r.pack.data(), r.pack.size(), false, &err);
    auto again = ThemeResolver::create();
    if (!pack || !again->addPack(pack, &err)) {
      c.error("", "internal: the written pack does not load: " + err);
      r.pack.clear();
      return r;
    }
    if (!options.validateOnly && !writeOutputs(c, *pack)) return r;
    r.ok = true;
  } catch (const std::bad_alloc&) {
    c.error("", "out of memory");
  }
  return r;
}

// ---- CLI ------------------------------------------------------------------------------------------

namespace {
void usage(std::FILE* f) {
  std::fputs(
      "usage: sq-skinc [options] <theme-folder | folder-of-themes>...\n"
      "  -o, --out FILE          write the theme pack (SQTHMPK1)\n"
      "      --rc FILE           write a Windows .rc embedding the pack as RCDATA\n"
      "      --header FILE       write a C header (resource name, size, sha256)\n"
      "      --manifest FILE     write a JSON listing of the packed files\n"
      "      --resource-name N   RCDATA name (default SQTHEMES)\n"
      "      --element NAME      accept a host-registered skin element (repeatable)\n"
      "      --validate-only     check everything, write nothing\n"
      "      --strict            warnings fail the run\n"
      "  -q, --quiet             only print problems\n"
      "exit status: 0 ok, 1 validation failed, 2 usage or IO error\n",
      f);
}
}  // namespace

int runCli(int argc, char** argv, std::FILE* out, std::FILE* err) {
  Options o;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (a == "-h" || a == "--help") {
      usage(out);
      return 0;
    } else if (a == "-o" || a == "--out") {
      if (!value(o.out)) return usage(err), 2;
    } else if (a == "--rc") {
      if (!value(o.rc)) return usage(err), 2;
    } else if (a == "--header") {
      if (!value(o.header)) return usage(err), 2;
    } else if (a == "--manifest") {
      if (!value(o.manifest)) return usage(err), 2;
    } else if (a == "--resource-name") {
      if (!value(o.resourceName)) return usage(err), 2;
    } else if (a == "--element") {
      if (!value(v)) return usage(err), 2;
      o.customElements.push_back(v);
    } else if (a == "--validate-only") {
      o.validateOnly = true;
    } else if (a == "--strict") {
      o.strict = true;
    } else if (a == "-q" || a == "--quiet") {
      quiet = true;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(err, "sq-skinc: unknown option %s\n", a.c_str());
      usage(err);
      return 2;
    } else {
      o.inputs.push_back(a);
    }
  }
  if (o.inputs.empty() || (!o.validateOnly && o.out.empty())) {
    usage(err);
    return 2;
  }
  Result r = compile(o);
  for (const auto& d : r.diagnostics)
    std::fprintf(err, "%s: %s: %s\n", d.file.empty() ? "sq-skinc" : d.file.c_str(), d.error ? "error" : "warning", d.message.c_str());
  if (!r.ok) {
    std::fprintf(err, "sq-skinc: FAILED (%zu error(s), %zu warning(s)%s)\n", r.errors(), r.warnings(),
                 o.strict && r.warnings() ? ", --strict" : "");
    bool io = std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                          [](const Diagnostic& d) { return d.error && (d.message == "cannot write" || d.message == "cannot read file"); });
    return io ? 2 : 1;
  }
  if (!quiet) {
    std::fprintf(out, "sq-skinc: %zu theme(s), %zu file(s), %zu warning(s)%s\n", r.themes.size(), r.files, r.warnings(),
                 o.validateOnly ? ", validated (nothing written)" : "");
    if (!o.validateOnly) std::fprintf(out, "sq-skinc: %s: %zu bytes, sha256 %s\n", o.out.c_str(), r.pack.size(), r.sha256.c_str());
  }
  return 0;
}

}  // namespace sq::tools::skinc
