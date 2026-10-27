// Squizzy Studio formats - snapshot .package: ZIP with snapshots.json + image files
// (plugin thumbnails). Schema: SCHEMAS.md "Snapshot .package".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

inline constexpr int kSnapshotsVersion = 1;
inline constexpr const char* kSnapshotsManifest = "snapshots.json";

struct Snapshot {
  std::string id;       // what the image depicts, e.g. a plugin id; unique in the package
  std::string file;     // member path inside the ZIP (e.g. "images/foo.png")
  uint32_t width = 0;   // pixels; filled from the PNG header when 0 on write
  uint32_t height = 0;
  double scale = 1.0;   // 1 = normal, 2 = @2x
  std::string data;     // image bytes (PNG or WebP)
};

struct SnapshotPackage {
  KeyValues info;  // free-form string info ("name", "vendor", ...)
  std::vector<Snapshot> snapshots;
};

Status snapshotPackageWrite(const SnapshotPackage& pkg, std::string& zipOut);
Status snapshotPackageRead(std::string_view zipBytes, SnapshotPackage& out);

// PNG IHDR dimensions; false if not a PNG.
bool pngDimensions(std::string_view data, uint32_t& w, uint32_t& h);

}  // namespace sq::formats
