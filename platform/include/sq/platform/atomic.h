/* Squizzy Studio platform - header-only atomics + spinlock (lock-free, realtime safe).
   C++ code should prefer std::atomic; these exist for C callers and shared-memory layouts. */
#ifndef SQ_PLATFORM_ATOMIC_H
#define SQ_PLATFORM_ATOMIC_H

#include "sq/platform/api.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
SQ_INLINE int32_t sq_atomic_load32(volatile int32_t* p) { return _InterlockedCompareExchange((volatile long*)p, 0, 0); }
SQ_INLINE void sq_atomic_store32(volatile int32_t* p, int32_t v) { _InterlockedExchange((volatile long*)p, v); }
SQ_INLINE int32_t sq_atomic_add32(volatile int32_t* p, int32_t v) { return _InterlockedExchangeAdd((volatile long*)p, v) + v; }
SQ_INLINE int32_t sq_atomic_exchange32(volatile int32_t* p, int32_t v) { return _InterlockedExchange((volatile long*)p, v); }
SQ_INLINE int sq_atomic_cas32(volatile int32_t* p, int32_t expected, int32_t desired) {
  return _InterlockedCompareExchange((volatile long*)p, desired, expected) == expected;
}
SQ_INLINE int64_t sq_atomic_load64(volatile int64_t* p) { return _InterlockedCompareExchange64(p, 0, 0); }
SQ_INLINE void sq_atomic_store64(volatile int64_t* p, int64_t v) { _InterlockedExchange64(p, v); }
SQ_INLINE int64_t sq_atomic_add64(volatile int64_t* p, int64_t v) { return _InterlockedExchangeAdd64(p, v) + v; }
SQ_INLINE int sq_atomic_cas64(volatile int64_t* p, int64_t expected, int64_t desired) {
  return _InterlockedCompareExchange64(p, desired, expected) == expected;
}
SQ_INLINE void sq_cpu_relax(void) { _mm_pause(); }
#else
SQ_INLINE int32_t sq_atomic_load32(volatile int32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
SQ_INLINE void sq_atomic_store32(volatile int32_t* p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
SQ_INLINE int32_t sq_atomic_add32(volatile int32_t* p, int32_t v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
SQ_INLINE int32_t sq_atomic_exchange32(volatile int32_t* p, int32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
SQ_INLINE int sq_atomic_cas32(volatile int32_t* p, int32_t expected, int32_t desired) {
  return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SQ_INLINE int64_t sq_atomic_load64(volatile int64_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
SQ_INLINE void sq_atomic_store64(volatile int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
SQ_INLINE int64_t sq_atomic_add64(volatile int64_t* p, int64_t v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
SQ_INLINE int sq_atomic_cas64(volatile int64_t* p, int64_t expected, int64_t desired) {
  return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#if defined(__x86_64__) || defined(__i386__)
SQ_INLINE void sq_cpu_relax(void) { __builtin_ia32_pause(); }
#elif defined(__aarch64__)
SQ_INLINE void sq_cpu_relax(void) { __asm__ __volatile__("yield"); }
#else
SQ_INLINE void sq_cpu_relax(void) {}
#endif
#endif

/* Spinlock: 0 = free. Only for very short critical sections; never hold across a syscall. */
typedef struct sq_spinlock {
  volatile int32_t state;
} sq_spinlock;
SQ_INLINE int sq_spin_trylock(sq_spinlock* l) { return sq_atomic_cas32(&l->state, 0, 1); }
SQ_INLINE void sq_spin_lock(sq_spinlock* l) {
  while (!sq_atomic_cas32(&l->state, 0, 1)) {
    while (sq_atomic_load32(&l->state) != 0) sq_cpu_relax();
  }
}
SQ_INLINE void sq_spin_unlock(sq_spinlock* l) { sq_atomic_store32(&l->state, 0); }

#endif
