// Squizzy Studio formats - script/skin .package bundles: own binary header + zlib payload.
// Layout (little endian, see SCHEMAS.md "Script/skin .package"):
//   0  u8[8]  magic 89 'S' 'Q' 'P' 'K' 0D 0A 1A
//   8  u8     container version (1)
//   9  u8     kind     (PackageKind)
//   10 u8     content  (PackageContent)
//   11 u8     flags    (0; reserved)
//   12 u32    uncompressed payload size
//   16 u32    compressed payload size
//   20 u32    CRC-32 of the uncompressed payload
//   24 ...    zlib (RFC 1950) stream
// Payload: an XML document (content Xml), a JavaScript source (JavaScript), or a file bundle
// (Bundle: "SQB1" u32 count, then per file u16 nameLen, name, u32 dataLen, data).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

inline constexpr uint8_t kPackageVersion = 1;
inline constexpr size_t kPackageHeaderSize = 24;
inline constexpr uint32_t kPackageMaxPayload = 256u * 1024 * 1024;

enum class PackageKind : uint8_t { Generic = 0, Script = 1, Skin = 2 };
enum class PackageContent : uint8_t { Xml = 1, JavaScript = 2, Bundle = 3 };

struct ScriptPackage {
  PackageKind kind = PackageKind::Script;
  PackageContent content = PackageContent::Xml;
  std::string text;              // Xml / JavaScript payload (UTF-8)
  std::vector<FileEntry> files;  // Bundle payload (names must be safe relative paths)
};

Status packageWrite(const ScriptPackage& pkg, std::string& out);
Status packageRead(std::string_view bytes, ScriptPackage& out);
bool packageProbe(std::string_view head);  // magic check (first 8 bytes)

const char* packageKindName(PackageKind k);
const char* packageContentName(PackageContent c);

}  // namespace sq::formats
