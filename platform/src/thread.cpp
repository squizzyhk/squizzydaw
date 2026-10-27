#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "internal.h"

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

struct sq_mutex {
  bool recursive = false;
  std::mutex m;
  std::recursive_mutex rm;
};

struct sq_rwlock {
  std::shared_mutex m;
};

struct sq_event {
  std::mutex m;
  std::condition_variable cv;
  bool manual = false;
  bool set = false;
};

struct sq_thread {
  std::thread t;
};

struct sq_pool {
  std::mutex m;
  std::condition_variable cv;      // work available / stop
  std::condition_variable idleCv;  // became idle
  std::deque<std::pair<sq_thread_fn, void*>> queue;
  std::vector<std::thread> threads;
  uint32_t running = 0;
  bool stop = false;
  std::string name;
};

namespace {

void setNativePriority(
#ifdef _WIN32
    HANDLE h,
#else
    pthread_t h,
#endif
    sq_thread_priority p, sq_result* r) {
#ifdef _WIN32
  int prio = THREAD_PRIORITY_NORMAL;
  switch (p) {
    case SQ_PRIORITY_LOW: prio = THREAD_PRIORITY_BELOW_NORMAL; break;
    case SQ_PRIORITY_NORMAL: prio = THREAD_PRIORITY_NORMAL; break;
    case SQ_PRIORITY_HIGH: prio = THREAD_PRIORITY_HIGHEST; break;
    case SQ_PRIORITY_REALTIME: prio = THREAD_PRIORITY_TIME_CRITICAL; break;
  }
  *r = SetThreadPriority(h, prio) ? SQ_OK : SQ_E_ACCESS;
#else
  int policy = SCHED_OTHER;
  sched_param sp{};
  if (p == SQ_PRIORITY_REALTIME) {
    policy = SCHED_FIFO;
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO) - 10;
  }
  *r = pthread_setschedparam(h, policy, &sp) == 0 ? SQ_OK : SQ_E_ACCESS;  // RT usually needs privileges
#endif
}

void setCurrentName(const char* name) {
  if (!name || !*name) return;
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), sqp::widen(name).c_str());
#elif defined(__APPLE__)
  pthread_setname_np(name);
#else
  char buf[16];
  std::strncpy(buf, name, 15);
  buf[15] = '\0';
  pthread_setname_np(pthread_self(), buf);
#endif
}

void poolWorker(sq_pool* p, uint32_t index) {
  setCurrentName((p->name + "-" + std::to_string(index)).c_str());
  for (;;) {
    std::pair<sq_thread_fn, void*> job;
    {
      std::unique_lock<std::mutex> lk(p->m);
      p->cv.wait(lk, [p] { return p->stop || !p->queue.empty(); });
      if (p->queue.empty()) return;  // stop requested and drained
      job = p->queue.front();
      p->queue.pop_front();
      ++p->running;
    }
    job.first(job.second);
    {
      std::lock_guard<std::mutex> lk(p->m);
      --p->running;
      if (p->running == 0 && p->queue.empty()) p->idleCv.notify_all();
    }
  }
}

}  // namespace

extern "C" {

// ---- mutex ----
sq_result sq_mutex_create(sq_bool recursive, sq_mutex** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = new (std::nothrow) sq_mutex();
  if (!*out) return SQ_E_OUTOFMEMORY;
  (*out)->recursive = recursive != 0;
  return SQ_OK;
}
void sq_mutex_destroy(sq_mutex* m) { delete m; }
void sq_mutex_lock(sq_mutex* m) {
  if (m->recursive) m->rm.lock();
  else m->m.lock();
}
sq_bool sq_mutex_trylock(sq_mutex* m) { return (m->recursive ? m->rm.try_lock() : m->m.try_lock()) ? SQ_TRUE : SQ_FALSE; }
void sq_mutex_unlock(sq_mutex* m) {
  if (m->recursive) m->rm.unlock();
  else m->m.unlock();
}

// ---- rwlock ----
sq_result sq_rwlock_create(sq_rwlock** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = new (std::nothrow) sq_rwlock();
  return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
}
void sq_rwlock_destroy(sq_rwlock* l) { delete l; }
void sq_rwlock_lock_shared(sq_rwlock* l) { l->m.lock_shared(); }
void sq_rwlock_unlock_shared(sq_rwlock* l) { l->m.unlock_shared(); }
void sq_rwlock_lock(sq_rwlock* l) { l->m.lock(); }
void sq_rwlock_unlock(sq_rwlock* l) { l->m.unlock(); }

// ---- event ----
sq_result sq_event_create(sq_bool manual_reset, sq_bool initially_set, sq_event** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = new (std::nothrow) sq_event();
  if (!*out) return SQ_E_OUTOFMEMORY;
  (*out)->manual = manual_reset != 0;
  (*out)->set = initially_set != 0;
  return SQ_OK;
}
void sq_event_destroy(sq_event* e) { delete e; }
void sq_event_set(sq_event* e) {
  {
    std::lock_guard<std::mutex> lk(e->m);
    e->set = true;
  }
  if (e->manual) e->cv.notify_all();
  else e->cv.notify_one();
}
void sq_event_reset(sq_event* e) {
  std::lock_guard<std::mutex> lk(e->m);
  e->set = false;
}
sq_result sq_event_wait(sq_event* e, int32_t timeout_ms) {
  std::unique_lock<std::mutex> lk(e->m);
  if (timeout_ms < 0) {
    e->cv.wait(lk, [e] { return e->set; });
  } else if (!e->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [e] { return e->set; })) {
    return SQ_E_TIMEOUT;
  }
  if (!e->manual) e->set = false;
  return SQ_OK;
}

