/* Squizzy Studio SDK - GUID type, parse/format. Header-only, C and C++.
   Canonical text form: "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" (uppercase hex, braces).
   Parsing accepts upper/lower case, with or without braces. */
#ifndef SQ_GUID_H
#define SQ_GUID_H

#include <string.h>
#include "sq/types.h"

typedef struct sq_guid {
  uint32_t data1;
  uint16_t data2;
  uint16_t data3;
  uint8_t data4[8];
} sq_guid;

/* Buffer size for sq_guid_format, including braces and NUL. */
#define SQ_GUID_STRING_SIZE 39

/* Initializer: SQ_GUID_INIT(0x12345678, 0x1234, 0x1234, 0x12,0x34, 0x12,0x34,0x56,0x78,0x9A,0xBC) */
#define SQ_GUID_INIT(d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7) \
  { (uint32_t)(d1), (uint16_t)(d2), (uint16_t)(d3), { (uint8_t)(b0), (uint8_t)(b1), (uint8_t)(b2), (uint8_t)(b3), (uint8_t)(b4), (uint8_t)(b5), (uint8_t)(b6), (uint8_t)(b7) } }

SQ_INLINE int sq_guid_equal(const sq_guid* a, const sq_guid* b) {
  return memcmp(a, b, sizeof(sq_guid)) == 0;
}

SQ_INLINE int sq_guid_is_null(const sq_guid* g) {
  static const sq_guid z = SQ_GUID_INIT(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  return sq_guid_equal(g, &z);
}

SQ_INLINE int sq_guid_hexval_(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parses "{8-4-4-4-12}" or "8-4-4-4-12". Returns 1 on success, 0 on malformed input (out untouched). */
SQ_INLINE int sq_guid_parse(const char* s, sq_guid* out) {
  sq_guid g;
  const char* p;
  int braces, i, h, lo;
  uint8_t bytes[16];
  int nb = 0;
  if (!s || !out) return 0;
  braces = (s[0] == '{');
  p = s + braces;
  for (i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (p[i] != '-') return 0;
      continue;
    }
    h = sq_guid_hexval_(p[i]);
    if (h < 0) return 0;
    lo = sq_guid_hexval_(p[i + 1]);
    if (lo < 0) return 0;
    bytes[nb++] = (uint8_t)((h << 4) | lo);
    ++i;
  }
  if (braces) {
    if (p[36] != '}' || p[37] != '\0') return 0;
  } else if (p[36] != '\0') {
    return 0;
  }
  g.data1 = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
  g.data2 = (uint16_t)((bytes[4] << 8) | bytes[5]);
  g.data3 = (uint16_t)((bytes[6] << 8) | bytes[7]);
  for (i = 0; i < 8; ++i) g.data4[i] = bytes[8 + i];
  *out = g;
  return 1;
}

/* Writes the canonical braced uppercase form into out[SQ_GUID_STRING_SIZE]. */
SQ_INLINE void sq_guid_format(const sq_guid* g, char* out) {
  static const char hx[] = "0123456789ABCDEF";
  uint8_t b[16];
  int i, o = 0;
  b[0] = (uint8_t)(g->data1 >> 24); b[1] = (uint8_t)(g->data1 >> 16);
  b[2] = (uint8_t)(g->data1 >> 8);  b[3] = (uint8_t)(g->data1);
  b[4] = (uint8_t)(g->data2 >> 8);  b[5] = (uint8_t)(g->data2);
  b[6] = (uint8_t)(g->data3 >> 8);  b[7] = (uint8_t)(g->data3);
  for (i = 0; i < 8; ++i) b[8 + i] = g->data4[i];
  out[o++] = '{';
  for (i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
    out[o++] = hx[b[i] >> 4];
    out[o++] = hx[b[i] & 15];
  }
  out[o++] = '}';
  out[o] = '\0';
}

#ifdef __cplusplus
#include <cstddef>
#include <functional>
#include <string>

namespace sq {

namespace detail {
constexpr int guidHex(char c) {
  return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}
constexpr bool guidParse(const char* s, std::size_t n, sq_guid& g) {
  std::size_t off = 0;
  if (n == 38) {
    if (s[0] != '{' || s[37] != '}') return false;
    off = 1;
  } else if (n != 36) {
    return false;
  }
  unsigned char b[16] = {};
  int nb = 0;
  for (std::size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[off + i] != '-') return false;
      continue;
    }
    int h = guidHex(s[off + i]), l = guidHex(s[off + i + 1]);
    if (h < 0 || l < 0) return false;
    b[nb++] = static_cast<unsigned char>((h << 4) | l);
    ++i;
  }
  g.data1 = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
  g.data2 = static_cast<uint16_t>((b[4] << 8) | b[5]);
  g.data3 = static_cast<uint16_t>((b[6] << 8) | b[7]);
  for (int i = 0; i < 8; ++i) g.data4[i] = b[8 + i];
  return true;
}
}  // namespace detail

/* Compile-time GUID literal: constexpr sq_guid id = sq::guid("{...}");
   A malformed literal is a compile error when used in a constant expression. */
template <std::size_t N>
constexpr sq_guid guid(const char (&s)[N]) {
  sq_guid g{};
  if (!detail::guidParse(s, N - 1, g)) throw "sq::guid: malformed GUID literal";
  return g;
}

/* Runtime parse; returns false on malformed input. */
inline bool parseGuid(const std::string& s, sq_guid& out) { return sq_guid_parse(s.c_str(), &out) != 0; }

inline std::string toString(const sq_guid& g) {
  char buf[SQ_GUID_STRING_SIZE];
  sq_guid_format(&g, buf);
  return std::string(buf);
}

}  // namespace sq

constexpr bool operator==(const sq_guid& a, const sq_guid& b) {
  if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) return false;
  for (int i = 0; i < 8; ++i)
    if (a.data4[i] != b.data4[i]) return false;
  return true;
}
constexpr bool operator!=(const sq_guid& a, const sq_guid& b) { return !(a == b); }
constexpr bool operator<(const sq_guid& a, const sq_guid& b) {
  if (a.data1 != b.data1) return a.data1 < b.data1;
  if (a.data2 != b.data2) return a.data2 < b.data2;
  if (a.data3 != b.data3) return a.data3 < b.data3;
  for (int i = 0; i < 8; ++i)
    if (a.data4[i] != b.data4[i]) return a.data4[i] < b.data4[i];
  return false;
}

template <>
struct std::hash<sq_guid> {
  std::size_t operator()(const sq_guid& g) const noexcept {
    uint64_t h = 14695981039346656037ull;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&g);
    for (std::size_t i = 0; i < sizeof(sq_guid); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return static_cast<std::size_t>(h);
  }
};

static_assert(sizeof(sq_guid) == 16, "sq_guid must be 16 bytes");
#endif /* __cplusplus */

#endif /* SQ_GUID_H */
