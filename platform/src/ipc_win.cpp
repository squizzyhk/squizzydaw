// IPC primitives - Windows implementation.
#ifdef _WIN32
#include <string>

#include "internal.h"

namespace {

constexpr uint64_t kShmMagic = 0x53515348'4D303031ull;  // "SQSHM001"
constexpr size_t kShmHeader = 64;

struct ShmHeader {
  uint64_t magic;
  uint64_t size;
};

std::wstring pipePath(const char* name) { return L"\\\\.\\pipe\\sq." + sqp::widen(name); }
std::wstring objName(const char* kind, const char* name) { return L"Local\\sq." + sqp::widen(kind) + L"." + sqp::widen(name); }

HANDLE createInstance(const std::wstring& path, bool first) {
  DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
  return CreateNamedPipeW(path.c_str(), open, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                          PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
}

}  // namespace

struct sq_pipe_server {
  std::wstring path;
  HANDLE pending = INVALID_HANDLE_VALUE;
};

struct sq_pipe {
  HANDLE h = INVALID_HANDLE_VALUE;
  HANDLE ev = nullptr;
};

struct sq_shm {
  HANDLE map = nullptr;
  void* base = nullptr;
  size_t size = 0;
};

struct sq_sem {
  HANDLE h = nullptr;
};

static sq_pipe* makePipe(HANDLE h) {
  auto* p = new (std::nothrow) sq_pipe();
  if (!p) {
    CloseHandle(h);
    return nullptr;
  }
  p->h = h;
  p->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  return p;
}

extern "C" {

sq_result sq_pipe_listen(const char* name, sq_pipe_server** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  auto* s = new (std::nothrow) sq_pipe_server();
  if (!s) return SQ_E_OUTOFMEMORY;
  s->path = pipePath(name);
  s->pending = createInstance(s->path, true);
  if (s->pending == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    delete s;
    return e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY ? SQ_E_EXISTS : sqp::fromWinError(e);
  }
  *out = s;
  return SQ_OK;
}

sq_result sq_pipe_accept(sq_pipe_server* s, int32_t timeout_ms, sq_pipe** out) {
  if (!s || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (s->pending == INVALID_HANDLE_VALUE) {
    s->pending = createInstance(s->path, false);
    if (s->pending == INVALID_HANDLE_VALUE) return sqp::fromWinError(GetLastError());
  }
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return SQ_E_FAIL;
  sq_result r = SQ_OK;
  if (!ConnectNamedPipe(s->pending, &ov)) {
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) {
      DWORD w = WaitForSingleObject(ov.hEvent, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
      DWORD dummy = 0;
      if (w != WAIT_OBJECT_0) {
        CancelIoEx(s->pending, &ov);
        GetOverlappedResult(s->pending, &ov, &dummy, TRUE);
        r = SQ_E_TIMEOUT;
      } else if (!GetOverlappedResult(s->pending, &ov, &dummy, FALSE)) {
        r = sqp::fromWinError(GetLastError());
      }
    } else if (e != ERROR_PIPE_CONNECTED) {
      r = sqp::fromWinError(e);
    }
  }
  CloseHandle(ov.hEvent);
  if (r != SQ_OK) {
    CloseHandle(s->pending);
    s->pending = INVALID_HANDLE_VALUE;
    return r;
  }
  sq_pipe* p = makePipe(s->pending);
  s->pending = INVALID_HANDLE_VALUE;
  if (!p) return SQ_E_OUTOFMEMORY;
  *out = p;
  return SQ_OK;
}

void sq_pipe_server_close(sq_pipe_server* s) {
  if (!s) return;
  if (s->pending != INVALID_HANDLE_VALUE) CloseHandle(s->pending);
  delete s;
}

sq_result sq_pipe_connect(const char* name, int32_t timeout_ms, sq_pipe** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  std::wstring path = pipePath(name);
  uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : GetTickCount64() + static_cast<uint64_t>(timeout_ms);
  for (;;) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      sq_pipe* p = makePipe(h);
      if (!p) return SQ_E_OUTOFMEMORY;
      *out = p;
      return SQ_OK;
    }
    DWORD e = GetLastError();
    uint64_t now = GetTickCount64();
    if (now >= deadline) return SQ_E_TIMEOUT;
    if (e == ERROR_PIPE_BUSY) {
      uint64_t left = deadline - now;
      WaitNamedPipeW(path.c_str(), static_cast<DWORD>(left > 100 ? 100 : left));
    } else if (e == ERROR_FILE_NOT_FOUND) {
      Sleep(5);
    } else {
      return sqp::fromWinError(e);
    }
  }
}

sq_result sq_pipe_write(sq_pipe* p, const void* data, size_t size) {
  if (!p || (!data && size)) return SQ_E_INVALIDARG;
  const char* d = static_cast<const char*>(data);
  while (size > 0) {
    DWORD chunk = static_cast<DWORD>(size > 0x10000000u ? 0x10000000u : size);
    OVERLAPPED ov{};
    ov.hEvent = p->ev;
    ResetEvent(p->ev);
    DWORD n = 0;
    if (!WriteFile(p->h, d, chunk, &n, &ov)) {
      DWORD e = GetLastError();
      if (e != ERROR_IO_PENDING) return sqp::fromWinError(e);
      if (!GetOverlappedResult(p->h, &ov, &n, TRUE)) return sqp::fromWinError(GetLastError());
    }
    d += n;
    size -= n;
  }
  return SQ_OK;
}

sq_result sq_pipe_read(sq_pipe* p, void* buf, size_t size, size_t* got, int32_t timeout_ms) {
  if (got) *got = 0;
  if (!p || !buf || size == 0) return SQ_E_INVALIDARG;
  DWORD chunk = static_cast<DWORD>(size > 0x10000000u ? 0x10000000u : size);
  OVERLAPPED ov{};
  ov.hEvent = p->ev;
  ResetEvent(p->ev);
  DWORD n = 0;
  if (!ReadFile(p->h, buf, chunk, &n, &ov)) {
    DWORD e = GetLastError();
    if (e != ERROR_IO_PENDING) return sqp::fromWinError(e);
    DWORD w = WaitForSingleObject(p->ev, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
    if (w != WAIT_OBJECT_0) {
      CancelIoEx(p->h, &ov);
      if (GetOverlappedResult(p->h, &ov, &n, TRUE) && n > 0) {  // completed while cancelling
        if (got) *got = n;
        return SQ_OK;
      }
      return SQ_E_TIMEOUT;
    }
    if (!GetOverlappedResult(p->h, &ov, &n, FALSE)) return sqp::fromWinError(GetLastError());
  }
  if (n == 0) return SQ_E_CLOSED;
  if (got) *got = n;
  return SQ_OK;
}

sq_result sq_pipe_read_exact(sq_pipe* p, void* buf, size_t size, int32_t timeout_ms) {
  char* d = static_cast<char*>(buf);
  while (size > 0) {
    size_t n = 0;
    sq_result r = sq_pipe_read(p, d, size, &n, timeout_ms);
    if (r != SQ_OK) return r;
    d += n;
    size -= n;
  }
  return SQ_OK;
}

void sq_pipe_close(sq_pipe* p) {
  if (!p) return;
  if (p->h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(p->h);
    CloseHandle(p->h);
  }
  if (p->ev) CloseHandle(p->ev);
  delete p;
}

// ---- shared memory ----
sq_result sq_shm_create(const char* name, size_t size, sq_shm** out) {
  if (!out || !sqp::validIpcName(name) || size == 0) return SQ_E_INVALIDARG;
  *out = nullptr;
  uint64_t total = static_cast<uint64_t>(size) + kShmHeader;
  HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(total >> 32),
                                static_cast<DWORD>(total & 0xFFFFFFFFu), objName("shm", name).c_str());
  if (!m) return sqp::fromWinError(GetLastError());
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(m);
    return SQ_E_EXISTS;
  }
  void* base = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!base) {
    CloseHandle(m);
    return SQ_E_FAIL;
  }
  auto* hdr = static_cast<ShmHeader*>(base);
  hdr->size = size;
  hdr->magic = kShmMagic;
  auto* s = new (std::nothrow) sq_shm();
  if (!s) {
    UnmapViewOfFile(base);
    CloseHandle(m);
    return SQ_E_OUTOFMEMORY;
  }
  s->map = m;
  s->base = base;
  s->size = size;
  *out = s;
  return SQ_OK;
}

