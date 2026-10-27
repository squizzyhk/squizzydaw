#include <cstdlib>

#include "internal.h"

extern "C" {

uint32_t sq_platform_abi_version(void) { return SQ_PLATFORM_ABI; }
const char* sq_platform_version_string(void) { return SQ_BUILD_STAMP; }

const char* sq_result_string(sq_result r) {
  switch (r) {
    case SQ_OK: return "ok";
    case SQ_S_FALSE: return "false";
    case SQ_E_FAIL: return "failed";
    case SQ_E_INVALIDARG: return "invalid argument";
    case SQ_E_NOINTERFACE: return "no such interface";
    case SQ_E_NOTFOUND: return "not found";
    case SQ_E_OUTOFMEMORY: return "out of memory";
    case SQ_E_ABI_MISMATCH: return "ABI version mismatch";
    case SQ_E_BUSY: return "busy";
    case SQ_E_TIMEOUT: return "timeout";
    case SQ_E_IO: return "I/O error";
    case SQ_E_PARSE: return "parse error";
    case SQ_E_NOTIMPL: return "not implemented";
    case SQ_E_BUFFER_TOO_SMALL: return "buffer too small";
    case SQ_E_CLOSED: return "closed";
    case SQ_E_EXISTS: return "already exists";
    case SQ_E_ACCESS: return "access denied";
    case SQ_E_TYPE: return "type mismatch";
    case SQ_E_UNSUPPORTED: return "unsupported";
    default: return r >= 0 ? "success" : "unknown error";
  }
}

void* sq_alloc(size_t size) { return std::malloc(size ? size : 1); }
void* sq_realloc(void* p, size_t size) { return std::realloc(p, size ? size : 1); }
void sq_free(void* p) { std::free(p); }
void sq_buffer_free(sq_buffer* b) {
  if (!b) return;
  std::free(b->data);
  b->data = nullptr;
  b->size = 0;
}

}  // extern "C"
