/* Squizzy Studio platform - UTF-8 / UTF-16 text services. Pure functions, realtime safe. */
#ifndef SQ_PLATFORM_TEXT_H
#define SQ_PLATFORM_TEXT_H

#include "sq/platform/api.h"

/* Returns SQ_TRUE if [s, s+len) is well-formed UTF-8 (no overlongs, no surrogates, <= U+10FFFF). */
SQP_API sq_bool sq_utf8_validate(const char* s, size_t len);
/* Decodes one code point at *p (advancing *p). Invalid sequences yield U+FFFD and advance 1 byte.
   Returns SQ_FALSE at end. */
SQP_API sq_bool sq_utf8_next(const char** p, const char* end, uint32_t* cp);
/* Encodes cp into out[4]; returns byte count (0 if cp invalid). */
SQP_API size_t sq_utf8_encode(uint32_t cp, char* out);
/* Conversions. If out is NULL or too small, *out_len receives the required unit count
   (excluding NUL) and SQ_E_BUFFER_TOO_SMALL is returned. Output is NUL-terminated when it fits.
   Invalid input is replaced by U+FFFD. */
SQP_API sq_result sq_utf8_to_utf16(const char* s, size_t len, uint16_t* out, size_t out_cap, size_t* out_len);
SQP_API sq_result sq_utf16_to_utf8(const uint16_t* s, size_t len, char* out, size_t out_cap, size_t* out_len);
/* ASCII-only case helpers (locale independent). */
SQP_API int sq_ascii_casecmp(const char* a, const char* b);
SQP_API void sq_ascii_lower(char* s);

#endif
