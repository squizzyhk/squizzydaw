/* Squizzy Studio platform - locks, events, TLS, threads, worker pool, time.
   Nothing in this header except sq_time_monotonic_ns is realtime safe. */
#ifndef SQ_PLATFORM_THREAD_H
#define SQ_PLATFORM_THREAD_H

#include "sq/platform/api.h"

/* ---- mutex / rwlock / event (NOT for the audio thread) ---- */
typedef struct sq_mutex sq_mutex;
SQP_API sq_result sq_mutex_create(sq_bool recursive, sq_mutex** out);
SQP_API void sq_mutex_destroy(sq_mutex* m);
SQP_API void sq_mutex_lock(sq_mutex* m);
SQP_API sq_bool sq_mutex_trylock(sq_mutex* m);
SQP_API void sq_mutex_unlock(sq_mutex* m);

typedef struct sq_rwlock sq_rwlock;
SQP_API sq_result sq_rwlock_create(sq_rwlock** out);
SQP_API void sq_rwlock_destroy(sq_rwlock* l);
SQP_API void sq_rwlock_lock_shared(sq_rwlock* l);
SQP_API void sq_rwlock_unlock_shared(sq_rwlock* l);
SQP_API void sq_rwlock_lock(sq_rwlock* l);
SQP_API void sq_rwlock_unlock(sq_rwlock* l);

typedef struct sq_event sq_event;
SQP_API sq_result sq_event_create(sq_bool manual_reset, sq_bool initially_set, sq_event** out);
SQP_API void sq_event_destroy(sq_event* e);
SQP_API void sq_event_set(sq_event* e);
SQP_API void sq_event_reset(sq_event* e);
/* timeout_ms < 0 = infinite. SQ_OK or SQ_E_TIMEOUT. */
SQP_API sq_result sq_event_wait(sq_event* e, int32_t timeout_ms);

/* ---- thread-local storage ---- */
typedef uintptr_t sq_tls_key;
SQP_API sq_result sq_tls_alloc(sq_tls_key* out);
SQP_API void sq_tls_free(sq_tls_key key);
SQP_API void* sq_tls_get(sq_tls_key key);
SQP_API sq_result sq_tls_set(sq_tls_key key, void* value);

/* ---- threads ---- */
typedef enum sq_thread_priority {
  SQ_PRIORITY_LOW = 0,
  SQ_PRIORITY_NORMAL = 1,
  SQ_PRIORITY_HIGH = 2,
  SQ_PRIORITY_REALTIME = 3 /* audio render thread; drivers may add MMCSS / SCHED_FIFO on top */
} sq_thread_priority;

typedef void (*sq_thread_fn)(void* arg);
typedef struct sq_thread sq_thread;
SQP_API sq_result sq_thread_create(sq_thread_fn fn, void* arg, const char* name, sq_thread** out);
SQP_API sq_result sq_thread_set_priority(sq_thread* t, sq_thread_priority p);
SQP_API void sq_thread_join(sq_thread* t); /* joins and frees */
SQP_API sq_result sq_thread_set_current_priority(sq_thread_priority p);
SQP_API void sq_thread_set_current_name(const char* name);
SQP_API uint64_t sq_thread_current_id(void);
SQP_API void sq_thread_sleep_ms(uint32_t ms);
SQP_API void sq_thread_yield(void);
SQP_API uint32_t sq_cpu_count(void);

/* ---- worker pool (FIFO, fixed size) ---- */
typedef struct sq_pool sq_pool;
SQP_API sq_result sq_pool_create(uint32_t threads /* 0 = cpu count */, const char* name, sq_pool** out);
SQP_API sq_result sq_pool_submit(sq_pool* p, sq_thread_fn fn, void* arg);
SQP_API void sq_pool_wait_idle(sq_pool* p); /* until the queue is empty and no job is running */
SQP_API uint32_t sq_pool_thread_count(sq_pool* p);
SQP_API void sq_pool_destroy(sq_pool* p); /* runs remaining queued jobs, then joins */

/* ---- time ---- */
SQP_API uint64_t sq_time_monotonic_ns(void);
SQP_API int64_t sq_time_unix_ms(void);

#endif
