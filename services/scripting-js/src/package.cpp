// scripting-js: script packages (directory, ZIP, or the binary .package container of formats/core)
// and their package.xml manifest.
//
//   <ScriptPackage id="squizzy.trackedit" name="Track Edit" version="1.0.0" abi="1.0">
//     <ClassFactory>
//       <ScriptClass classID="{GUID}" category="Command" name="Track Edit: Rename"
//                    sourceFile="rename.js" functionName="createRenameCommand">
//         <Command category="Track" name="Rename With Pattern" title="Rename Tracks..."/>
//       </ScriptClass>
//     </ClassFactory>
//   </ScriptPackage>
//
// A bare <ClassFactory> root is accepted too (package id = directory/file name). `abi` gates the
// package on the host platform ABI: same major, minor <= host minor.
#include <cstring>
#include <map>

#include "engine.hpp"
#include "miniz.h"
#include "sq/platform.hpp"
#if SQJS_HAVE_FORMATS_CORE
#include "sq/formats/core/package.hpp"
#endif

namespace sqjs {

namespace {

class DirSource final : public PackageSource {
 public:
  explicit DirSource(std::string root) : root_(std::move(root)) {}
  bool read(const std::string& rel, std::string& out) const override {
    auto data = sq::readFile(sq::joinPath(root_, rel));
    if (!data) return false;
    out = std::move(*data);
    return true;
  }
  std::string describe() const override { return root_; }

 private:
  std::string root_;
};

class ZipSource final : public PackageSource {
 public:
  ZipSource(std::string path, std::map<std::string, std::string> files) : path_(std::move(path)), files_(std::move(files)) {}
  bool read(const std::string& rel, std::string& out) const override {
    auto it = files_.find(rel);
    if (it == files_.end()) return false;
    out = it->second;
    return true;
  }
  std::string describe() const override { return path_; }

 private:
  std::string path_;
  std::map<std::string, std::string> files_;
};

std::unique_ptr<PackageSource> makeMapSource(const std::string& path, std::map<std::string, std::string> files);

std::unique_ptr<PackageSource> openZip(const std::string& path, const std::string* data, std::string* err) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_mem(&zip, data->data(), data->size(), 0)) {
    if (err) *err = "not a valid ZIP archive";
    return nullptr;
  }
  std::map<std::string, std::string> files;
  bool ok = true;
  const mz_uint n = mz_zip_reader_get_num_files(&zip);
  uint64_t total = 0;
  for (mz_uint i = 0; i < n && ok; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      ok = false;
      break;
    }
    if (st.m_is_directory) continue;
    total += st.m_uncomp_size;
    if (st.m_uncomp_size > (64u << 20) || total > (256u << 20)) {
      if (err) *err = "archive too large";
      ok = false;
      break;
    }
    size_t size = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
    if (!p) {
      if (err) *err = std::string("cannot extract ") + st.m_filename;
      ok = false;
      break;
    }
    files[st.m_filename] = std::string(static_cast<const char*>(p), size);
    mz_free(p);
  }
  mz_zip_reader_end(&zip);
  if (!ok) return nullptr;
  return makeMapSource(path, std::move(files));
}

// Tolerates archives/bundles that wrap everything in one top-level folder.
std::unique_ptr<PackageSource> makeMapSource(const std::string& path, std::map<std::string, std::string> files) {
  if (!files.count("package.xml")) {
    std::string prefix;
    for (auto& [name, body] : files) {
      size_t slash = name.find('/');
      if (slash != std::string::npos && name.compare(slash + 1, std::string::npos, "package.xml") == 0) {
        prefix = name.substr(0, slash + 1);
        break;
      }
    }
    if (!prefix.empty()) {
      std::map<std::string, std::string> stripped;
      for (auto& [name, body] : files)
        if (name.rfind(prefix, 0) == 0) stripped[name.substr(prefix.size())] = std::move(body);
      files.swap(stripped);
    }
  }
  return std::make_unique<ZipSource>(path, std::move(files));
}

