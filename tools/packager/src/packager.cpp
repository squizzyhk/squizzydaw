// sq-pack implementation: see include/sq/tools/packager/packager.hpp.
#include "sq/tools/packager/packager.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "sq/platform.hpp"

namespace sq::packager {

namespace sf = sq::formats;
using sf::FileEntry;
using sf::FileFormat;

namespace {

// ---- small helpers ------------------------------------------------------------------------------
Status err(sq_result c, std::string m) { return Status::error(c, std::move(m)); }

void line(std::string& r, const std::string& k, const std::string& v) { r += k + ": " + v + "\n"; }
void item(std::string& r, const std::string& v) { r += "  " + v + "\n"; }

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string abiText(uint32_t abi) {
  return std::to_string(SQ_ABI_GET_MAJOR(abi)) + "." +
         (SQ_ABI_GET_MINOR(abi) == 0xFFFF ? std::string("*") : std::to_string(SQ_ABI_GET_MINOR(abi)));
}

// Every regular file under dir (sorted, '/' separated, relative).
Status loadTree(const std::string& dir, std::vector<FileEntry>& out) {
  out.clear();
  std::vector<std::string> names;
  SQF_TRY(sf::listFilesRecursive(dir, names));
  std::sort(names.begin(), names.end());
  for (const auto& n : names) {
    if (!sf::isSafeRelativePath(n)) return err(SQ_E_INVALIDARG, "unsafe file name in tree: '" + n + "'");
    FileEntry e;
    e.name = n;
    SQF_TRY(sf::readFileBytes(sq::joinPath(dir, n), e.data));
    out.push_back(std::move(e));
  }
  return {};
}

Status zipFromTree(const std::vector<FileEntry>& files, std::string& bytes) {
  sf::ZipArchive zip;
  for (const auto& f : files)
    if (!zip.add(f.name, f.data)) return err(SQ_E_INVALIDARG, "unsafe member name '" + f.name + "'");
  return sf::zipWrite(zip, bytes);
}

bool wellFormedXml(const std::string& text, std::string* why) {
  std::string e;
  sq::XmlDocPtr d = sq::xmlParse(text, &e);
  if (!d && why) *why = e;
  return static_cast<bool>(d);
}

// ---- script package manifest ---------------------------------------------------------------------
struct ScriptManifest {
  std::string id, name, version, abi;
  struct Cls {
    std::string classId, category, name, sourceFile, functionName;
  };
  std::vector<Cls> classes;
};

// Locates package.xml in a bundle (root, or exactly one top folder): returns the prefix.
bool findManifest(const std::vector<FileEntry>& files, std::string& prefix) {
  for (const auto& f : files)
    if (f.name == "package.xml") { prefix.clear(); return true; }
  std::set<std::string> tops;
  for (const auto& f : files) {
    size_t s = f.name.find('/');
    tops.insert(s == std::string::npos ? std::string() : f.name.substr(0, s));
  }
  if (tops.size() != 1 || tops.begin()->empty()) return false;
  prefix = *tops.begin() + "/";
  for (const auto& f : files)
    if (f.name == prefix + "package.xml") return true;
  return false;
}

// The script engine's package id rule (it ends up in package:// URLs): 1..128 chars of
// [A-Za-z0-9._-], starting with a letter or digit.
bool validScriptPackageId(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  const char c0 = id[0];
  if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || (c0 >= '0' && c0 <= '9'))) return false;
  return sf::isSafeIdentifier(id);
}

Status parseManifest(const std::string& xml, ScriptManifest& m) {
  std::string e;
  sq::XmlDocPtr doc = sq::xmlParse(xml, &e);
  if (!doc) return err(SQ_E_PARSE, "package.xml: " + e);
  sq::XmlNode root = sq::xmlRoot(doc);
  sq::XmlNode factory;
  if (root.name() == "ScriptPackage") {
    m.id = root.attr("id");
    m.name = root.attr("name");
    m.version = root.attr("version");
    m.abi = root.attr("abi");
    if (!m.id.empty() && !validScriptPackageId(m.id))
      return err(SQ_E_PARSE, "package.xml: invalid package id '" + m.id + "' (letters, digits, . _ -; starts with a letter or digit)");
    factory = root.child("ClassFactory");
  } else if (root.name() == "ClassFactory") {
    factory = root;
  } else {
    return err(SQ_E_PARSE, "package.xml: root must be <ScriptPackage> or <ClassFactory>");
  }
  if (!factory) return err(SQ_E_PARSE, "package.xml: no <ClassFactory>");
  std::set<std::string> ids;
  for (sq::XmlNode sc : factory.children("ScriptClass")) {
    ScriptManifest::Cls c{sc.attr("classID"), sc.attr("category", "ScriptClass"), sc.attr("name"), sc.attr("sourceFile"),
                          sc.attr("functionName")};
    sq_guid g{};
    if (!sq_guid_parse(c.classId.c_str(), &g) || sq_guid_is_null(&g))
      return err(SQ_E_PARSE, "package.xml: ScriptClass with invalid classID '" + c.classId + "'");
    if (!ids.insert(c.classId).second) return err(SQ_E_PARSE, "package.xml: duplicate classID " + c.classId);
    if (c.sourceFile.empty() || c.functionName.empty() || !sf::isSafeRelativePath(c.sourceFile))
      return err(SQ_E_PARSE, "package.xml: ScriptClass " + c.classId + " needs a safe sourceFile and a functionName");
    m.classes.push_back(std::move(c));
  }
  return {};
}

