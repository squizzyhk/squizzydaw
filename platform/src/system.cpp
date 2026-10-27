#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

#include "internal.h"

#ifdef _WIN32
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

struct sq_dylib {
#ifdef _WIN32
  HMODULE h = nullptr;
#else
  void* h = nullptr;
#endif
};

namespace {

std::string exePath() {
#ifdef _WIN32
  std::wstring buf(1024, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return std::string();
    if (n < buf.size()) {
      buf.resize(n);
      return sqp::narrow(buf);
    }
    buf.resize(buf.size() * 2);
  }
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0) return std::string();
  std::error_code ec;
  auto p = fs::canonical(buf, ec);
  return ec ? std::string(buf) : p.string();
#else
  std::error_code ec;
  auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.string();
#endif
}

std::string withAppFolder(const std::string& base) { return base.empty() ? base : base + "/SquizzyStudio"; }

std::string envOr(const char* name, const std::string& def) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : def;
}

#ifdef _WIN32
std::string knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::string r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) r = sqp::narrow(p);
  if (p) CoTaskMemFree(p);
  for (auto& c : r)
    if (c == '\\') c = '/';
  return r;
}
#endif

}  // namespace

extern "C" {

sq_result sq_dylib_open(const char* path, sq_dylib** out, char* err, size_t err_size) {
  if (!path || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
#ifdef _WIN32
  std::error_code ec;
  fs::path abs = fs::absolute(fs::path(sqp::widen(path)), ec);
  std::wstring w = (ec ? fs::path(sqp::widen(path)) : abs).make_preferred().wstring();
  HMODULE h = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) {
    DWORD e = GetLastError();
    sqp::copyErr(err, err_size, sqp::lastErrorString(e));
    return e == ERROR_MOD_NOT_FOUND || e == ERROR_FILE_NOT_FOUND ? SQ_E_NOTFOUND : SQ_E_FAIL;
  }
#else
  void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char* e = dlerror();
    sqp::copyErr(err, err_size, e ? e : "dlopen failed");
    return SQ_E_FAIL;
  }
#endif
  auto* lib = new (std::nothrow) sq_dylib();
  if (!lib) return SQ_E_OUTOFMEMORY;
  lib->h = h;
  *out = lib;
  return SQ_OK;
}

void* sq_dylib_symbol(sq_dylib* lib, const char* name) {
  if (!lib || !name) return nullptr;
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(lib->h, name));
#else
  return dlsym(lib->h, name);
#endif
}

void sq_dylib_close(sq_dylib* lib) {
  if (!lib) return;
#ifdef _WIN32
  if (lib->h) FreeLibrary(lib->h);
#else
  if (lib->h) dlclose(lib->h);
#endif
  delete lib;
}

sq_bool sq_dylib_is_loaded(const char* path) {
  if (!path) return SQ_FALSE;
#ifdef _WIN32
  std::error_code ec;
  fs::path abs = fs::absolute(fs::path(sqp::widen(path)), ec);
  std::wstring w = (ec ? fs::path(sqp::widen(path)) : abs).make_preferred().wstring();
  return GetModuleHandleW(w.c_str()) != nullptr ? SQ_TRUE : SQ_FALSE;
#else
#ifdef RTLD_NOLOAD
  void* h = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
  if (h) {
    dlclose(h);
    return SQ_TRUE;
  }
#endif
  return SQ_FALSE;
#endif
}

const char* sq_dylib_suffix(void) {
#ifdef _WIN32
  return ".dll";
#elif defined(__APPLE__)
  return ".so";  // CMake MODULE libraries use .so on macOS too
#else
  return ".so";
#endif
}

sq_result sq_exe_path(sq_buffer* out) {
  std::string p = exePath();
  if (p.empty()) return SQ_E_FAIL;
  for (auto& c : p)
    if (c == '\\') c = '/';
  return sqp::toBuffer(p, out);
}

