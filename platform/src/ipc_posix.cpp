// IPC primitives - POSIX implementation (Linux/macOS). Compiled but not exercised on the
// Windows dev box; CI builds and tests it on ubuntu/macos.
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <semaphore.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <string>

#include "internal.h"

namespace {

constexpr uint64_t kShmMagic = 0x5351534D'4D303031ull;
constexpr size_t kShmHeader = 64;
struct ShmHeader {
  uint64_t magic;
  uint64_t size;
};

std::string sockPath(const char* name) {
  const char* tmp = getenv("TMPDIR");
  std::string dir = (tmp && *tmp) ? tmp : "/tmp";
  if (dir.back() == '/') dir.pop_back();
  std::string p = dir + "/sq." + name + ".sock";
  if (p.size() >= sizeof(sockaddr_un{}.sun_path)) p = std::string("/tmp/sq.") + name + ".sock";  // macOS TMPDIR is long
  return p;
}
std::string posixName(const char* kind, const char* name) { return std::string("/sq.") + kind + "." + name; }

uint64_t nowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

// Waits for fd readiness; returns 1 ready, 0 timeout, -1 error.
int waitFd(int fd, short events, int32_t timeout_ms) {
  pollfd p{fd, events, 0};
  for (;;) {
    int r = poll(&p, 1, timeout_ms < 0 ? -1 : timeout_ms);
    if (r < 0 && errno == EINTR) continue;
    return r < 0 ? -1 : (r == 0 ? 0 : 1);
  }
}

bool fillAddr(const std::string& path, sockaddr_un& a) {
  std::memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) return false;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  return true;
}

std::atomic<uint32_t> gLocalSemCounter{0};

}  // namespace

struct sq_pipe_server {
  int fd = -1;
  int lockFd = -1;
  std::string path;
  std::string lockPath;
};
struct sq_pipe {
  int fd = -1;
};
struct sq_shm {
  void* base = nullptr;
  size_t mapped = 0;
  size_t size = 0;
  std::string name;
  bool owner = false;
};
struct sq_sem {
  sem_t* s = nullptr;
};

extern "C" {

sq_result sq_pipe_listen(const char* name, sq_pipe_server** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  std::string path = sockPath(name);
  std::string lockPath = path + ".lock";
  sockaddr_un a;
  if (!fillAddr(path, a)) return SQ_E_INVALIDARG;

  // A sidecar lock file makes duplicate-listen detection non-invasive. Probing a live Unix
  // socket with connect() would enqueue a phantom connection for the listening server to accept.
  int lockFd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockFd < 0) return SQ_E_IO;
  if (flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
    close(lockFd);
    return errno == EWOULDBLOCK ? SQ_E_EXISTS : SQ_E_IO;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    close(lockFd);
    unlink(lockPath.c_str());
    return SQ_E_FAIL;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    // We hold the lock, so nobody is listening: an existing socket file is stale.
    if (errno != EADDRINUSE) {
      close(fd);
      close(lockFd);
      unlink(lockPath.c_str());
      return SQ_E_IO;
    }
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      close(fd);
      close(lockFd);
      unlink(lockPath.c_str());
      return SQ_E_IO;
    }
  }
  if (listen(fd, 16) != 0) {
    close(fd);
    unlink(path.c_str());
    close(lockFd);
    unlink(lockPath.c_str());
    return SQ_E_IO;
  }
  auto* s = new (std::nothrow) sq_pipe_server();
  if (!s) {
    close(fd);
    unlink(path.c_str());
    close(lockFd);
    unlink(lockPath.c_str());
    return SQ_E_OUTOFMEMORY;
  }
  s->fd = fd;
  s->lockFd = lockFd;
  s->path = path;
  s->lockPath = lockPath;
  *out = s;
  return SQ_OK;
}

