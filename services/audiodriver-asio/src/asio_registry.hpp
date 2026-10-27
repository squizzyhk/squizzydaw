// Squizzy Studio - ASIO driver service: installed-driver enumeration (SDK-independent).
//
// Installed ASIO drivers are listed under HKEY_LOCAL_MACHINE\SOFTWARE\ASIO: one subkey per
// driver with a string value "CLSID" (the COM class of the driver) and an optional string value
// "Description" (display name; the subkey name is used when it is missing or empty).
//
// The parser works on an abstract RegistryView so tests can feed a fake registry. Rules:
//   - entries without a CLSID value, or whose CLSID is not a well-formed GUID, are skipped (with a
//     reason in EnumerationResult::skipped);
//   - duplicate CLSIDs (two keys pointing at the same driver) keep the first key in key order;
//   - names are trimmed; device ids are "asio:" + the braced, upper-case CLSID (stable across runs
//     and independent of the display name);
//   - the result is sorted by display name (ASCII case-insensitive), ties by id;
//   - at most kMaxDrivers entries are considered.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace sq::asio {

inline constexpr size_t kMaxDrivers = 256;
inline constexpr const char* kDeviceIdPrefix = "asio:";

class RegistryView {
 public:
  virtual ~RegistryView() = default;
  // Names of the direct subkeys of the ASIO key (UTF-8). False if the ASIO key does not exist.
  virtual bool listSubkeys(std::vector<std::string>& out) = 0;
  // String value `value` of subkey `subkey` (UTF-8). False if absent or not a string.
  virtual bool readString(const std::string& subkey, const char* value, std::string& out) = 0;
};

struct DriverEntry {
  std::string id;       // "asio:{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
  std::string name;     // display name
  std::string clsid;    // "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" (upper case)
  std::string keyName;  // registry subkey name
};

struct EnumerationResult {
  bool keyPresent = false;             // the ASIO key exists at all
  std::vector<DriverEntry> drivers;
  std::vector<std::string> skipped;    // "<key>: <reason>"
};

// Accepts "{8-4-4-4-12}" or the same without braces, any hex case, surrounding whitespace.
// Writes the braced upper-case form. False on anything else.
bool normalizeClsid(const std::string& in, std::string& out);

EnumerationResult enumerateDrivers(RegistryView& view);

// The real HKLM\SOFTWARE\ASIO (native registry view of this process). On non-Windows builds a
// view that reports no key.
std::unique_ptr<RegistryView> openSystemRegistry();

}  // namespace sq::asio