// Same rule as the script engine: same ABI major, package minor <= host minor.
Status gateScriptAbi(const ScriptManifest& m, uint32_t hostAbi) {
  if (m.abi.empty()) return {};
  uint32_t abi = 0;
  if (!sf::parseAbi(m.abi, abi) || SQ_ABI_GET_MINOR(abi) == 0xFFFF) return err(SQ_E_PARSE, "package.xml: bad abi '" + m.abi + "'");
  if (SQ_ABI_GET_MAJOR(abi) != SQ_ABI_GET_MAJOR(hostAbi) || SQ_ABI_GET_MINOR(abi) > SQ_ABI_GET_MINOR(hostAbi))
    return err(SQ_E_ABI_MISMATCH, "package requires platform ABI " + m.abi + ", host has " + abiText(hostAbi));
  return {};
}

// Validates a script bundle: manifest present + parseable, every sourceFile in the bundle.
Status checkScriptBundle(const std::vector<FileEntry>& files, ScriptManifest& m) {
  std::string prefix;
  if (!findManifest(files, prefix)) return err(SQ_E_PARSE, "script package: no package.xml (at the root or in one top folder)");
  const FileEntry* mf = nullptr;
  for (const auto& f : files)
    if (f.name == prefix + "package.xml") mf = &f;
  SQF_TRY(parseManifest(mf->data, m));
  for (const auto& c : m.classes) {
    bool found = false;
    for (const auto& f : files) found = found || f.name == prefix + c.sourceFile;
    if (!found) return err(SQ_E_PARSE, "script package: sourceFile '" + c.sourceFile + "' of " + c.classId + " is missing");
  }
  return {};
}

// A manifest-only (Xml payload) script package carries no source files, so a ScriptClass in it can
// never be instantiated (the engine refuses such packages): only class-less manifests are valid.
Status checkScriptXmlPayload(const std::string& xml, ScriptManifest& m) {
  SQF_TRY(parseManifest(xml, m));
  if (!m.classes.empty())
    return err(SQ_E_PARSE, "script package: Xml payload declares ScriptClass " + m.classes.front().classId + " but carries no '" +
                               m.classes.front().sourceFile + "' (build the package from a directory instead)");
  return {};
}

// Windows-portable member names for unpack: no reserved device names, no trailing dot/space
// segments (Windows strips them, so two members could land on one file).
bool portableMemberName(const std::string& name) {
  static const char* const kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                          "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find('/', start);
    if (end == std::string::npos) end = name.size();
    const std::string seg = name.substr(start, end - start);
    if (seg.empty() || seg.back() == '.' || seg.back() == ' ') return false;
    std::string stem = seg.substr(0, seg.find('.'));
    for (char& c : stem) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (const char* r : kReserved)
      if (stem == r) return false;
    if (end == name.size()) break;
    start = end + 1;
  }
  return true;
}

std::string asciiLower(std::string s) {
  for (char& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  return s;
}

Status checkSkinBundle(const std::vector<FileEntry>& files) {
  for (const auto& f : files) {
    std::string why;
    if (sf::fileExtension(f.name) == "xml" && !wellFormedXml(f.data, &why))
      return err(SQ_E_PARSE, "skin package: " + f.name + " is not well-formed XML: " + why);
  }
  return {};
}

// ---- langpack catalogs: .po sources become .mo members ------------------------------------------
Status compilePoCatalogs(std::vector<FileEntry>& files) {
  std::set<std::string> mo;
  for (const auto& f : files)
    if (f.name.rfind("catalogs/", 0) == 0 && endsWith(f.name, ".mo")) mo.insert(f.name.substr(0, f.name.size() - 3));
  for (auto& f : files) {
    if (f.name.rfind("catalogs/", 0) != 0 || !endsWith(f.name, ".po")) continue;
    std::string stem = f.name.substr(0, f.name.size() - 3);
    if (mo.count(stem)) return err(SQ_E_INVALIDARG, "langpack: both " + stem + ".po and " + stem + ".mo");
    sf::Catalog c;
    Status s = sf::poParse(f.data, c);
    if (!s.ok()) return err(s.code, f.name + ": " + s.message);
    std::string bin;
    SQF_TRY(sf::moWrite(c, bin));
    f.name = stem + ".mo";
    f.data = std::move(bin);
  }
  std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
  return {};
}

sf::PresetKind presetKindOf(BuildType t) {
  switch (t) {
    case BuildType::MultiPreset: return sf::PresetKind::MultiPreset;
    case BuildType::Instrument: return sf::PresetKind::Instrument;
    case BuildType::TrackPreset: return sf::PresetKind::TrackPreset;
    default: return sf::PresetKind::Preset;
  }
}

bool isImage(const std::string& name) {
  std::string e = sf::fileExtension(name);
  return e == "png" || e == "webp";
}

Status autoSnapshots(const std::vector<FileEntry>& files, const BuildOptions& opt, sf::SnapshotPackage& pkg) {
  pkg.info = opt.info;
  std::set<std::string> ids;
  for (const auto& f : files) {
    if (!isImage(f.name)) return err(SQ_E_INVALIDARG, "snapshot tree without snapshots.json: unexpected file " + f.name);
    sf::Snapshot s;
    // id = path without extension; "<name>@2x" images are the 2x variants (own entry, scale 2)
    s.id = f.name.substr(0, f.name.rfind('.'));
    s.scale = endsWith(s.id, "@2x") ? 2.0 : 1.0;
    s.file = f.name;
    s.data = f.data;
    if (!ids.insert(s.id).second) return err(SQ_E_INVALIDARG, "duplicate snapshot id " + s.id);
    pkg.snapshots.push_back(std::move(s));
  }
  if (pkg.snapshots.empty()) return err(SQ_E_INVALIDARG, "snapshot tree has no images");
  return {};
}

}  // namespace