sq_result sq_pipe_accept(sq_pipe_server* s, int32_t timeout_ms, sq_pipe** out) {
  if (!s || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  const uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : nowMs() + static_cast<uint64_t>(timeout_ms);
  int c = -1;
  for (;;) {
    const uint64_t now = nowMs();
    const int32_t left = timeout_ms < 0 ? -1 : static_cast<int32_t>(deadline > now ? deadline - now : 0);
    int w = waitFd(s->fd, POLLIN, left);
    if (w == 0) return SQ_E_TIMEOUT;
    if (w < 0) return SQ_E_IO;
    c = accept(s->fd, nullptr, nullptr);
    if (c < 0) return SQ_E_IO;
    // Skip connections their peer already closed without sending anything: the liveness probe of
    // a second sq_pipe_listen on the same name connects and hangs up immediately.
    char peek;
    ssize_t n = recv(c, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) {
      close(c);
      continue;
    }
    break;
  }
  fcntl(c, F_SETFD, FD_CLOEXEC);
  auto* p = new (std::nothrow) sq_pipe();
  if (!p) {
    close(c);
    return SQ_E_OUTOFMEMORY;
  }
  p->fd = c;
  *out = p;
  return SQ_OK;
}

void sq_pipe_server_close(sq_pipe_server* s) {
  if (!s) return;
  if (s->fd >= 0) close(s->fd);
  unlink(s->path.c_str());
  if (s->lockFd >= 0) close(s->lockFd);
  if (!s->lockPath.empty()) unlink(s->lockPath.c_str());
  delete s;
}

sq_result sq_pipe_connect(const char* name, int32_t timeout_ms, sq_pipe** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  sockaddr_un a;
  if (!fillAddr(sockPath(name), a)) return SQ_E_INVALIDARG;
  uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : nowMs() + static_cast<uint64_t>(timeout_ms);
  for (;;) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return SQ_E_FAIL;
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      auto* p = new (std::nothrow) sq_pipe();
      if (!p) {
        close(fd);
        return SQ_E_OUTOFMEMORY;
      }
      p->fd = fd;
      *out = p;
      return SQ_OK;
    }
    close(fd);
    if (nowMs() >= deadline) return SQ_E_TIMEOUT;
    usleep(5000);
  }
}

sq_result sq_pipe_write(sq_pipe* p, const void* data, size_t size) {
  if (!p || (!data && size)) return SQ_E_INVALIDARG;
  const char* d = static_cast<const char*>(data);
  while (size > 0) {
#ifdef MSG_NOSIGNAL
    ssize_t n = send(p->fd, d, size, MSG_NOSIGNAL);
#else
    ssize_t n = send(p->fd, d, size, 0);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      return errno == EPIPE ? SQ_E_CLOSED : SQ_E_IO;
    }
    d += n;
    size -= static_cast<size_t>(n);
  }
  return SQ_OK;
}