sq_result sq_special_dir_get(sq_special_dir which, sq_buffer* out) {
  if (!out) return SQ_E_INVALIDARG;
  std::string r;
  switch (which) {
    case SQ_DIR_EXE: {
      std::string p = exePath();
      for (auto& c : p)
        if (c == '\\') c = '/';
      size_t s = p.find_last_of('/');
      r = s == std::string::npos ? std::string(".") : p.substr(0, s);
      break;
    }
#ifdef _WIN32
    case SQ_DIR_USER_CONFIG: r = withAppFolder(knownFolder(FOLDERID_RoamingAppData)); break;
    case SQ_DIR_USER_DATA: r = withAppFolder(knownFolder(FOLDERID_LocalAppData)); break;
    case SQ_DIR_HOME: r = knownFolder(FOLDERID_Profile); break;
    case SQ_DIR_DOCUMENTS: r = knownFolder(FOLDERID_Documents); break;
    case SQ_DIR_TEMP: {
      wchar_t buf[MAX_PATH + 2];
      DWORD n = GetTempPathW(MAX_PATH + 1, buf);
      r = sqp::narrow(std::wstring_view(buf, n));
      for (auto& c : r)
        if (c == '\\') c = '/';
      while (r.size() > 3 && r.back() == '/') r.pop_back();
      break;
    }
#else
    case SQ_DIR_HOME: r = envOr("HOME", "/tmp"); break;
#if defined(__APPLE__)
    case SQ_DIR_USER_CONFIG:
    case SQ_DIR_USER_DATA: r = withAppFolder(envOr("HOME", "/tmp") + "/Library/Application Support"); break;
#else
    case SQ_DIR_USER_CONFIG: r = withAppFolder(envOr("XDG_CONFIG_HOME", envOr("HOME", "/tmp") + "/.config")); break;
    case SQ_DIR_USER_DATA: r = withAppFolder(envOr("XDG_DATA_HOME", envOr("HOME", "/tmp") + "/.local/share")); break;
#endif
    case SQ_DIR_DOCUMENTS: r = envOr("HOME", "/tmp") + "/Documents"; break;
    case SQ_DIR_TEMP: r = envOr("TMPDIR", "/tmp"); break;
#endif
    default: return SQ_E_INVALIDARG;
  }
  if (r.empty()) return SQ_E_NOTFOUND;
  return sqp::toBuffer(r, out);
}

uint32_t sq_process_id(void) {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<uint32_t>(getpid());
#endif
}

sq_result sq_env_get(const char* name, sq_buffer* out) {
  if (!name || !out) return SQ_E_INVALIDARG;
#ifdef _WIN32
  std::wstring wn = sqp::widen(name);
  DWORD n = GetEnvironmentVariableW(wn.c_str(), nullptr, 0);
  if (n == 0) return SQ_E_NOTFOUND;
  std::wstring v(n, L'\0');
  n = GetEnvironmentVariableW(wn.c_str(), v.data(), n);
  v.resize(n);
  return sqp::toBuffer(sqp::narrow(v), out);
#else
  const char* v = std::getenv(name);
  if (!v) return SQ_E_NOTFOUND;
  return sqp::toBuffer(v, out);
#endif
}

sq_result sq_env_set(const char* name, const char* value) {
  if (!name || !*name) return SQ_E_INVALIDARG;
#ifdef _WIN32
  BOOL ok = SetEnvironmentVariableW(sqp::widen(name).c_str(), value ? sqp::widen(value).c_str() : nullptr);
  return ok ? SQ_OK : SQ_E_FAIL;
#else
  int rc = value ? setenv(name, value, 1) : unsetenv(name);
  return rc == 0 ? SQ_OK : SQ_E_FAIL;
#endif
}

sq_result sq_guid_generate(sq_guid* out) {
  if (!out) return SQ_E_INVALIDARG;
  uint8_t b[16];
  try {
    std::random_device rd;  // MSVC: RtlGenRandom; libstdc++/libc++: /dev/urandom or getrandom
    for (int i = 0; i < 16; i += 4) {
      uint32_t v = rd();
      b[i] = static_cast<uint8_t>(v);
      b[i + 1] = static_cast<uint8_t>(v >> 8);
      b[i + 2] = static_cast<uint8_t>(v >> 16);
      b[i + 3] = static_cast<uint8_t>(v >> 24);
    }
  } catch (...) {
    return SQ_E_FAIL;
  }
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
  out->data1 = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
  out->data2 = static_cast<uint16_t>((b[4] << 8) | b[5]);
  out->data3 = static_cast<uint16_t>((b[6] << 8) | b[7]);
  for (int i = 0; i < 8; ++i) out->data4[i] = b[8 + i];
  return SQ_OK;
}

}  // extern "C"