// ---- build types ----------------------------------------------------------------------------------
bool buildTypeFromName(const std::string& s, BuildType& out) {
  static const std::pair<const char*, BuildType> k[] = {
      {"script", BuildType::Script},         {"skin", BuildType::Skin},
      {"snapshot", BuildType::Snapshot},     {"snapshots", BuildType::Snapshot},
      {"langpack", BuildType::LangPack},     {"preset", BuildType::Preset},
      {"multipreset", BuildType::MultiPreset}, {"instrument", BuildType::Instrument},
      {"trackpreset", BuildType::TrackPreset},
  };
  for (const auto& [n, t] : k)
    if (s == n) { out = t; return true; }
  return false;
}
const char* buildTypeName(BuildType t) {
  switch (t) {
    case BuildType::Script: return "script";
    case BuildType::Skin: return "skin";
    case BuildType::Snapshot: return "snapshot";
    case BuildType::LangPack: return "langpack";
    case BuildType::Preset: return "preset";
    case BuildType::MultiPreset: return "multipreset";
    case BuildType::Instrument: return "instrument";
    case BuildType::TrackPreset: return "trackpreset";
  }
  return "?";
}
const char* buildTypeExtension(BuildType t) {
  switch (t) {
    case BuildType::Script: case BuildType::Skin: case BuildType::Snapshot: return "package";
    case BuildType::LangPack: return "langpack";
    default: return buildTypeName(t);
  }
}

// ---- build ----------------------------------------------------------------------------------------
Status build(BuildType type, const std::string& src, std::string& bytes, const BuildOptions& opt) {
  bytes.clear();
  if (type == BuildType::Script || type == BuildType::Skin) {
    sf::ScriptPackage p;
    p.kind = type == BuildType::Script ? sf::PackageKind::Script : sf::PackageKind::Skin;
    if (sq_fs_is_file(src.c_str())) {
      const std::string ext = sf::fileExtension(src);
      SQF_TRY(sf::readFileBytes(src, p.text));
      if (ext == "xml") {
        p.content = sf::PackageContent::Xml;
        if (type == BuildType::Script) {
          ScriptManifest m;
          SQF_TRY(checkScriptXmlPayload(p.text, m));
        }
      } else if (ext == "js") {
        // The script engine loads packages through their package.xml manifest; a bare script
        // payload would build a script package nothing can load.
        if (type == BuildType::Script)
          return err(SQ_E_UNSUPPORTED, "a script package needs a package.xml manifest: build it from a directory, not a single .js file");
        p.content = sf::PackageContent::JavaScript;
      } else {
        return err(SQ_E_INVALIDARG, "single-file " + std::string(buildTypeName(type)) + " package must be .xml or .js");
      }
    } else if (sq_fs_is_dir(src.c_str())) {
      p.content = sf::PackageContent::Bundle;
      SQF_TRY(loadTree(src, p.files));
      if (p.files.empty()) return err(SQ_E_INVALIDARG, "empty package tree");
      if (type == BuildType::Script) {
        ScriptManifest m;
        SQF_TRY(checkScriptBundle(p.files, m));
      } else {
        SQF_TRY(checkSkinBundle(p.files));
      }
    } else {
      return err(SQ_E_NOTFOUND, "no such file or directory: " + src);
    }
    return sf::packageWrite(p, bytes);
  }

  if (!sq_fs_is_dir(src.c_str())) return err(SQ_E_NOTFOUND, "not a directory: " + src);
  std::vector<FileEntry> files;
  SQF_TRY(loadTree(src, files));
  std::string raw;
  switch (type) {
    case BuildType::Snapshot: {
      sf::SnapshotPackage pkg;
      bool manifest = std::any_of(files.begin(), files.end(), [](const FileEntry& f) { return f.name == sf::kSnapshotsManifest; });
      if (manifest) {
        SQF_TRY(zipFromTree(files, raw));
        SQF_TRY(sf::snapshotPackageRead(raw, pkg));
        for (const auto& [k, v] : opt.info) sf::setValue(pkg.info, k, v);
        // members the manifest does not name would be dropped silently: refuse instead
        std::set<std::string> named{sf::kSnapshotsManifest};
        for (const auto& s : pkg.snapshots) named.insert(s.file);
        for (const auto& f : files)
          if (!named.count(f.name)) return err(SQ_E_INVALIDARG, "snapshots.json does not list " + f.name);
      } else {
        SQF_TRY(autoSnapshots(files, opt, pkg));
      }
      return sf::snapshotPackageWrite(pkg, bytes);
    }
    case BuildType::LangPack: {
      SQF_TRY(compilePoCatalogs(files));
      SQF_TRY(zipFromTree(files, raw));
      sf::LangPack lp;
      SQF_TRY(sf::langPackRead(raw, lp));
      // members outside the schema would be dropped by the canonical rewrite: refuse instead
      for (const auto& f : files)
        if (f.name != sf::kMetaInfoFile && !(f.name.rfind("catalogs/", 0) == 0 && endsWith(f.name, ".mo")) &&
            !(f.name.rfind("properties/", 0) == 0 && endsWith(f.name, ".properties")))
          return err(SQ_E_INVALIDARG, "langpack: unexpected file " + f.name);
      return sf::langPackWrite(lp, bytes);
    }
    default: {
      SQF_TRY(zipFromTree(files, raw));
      sf::Preset p;
      SQF_TRY(sf::presetRead(raw, p));
      if (p.kind != presetKindOf(type))
        return err(SQ_E_INVALIDARG, std::string("preset tree is a ") + sf::presetKindName(p.kind) + ", not a " + buildTypeName(type));
      SQF_TRY(sf::presetWrite(p, bytes));
      // the canonical form must carry every input member
      sf::ZipArchive canon;
      SQF_TRY(sf::zipRead(bytes, canon, sf::ZipLimits::metadata()));
      for (const auto& f : files)
        if (!canon.contains(f.name)) return err(SQ_E_INVALIDARG, "preset: unexpected file " + f.name);
      return {};
    }
  }
}

