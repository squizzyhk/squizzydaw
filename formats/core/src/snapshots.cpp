#include "sq/formats/core/snapshots.hpp"

#include <cstring>
#include <set>

#include "sq/formats/core/jsonutil.hpp"
#include "sq/formats/core/zip.hpp"

namespace sq::formats {

bool pngDimensions(std::string_view d, uint32_t& w, uint32_t& h) {
  static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (d.size() < 24 || std::memcmp(d.data(), sig, 8) != 0 || d.substr(12, 4) != "IHDR") return false;
  auto p = reinterpret_cast<const unsigned char*>(d.data());
  auto be = [&](size_t o) {
    return (uint32_t(p[o]) << 24) | (uint32_t(p[o + 1]) << 16) | (uint32_t(p[o + 2]) << 8) | uint32_t(p[o + 3]);
  };
  w = be(16);
  h = be(20);
  return true;
}

Status snapshotPackageWrite(const SnapshotPackage& pkg, std::string& zipOut) {
  ZipArchive zip;
  Json root = Json::object();
  root.set("format", Json::string("squizzy.snapshots"));
  root.set("version", Json::number(kSnapshotsVersion));
  Json info = Json::object();
  for (const auto& [k, v] : pkg.info) info.set(k.c_str(), Json::string(v));
  root.set("info", std::move(info));
  Json list = Json::array();
  std::set<std::string> ids, files;
  for (const auto& s : pkg.snapshots) {
    if (s.id.empty() || !ids.insert(s.id).second) return Status::error(SQ_E_INVALIDARG, "snapshots: empty or duplicate id '" + s.id + "'");
    if (!isSafeRelativePath(s.file) || s.file == kSnapshotsManifest || !files.insert(s.file).second)
      return Status::error(SQ_E_INVALIDARG, "snapshots: invalid or duplicate file '" + s.file + "'");
    uint32_t w = s.width, h = s.height;
    if (w == 0 || h == 0) pngDimensions(s.data, w, h);
    Json e = Json::object();
    e.set("id", Json::string(s.id));
    e.set("file", Json::string(s.file));
    e.set("width", Json::number(w));
    e.set("height", Json::number(h));
    e.set("scale", Json::number(s.scale));
    list.append(std::move(e));
    zip.add(s.file, s.data);
  }
  root.set("snapshots", std::move(list));
  zip.entries.insert(zip.entries.begin(), FileEntry{kSnapshotsManifest, root.write(true)});
  // Images are already compressed: store them.
  return zipWrite(zip, zipOut, 1);
}

Status snapshotPackageRead(std::string_view zipBytes, SnapshotPackage& out) {
  out = SnapshotPackage{};
  ZipArchive zip;
  SQF_TRY(zipRead(zipBytes, zip, ZipLimits::metadata()));
  const std::string* manifest = zip.find(kSnapshotsManifest);
  if (!manifest) return Status::error(SQ_E_PARSE, "snapshots: missing snapshots.json");
  Json root;
  SQF_TRY(Json::parse(*manifest, root));
  if (root.get("format").str() != "squizzy.snapshots") return Status::error(SQ_E_PARSE, "snapshots: wrong format tag");
  int64_t ver = root.get("version").integer();
  if (ver < 1) return Status::error(SQ_E_PARSE, "snapshots: invalid version");
  if (ver > kSnapshotsVersion) return Status::error(SQ_E_UNSUPPORTED, "snapshots: version " + std::to_string(ver) + " is newer than supported");
  Json info = root.get("info");
  for (size_t i = 0; i < info.size(); ++i) out.info.emplace_back(info.keyAt(i), info.at(i).str());
  Json list = root.get("snapshots");
  if (!list.isArray()) return Status::error(SQ_E_PARSE, "snapshots: 'snapshots' must be an array");
  std::set<std::string> ids;
  for (size_t i = 0; i < list.size(); ++i) {
    Json e = list.at(i);
    Snapshot s;
    s.id = e.get("id").str();
    s.file = e.get("file").str();
    s.width = static_cast<uint32_t>(e.get("width").integer());
    s.height = static_cast<uint32_t>(e.get("height").integer());
    s.scale = e.get("scale").isNumber() ? e.get("scale").number() : 1.0;
    if (s.id.empty() || !ids.insert(s.id).second) return Status::error(SQ_E_PARSE, "snapshots: empty or duplicate id");
    const std::string* data = zip.find(s.file);
    if (!data) return Status::error(SQ_E_PARSE, "snapshots: '" + s.file + "' listed but missing");
    s.data = *data;
    out.snapshots.push_back(std::move(s));
  }
  return {};
}

}  // namespace sq::formats