// Binary .package container (formats/core: header + zlib payload). A Bundle payload is the package
// file tree; an Xml payload is a manifest-only package (its package.xml). JavaScript-only payloads
// carry no manifest, and skin packages are not script packages: both are refused.
std::unique_ptr<PackageSource> openBinaryPackage(const std::string& path, const std::string& bytes, std::string* err) {
#if SQJS_HAVE_FORMATS_CORE
  namespace sf = sq::formats;
  sf::ScriptPackage pkg;
  sf::Status st = sf::packageRead(bytes, pkg);
  if (!st.ok()) {
    if (err) *err = st.message;
    return nullptr;
  }
  if (pkg.kind == sf::PackageKind::Skin) {
    if (err) *err = "binary .package is a skin package, not a script package";
    return nullptr;
  }
  std::map<std::string, std::string> files;
  switch (pkg.content) {
    case sf::PackageContent::Bundle:
      for (auto& f : pkg.files) files[f.name] = std::move(f.data);
      break;
    case sf::PackageContent::Xml:
      files["package.xml"] = std::move(pkg.text);
      break;
    default:
      if (err) *err = "binary .package holds a bare script without a package.xml manifest (pack it as a bundle)";
      return nullptr;
  }
  return makeMapSource(path, std::move(files));
#else
  (void)path;
  (void)bytes;
  if (err) *err = "binary .package containers are not supported by this build (formats/core not built)";
  return nullptr;
#endif
}

std::string baseName(const std::string& path) {
  std::string p = path;
  while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
  size_t s = p.find_last_of("/\\");
  std::string b = s == std::string::npos ? p : p.substr(s + 1);
  size_t dot = b.rfind('.');
  if (dot != std::string::npos && dot > 0 && sq_fs_is_file(path.c_str())) b = b.substr(0, dot);
  return b;
}

bool validPackageId(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;
  // Must start with a letter or digit: no "." / ".." / "-x" ids (they end up in package:// URLs).
  const char c0 = id[0];
  if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || (c0 >= '0' && c0 <= '9'))) return false;
  for (char c : id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool parseAbi(const std::string& s, uint32_t* major, uint32_t* minor) {
  unsigned a = 0, b = 0;
  char tail = 0;
  int n = std::sscanf(s.c_str(), "%u.%u%c", &a, &b, &tail);
  if (n != 2) return false;
  *major = a;
  *minor = b;
  return true;
}

}  // namespace

std::unique_ptr<PackageSource> PackageSource::open(const std::string& path, std::string* err) {
  if (sq_fs_is_dir(path.c_str())) {
    if (!sq_fs_is_file(sq::joinPath(path, "package.xml").c_str())) {
      if (err) *err = "directory has no package.xml";
      return nullptr;
    }
    return std::make_unique<DirSource>(path);
  }
  if (!sq_fs_is_file(path.c_str())) {
    if (err) *err = "no such file or directory";
    return nullptr;
  }
  auto data = sq::readFile(path);
  if (!data) {
    if (err) *err = "cannot read file";
    return nullptr;
  }
  static const unsigned char kBinaryMagic[8] = {0x89, 'S', 'Q', 'P', 'K', 0x0D, 0x0A, 0x1A};
  if (data->size() >= 8 && std::memcmp(data->data(), kBinaryMagic, 8) == 0) return openBinaryPackage(path, *data, err);
  if (data->size() >= 4 && std::memcmp(data->data(), "PK\x03\x04", 4) == 0) return openZip(path, &*data, err);
  if (err) *err = "unsupported package container (expected a directory with package.xml, a ZIP archive or a binary .package)";
  return nullptr;
}