// ---- unpack ---------------------------------------------------------------------------------------
Status unpack(const std::string& file, const std::string& outDir, bool poCatalogs, std::vector<std::string>* written) {
  std::string bytes;
  SQF_TRY(sf::readFileBytes(file, bytes));
  std::vector<FileEntry> out;
  const FileFormat fmt = sf::detectFormat(file, bytes);
  switch (fmt) {
    case FileFormat::ScriptPackage: {
      sf::ScriptPackage p;
      SQF_TRY(sf::packageRead(bytes, p));
      if (p.content == sf::PackageContent::Bundle) out = std::move(p.files);
      else out.push_back(FileEntry{p.content == sf::PackageContent::Xml ? "payload.xml" : "payload.js", std::move(p.text)});
      break;
    }
    case FileFormat::SnapshotPackage: {
      sf::SnapshotPackage p;
      SQF_TRY(sf::snapshotPackageRead(bytes, p));
      std::string canon;
      SQF_TRY(sf::snapshotPackageWrite(p, canon));
      sf::ZipArchive z;
      SQF_TRY(sf::zipRead(canon, z, sf::ZipLimits::metadata()));
      out = std::move(z.entries);
      break;
    }
    case FileFormat::LangPack: {
      sf::LangPack lp;
      SQF_TRY(sf::langPackRead(bytes, lp));
      std::string canon;
      SQF_TRY(sf::langPackWrite(lp, canon));
      sf::ZipArchive z;
      SQF_TRY(sf::zipRead(canon, z, sf::ZipLimits::metadata()));
      for (auto& e : z.entries) {
        if (poCatalogs && e.name.rfind("catalogs/", 0) == 0 && endsWith(e.name, ".mo")) {
          sf::Catalog c;
          SQF_TRY(sf::moRead(e.data, c));
          e.data = sf::poWrite(c);
          e.name = e.name.substr(0, e.name.size() - 3) + ".po";
        }
        out.push_back(std::move(e));
      }
      break;
    }
    case FileFormat::Preset: case FileFormat::MultiPreset: case FileFormat::Instrument: case FileFormat::TrackPreset: {
      sf::Preset p;
      SQF_TRY(sf::presetRead(bytes, p));
      std::string canon;
      SQF_TRY(sf::presetWrite(p, canon));
      sf::ZipArchive z;
      SQF_TRY(sf::zipRead(canon, z, sf::ZipLimits::metadata()));
      out = std::move(z.entries);
      break;
    }
    default:
      return err(SQ_E_UNSUPPORTED, file + ": not a container sq-pack can unpack (" + sf::fileFormatName(fmt) + ")");
  }
  std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
  // Validate every member before anything is written: unsafe or non-portable names, and names that
  // collide on a case-insensitive file system or as file vs. directory, would make the unpacked
  // tree differ from the container (and the rebuild differ from the original).
  {
    std::set<std::string> files, dirs;
    for (const auto& e : out) {
      if (!sf::isSafeRelativePath(e.name)) return err(SQ_E_PARSE, "unsafe member name '" + e.name + "'");
      if (!portableMemberName(e.name)) return err(SQ_E_PARSE, "member name not portable to every file system: '" + e.name + "'");
      const std::string low = asciiLower(e.name);
      if (!files.insert(low).second || dirs.count(low))
        return err(SQ_E_PARSE, "member '" + e.name + "' collides with another member (names differ only in case, or file vs. folder)");
      for (size_t s = low.find('/'); s != std::string::npos; s = low.find('/', s + 1)) {
        const std::string d = low.substr(0, s);
        if (files.count(d)) return err(SQ_E_PARSE, "member '" + e.name + "' collides with another member (file vs. folder)");
        dirs.insert(d);
      }
    }
  }
  // Unpacking over an existing tree would mix stale files into the next build.
  if (sq_fs_is_file(outDir.c_str())) return err(SQ_E_EXISTS, outDir + " is a file");
  if (sq_fs_is_dir(outDir.c_str())) {
    bool any = false;
    sq_fs_list_dir(
        outDir.c_str(),
        [](void* user, const char*, sq_bool) -> sq_bool {
          *static_cast<bool*>(user) = true;
          return SQ_FALSE;
        },
        &any);
    if (any) return err(SQ_E_EXISTS, "output directory " + outDir + " is not empty");
  }
  if (sq_fs_create_dirs(outDir.c_str()) != SQ_OK) return err(SQ_E_IO, "cannot create " + outDir);
  for (const auto& e : out) {
    SQF_TRY(sf::writeFileBytes(sq::joinPath(outDir, e.name), e.data));
    if (written) written->push_back(e.name);
  }
  return {};
}

