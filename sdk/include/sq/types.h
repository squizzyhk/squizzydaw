/* Squizzy Studio SDK - fundamental ABI types, result codes, version packing. C and C++. */
#ifndef SQ_TYPES_H
#define SQ_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include "sq/config.h"
#include "sq/version.h" /* generated: SQ_VERSION_*, SQ_PLATFORM_ABI_MAJOR/MINOR */

typedef int32_t sq_result;
typedef int32_t sq_bool;
#define SQ_TRUE 1
#define SQ_FALSE 0

/* Result codes. >= 0 success, < 0 failure. */
#define SQ_OK                    0
#define SQ_S_FALSE               1   /* success, but "no"/nothing done */
#define SQ_E_FAIL               (-1)
#define SQ_E_INVALIDARG         (-2)
#define SQ_E_NOINTERFACE        (-3)
#define SQ_E_NOTFOUND           (-4)
#define SQ_E_OUTOFMEMORY        (-5)
#define SQ_E_ABI_MISMATCH       (-6)
#define SQ_E_BUSY               (-7)
#define SQ_E_TIMEOUT            (-8)
#define SQ_E_IO                 (-9)
#define SQ_E_PARSE              (-10)
#define SQ_E_NOTIMPL            (-11)
#define SQ_E_BUFFER_TOO_SMALL   (-12)
#define SQ_E_CLOSED             (-13)
#define SQ_E_EXISTS             (-14)
#define SQ_E_ACCESS             (-15)
#define SQ_E_TYPE               (-16)
#define SQ_E_UNSUPPORTED        (-17)

#define SQ_SUCCEEDED(r) ((sq_result)(r) >= 0)
#define SQ_FAILED(r)    ((sq_result)(r) < 0)

/* FourCC module tag, e.g. SQ_FOURCC('D','M','M','Y'). */
#define SQ_FOURCC(a, b, c, d) \
  (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/* Module/product version packing: 8 bits major, 8 bits minor, 16 bits patch. */
#define SQ_MAKE_VERSION(maj, min, patch) \
  ((((uint32_t)(maj) & 0xFFu) << 24) | (((uint32_t)(min) & 0xFFu) << 16) | ((uint32_t)(patch) & 0xFFFFu))
#define SQ_VERSION_GET_MAJOR(v) (((uint32_t)(v) >> 24) & 0xFFu)
#define SQ_VERSION_GET_MINOR(v) (((uint32_t)(v) >> 16) & 0xFFu)
#define SQ_VERSION_GET_PATCH(v) ((uint32_t)(v) & 0xFFFFu)
#define SQ_VERSION SQ_MAKE_VERSION(SQ_VERSION_MAJOR, SQ_VERSION_MINOR, SQ_VERSION_PATCH)

/* Platform ABI version: 16 bits major, 16 bits minor.
   A module declares the range [abi_min, abi_max] it accepts; the loader rejects it if the
   running host's SQ_PLATFORM_ABI is outside that range. */
#define SQ_ABI_MAKE(maj, min) ((((uint32_t)(maj) & 0xFFFFu) << 16) | ((uint32_t)(min) & 0xFFFFu))
#define SQ_ABI_GET_MAJOR(v) (((uint32_t)(v) >> 16) & 0xFFFFu)
#define SQ_ABI_GET_MINOR(v) ((uint32_t)(v) & 0xFFFFu)
#define SQ_PLATFORM_ABI SQ_ABI_MAKE(SQ_PLATFORM_ABI_MAJOR, SQ_PLATFORM_ABI_MINOR)
/* Default range for a module built against this SDK: same major, minor >= the one compiled against. */
#define SQ_ABI_DEFAULT_MIN SQ_PLATFORM_ABI
#define SQ_ABI_DEFAULT_MAX SQ_ABI_MAKE(SQ_PLATFORM_ABI_MAJOR, 0xFFFFu)

SQ_INLINE sq_bool sq_abi_in_range(uint32_t host_abi, uint32_t abi_min, uint32_t abi_max) {
  return (host_abi >= abi_min && host_abi <= abi_max) ? 1 : 0;
}

/* Log levels (shared by platform logging and IHostContext::log). */
#define SQ_LOG_TRACE 0
#define SQ_LOG_DEBUG 1
#define SQ_LOG_INFO  2
#define SQ_LOG_WARN  3
#define SQ_LOG_ERROR 4
#define SQ_LOG_FATAL 5
#define SQ_LOG_OFF   6

#endif /* SQ_TYPES_H */
