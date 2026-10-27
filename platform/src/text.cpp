#include "internal.h"

namespace {

// Decodes one code point; returns length consumed (>=1) and sets ok=false on malformed input.
size_t decodeUtf8(const unsigned char* p, const unsigned char* end, uint32_t& cp, bool& ok) {
  ok = false;
  cp = 0xFFFD;
  unsigned char c = p[0];
  if (c < 0x80) {
    cp = c;
    ok = true;
    return 1;
  }
  size_t n;
  uint32_t min;
  if ((c & 0xE0) == 0xC0) {
    n = 2;
    cp = c & 0x1F;
    min = 0x80;
  } else if ((c & 0xF0) == 0xE0) {
    n = 3;
    cp = c & 0x0F;
    min = 0x800;
  } else if ((c & 0xF8) == 0xF0) {
    n = 4;
    cp = c & 0x07;
    min = 0x10000;
  } else {
    cp = 0xFFFD;
    return 1;
  }
  if (static_cast<size_t>(end - p) < n) {
    cp = 0xFFFD;
    return 1;
  }
  for (size_t i = 1; i < n; ++i) {
    if ((p[i] & 0xC0) != 0x80) {
      cp = 0xFFFD;
      return 1;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = 0xFFFD;
    return 1;
  }
  ok = true;
  return n;
}

}  // namespace

extern "C" {

sq_bool sq_utf8_validate(const char* s, size_t len) {
  if (!s) return len == 0 ? SQ_TRUE : SQ_FALSE;
  auto p = reinterpret_cast<const unsigned char*>(s);
  auto end = p + len;
  while (p < end) {
    uint32_t cp;
    bool ok;
    size_t n = decodeUtf8(p, end, cp, ok);
    if (!ok) return SQ_FALSE;
    p += n;
  }
  return SQ_TRUE;
}

sq_bool sq_utf8_next(const char** p, const char* end, uint32_t* cp) {
  if (!p || !*p || *p >= end) return SQ_FALSE;
  auto up = reinterpret_cast<const unsigned char*>(*p);
  uint32_t c;
  bool ok;
  size_t n = decodeUtf8(up, reinterpret_cast<const unsigned char*>(end), c, ok);
  *p += n;
  if (cp) *cp = c;
  return SQ_TRUE;
}

size_t sq_utf8_encode(uint32_t cp, char* out) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

sq_result sq_utf8_to_utf16(const char* s, size_t len, uint16_t* out, size_t out_cap, size_t* out_len) {
  if (!s && len) return SQ_E_INVALIDARG;
  auto p = reinterpret_cast<const unsigned char*>(s);
  auto end = p + len;
  size_t n = 0;
  while (p < end) {
    uint32_t cp;
    bool ok;
    p += decodeUtf8(p, end, cp, ok);
    if (cp >= 0x10000) {
      if (out && n + 2 <= out_cap) {
        cp -= 0x10000;
        out[n] = static_cast<uint16_t>(0xD800 + (cp >> 10));
        out[n + 1] = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
      }
      n += 2;
    } else {
      if (out && n + 1 <= out_cap) out[n] = static_cast<uint16_t>(cp);
      n += 1;
    }
  }
  if (out_len) *out_len = n;
  if (!out || n + 1 > out_cap) return SQ_E_BUFFER_TOO_SMALL;
  out[n] = 0;
  return SQ_OK;
}

sq_result sq_utf16_to_utf8(const uint16_t* s, size_t len, char* out, size_t out_cap, size_t* out_len) {
  if (!s && len) return SQ_E_INVALIDARG;
  size_t n = 0;
  for (size_t i = 0; i < len; ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;  // lone surrogate
    }
    char tmp[4];
    size_t k = sq_utf8_encode(cp, tmp);
    if (out && n + k <= out_cap)
      for (size_t j = 0; j < k; ++j) out[n + j] = tmp[j];
    n += k;
  }
  if (out_len) *out_len = n;
  if (!out || n + 1 > out_cap) return SQ_E_BUFFER_TOO_SMALL;
  out[n] = '\0';
  return SQ_OK;
}

int sq_ascii_casecmp(const char* a, const char* b) {
  if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
  for (;; ++a, ++b) {
    unsigned char ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + 32);
    if (ca != cb) return ca < cb ? -1 : 1;
    if (!ca) return 0;
  }
}

void sq_ascii_lower(char* s) {
  if (!s) return;
  for (; *s; ++s)
    if (*s >= 'A' && *s <= 'Z') *s = static_cast<char>(*s + 32);
}

}  // extern "C"