// ---- inspect / verify ------------------------------------------------------------------------------
namespace {

Status examineExtension(const std::string& dir, const VerifyOptions* vo, std::string& r) {
  sf::Extension ext;
  line(r, "format", "extension");
  SQF_TRY(sf::extensionLoad(dir, ext));
  line(r, "id", ext.id());
  line(r, "version", ext.meta.get(sf::meta::kVersion));
  if (ext.meta.has(sf::meta::kName)) line(r, "name", ext.meta.get(sf::meta::kName));
  line(r, "targets", std::to_string(ext.install.targets.size()));
  for (const auto& t : ext.install.targets)
    item(r, t.app + " abi " + abiText(t.abiMin) + ".." + abiText(t.abiMax) + " os " + t.os + " arch " + t.arch);
  const sf::HostIdentity host = vo ? vo->host : sf::HostIdentity::current();
  Status g = sf::extensionCheck(ext, host);
  line(r, "host", host.app + " abi " + abiText(host.abi) + " " + host.os + "/" + host.arch);
  line(r, "gate", g.ok() ? std::string("accepted") : "rejected (" + g.message + ")");
  if (vo && vo->gate && !g.ok()) return g;
  return {};
}

Status canonicalCheck(bool enabled, const std::string& original, const std::string& rewritten, std::string& r) {
  if (!enabled) return {};
  const bool same = original == rewritten;
  line(r, "canonical", same ? "yes" : "no");
  if (!same) return err(SQ_E_PARSE, "not canonical: rewriting the parsed content gives different bytes");
  return {};
}

Status examineFile(const std::string& path, const VerifyOptions* vo, std::string& r) {
  std::string bytes;
  SQF_TRY(sf::readFileBytes(path, bytes));
  const FileFormat fmt = sf::detectFormat(path, bytes);
  line(r, "format", sf::fileFormatName(fmt));
  line(r, "size", std::to_string(bytes.size()));
  line(r, "sha256", sq::sha256Hex(bytes));
  const bool canon = vo && vo->canonical;
  std::string again;
  switch (fmt) {
    case FileFormat::ScriptPackage: {
      sf::ScriptPackage p;
      SQF_TRY(sf::packageRead(bytes, p));
      line(r, "kind", sf::packageKindName(p.kind));
      line(r, "content", sf::packageContentName(p.content));
      if (p.content == sf::PackageContent::Bundle) {
        line(r, "files", std::to_string(p.files.size()));
        for (const auto& f : p.files) item(r, f.name + " (" + std::to_string(f.data.size()) + " bytes)");
      } else {
        line(r, "payload", std::to_string(p.text.size()) + " bytes");
      }
      if (p.kind == sf::PackageKind::Script && p.content == sf::PackageContent::JavaScript) {
        // Same rule as the script engine: no manifest, not loadable as a script package.
        line(r, "loadable", "no (bare script payload without package.xml)");
        if (vo && vo->gate)
          return err(SQ_E_UNSUPPORTED, "script package holds a bare script without package.xml; the script engine refuses it");
      } else if (p.kind == sf::PackageKind::Script) {
        ScriptManifest m;
        if (p.content == sf::PackageContent::Bundle) SQF_TRY(checkScriptBundle(p.files, m));
        else SQF_TRY(checkScriptXmlPayload(p.text, m));
        if (m.id.empty() && !validScriptPackageId(sf::fileStem(path)))
          return err(SQ_E_PARSE, "package.xml has no id and the file name '" + sf::fileStem(path) + "' is not a valid package id");
        line(r, "package-id", m.id.empty() ? std::string("(file name)") : m.id);
        if (!m.version.empty()) line(r, "version", m.version);
        line(r, "abi", m.abi.empty() ? std::string("(any)") : m.abi);
        line(r, "classes", std::to_string(m.classes.size()));
        for (const auto& c : m.classes) item(r, c.classId + " " + c.category + " \"" + c.name + "\" " + c.sourceFile + ":" + c.functionName);
        if (vo && vo->gate) {
          Status g = gateScriptAbi(m, vo->host.abi);
          line(r, "gate", g.ok() ? std::string("accepted") : "rejected (" + g.message + ")");
          if (!g.ok()) return g;
        }
      } else if (p.kind == sf::PackageKind::Skin && p.content == sf::PackageContent::Bundle) {
        SQF_TRY(checkSkinBundle(p.files));
      }
      SQF_TRY(sf::packageWrite(p, again));
      return canonicalCheck(canon, bytes, again, r);
    }
    case FileFormat::SnapshotPackage: {
      sf::SnapshotPackage p;
      SQF_TRY(sf::snapshotPackageRead(bytes, p));
      for (const auto& [k, v] : p.info) line(r, "info." + k, v);
      line(r, "snapshots", std::to_string(p.snapshots.size()));
      for (const auto& s : p.snapshots)
        item(r, s.id + " " + s.file + " " + std::to_string(s.width) + "x" + std::to_string(s.height) + " @" + sf::formatDouble(s.scale) + "x");
      SQF_TRY(sf::snapshotPackageWrite(p, again));
      return canonicalCheck(canon, bytes, again, r);
    }
    case FileFormat::LangPack: {
      sf::LangPack lp;
      SQF_TRY(sf::langPackRead(bytes, lp));
      line(r, "id", lp.meta.get(sf::meta::kId));
      line(r, "language", lp.meta.get(sf::meta::kLanguage));
      line(r, "catalogs", std::to_string(lp.catalogs.size()));
      for (const auto& [m, c] : lp.catalogs) item(r, m + " (" + std::to_string(c.messages.size()) + " messages)");
      line(r, "properties", std::to_string(lp.properties.size()));
      for (const auto& [n, kv] : lp.properties) item(r, n + " (" + std::to_string(kv.size()) + " keys)");
      SQF_TRY(sf::langPackWrite(lp, again));
      return canonicalCheck(canon, bytes, again, r);
    }
    case FileFormat::Preset: case FileFormat::MultiPreset: case FileFormat::Instrument: case FileFormat::TrackPreset: {
      sf::Preset p;
      SQF_TRY(sf::presetRead(bytes, p));
      line(r, "kind", sf::presetKindName(p.kind));
      if (p.meta.has(sf::meta::kId)) line(r, "id", p.meta.get(sf::meta::kId));
      if (p.meta.has(sf::meta::kName)) line(r, "name", p.meta.get(sf::meta::kName));
      line(r, "slots", std::to_string(p.slots.size()));
      for (const auto& s : p.slots)
        item(r, s.format + " " + s.classId + " \"" + s.name + "\" state " + std::to_string(s.state.size()) + " bytes" + (s.bypassed ? " (bypassed)" : ""));
      line(r, "settings", std::to_string(p.settings.size()));
      line(r, "attachments", std::to_string(p.attachments.size()));
      SQF_TRY(sf::presetWrite(p, again));
      return canonicalCheck(canon, bytes, again, r);
    }
    case FileFormat::DspPreset: {
      sf::DspPreset d;
      SQF_TRY(sf::dspPresetRead(bytes, d));
      line(r, "name", d.name);
      line(r, "device", d.deviceVendor + " " + d.deviceModel);
      line(r, "blocks", std::to_string(d.blocks.size()));
      SQF_TRY(sf::dspPresetWrite(d, again));
      return canonicalCheck(canon, bytes, again, r);
    }
    case FileFormat::Chords: {
      sf::ChordSet s;
      SQF_TRY(sf::chordSetFromXml(bytes, s));
      line(r, "name", s.name);
      line(r, "chords", std::to_string(s.chords.size()));
      return canonicalCheck(canon, bytes, sf::chordSetToXml(s), r);
    }
    case FileFormat::MusicalScale: {
      sf::MusicalScale s;
      SQF_TRY(sf::musicalScaleFromXml(bytes, s));
      line(r, "name", s.name);
      line(r, "degrees", std::to_string(s.degrees.size()));
      line(r, "period-cents", sf::formatDouble(s.periodCents));
      return canonicalCheck(canon, bytes, sf::musicalScaleToXml(s), r);
    }
    case FileFormat::ColorScheme: {
      sf::ColorScheme s;
      SQF_TRY(sf::colorSchemeFromXml(bytes, s));
      line(r, "name", s.name);
      line(r, "colors", std::to_string(s.colors.size()));
      return canonicalCheck(canon, bytes, sf::colorSchemeToXml(s), r);
    }
    default:
      return err(SQ_E_UNSUPPORTED, path + ": unknown content format");
  }
}

Status examine(const std::string& path, const VerifyOptions* vo, std::string& r) {
  r.clear();
  line(r, "path", path);
  if (sq_fs_is_dir(path.c_str())) {
    if (!sf::isExtensionDirectory(path)) return err(SQ_E_UNSUPPORTED, path + ": directory is not an extension (metainfo.xml + installdata.xml)");
    return examineExtension(path, vo, r);
  }
  if (!sq_fs_is_file(path.c_str())) return err(SQ_E_NOTFOUND, "no such file or directory: " + path);
  return examineFile(path, vo, r);
}

}  // namespace