bool PackageSource::safeRelative(const std::string& p) {
  if (p.empty() || p.size() > 512 || p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
  size_t start = 0;
  while (start <= p.size()) {
    size_t slash = p.find('/', start);
    std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

// ============================================================================================
Package::Package(Engine* e, std::unique_ptr<PackageSource> src) : engine(e), src_(std::move(src)) {}

Package::~Package() {
  for (auto& c : classes_)
    if (c.factory) c.factory->detach();
}

sq_result Package::parseManifest(uint32_t hostAbi, std::string* err) {
  std::string xml;
  if (!src_->read("package.xml", xml)) {
    *err = "package.xml missing";
    return SQ_E_NOTFOUND;
  }
  std::string perr;
  sq::XmlDocPtr doc = sq::xmlParse(xml, &perr);
  if (!doc) {
    *err = "package.xml: " + perr;
    return SQ_E_PARSE;
  }
  sq::XmlNode root = sq::xmlRoot(doc);
  sq::XmlNode factory;
  if (root.name() == "ScriptPackage") {
    id_ = root.attr("id");
    name_ = root.attr("name");
    version_ = root.attr("version", "0.0.0");
    std::string abi = root.attr("abi");
    if (!abi.empty()) {
      uint32_t maj = 0, min = 0;
      if (!parseAbi(abi, &maj, &min)) {
        *err = "package.xml: bad abi attribute '" + abi + "'";
        return SQ_E_PARSE;
      }
      if (maj != SQ_ABI_GET_MAJOR(hostAbi) || min > SQ_ABI_GET_MINOR(hostAbi)) {
        *err = "package requires platform ABI " + abi + ", host has " + std::to_string(SQ_ABI_GET_MAJOR(hostAbi)) + "." +
               std::to_string(SQ_ABI_GET_MINOR(hostAbi));
        return SQ_E_ABI_MISMATCH;
      }
    }
    factory = root.child("ClassFactory");
  } else if (root.name() == "ClassFactory") {
    factory = root;
  } else {
    *err = "package.xml: root must be <ScriptPackage> or <ClassFactory>";
    return SQ_E_PARSE;
  }
  if (id_.empty()) id_ = baseName(src_->describe());
  if (name_.empty()) name_ = id_;
  if (!validPackageId(id_)) {
    *err = "package.xml: invalid package id '" + id_ + "' (allowed: letters, digits, . _ -)";
    return SQ_E_PARSE;
  }
  if (!factory) {
    *err = "package.xml: no <ClassFactory>";
    return SQ_E_PARSE;
  }
  for (sq::XmlNode sc : factory.children("ScriptClass")) {
    ClassDef c;
    const std::string idText = sc.attr("classID");
    if (!parseAnyGuid(idText, &c.id) || sq_guid_is_null(&c.id)) {
      *err = "package.xml: ScriptClass with invalid classID '" + idText + "'";
      return SQ_E_PARSE;
    }
    for (const ClassDef& o : classes_) {
      if (o.id == c.id) {
        *err = "package.xml: duplicate classID " + idText;
        return SQ_E_PARSE;
      }
    }
    c.category = sc.attr("category", sq::scripting::kScriptClassCategory);
    c.sourceFile = sc.attr("sourceFile");
    c.functionName = sc.attr("functionName");
    c.name = sc.attr("name", c.functionName);
    if (c.sourceFile.empty() || c.functionName.empty() || !PackageSource::safeRelative(c.sourceFile)) {
      *err = "package.xml: ScriptClass " + idText + " needs sourceFile and functionName";
      return SQ_E_PARSE;
    }
    // The source must be in the package: otherwise the class would be registered but could never
    // be instantiated (e.g. a manifest-only binary .package with an Xml payload).
    std::string body;
    if (!src_->read(c.sourceFile, body)) {
      *err = "package.xml: ScriptClass " + idText + ": sourceFile '" + c.sourceFile + "' is not in the package";
      return SQ_E_PARSE;
    }
    if (c.category.empty()) c.category = sq::scripting::kScriptClassCategory;
    for (sq::XmlNode cmd : sc.children("Command")) {
      CommandDef d{cmd.attr("category"), cmd.attr("name"), cmd.attr("title")};
      if (d.category.empty() || d.name.empty()) continue;
      if (d.title.empty()) d.title = d.name;
      c.commands.push_back(std::move(d));
    }
    classes_.push_back(std::move(c));
  }
  return SQ_OK;
}

sq_result SQ_CALL Package::getId(char* buf, uint32_t bufSize, uint32_t* needed) {
  const uint32_t need = static_cast<uint32_t>(id_.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize < need) {
    if (buf && bufSize) {
      std::memcpy(buf, id_.data(), bufSize - 1);
      buf[bufSize - 1] = '\0';
    }
    return SQ_E_BUFFER_TOO_SMALL;
  }
  std::memcpy(buf, id_.c_str(), need);
  return SQ_OK;
}

uint32_t SQ_CALL Package::getClassCount() { return static_cast<uint32_t>(classes_.size()); }

sq_result SQ_CALL Package::getClassInfo(uint32_t index, sq_class_info* out) {
  if (!out || out->struct_size < sizeof(sq_class_info)) return SQ_E_INVALIDARG;
  if (index >= classes_.size()) return SQ_E_NOTFOUND;
  const ClassDef& c = classes_[index];
  out->class_id = c.id;
  out->category = c.category.c_str();
  out->name = c.name.c_str();
  out->flags = 0;
  out->module_fourcc = 0;
  return SQ_OK;
}

sq_result SQ_CALL Package::unload() {
  Engine* e = engine;
  if (!e) return SQ_S_FALSE;
  return e->unloadPackage(this, false);
}

}  // namespace sqjs