// ---- TLS ----
sq_result sq_tls_alloc(sq_tls_key* out) {
  if (!out) return SQ_E_INVALIDARG;
#ifdef _WIN32
  DWORD k = TlsAlloc();
  if (k == TLS_OUT_OF_INDEXES) return SQ_E_OUTOFMEMORY;
  *out = static_cast<sq_tls_key>(k);
#else
  pthread_key_t k;
  if (pthread_key_create(&k, nullptr) != 0) return SQ_E_OUTOFMEMORY;
  *out = static_cast<sq_tls_key>(k);
#endif
  return SQ_OK;
}
void sq_tls_free(sq_tls_key key) {
#ifdef _WIN32
  TlsFree(static_cast<DWORD>(key));
#else
  pthread_key_delete(static_cast<pthread_key_t>(key));
#endif
}
void* sq_tls_get(sq_tls_key key) {
#ifdef _WIN32
  return TlsGetValue(static_cast<DWORD>(key));
#else
  return pthread_getspecific(static_cast<pthread_key_t>(key));
#endif
}
sq_result sq_tls_set(sq_tls_key key, void* value) {
#ifdef _WIN32
  return TlsSetValue(static_cast<DWORD>(key), value) ? SQ_OK : SQ_E_FAIL;
#else
  return pthread_setspecific(static_cast<pthread_key_t>(key), value) == 0 ? SQ_OK : SQ_E_FAIL;
#endif
}

// ---- threads ----
sq_result sq_thread_create(sq_thread_fn fn, void* arg, const char* name, sq_thread** out) {
  if (!fn || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  auto* t = new (std::nothrow) sq_thread();
  if (!t) return SQ_E_OUTOFMEMORY;
  std::string n = name ? name : "";
  try {
    t->t = std::thread([fn, arg, n] {
      setCurrentName(n.c_str());
      fn(arg);
    });
  } catch (...) {
    delete t;
    return SQ_E_FAIL;
  }
  *out = t;
  return SQ_OK;
}
sq_result sq_thread_set_priority(sq_thread* t, sq_thread_priority p) {
  if (!t) return SQ_E_INVALIDARG;
  sq_result r = SQ_E_FAIL;
  setNativePriority(t->t.native_handle(), p, &r);
  return r;
}
void sq_thread_join(sq_thread* t) {
  if (!t) return;
  if (t->t.joinable()) t->t.join();
  delete t;
}
sq_result sq_thread_set_current_priority(sq_thread_priority p) {
  sq_result r = SQ_E_FAIL;
#ifdef _WIN32
  setNativePriority(GetCurrentThread(), p, &r);
#else
  setNativePriority(pthread_self(), p, &r);
#endif
  return r;
}
void sq_thread_set_current_name(const char* name) { setCurrentName(name); }
uint64_t sq_thread_current_id(void) {
#ifdef _WIN32
  return GetCurrentThreadId();
#elif defined(__APPLE__)
  uint64_t id = 0;
  pthread_threadid_np(nullptr, &id);
  return id;
#elif defined(__linux__)
  return static_cast<uint64_t>(syscall(SYS_gettid));
#else
  return reinterpret_cast<uint64_t>(pthread_self());
#endif
}
void sq_thread_sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void sq_thread_yield(void) { std::this_thread::yield(); }
uint32_t sq_cpu_count(void) {
  unsigned n = std::thread::hardware_concurrency();
  return n ? n : 1;
}

// ---- pool ----
sq_result sq_pool_create(uint32_t threads, const char* name, sq_pool** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  auto* p = new (std::nothrow) sq_pool();
  if (!p) return SQ_E_OUTOFMEMORY;
  p->name = name && *name ? name : "sq-pool";
  if (threads == 0) threads = sq_cpu_count();
  try {
    for (uint32_t i = 0; i < threads; ++i) p->threads.emplace_back(poolWorker, p, i);
  } catch (...) {
    sq_pool_destroy(p);
    return SQ_E_FAIL;
  }
  *out = p;
  return SQ_OK;
}
sq_result sq_pool_submit(sq_pool* p, sq_thread_fn fn, void* arg) {
  if (!p || !fn) return SQ_E_INVALIDARG;
  {
    std::lock_guard<std::mutex> lk(p->m);
    if (p->stop) return SQ_E_CLOSED;
    p->queue.emplace_back(fn, arg);
  }
  p->cv.notify_one();
  return SQ_OK;
}
void sq_pool_wait_idle(sq_pool* p) {
  std::unique_lock<std::mutex> lk(p->m);
  p->idleCv.wait(lk, [p] { return p->queue.empty() && p->running == 0; });
}
uint32_t sq_pool_thread_count(sq_pool* p) { return p ? static_cast<uint32_t>(p->threads.size()) : 0; }
void sq_pool_destroy(sq_pool* p) {
  if (!p) return;
  {
    std::lock_guard<std::mutex> lk(p->m);
    p->stop = true;
  }
  p->cv.notify_all();
  for (auto& t : p->threads)
    if (t.joinable()) t.join();
  delete p;
}

// ---- time ----
uint64_t sq_time_monotonic_ns(void) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
int64_t sq_time_unix_ms(void) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // extern "C"
