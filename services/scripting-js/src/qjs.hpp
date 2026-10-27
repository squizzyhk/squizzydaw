// scripting-js: QuickJS-ng include wrapper + small helpers. Internal to this service.
#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "quickjs.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <string>

#include "sq/guid.h"

namespace sqjs {

// JS value -> UTF-8 std::string (ToString semantics). Empty on failure (exception cleared).
inline std::string toStdString(JSContext* ctx, JSValueConst v) {
  size_t len = 0;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return {};
  }
  std::string r(s, len);
  JS_FreeCString(ctx, s);
  return r;
}

inline JSValue newString(JSContext* ctx, const std::string& s) { return JS_NewStringLen(ctx, s.data(), s.size()); }

inline std::string guidToString(const sq_guid& g) {
  char buf[SQ_GUID_STRING_SIZE];
  sq_guid_format(&g, buf);
  return buf;
}

// Strict: only the canonical braced 38-char form counts as a GUID when converting script values.
inline bool parseBracedGuid(const char* s, size_t len, sq_guid* out) {
  if (len != 38 || s[0] != '{' || s[37] != '}') return false;
  return sq_guid_parse(s, out) != 0;
}

// Lenient (braced or not) for explicit GUID parameters.
inline bool parseAnyGuid(const std::string& s, sq_guid* out) { return sq_guid_parse(s.c_str(), out) != 0; }

}  // namespace sqjs
