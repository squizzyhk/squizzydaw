// Squizzy Studio formats - shared types for every content-format reader/writer.
// In-process C++ (no DLL boundary): link sq::formats_core. Not thread-safe per object; never use
// on the audio thread (everything here allocates and may do file I/O).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sq/types.h"

namespace sq::formats {

// Result of an operation: an sq_result code plus a human-readable message on failure.
struct Status {
  sq_result code = SQ_OK;
  std::string message;

  bool ok() const { return code >= 0; }
  explicit operator bool() const { return ok(); }
  static Status success() { return {}; }
  static Status error(sq_result c, std::string msg) { return Status{c, std::move(msg)}; }
};

#define SQF_TRY(expr)                    \
  do {                                   \
    ::sq::formats::Status sqf_s_ = (expr); \
    if (!sqf_s_.ok()) return sqf_s_;     \
  } while (0)

// Ordered key/value list (insertion order is preserved, keys unique via setValue).
using KeyValues = std::vector<std::pair<std::string, std::string>>;
const std::string* findValue(const KeyValues& kv, std::string_view key);
void setValue(KeyValues& kv, std::string_view key, std::string value);

// A named binary blob: archive member, bundle file, embedded asset. `data` is binary-safe.
struct FileEntry {
  std::string name;  // relative path, '/' separated
  std::string data;
};

// Relative archive path safety: non-empty, '/' separated, no leading '/', no drive letter,
// no '\\', no "." / ".." / empty segments, no control characters. Applied to every member name.
bool isSafeRelativePath(std::string_view p);
// Identifier safety for ids that become file/dir names: [A-Za-z0-9._-]+, not "." or "..".
bool isSafeIdentifier(std::string_view s);

// Lower-case extension without the dot ("" if none): "a/b.Preset" -> "preset".
std::string fileExtension(std::string_view path);
std::string fileBaseName(std::string_view path);  // last path segment
std::string fileStem(std::string_view path);      // base name without the last extension

// File helpers over the platform layer (UTF-8 paths).
Status readFileBytes(const std::string& path, std::string& out);
Status writeFileBytes(const std::string& path, std::string_view data);  // atomic, creates parent dirs
// Every regular file under dir, as sorted '/' separated paths relative to dir.
Status listFilesRecursive(const std::string& dir, std::vector<std::string>& out);
// Recursive copy of a directory tree (creates dst).
Status copyTree(const std::string& src, const std::string& dst);

// Little-endian helpers for binary layouts.
void putU16(std::string& out, uint16_t v);
void putU32(std::string& out, uint32_t v);
uint16_t getU16(const unsigned char* p);
uint32_t getU32(const unsigned char* p);

// "major.minor" <-> packed SQ_ABI_MAKE value. "1.*" parses as minor 0xFFFF.
bool parseAbi(std::string_view s, uint32_t& out);
std::string formatAbi(uint32_t abi);

// Number formatting that round-trips doubles (shortest representation).
std::string formatDouble(double v);
bool parseDouble(std::string_view s, double& out);
bool parseInt(std::string_view s, int64_t& out);

// "#RRGGBB" / "#RRGGBBAA" <-> 0xRRGGBBAA (alpha defaults to FF).
bool parseColor(std::string_view s, uint32_t& rgba);
std::string formatColor(uint32_t rgba, bool withAlpha = true);

}  // namespace sq::formats
