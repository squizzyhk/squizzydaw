// Squizzy Studio - ASIO driver service: installed-driver enumeration (see asio_registry.hpp).
#include "asio_registry.hpp"

#include <algorithm>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sq::asio {
namespace {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == '\0')) --e;
  return s.substr(b, e - b);
}

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

int compareNoCase(const std::string& a, const std::string& b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const char x = upper(a[i]), y = upper(b[i]);
    if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

}  // namespace

bool normalizeClsid(const std::string& in, std::string& out) {
  std::string s = trim(in);
  if (s.size() == 38 && s.front() == '{' && s.back() == '}') s = s.substr(1, 36);
  if (s.size() != 36) return false;
  std::string r = "{";
  for (size_t i = 0; i < 36; ++i) {
    const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
    if (dash) {
      if (s[i] != '-') return false;
    } else if (!isHex(s[i])) {
      return false;
    }
    r += upper(s[i]);
  }
  r += '}';
  out = std::move(r);
  return true;
}

EnumerationResult enumerateDrivers(RegistryView& view) {
  EnumerationResult res;
  std::vector<std::string> keys;
  res.keyPresent = view.listSubkeys(keys);
  if (!res.keyPresent) return res;
  if (keys.size() > kMaxDrivers) {
    res.skipped.push_back("(registry): more than " + std::to_string(kMaxDrivers) + " entries, the rest ignored");
    keys.resize(kMaxDrivers);
  }
  std::unordered_set<std::string> seen;
  for (const std::string& key : keys) {
    if (key.empty()) continue;
    std::string raw;
    if (!view.readString(key, "CLSID", raw)) {
      res.skipped.push_back(key + ": no CLSID value");
      continue;
    }
    DriverEntry e;
    if (!normalizeClsid(raw, e.clsid)) {
      res.skipped.push_back(key + ": malformed CLSID '" + trim(raw) + "'");
      continue;
    }
    if (!seen.insert(e.clsid).second) {
      res.skipped.push_back(key + ": duplicate CLSID " + e.clsid);
      continue;
    }
    std::string desc;
    if (view.readString(key, "Description", desc)) desc = trim(desc);
    e.keyName = key;
    e.name = desc.empty() ? trim(key) : desc;
    if (e.name.empty()) e.name = e.clsid;
    e.id = std::string(kDeviceIdPrefix) + e.clsid;
    res.drivers.push_back(std::move(e));
  }
  std::stable_sort(res.drivers.begin(), res.drivers.end(), [](const DriverEntry& a, const DriverEntry& b) {
    const int c = compareNoCase(a.name, b.name);
    return c != 0 ? c < 0 : a.id < b.id;
  });
  return res;
}

#ifdef _WIN32
namespace {

std::string toUtf8(const wchar_t* w, size_t len) {
  if (!w || len == 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring toWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

class Win32Registry final : public RegistryView {
 public:
  Win32Registry() {
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &key_) != ERROR_SUCCESS) key_ = nullptr;
  }
  ~Win32Registry() override {
    if (key_) RegCloseKey(key_);
  }
  Win32Registry(const Win32Registry&) = delete;
  Win32Registry& operator=(const Win32Registry&) = delete;

  bool listSubkeys(std::vector<std::string>& out) override {
    out.clear();
    if (!key_) return false;
    for (DWORD i = 0; i < 4096; ++i) {
      wchar_t name[256];
      DWORD len = 256;
      const LONG r = RegEnumKeyExW(key_, i, name, &len, nullptr, nullptr, nullptr, nullptr);
      if (r == ERROR_NO_MORE_ITEMS) break;
      if (r != ERROR_SUCCESS) continue;  // e.g. a name longer than 255 chars: skip it
      out.push_back(toUtf8(name, len));
    }
    return true;
  }

  bool readString(const std::string& subkey, const char* value, std::string& out) override {
    out.clear();
    if (!key_ || !value) return false;
    const std::wstring sk = toWide(subkey), v = toWide(value);
    DWORD bytes = 0;
    if (RegGetValueW(key_, sk.c_str(), v.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return false;
    if (bytes == 0 || bytes > 64 * 1024) return false;
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD got = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(key_, sk.c_str(), v.c_str(), RRF_RT_REG_SZ, nullptr, buf.data(), &got) != ERROR_SUCCESS) return false;
    size_t len = 0;
    while (len < buf.size() && buf[len] != L'\0') ++len;
    out = toUtf8(buf.data(), len);
    return true;
  }

 private:
  HKEY key_ = nullptr;
};

}  // namespace

std::unique_ptr<RegistryView> openSystemRegistry() { return std::make_unique<Win32Registry>(); }

#else

namespace {
class NoRegistry final : public RegistryView {
 public:
  bool listSubkeys(std::vector<std::string>& out) override {
    out.clear();
    return false;
  }
  bool readString(const std::string&, const char*, std::string& out) override {
    out.clear();
    return false;
  }
};
}  // namespace

std::unique_ptr<RegistryView> openSystemRegistry() { return std::make_unique<NoRegistry>(); }

#endif

}  // namespace sq::asio