Status inspect(const std::string& path, std::string& report) { return examine(path, nullptr, report); }

Status verify(const std::string& path, const VerifyOptions& opt, std::string& report) {
  Status s = examine(path, &opt, report);
  line(report, "result", s.ok() ? std::string("ok") : "FAILED (" + s.message + ")");
  return s;
}

Status extensionInit(const std::string& dir, const ExtensionInit& init) {
  const std::string id = init.meta.get(sf::meta::kId);
  if (!sf::isSafeIdentifier(id)) return err(SQ_E_INVALIDARG, "extension id must be [A-Za-z0-9._-]+: '" + id + "'");
  if (init.meta.get(sf::meta::kVersion).empty()) return err(SQ_E_INVALIDARG, "extension needs Package.Version");
  if (init.install.targets.empty()) return err(SQ_E_INVALIDARG, "extension needs at least one install target");
  for (const auto& t : init.install.targets)
    if (t.abiMin > t.abiMax) return err(SQ_E_INVALIDARG, "install target abiMin > abiMax");
  // validate through the readers before anything is written
  sf::InstallData back;
  const std::string inst = sf::installDataToXml(init.install);
  SQF_TRY(sf::installDataFromXml(inst, back));
  SQF_TRY(sf::writeFileBytes(sq::joinPath(dir, sf::kMetaInfoFile), sf::metaInfoToXml(init.meta)));
  SQF_TRY(sf::writeFileBytes(sq::joinPath(dir, sf::kInstallDataFile), inst));
  sf::Extension check;
  return sf::extensionLoad(dir, check);
}

