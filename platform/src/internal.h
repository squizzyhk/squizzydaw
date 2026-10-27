// sqplatform internal helpers (not installed, not part of the ABI).
#pragma once

#include <cstdio>
#include <cstring>
#include <charconv>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

#include "sq/platform.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace sqp {

// Copies s into a platform-owned, NUL-terminated buffer.
inline sq_result toBuffer(std::string_view s, sq_buffer* out) {
  if (!out) return SQ_E_INVALIDARG;
  out->data = nullptr;
  out->size = 0;
  char* p = static_cast<char*>(sq_alloc(s.size() + 1));
  if (!p) return SQ_E_OUTOFMEMORY;
  if (!s.empty()) std::memcpy(p, s.data(), s.size());
  p[s.size()] = '\0';
  out->data = p;
  out->size = s.size();
  return SQ_OK;
}

inline void copyErr(char* dst, size_t cap, std::string_view msg) {
  if (!dst || cap == 0) return;
  size_t n = msg.size() < cap - 1 ? msg.size() : cap - 1;
  std::memcpy(dst, msg.data(), n);
  dst[n] = '\0';
}

#ifdef _WIN32
inline std::wstring widen(std::string_view s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
inline std::string narrow(std::wstring_view w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
inline std::string lastErrorString(DWORD code = GetLastError()) {
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                 reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::string s = msg ? narrow(msg) : std::string("error");
  if (msg) LocalFree(msg);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s + " (" + std::to_string(code) + ")";
}
inline sq_result fromWinError(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return SQ_E_NOTFOUND;
    case ERROR_ACCESS_DENIED:
      return SQ_E_ACCESS;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
      return SQ_E_EXISTS;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return SQ_E_OUTOFMEMORY;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
      return SQ_E_CLOSED;
    default:
      return SQ_E_IO;
  }
}
#endif

// Locale-independent strict double parse of [b, e). Apple libc++ lacks floating from_chars.
inline bool parseDouble(const char* b, const char* e, double& out) {
#if defined(__APPLE__)
  std::istringstream ss(std::string(b, e));
  ss.imbue(std::locale::classic());
  ss >> out;
  return !ss.fail() && ss.peek() == std::char_traits<char>::eof();
#else
  auto r = std::from_chars(b, e, out);
  return r.ec == std::errc() && r.ptr == e;
#endif
}

// Names for IPC objects: [A-Za-z0-9._-], 1..100 chars.
inline bool validIpcName(const char* name) {
  if (!name || !*name) return false;
  size_t n = 0;
  for (const char* p = name; *p; ++p, ++n) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok || n >= 100) return false;
  }
  return true;
}

}  // namespace sqp