sq_result sq_pipe_read(sq_pipe* p, void* buf, size_t size, size_t* got, int32_t timeout_ms) {
  if (got) *got = 0;
  if (!p || !buf || size == 0) return SQ_E_INVALIDARG;
  int w = waitFd(p->fd, POLLIN, timeout_ms);
  if (w == 0) return SQ_E_TIMEOUT;
  if (w < 0) return SQ_E_IO;
  for (;;) {
    ssize_t n = recv(p->fd, buf, size, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return SQ_E_IO;
    if (n == 0) return SQ_E_CLOSED;
    if (got) *got = static_cast<size_t>(n);
    return SQ_OK;
  }
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
  if (p->fd >= 0) close(p->fd);
  delete p;
}

// ---- shared memory ----
sq_result sq_shm_create(const char* name, size_t size, sq_shm** out) {
  if (!out || !sqp::validIpcName(name) || size == 0) return SQ_E_INVALIDARG;
  *out = nullptr;
  std::string n = posixName("shm", name);
  int fd = shm_open(n.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return errno == EEXIST ? SQ_E_EXISTS : SQ_E_IO;
  size_t total = size + kShmHeader;
  if (ftruncate(fd, static_cast<off_t>(total)) != 0) {
    close(fd);
    shm_unlink(n.c_str());
    return SQ_E_IO;
  }
  void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (base == MAP_FAILED) {
    shm_unlink(n.c_str());
    return SQ_E_FAIL;
  }
  auto* hdr = static_cast<ShmHeader*>(base);
  hdr->size = size;
  hdr->magic = kShmMagic;
  auto* s = new (std::nothrow) sq_shm();
  if (!s) {
    munmap(base, total);
    shm_unlink(n.c_str());
    return SQ_E_OUTOFMEMORY;
  }
  s->base = base;
  s->mapped = total;
  s->size = size;
  s->name = n;
  s->owner = true;
  *out = s;
  return SQ_OK;
}

sq_result sq_shm_open(const char* name, sq_shm** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  std::string n = posixName("shm", name);
  int fd = shm_open(n.c_str(), O_RDWR, 0600);
  if (fd < 0) return errno == ENOENT ? SQ_E_NOTFOUND : SQ_E_IO;
  struct stat st;
  if (fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < kShmHeader) {
    close(fd);
    return SQ_E_TYPE;
  }
  size_t total = static_cast<size_t>(st.st_size);
  void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (base == MAP_FAILED) return SQ_E_FAIL;
  auto* hdr = static_cast<ShmHeader*>(base);
  if (hdr->magic != kShmMagic) {
    munmap(base, total);
    return SQ_E_TYPE;
  }
  auto* s = new (std::nothrow) sq_shm();
  if (!s) {
    munmap(base, total);
    return SQ_E_OUTOFMEMORY;
  }
  s->base = base;
  s->mapped = total;
  s->size = static_cast<size_t>(hdr->size);
  s->name = n;
  *out = s;
  return SQ_OK;
}

void* sq_shm_data(sq_shm* s) { return s ? static_cast<char*>(s->base) + kShmHeader : nullptr; }
size_t sq_shm_size(sq_shm* s) { return s ? s->size : 0; }
void sq_shm_close(sq_shm* s) {
  if (!s) return;
  if (s->base) munmap(s->base, s->mapped);
  if (s->owner) shm_unlink(s->name.c_str());
  delete s;
}

// ---- semaphores (named POSIX semaphores; local ones use a unique, immediately unlinked name) ----
sq_result sq_sem_create(const char* name, uint32_t initial, uint32_t max_count, sq_sem** out) {
  if (!out || max_count == 0 || initial > max_count) return SQ_E_INVALIDARG;
  if (name && !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  std::string n = name ? posixName("sem", name)
                       : "/sq.lsem." + std::to_string(getpid()) + "." + std::to_string(gLocalSemCounter.fetch_add(1));
  sem_t* sem = sem_open(n.c_str(), O_CREAT | O_EXCL, 0600, initial);
  if (sem == SEM_FAILED) return errno == EEXIST ? SQ_E_EXISTS : SQ_E_IO;
  if (!name) sem_unlink(n.c_str());
  auto* s = new (std::nothrow) sq_sem();
  if (!s) {
    sem_close(sem);
    return SQ_E_OUTOFMEMORY;
  }
  s->s = sem;
  *out = s;
  return SQ_OK;
}

sq_result sq_sem_open(const char* name, sq_sem** out) {
  if (!out || !sqp::validIpcName(name)) return SQ_E_INVALIDARG;
  *out = nullptr;
  sem_t* sem = sem_open(posixName("sem", name).c_str(), 0);
  if (sem == SEM_FAILED) return errno == ENOENT ? SQ_E_NOTFOUND : SQ_E_IO;
  auto* s = new (std::nothrow) sq_sem();
  if (!s) {
    sem_close(sem);
    return SQ_E_OUTOFMEMORY;
  }
  s->s = sem;
  *out = s;
  return SQ_OK;
}

sq_result sq_sem_wait(sq_sem* s, int32_t timeout_ms) {
  if (!s) return SQ_E_INVALIDARG;
  if (timeout_ms < 0) {
    while (sem_wait(s->s) != 0)
      if (errno != EINTR) return SQ_E_FAIL;
    return SQ_OK;
  }
  // Portable timed wait (macOS lacks sem_timedwait): poll with short sleeps.
  uint64_t deadline = nowMs() + static_cast<uint64_t>(timeout_ms);
  for (;;) {
    if (sem_trywait(s->s) == 0) return SQ_OK;
    if (errno != EAGAIN && errno != EINTR) return SQ_E_FAIL;
    if (nowMs() >= deadline) return SQ_E_TIMEOUT;
    usleep(500);
  }
}

sq_result sq_sem_post(sq_sem* s, uint32_t count) {
  if (!s || count == 0) return SQ_E_INVALIDARG;
  for (uint32_t i = 0; i < count; ++i)
    if (sem_post(s->s) != 0) return SQ_E_FAIL;
  return SQ_OK;
}

void sq_sem_close(sq_sem* s) {
  if (!s) return;
  if (s->s) sem_close(s->s);
  delete s;
}

}  // extern "C"

#endif  // !_WIN32
