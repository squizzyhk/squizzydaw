#include "sq/formats/core/package.hpp"

#include <cstring>
#include <set>

#include "sq/formats/core/xmlutil.hpp"
#include "sq/formats/core/zip.hpp"
#include "sq/platform.h"

namespace sq::formats {

namespace {
constexpr unsigned char kMagic[8] = {0x89, 'S', 'Q', 'P', 'K', 0x0D, 0x0A, 0x1A};
constexpr char kBundleMagic[4] = {'S', 'Q', 'B', '1'};

Status encodeBundle(const std::vector<FileEntry>& files, std::string& out) {
  out.assign(kBundleMagic, 4);
  putU32(out, static_cast<uint32_t>(files.size()));
  std::set<std::string> seen;
  for (const auto& f : files) {
    if (!isSafeRelativePath(f.name) || f.name.size() > 0xFFFF)
      return Status::error(SQ_E_INVALIDARG, "package: unsafe bundle file name '" + f.name + "'");
    if (!seen.insert(f.name).second) return Status::error(SQ_E_INVALIDARG, "package: duplicate bundle file '" + f.name + "'");
    if (f.data.size() > kPackageMaxPayload) return Status::error(SQ_E_INVALIDARG, "package: bundle file too large");
    putU16(out, static_cast<uint16_t>(f.name.size()));
    out += f.name;
    putU32(out, static_cast<uint32_t>(f.data.size()));
    out += f.data;
  }
  return {};
}

Status decodeBundle(std::string_view in, std::vector<FileEntry>& files) {
  files.clear();
  auto p = reinterpret_cast<const unsigned char*>(in.data());
  size_t n = in.size(), pos = 8;
  if (n < 8 || std::memcmp(in.data(), kBundleMagic, 4) != 0) return Status::error(SQ_E_PARSE, "package: bad bundle header");
  uint32_t count = getU32(p + 4);
  std::set<std::string> seen;
  for (uint32_t i = 0; i < count; ++i) {
    if (n - pos < 2) return Status::error(SQ_E_PARSE, "package: truncated bundle");
    uint16_t nl = getU16(p + pos);
    pos += 2;
    if (n - pos < nl + 4u) return Status::error(SQ_E_PARSE, "package: truncated bundle");
    std::string name(in.substr(pos, nl));
    pos += nl;
    uint32_t dl = getU32(p + pos);
    pos += 4;
    if (n - pos < dl) return Status::error(SQ_E_PARSE, "package: truncated bundle");
    if (!isSafeRelativePath(name)) return Status::error(SQ_E_PARSE, "package: unsafe bundle file name '" + name + "'");
    if (!seen.insert(name).second) return Status::error(SQ_E_PARSE, "package: duplicate bundle file '" + name + "'");
    files.push_back(FileEntry{std::move(name), std::string(in.substr(pos, dl))});
    pos += dl;
  }
  if (pos != n) return Status::error(SQ_E_PARSE, "package: trailing bytes in bundle");
  return {};
}
}  // namespace

bool packageProbe(std::string_view head) { return head.size() >= 8 && std::memcmp(head.data(), kMagic, 8) == 0; }

const char* packageKindName(PackageKind k) {
  switch (k) {
    case PackageKind::Script: return "script";
    case PackageKind::Skin: return "skin";
    default: return "generic";
  }
}

const char* packageContentName(PackageContent c) {
  switch (c) {
    case PackageContent::Xml: return "xml";
    case PackageContent::JavaScript: return "javascript";
    case PackageContent::Bundle: return "bundle";
  }
  return "?";
}

Status packageWrite(const ScriptPackage& pkg, std::string& out) {
  std::string payload;
  switch (pkg.content) {
    case PackageContent::Xml: {
      std::string err;
      if (!xmlParse(pkg.text, &err)) return Status::error(SQ_E_INVALIDARG, "package: payload is not well-formed XML: " + err);
      payload = pkg.text;
      break;
    }
    case PackageContent::JavaScript:
      if (!sq_utf8_validate(pkg.text.data(), pkg.text.size()))
        return Status::error(SQ_E_INVALIDARG, "package: script is not valid UTF-8");
      payload = pkg.text;
      break;
    case PackageContent::Bundle:
      SQF_TRY(encodeBundle(pkg.files, payload));
      break;
    default:
      return Status::error(SQ_E_INVALIDARG, "package: unknown content type");
  }
  if (payload.size() > kPackageMaxPayload) return Status::error(SQ_E_INVALIDARG, "package: payload too large");
  std::string z;
  SQF_TRY(zlibCompress(payload, z));
  out.assign(reinterpret_cast<const char*>(kMagic), 8);
  out.push_back(static_cast<char>(kPackageVersion));
  out.push_back(static_cast<char>(pkg.kind));
  out.push_back(static_cast<char>(pkg.content));
  out.push_back(0);
  putU32(out, static_cast<uint32_t>(payload.size()));
  putU32(out, static_cast<uint32_t>(z.size()));
  putU32(out, sq_crc32(0, payload.data(), payload.size()));
  out += z;
  return {};
}

Status packageRead(std::string_view bytes, ScriptPackage& out) {
  out = ScriptPackage{};
  if (bytes.size() < kPackageHeaderSize || !packageProbe(bytes)) return Status::error(SQ_E_PARSE, "package: bad magic");
  auto p = reinterpret_cast<const unsigned char*>(bytes.data());
  if (p[8] == 0 || p[8] > kPackageVersion)
    return Status::error(SQ_E_UNSUPPORTED, "package: container version " + std::to_string(p[8]) + " not supported");
  if (p[9] > static_cast<uint8_t>(PackageKind::Skin)) return Status::error(SQ_E_PARSE, "package: unknown kind");
  if (p[10] < 1 || p[10] > 3) return Status::error(SQ_E_PARSE, "package: unknown content type");
  if (p[11] != 0) return Status::error(SQ_E_UNSUPPORTED, "package: unknown flags");
  uint32_t usize = getU32(p + 12), csize = getU32(p + 16), crc = getU32(p + 20);
  if (usize > kPackageMaxPayload) return Status::error(SQ_E_PARSE, "package: payload too large");
  if (csize != bytes.size() - kPackageHeaderSize) return Status::error(SQ_E_PARSE, "package: size mismatch (truncated?)");
  std::string payload;
  SQF_TRY(zlibDecompress(bytes.substr(kPackageHeaderSize), usize, payload));
  if (sq_crc32(0, payload.data(), payload.size()) != crc) return Status::error(SQ_E_PARSE, "package: CRC mismatch");
  out.kind = static_cast<PackageKind>(p[9]);
  out.content = static_cast<PackageContent>(p[10]);
  if (out.content == PackageContent::Bundle) return decodeBundle(payload, out.files);
  out.text = std::move(payload);
  return {};
}

}  // namespace sq::formats