// ---- CLI ------------------------------------------------------------------------------------------
std::string usage() {
  return "sq-pack - Squizzy Studio content packager\n"
         "\n"
         "  sq-pack build <type> <src> -o <out> [--info key=value]...\n"
         "      type: script | skin | snapshot | langpack | preset | multipreset | instrument | trackpreset\n"
         "      src:  a directory (layout as written by unpack); script/skin also take one .xml or .js file\n"
         "  sq-pack unpack <file> -o <dir> [--po]\n"
         "  sq-pack inspect <file|extension-dir>\n"
         "  sq-pack verify <file|extension-dir> [--canonical] [--no-gate] [gate options]\n"
         "  sq-pack ext init <dir> --id <id> --version <v> [--name <n>] [--vendor <v>] [--description <d>]\n"
         "                [--target-app <id>] [--abi-min <M.m>] [--abi-max <M.m|M.*>] [--os <os|*>] [--arch <arch|*>]\n"
         "  sq-pack ext install <dir> <extensions-root> [gate options]\n"
         "  sq-pack ext uninstall <extensions-root> <id>\n"
         "  sq-pack ext scan <extensions-root> [gate options]\n"
         "\n"
         "  gate options (default: this build): --app <id> --abi <M.m> --os <os> --arch <arch>\n"
         "  exit codes: 0 ok, 1 invalid content, 2 usage, 3 ABI mismatch, 4 unsupported app/platform/format\n";
}

namespace {

int exitFor(const Status& s) {
  if (s.ok()) return kExitOk;
  if (s.code == SQ_E_ABI_MISMATCH) return kExitAbiMismatch;
  if (s.code == SQ_E_UNSUPPORTED) return kExitUnsupported;
  return kExitInvalid;
}

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::vector<std::string>> opt;
  std::set<std::string> flags;
  std::string error;
};

Args parseArgs(const std::vector<std::string>& a, size_t from, const std::set<std::string>& flagNames) {
  Args r;
  for (size_t i = from; i < a.size(); ++i) {
    const std::string& s = a[i];
    if (s.size() > 1 && s[0] == '-' && !(s.size() > 1 && s[1] >= '0' && s[1] <= '9')) {
      if (flagNames.count(s)) { r.flags.insert(s); continue; }
      if (i + 1 >= a.size()) { r.error = "option " + s + " needs a value"; return r; }
      r.opt[s].push_back(a[++i]);
    } else {
      r.pos.push_back(s);
    }
  }
  return r;
}

std::string one(const Args& a, const char* key, const std::string& def = {}) {
  auto it = a.opt.find(key);
  return it == a.opt.end() || it->second.empty() ? def : it->second.back();
}

bool hostFromArgs(const Args& a, sf::HostIdentity& h, std::string& error) {
  h = sf::HostIdentity::current();
  if (a.opt.count("--app")) h.app = one(a, "--app");
  if (a.opt.count("--os")) h.os = one(a, "--os");
  if (a.opt.count("--arch")) h.arch = one(a, "--arch");
  if (a.opt.count("--abi")) {
    uint32_t abi = 0;
    if (!sf::parseAbi(one(a, "--abi"), abi) || SQ_ABI_GET_MINOR(abi) == 0xFFFF) {
      error = "bad --abi '" + one(a, "--abi") + "' (expected major.minor)";
      return false;
    }
    h.abi = abi;
  }
  return true;
}

int fail(std::string& err, const Status& s) {
  err += "sq-pack: " + s.message + "\n";
  return exitFor(s);
}

}  // namespace