sq_result sq_shm_open(const char* name, sq_shm** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, objName("shm", name).c_str());
  if (!m) return sqp::fromWinError(GetLastError());
  void* base = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!base) {
    CloseHandle(m);
    return SQ_E_FAIL;
  }
  auto* hdr = static_cast<ShmHeader*>(base);
  if (hdr->magic != kShmMagic) {
    UnmapViewOfFile(base);
    CloseHandle(m);
    return SQ_E_TYPE;
  }
  auto* s = new (std::nothrow) sq_shm();
  if (!s) {
    UnmapViewOfFile(base);
    CloseHandle(m);
    return SQ_E_OUTOFMEMORY;
  }
  s->map = m;
  s->base = base;
  s->size = static_cast<size_t>(hdr->size);
  *out = s;
  return SQ_OK;
}

void* sq_shm_data(sq_shm* s) { return s ? static_cast<char*>(s->base) + kShmHeader : nullptr; }
size_t sq_shm_size(sq_shm* s) { return s ? s->size : 0; }
void sq_shm_close(sq_shm* s) {
  if (!s) return;
  if (s->base) UnmapViewOfFile(s->base);
  if (s->map) CloseHandle(s->map);
  delete s;
}

// ---- semaphores ----
sq_result sq_sem_create(const char* name, uint32_t initial, uint32_t max_count, sq_sem** out) {
  if (!out || max_count == 0 || initial > max_count || max_count > 0x7FFFFFFF) return SQ_E_INVALIDARG;
  if (name && !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  std::wstring n = name ? objName("sem", name) : std::wstring();
  HANDLE h = CreateSemaphoreW(nullptr, static_cast<LONG>(initial), static_cast<LONG>(max_count), name ? n.c_str() : nullptr);
  if (!h) return sqp::fromWinError(GetLastError());
  if (name && GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(h);
    return SQ_E_EXISTS;
  }
  auto* s = new (std::nothrow) sq_sem();
  if (!s) {
    CloseHandle(h);
    return SQ_E_OUTOFMEMORY;
  }
  s->h = h;
  *out = s;
  return SQ_OK;
}

sq_result sq_sem_open(const char* name, sq_sem** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  HANDLE h = OpenSemaphoreW(SEMAPHORE_ALL_ACCESS, FALSE, objName("sem", name).c_str());
  if (!h) return sqp::fromWinError(GetLastError());
  auto* s = new (std::nothrow) sq_sem();
  if (!s) {
    CloseHandle(h);
    return SQ_E_OUTOFMEMORY;
  }
  s->h = h;
  *out = s;
  return SQ_OK;
}

sq_result sq_sem_wait(sq_sem* s, int32_t timeout_ms) {
  if (!s) return SQ_E_INVALIDARG;
  DWORD w = WaitForSingleObject(s->h, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  if (w == WAIT_OBJECT_0) return SQ_OK;
  if (w == WAIT_TIMEOUT) return SQ_E_TIMEOUT;
  return SQ_E_FAIL;
}

sq_result sq_sem_post(sq_sem* s, uint32_t count) {
  if (!s || count == 0 || count > 0x7FFFFFFF) return SQ_E_INVALIDARG;
  return ReleaseSemaphore(s->h, static_cast<LONG>(count), nullptr) ? SQ_OK : SQ_E_FAIL;
}

void sq_sem_close(sq_sem* s) {
  if (!s) return;
  if (s->h) CloseHandle(s->h);
  delete s;
}

}  // extern "C"

#endif  // _WIN32
