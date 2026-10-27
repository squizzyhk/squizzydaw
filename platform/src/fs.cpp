#include <chrono>
#include <filesystem>
#include <string>

#include "internal.h"

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path toPath(const char* s) {
#ifdef _WIN32
  return fs::path(sqp::widen(s));
#else
  return fs::path(s);
#endif
}

std::string fromPath(const fs::path& p) {
#ifdef _WIN32
  return sqp::narrow(p.generic_wstring());
#else
  return p.generic_string();
#endif
}

sq_result fromEc(const std::error_code& ec) {
  if (!ec) return SQ_OK;
  if (ec == std::errc::no_such_file_or_directory) return SQ_E_NOTFOUND;
  if (ec == std::errc::permission_denied) return SQ_E_ACCESS;
  if (ec == std::errc::file_exists) return SQ_E_EXISTS;
  return SQ_E_IO;
}

}  // namespace

struct sq_file {
#ifdef _WIN32
  HANDLE h = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif
  bool append = false;
};

extern "C" {

sq_bool sq_fs_exists(const char* path) {
  if (!path) return SQ_FALSE;
  std::error_code ec;
  return fs::exists(toPath(path), ec) ? SQ_TRUE : SQ_FALSE;
}
sq_bool sq_fs_is_dir(const char* path) {
  if (!path) return SQ_FALSE;
  std::error_code ec;
  return fs::is_directory(toPath(path), ec) ? SQ_TRUE : SQ_FALSE;
}
sq_bool sq_fs_is_file(const char* path) {
  if (!path) return SQ_FALSE;
  std::error_code ec;
  return fs::is_regular_file(toPath(path), ec) ? SQ_TRUE : SQ_FALSE;
}
sq_result sq_fs_create_dirs(const char* path) {
  if (!path || !*path) return SQ_E_INVALIDARG;
  std::error_code ec;
  fs::path p = toPath(path);
  if (fs::is_directory(p, ec)) return SQ_OK;
  fs::create_directories(p, ec);
  return fromEc(ec);
}
sq_result sq_fs_remove(const char* path) {
  if (!path) return SQ_E_INVALIDARG;
  std::error_code ec;
  bool removed = fs::remove(toPath(path), ec);
  if (ec) return fromEc(ec);
  return removed ? SQ_OK : SQ_E_NOTFOUND;
}
sq_result sq_fs_remove_all(const char* path) {
  if (!path) return SQ_E_INVALIDARG;
  std::error_code ec;
  fs::remove_all(toPath(path), ec);
  return fromEc(ec);
}
sq_result sq_fs_rename(const char* from, const char* to) {
  if (!from || !to) return SQ_E_INVALIDARG;
  std::error_code ec;
  fs::rename(toPath(from), toPath(to), ec);
  return fromEc(ec);
}
sq_result sq_fs_copy_file(const char* from, const char* to, sq_bool overwrite) {
  if (!from || !to) return SQ_E_INVALIDARG;
  std::error_code ec;
  bool ok = fs::copy_file(toPath(from), toPath(to), overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
  if (ec) return fromEc(ec);
  return ok ? SQ_OK : SQ_E_EXISTS;
}
sq_result sq_fs_file_size(const char* path, uint64_t* out) {
  if (!path || !out) return SQ_E_INVALIDARG;
  std::error_code ec;
  auto s = fs::file_size(toPath(path), ec);
  if (ec) return fromEc(ec);
  *out = static_cast<uint64_t>(s);
  return SQ_OK;
}
sq_result sq_fs_mtime(const char* path, int64_t* out_unix_ms) {
  if (!path || !out_unix_ms) return SQ_E_INVALIDARG;
#ifdef _WIN32
  std::error_code ec;
  auto t = fs::last_write_time(toPath(path), ec);
  if (ec) return fromEc(ec);
  auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
  *out_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
  return SQ_OK;
#else
  struct stat st;
  if (::stat(path, &st) != 0) return errno == ENOENT ? SQ_E_NOTFOUND : SQ_E_IO;
  *out_unix_ms = static_cast<int64_t>(st.st_mtime) * 1000;
  return SQ_OK;
#endif
}
sq_result sq_fs_absolute(const char* path, sq_buffer* out) {
  if (!path || !out) return SQ_E_INVALIDARG;
  std::error_code ec;
  fs::path p = fs::absolute(toPath(path), ec);
  if (ec) return fromEc(ec);
  return sqp::toBuffer(fromPath(p.lexically_normal()), out);
}

sq_result sq_fs_read_file(const char* path, sq_buffer* out) {
  if (!path || !out) return SQ_E_INVALIDARG;
  out->data = nullptr;
  out->size = 0;
  sq_file* f = nullptr;
  sq_result r = sq_file_open(path, SQ_FILE_READ, &f);
  if (r != SQ_OK) return r;
  uint64_t size = 0;
  r = sq_file_size(f, &size);
  if (r != SQ_OK) {
    sq_file_close(f);
    return r;
  }
  char* data = static_cast<char*>(sq_alloc(static_cast<size_t>(size) + 1));
  if (!data) {
    sq_file_close(f);
    return SQ_E_OUTOFMEMORY;
  }
  size_t total = 0;
  while (total < size) {
    size_t got = 0;
    r = sq_file_read(f, data + total, static_cast<size_t>(size) - total, &got);
    if (r != SQ_OK || got == 0) break;
    total += got;
  }
  sq_file_close(f);
  if (r != SQ_OK) {
    sq_free(data);
    return r;
  }
  data[total] = '\0';
  out->data = data;
  out->size = total;
  return SQ_OK;
}

sq_result sq_fs_write_file(const char* path, const void* data, size_t size) {
  if (!path || (!data && size)) return SQ_E_INVALIDARG;
  fs::path target = toPath(path);
  std::error_code ec;
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  std::string tmp = std::string(path) + ".tmp" + std::to_string(sq_process_id());
  sq_file* f = nullptr;
  sq_result r = sq_file_open(tmp.c_str(), SQ_FILE_WRITE | SQ_FILE_CREATE | SQ_FILE_TRUNCATE, &f);
  if (r != SQ_OK) return r;
  r = size ? sq_file_write(f, data, size) : SQ_OK;
  if (r == SQ_OK) r = sq_file_flush(f);
  sq_file_close(f);
  if (r != SQ_OK) {
    sq_fs_remove(tmp.c_str());
    return r;
  }
  r = sq_fs_rename(tmp.c_str(), path);
  if (r != SQ_OK) sq_fs_remove(tmp.c_str());
  return r;
}

sq_result sq_fs_list_dir(const char* path, sq_fs_list_fn fn, void* user) {
  if (!path || !fn) return SQ_E_INVALIDARG;
  std::error_code ec;
  fs::directory_iterator it(toPath(path), ec), end;
  if (ec) return fromEc(ec);
  for (; it != end; it.increment(ec)) {
    if (ec) return fromEc(ec);
    std::error_code ec2;
    bool dir = it->is_directory(ec2);
    std::string name = fromPath(it->path().filename());
    if (!fn(user, name.c_str(), dir ? SQ_TRUE : SQ_FALSE)) break;
  }
  return SQ_OK;
}

// ---- sq_file ----
sq_result sq_file_open(const char* path, uint32_t flags, sq_file** out) {
  if (!path || !out || !(flags & (SQ_FILE_READ | SQ_FILE_WRITE | SQ_FILE_APPEND))) return SQ_E_INVALIDARG;
  *out = nullptr;
  bool write = (flags & (SQ_FILE_WRITE | SQ_FILE_APPEND)) != 0;
#ifdef _WIN32
  DWORD access = 0;
  if (flags & SQ_FILE_READ) access |= GENERIC_READ;
  if (write) access |= GENERIC_WRITE;
  DWORD disp = OPEN_EXISTING;
  if ((flags & SQ_FILE_CREATE) && (flags & SQ_FILE_TRUNCATE)) disp = CREATE_ALWAYS;
  else if (flags & SQ_FILE_CREATE) disp = OPEN_ALWAYS;
  else if (flags & SQ_FILE_TRUNCATE) disp = TRUNCATE_EXISTING;
  HANDLE h = CreateFileW(sqp::widen(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_DELETE | (write ? 0 : FILE_SHARE_WRITE),
                         nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return sqp::fromWinError(GetLastError());
  auto* f = new (std::nothrow) sq_file();
  if (!f) {
    CloseHandle(h);
    return SQ_E_OUTOFMEMORY;
  }
  f->h = h;
#else
  int o = 0;
  if ((flags & SQ_FILE_READ) && write) o = O_RDWR;
  else if (write) o = O_WRONLY;
  else o = O_RDONLY;
  if (flags & SQ_FILE_CREATE) o |= O_CREAT;
  if (flags & SQ_FILE_TRUNCATE) o |= O_TRUNC;
  int fd = ::open(path, o | O_CLOEXEC, 0644);
  if (fd < 0) return errno == ENOENT ? SQ_E_NOTFOUND : errno == EACCES ? SQ_E_ACCESS : SQ_E_IO;
  auto* f = new (std::nothrow) sq_file();
  if (!f) {
    ::close(fd);
    return SQ_E_OUTOFMEMORY;
  }
  f->fd = fd;
#endif
  f->append = (flags & SQ_FILE_APPEND) != 0;
  if (f->append) sq_file_seek(f, 0, SQ_SEEK_END, nullptr);
  *out = f;
  return SQ_OK;
}

sq_result sq_file_read(sq_file* f, void* buf, size_t size, size_t* got) {
  if (!f || (!buf && size)) return SQ_E_INVALIDARG;
  size_t total = 0;
#ifdef _WIN32
  while (total < size) {
    DWORD chunk = static_cast<DWORD>((size - total) > 0x40000000u ? 0x40000000u : (size - total));
    DWORD n = 0;
    if (!ReadFile(f->h, static_cast<char*>(buf) + total, chunk, &n, nullptr)) return sqp::fromWinError(GetLastError());
    if (n == 0) break;
    total += n;
  }
#else
  while (total < size) {
    ssize_t n = ::read(f->fd, static_cast<char*>(buf) + total, size - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      return SQ_E_IO;
    }
    if (n == 0) break;
    total += static_cast<size_t>(n);
  }
#endif
  if (got) *got = total;
  return SQ_OK;
}

sq_result sq_file_write(sq_file* f, const void* buf, size_t size) {
  if (!f || (!buf && size)) return SQ_E_INVALIDARG;
  if (f->append) sq_file_seek(f, 0, SQ_SEEK_END, nullptr);
  size_t total = 0;
#ifdef _WIN32
  while (total < size) {
    DWORD chunk = static_cast<DWORD>((size - total) > 0x40000000u ? 0x40000000u : (size - total));
    DWORD n = 0;
    if (!WriteFile(f->h, static_cast<const char*>(buf) + total, chunk, &n, nullptr)) return sqp::fromWinError(GetLastError());
    total += n;
  }
#else
  while (total < size) {
    ssize_t n = ::write(f->fd, static_cast<const char*>(buf) + total, size - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      return SQ_E_IO;
    }
    total += static_cast<size_t>(n);
  }
#endif
  return SQ_OK;
}

sq_result sq_file_seek(sq_file* f, int64_t offset, int whence, uint64_t* new_pos) {
  if (!f) return SQ_E_INVALIDARG;
#ifdef _WIN32
  DWORD method = whence == SQ_SEEK_CUR ? FILE_CURRENT : whence == SQ_SEEK_END ? FILE_END : FILE_BEGIN;
  LARGE_INTEGER li, res;
  li.QuadPart = offset;
  if (!SetFilePointerEx(f->h, li, &res, method)) return sqp::fromWinError(GetLastError());
  if (new_pos) *new_pos = static_cast<uint64_t>(res.QuadPart);
#else
  int w = whence == SQ_SEEK_CUR ? SEEK_CUR : whence == SQ_SEEK_END ? SEEK_END : SEEK_SET;
  off_t r = ::lseek(f->fd, static_cast<off_t>(offset), w);
  if (r < 0) return SQ_E_IO;
  if (new_pos) *new_pos = static_cast<uint64_t>(r);
#endif
  return SQ_OK;
}

sq_result sq_file_size(sq_file* f, uint64_t* out) {
  if (!f || !out) return SQ_E_INVALIDARG;
#ifdef _WIN32
  LARGE_INTEGER li;
  if (!GetFileSizeEx(f->h, &li)) return sqp::fromWinError(GetLastError());
  *out = static_cast<uint64_t>(li.QuadPart);
#else
  struct stat st;
  if (fstat(f->fd, &st) != 0) return SQ_E_IO;
  *out = static_cast<uint64_t>(st.st_size);
#endif
  return SQ_OK;
}

sq_result sq_file_flush(sq_file* f) {
  if (!f) return SQ_E_INVALIDARG;
#ifdef _WIN32
  return FlushFileBuffers(f->h) ? SQ_OK : sqp::fromWinError(GetLastError());
#else
  return ::fsync(f->fd) == 0 ? SQ_OK : SQ_E_IO;
#endif
}

void sq_file_close(sq_file* f) {
  if (!f) return;
#ifdef _WIN32
  if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
#else
  if (f->fd >= 0) ::close(f->fd);
#endif
  delete f;
}

}  // extern "C"
