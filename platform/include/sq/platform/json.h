/* Squizzy Studio platform - JSON DOM (RFC 8259 parse + write). UTF-8.
   Ownership: a value is either a root (free with sq_json_free) or owned by its container.
   sq_json_append / sq_json_set take ownership of the child. Object key order is preserved. */
#ifndef SQ_PLATFORM_JSON_H
#define SQ_PLATFORM_JSON_H

#include "sq/platform/api.h"

typedef struct sq_json sq_json;

typedef enum sq_json_type {
  SQ_JSON_NULL = 0,
  SQ_JSON_BOOL = 1,
  SQ_JSON_NUMBER = 2,
  SQ_JSON_STRING = 3,
  SQ_JSON_ARRAY = 4,
  SQ_JSON_OBJECT = 5
} sq_json_type;

SQP_API sq_result sq_json_parse(const char* data, size_t len, sq_json** out, char* err, size_t err_size);
SQP_API sq_result sq_json_write(const sq_json* v, sq_bool pretty, sq_buffer* out);
SQP_API void sq_json_free(sq_json* v);

SQP_API sq_json* sq_json_new_null(void);
SQP_API sq_json* sq_json_new_bool(sq_bool b);
SQP_API sq_json* sq_json_new_number(double d);
SQP_API sq_json* sq_json_new_string(const char* s, size_t len /* (size_t)-1 = strlen */);
SQP_API sq_json* sq_json_new_array(void);
SQP_API sq_json* sq_json_new_object(void);

SQP_API sq_json_type sq_json_type_of(const sq_json* v);
SQP_API sq_bool sq_json_get_bool(const sq_json* v);
SQP_API double sq_json_get_number(const sq_json* v);
SQP_API int64_t sq_json_get_int(const sq_json* v); /* truncated number */
SQP_API const char* sq_json_get_string(const sq_json* v, size_t* len); /* "" if not a string */

SQP_API size_t sq_json_size(const sq_json* v);                /* array/object element count */
SQP_API sq_json* sq_json_at(const sq_json* v, size_t i);        /* array element or object value */
SQP_API const char* sq_json_key_at(const sq_json* v, size_t i); /* object key */
SQP_API sq_json* sq_json_get(const sq_json* obj, const char* key);

SQP_API sq_result sq_json_append(sq_json* arr, sq_json* child);
SQP_API sq_result sq_json_set(sq_json* obj, const char* key, sq_json* child); /* replaces */
SQP_API sq_result sq_json_remove(sq_json* obj, const char* key);

#endif