int runCli(const std::vector<std::string>& args, std::string& out, std::string& errOut) {
  try {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
      (args.empty() ? errOut : out) += usage();
      return args.empty() ? kExitUsage : kExitOk;
    }
    const std::string cmd = args[0];
    if (cmd == "--version") {
      out += std::string("sq-pack (Squizzy Studio, platform ABI ") + abiText(SQ_PLATFORM_ABI) + ")\n";
      return kExitOk;
    }
    if (cmd == "build") {
      Args a = parseArgs(args, 1, {});
      if (!a.error.empty() || a.pos.size() != 2 || one(a, "-o").empty()) {
        errOut += "sq-pack: usage: build <type> <src> -o <out>\n" + a.error;
        return kExitUsage;
      }
      BuildType t;
      if (!buildTypeFromName(a.pos[0], t)) {
        errOut += "sq-pack: unknown build type '" + a.pos[0] + "'\n";
        return kExitUsage;
      }
      BuildOptions bo;
      if (a.opt.count("--info"))
        for (const auto& kv : a.opt["--info"]) {
          size_t eq = kv.find('=');
          if (eq == std::string::npos || eq == 0) {
            errOut += "sq-pack: --info expects key=value\n";
            return kExitUsage;
          }
          sf::setValue(bo.info, kv.substr(0, eq), kv.substr(eq + 1));
        }
      std::string bytes;
      Status s = build(t, a.pos[1], bytes, bo);
      if (!s.ok()) return fail(errOut, s);
      const std::string dst = one(a, "-o");
      s = sf::writeFileBytes(dst, bytes);
      if (!s.ok()) return fail(errOut, s);
      out += "built " + std::string(buildTypeName(t)) + " " + dst + " (" + std::to_string(bytes.size()) + " bytes, sha256 " +
             sq::sha256Hex(bytes) + ")\n";
      return kExitOk;
    }
    if (cmd == "unpack") {
      Args a = parseArgs(args, 1, {"--po"});
      if (!a.error.empty() || a.pos.size() != 1 || one(a, "-o").empty()) {
        errOut += "sq-pack: usage: unpack <file> -o <dir> [--po]\n";
        return kExitUsage;
      }
      std::vector<std::string> written;
      Status s = unpack(a.pos[0], one(a, "-o"), a.flags.count("--po") != 0, &written);
      if (!s.ok()) return fail(errOut, s);
      for (const auto& w : written) out += w + "\n";
      return kExitOk;
    }
    if (cmd == "inspect" || cmd == "verify") {
      Args a = parseArgs(args, 1, {"--canonical", "--no-gate"});
      if (!a.error.empty() || a.pos.size() != 1) {
        errOut += "sq-pack: usage: " + cmd + " <path>\n";
        return kExitUsage;
      }
      std::string report;
      Status s;
      if (cmd == "inspect") {
        s = inspect(a.pos[0], report);
      } else {
        VerifyOptions vo;
        vo.canonical = a.flags.count("--canonical") != 0;
        vo.gate = a.flags.count("--no-gate") == 0;
        std::string e;
        if (!hostFromArgs(a, vo.host, e)) {
          errOut += "sq-pack: " + e + "\n";
          return kExitUsage;
        }
        s = verify(a.pos[0], vo, report);
      }
      out += report;
      if (!s.ok()) return fail(errOut, s);
      return kExitOk;
    }
    if (cmd == "ext") {
      if (args.size() < 2) {
        errOut += usage();
        return kExitUsage;
      }
      const std::string sub = args[1];
      Args a = parseArgs(args, 2, {});
      if (!a.error.empty()) {
        errOut += "sq-pack: " + a.error + "\n";
        return kExitUsage;
      }
      sf::HostIdentity host;
      std::string e;
      if (!hostFromArgs(a, host, e)) {
        errOut += "sq-pack: " + e + "\n";
        return kExitUsage;
      }
      if (sub == "init" && a.pos.size() == 1) {
        ExtensionInit init;
        init.meta.set(sf::meta::kId, one(a, "--id"));
        init.meta.set(sf::meta::kVersion, one(a, "--version"));
        if (a.opt.count("--name")) init.meta.set(sf::meta::kName, one(a, "--name"));
        if (a.opt.count("--vendor")) init.meta.set(sf::meta::kVendor, one(a, "--vendor"));
        if (a.opt.count("--description")) init.meta.set(sf::meta::kDescription, one(a, "--description"));
        sf::ExtensionTarget t;
        t.app = one(a, "--target-app", sf::kAppId);
        const std::string mn = one(a, "--abi-min", abiText(SQ_ABI_MAKE(SQ_ABI_GET_MAJOR(SQ_PLATFORM_ABI), 0)));
        const std::string mx = one(a, "--abi-max", std::to_string(SQ_ABI_GET_MAJOR(SQ_PLATFORM_ABI)) + ".*");
        if (!sf::parseAbi(mn, t.abiMin) || !sf::parseAbi(mx, t.abiMax)) {
          errOut += "sq-pack: bad --abi-min/--abi-max\n";
          return kExitUsage;
        }
        t.os = one(a, "--os", "*");
        t.arch = one(a, "--arch", "*");
        init.install.targets.push_back(t);
        Status s = extensionInit(a.pos[0], init);
        if (!s.ok()) return fail(errOut, s);
        out += "initialised extension " + one(a, "--id") + " in " + a.pos[0] + "\n";
        return kExitOk;
      }
      if (sub == "install" && a.pos.size() == 2) {
        std::string installed;
        Status s = sf::extensionInstall(a.pos[0], a.pos[1], host, &installed);
        if (!s.ok()) return fail(errOut, s);
        out += "installed " + installed + "\n";
        return kExitOk;
      }
      if (sub == "uninstall" && a.pos.size() == 2) {
        Status s = sf::extensionUninstall(a.pos[0], a.pos[1]);
        if (!s.ok()) return fail(errOut, s);
        out += "uninstalled " + a.pos[1] + "\n";
        return kExitOk;
      }
      if (sub == "scan" && a.pos.size() == 1) {
        sf::ScanResult sr;
        Status s = sf::extensionScan(a.pos[0], host, sr);
        if (!s.ok()) return fail(errOut, s);
        for (const auto& x : sr.accepted) out += "accepted " + x.id() + " " + x.meta.get(sf::meta::kVersion) + "\n";
        for (const auto& [dir, why] : sr.rejected) out += "rejected " + dir + ": " + why.message + "\n";
        return sr.rejected.empty() ? kExitOk : exitFor(sr.rejected.front().second);
      }
      errOut += "sq-pack: bad ext command\n" + usage();
      return kExitUsage;
    }
    errOut += "sq-pack: unknown command '" + cmd + "'\n" + usage();
    return kExitUsage;
  } catch (const std::bad_alloc&) {
    errOut += "sq-pack: out of memory\n";
    return kExitInvalid;
  } catch (const std::exception& ex) {
    errOut += std::string("sq-pack: ") + ex.what() + "\n";
    return kExitInvalid;
  }
}

}  // namespace sq::packager
