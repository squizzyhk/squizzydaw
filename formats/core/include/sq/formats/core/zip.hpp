// Squizzy Studio formats - ZIP containers (miniz) and raw zlib streams.
// Every ZIP-based format (.preset, .langpack, snapshot .package, DAWproject ...) goes through
// ZipArchive so member-name safety and size limits are enforced in one place.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

// Members are inflated in a streaming way: memory grows only as real decompressed bytes arrive
// (never pre-allocated from the declared size), every limit is enforced on the actual output,
// and a member that inflates beyond its declared size is rejected.
struct ZipLimits {
  uint32_t maxEntries = 65536;
  uint64_t maxEntrySize = 1024ull * 1024 * 1024;       // 1 GiB uncompressed per member
  uint64_t maxTotalSize = 4ull * 1024 * 1024 * 1024;   // 4 GiB uncompressed in total
  // Compression-bomb guard: a member may not expand more than maxRatio x its compressed size
  // once it is larger than ratioFloor bytes (0 = no ratio limit).
  uint32_t maxRatio = 0;
  uint64_t ratioFloor = 16ull * 1024 * 1024;

  // Limits for small metadata containers (presets, langpacks, snapshot packages, detection).
  static ZipLimits metadata() {
    ZipLimits l;
    l.maxEntries = 4096;
    l.maxEntrySize = 64ull * 1024 * 1024;
    l.maxTotalSize = 256ull * 1024 * 1024;
    l.maxRatio = 1100;  // just above deflate's theoretical maximum (~1032:1)
    l.ratioFloor = 1024 * 1024;
    return l;
  }
};

// In-memory archive: an ordered list of members (directory entries are not stored).
class ZipArchive {
 public:
  std::vector<FileEntry> entries;

  // Adds or replaces a member. Returns false (and does nothing) for an unsafe name.
  bool add(std::string_view name, std::string data);
  const std::string* find(std::string_view name) const;
  bool contains(std::string_view name) const { return find(name) != nullptr; }
  // Members whose name starts with prefix (in archive order).
  std::vector<const FileEntry*> withPrefix(std::string_view prefix) const;
};

// level: 0 (store) .. 10. Output is deterministic (fixed member timestamps).
Status zipWrite(const ZipArchive& zip, std::string& out, int level = 6);
// Rejects encrypted/unsupported members, unsafe or duplicate names, and anything over limits.
// Never throws: allocation failure is reported as SQ_E_OUTOFMEMORY.
Status zipRead(std::string_view bytes, ZipArchive& out, const ZipLimits& limits = {});
// Member names only (central directory; nothing is inflated). For format detection.
Status zipListNames(std::string_view bytes, std::vector<std::string>& names, uint32_t maxEntries = 65536);
Status zipWriteFile(const ZipArchive& zip, const std::string& path, int level = 6);
Status zipReadFile(const std::string& path, ZipArchive& out, const ZipLimits& limits = {});
// True if the bytes start with a ZIP local-file or end-of-central-directory signature.
bool zipProbe(std::string_view head);

// zlib (RFC 1950) single-shot helpers.
Status zlibCompress(std::string_view in, std::string& out, int level = 9);
// expectedSize must be the exact uncompressed size (stored by the caller's container).
Status zlibDecompress(std::string_view in, size_t expectedSize, std::string& out);

}  // namespace sq::formats
