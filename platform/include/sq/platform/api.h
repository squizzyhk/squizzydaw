/* Squizzy Studio platform layer - common definitions for the sqplatform C ABI.
   Every function is plain C (default calling convention), UTF-8 for all strings and paths.
   Memory returned by the platform (sq_buffer, objects) is freed with the matching sq_* call. */
#ifndef SQ_PLATFORM_API_H
#define SQ_PLATFORM_API_H

#include "sq/types.h"
#include "sq/guid.h"

#if defined(SQ_PLATFORM_BUILD)
#  define SQP_API SQ_EXTERN_C SQ_DLL_EXPORT
#else
#  define SQP_API SQ_EXTERN_C SQ_DLL_IMPORT
#endif

/* Heap block owned by sqplatform. data is always NUL-terminated (size excludes the NUL). */
typedef struct sq_buffer {
  char* data;
  size_t size;
} sq_buffer;

SQP_API uint32_t sq_platform_abi_version(void);       /* == SQ_PLATFORM_ABI the DLL was built with */
SQP_API const char* sq_platform_version_string(void); /* SQ_BUILD_STAMP */
SQP_API const char* sq_result_string(sq_result r);    /* static string, never NULL */

SQP_API void* sq_alloc(size_t size);
SQP_API void* sq_realloc(void* p, size_t size);
SQP_API void sq_free(void* p);
SQP_API void sq_buffer_free(sq_buffer* b); /* frees data, zeroes the struct; NULL-safe */

#endif
